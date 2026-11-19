/*
 * File:   cmt.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 11. srpna 2015, 12:07
 *
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzarch_config.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <glib.h>
#include <assert.h>
#include <math.h>

#include "cmt.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/bp_event.h"
#endif

#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/gdg.h"

#include "baseui/baseui.h"
#include "baseui/baseui_filechooser.h"

#define NOT_HAVE_CMT_UI

#include "ui-imgui/cmt/imgui_cmt.h"

/**
 * @brief Dřívější hook jádra pro starý seznam bloků (Tape Filelist): dnes nic.
 *
 * Okno Virtual CMT si stav plní samo každý snímek (cmt_view_fill()),
 * žádné oznámení změny nepotřebuje. Volání zůstávají jako značky míst, kde
 * se mění stav pásky viditelný v UI.
 */
#define ui_cmt_window_update() ((void)0)

#include "cfgmain.h"

#include "cmthack.h"
#include "mzarch/bootstrap_device.h"
#include "cmt_request.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "cmtext.h"
#include "cmtext_block.h"
#include "cmt_tmz.h"
#include "cmt_tmz_import.h"
#include "cmt_tmz_analyze.h"
#include "cmt_tmz_meta.h"
#include <glib/gstdio.h>
#include "cmt_tmz_record.h"
#include "hw-generic/pio8255/pio8255.h"
#include "device_request.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmt_stream/cmt_vstream.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "emulator.h"
#include "replay.h"
#include "media_writes.h"
#include "recent_media/recent_media.h"
#include "audio.h"

st_CMT g_cmt;
char *g_ui_cmt_filters = NULL;

/**
 * @brief Hloubka tichého režimu (cmt_quiet_begin() / cmt_quiet_end()).
 *
 * > 0 = přechody transportu nehlásí BP eventy CMT ani řádky "Virtual CMT
 * is stopped". Jen vlákno emulátoru.
 */
static int s_cmt_quiet = 0;


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/**
 * V1.5 fáze 2.2: HWE BP_EVENT_CMT_MSTATE edge fire helper pro
 * případ bez podvrženého motoru CMT hacku (bez hacku, nebo s hackem při
 * nahrávání; CMTHACK_TEST_FAKES_MOTOR). Mstate je odvozený z CMT_TEST_STOP/PAUSED:
 * mstate = (STOP || PAUSED) ? 0 : 1. Voláno z cmt_stop/cmt_pause/
 * cmt_play/cmt_eject po update state.
 *
 * Edge detection v breakpoints_enforce_hw_event eliminuje duplicate
 * fire na stejný level; helper jen forwarduje aktuální mstate
 * do enforce vrstvy.
 *
 * S podvrženým motorem CMT hacku (CMTHACK_TEST_FAKES_MOTOR) odpaluje
 * přepnutí bitu pio8255_write; tady jen přepnutí zdroje PC4 (konec
 * nahrávání se zapnutým hackem: PC4 čte zase podvržený bit), a to jen při
 * změně čtené hodnoty (cmt_bp_event_mstate_fire_if_changed()).
 */
static void cmt_bp_event_mstate_fire_if_changed ( void );

static void cmt_bp_event_mstate_fire ( void ) {
    if ( !g_bp_event_active[ BP_EVENT_CMT_MSTATE ] ) return;
    /* tichý režim: cache hrany srovná cmt_quiet_end() */
    if ( s_cmt_quiet ) return;
    if ( CMTHACK_TEST_FAKES_MOTOR ) {
        cmt_bp_event_mstate_fire_if_changed ( );
        return;
    }
    int mstate = ( ( CMT_TEST_STOP ) || ( CMT_TEST_PAUSED ) ) ? 0 : 1;
    bp_event_fire ( BP_EVENT_CMT_MSTATE, (int32_t) mstate );
}

/**
 * @brief Odpálí cmt:mstate, jen když se hodnota PC4 liší od cache hrany.
 *
 * Pro přechody, při kterých se přepne zdroj PC4 (pio8255_pc4_get(): stav
 * decku, nebo podvržený bit CMT hacku): začátek nahrávání a jeho konec se
 * zapnutým hackem. Program pak čte jinou hodnotu, aniž by se změnil stav
 * decku nebo podvržený bit; bez změny hodnoty se neodpaluje nic (BP
 * s podmínkou LOW / HIGH by jinak dostal zásah navíc).
 *
 * Vedlejší efekty: breakpoints_enforce_hw_event() (cache hrany, akce BP).
 * @pre Emu vlákno, po změně stavu decku.
 */
static void cmt_bp_event_mstate_fire_if_changed ( void ) {
    if ( !g_bp_event_active[ BP_EVENT_CMT_MSTATE ] ) return;
    if ( s_cmt_quiet ) return;
    uint8_t v = (uint8_t) ( pio8255_pc4_get ( ) & 1 );
    if ( v != g_bp_event_state[ BP_EVENT_CMT_MSTATE ] )
        bp_event_fire ( BP_EVENT_CMT_MSTATE, (int32_t) v );
}

/**
 * V1.6+ TODO 4.6: HWE BP_EVENT_CMT_STATE_CHANGE fire helper.
 *
 * Composes en_CMT_STATE (STOP/PLAY/RECORD) s g_cmt.paused flag do
 * en_BP_CMT_STATE (= BP_CMT_STATE_STOP/PLAY/RECORD/PAUSED). PAUSED je
 * override - pokud je tape paused, fire = PAUSED i kdyz underlying
 * state je PLAY nebo RECORD.
 *
 * Voláno z cmt_stop / cmt_play / cmt_pause / cmt_eject lifecycle
 * prechodů. Edge detection (BP_EVT_KIND_CHANGE) ne-vyuziva trigger
 * condition - implicit "happened", ale enforce vrstva odfilreuje
 * duplicate fire na shodne value (= prev/curr cache).
 */
static void cmt_bp_event_state_change_fire ( void ) {
    if ( !g_bp_event_active[ BP_EVENT_CMT_STATE_CHANGE ] ) return;
    if ( s_cmt_quiet ) return;
    int32_t value;
    if ( CMT_TEST_PAUSED ) {
        value = (int32_t) BP_CMT_STATE_PAUSED;
    } else if ( CMT_TEST_PLAY ) {
        value = (int32_t) BP_CMT_STATE_PLAY;
    } else if ( CMT_TEST_RECORD ) {
        value = (int32_t) BP_CMT_STATE_RECORD;
    } else {
        /* STOP nebo neznamy - fallback STOP. */
        value = (int32_t) BP_CMT_STATE_STOP;
    };
    bp_event_fire ( BP_EVENT_CMT_STATE_CHANGE, value );
}
#else
#define cmt_bp_event_mstate_fire() ((void)0)
#define cmt_bp_event_mstate_fire_if_changed() ((void)0)
#define cmt_bp_event_state_change_fire() ((void)0)
#endif


/* Doxygen viz cmt.h. */
void cmt_quiet_begin(void)
{
    s_cmt_quiet++;
}


/* Doxygen viz cmt.h. */
void cmt_quiet_end(void)
{
    if (s_cmt_quiet <= 0)
        return;
    if (--s_cmt_quiet > 0)
        return;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Cache hrany cmt:mstate na výsledný stav bez odpálení: další skutečný
     * přechod se porovná se stavem po tiché operaci, ne s tím před ní.
     * Hodnota je PC4, jak ho čte program (stav decku, nebo podvržený bit
     * CMT hacku, pio8255_pc4_get()). */
    if (g_bp_event_active[BP_EVENT_CMT_MSTATE])
        g_bp_event_state[BP_EVENT_CMT_MSTATE] = (uint8_t)(pio8255_pc4_get() & 1);
#endif
}


/* Doxygen viz cmt.h. */
bool cmt_quiet_active(void)
{
    return s_cmt_quiet > 0;
}


/* Doxygen viz cmt.h. */
void cmt_cpu_boost_apply(void)
{
    if ((g_cmt.cpu_boost != CMT_CPU_BOOST_ENABLED) || (!CMT_TEST_FILLED)
        || (CMT_TEST_STOP) || (CMT_TEST_PAUSED))
    {
        emulator_max_speed_boost(false);
        return;
    };

    /* PLAY i RECORD bez pauzy. Při RECORD ji pak cmt_screen_done_period()
     * dál vypíná/zapíná podle toho, zda program na pásku zapisuje. */
    emulator_max_speed_boost(true);
}

/**
 * @brief Společné vedlejší efekty přechodu transportu do STOP.
 *
 * Vypne MAX SPEED, pokud ji zapnul cpu_boost (uživatelem zvolenou MAX SPEED
 * nechá - viz cmt_cpu_boost_apply), odpálí BP eventy změny stavu
 * (CMT_MSTATE, CMT_STATE_CHANGE) a aktualizuje UI. Volá se po nastavení
 * g_cmt.state = STOP z cmt_stop() a cmt_reset_transport_without_tape().
 *
 * @pre Voláno z emulátorového vlákna; transport už je ve STOP.
 */
static void cmt_transport_stopped_notify(void)
{
    cmt_cpu_boost_apply();

    cmt_bp_event_mstate_fire ( );
    cmt_bp_event_state_change_fire ( );

    ui_cmt_window_update();
}

/**
 * @brief Převede transport bez vložené pásky do klidového stavu STOP.
 *
 * Obnovuje invariant st_CMT (bez pásky STOP) za běhu: state = STOP,
 * paused = 0, playsts = STOP, output = 0, start_time = paused_time = 0;
 * srovná patch zápisu CMT hacku (cmthack_save_patch_sync()).
 * Pokud byl transport aktivní (PLAY/RECORD nebo pauza), provede stejné
 * vedlejší efekty jako cmt_stop (cmt_transport_stopped_notify: MAX SPEED
 * při cpu_boost, BP eventy, UI). Nulování časové základny zaručí, že následné
 * cmt_open + cmt_play začne přehrávat novou pásku od začátku (dříve se
 * použila pozice ze stavu obnoveného snapshotem).
 *
 * @pre Voláno z emulátorového vlákna; g_cmt.ext == NULL.
 * @post CMT_TEST_STOP a !CMT_TEST_PAUSED.
 */
static void cmt_reset_transport_without_tape(void)
{
    bool was_active = (!CMT_TEST_STOP) || (g_cmt.paused != 0);

    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.user_paused = 0;
    g_cmt.motor_off = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    g_cmt.output = 0;
    g_cmt.start_time = 0;
    g_cmt.paused_time = 0;
    g_cmt.recording_to_stream = 0;
    /* mimo RECORD smí být v ROM patch zápisu (Direct Save) */
    cmthack_save_patch_sync();

    if (!was_active)
        return;

    if (!s_cmt_quiet)
        printf("Virtual CMT is stopped (no tape inserted).\n");

    cmt_transport_stopped_notify();
}

static void cmt_record_finish(void);

/**
 * @brief Cesta souboru, do kterého po STOP zapíše záznam cmt_record_to_file() (NULL = žádný).
 *
 * Vlastnictví: g_strdup() v cmt_record_to_file(), uvolní cmt_record_finish()
 * (použitá cesta), cmt_record() (nový záznam bez cesty) a cmt_eject()
 * (vysunutí, otevření jiné pásky). Záznam ukončený bez cmt_record_finish()
 * (např. cmt_sanitize_state() po načtení snapshotu) tak cestu nepředá
 * dalšímu nahrávání. Jen vlákno emulátoru.
 */
static char *s_record_path = NULL;

/**
 * @brief true = soubor s_record_path vytvořila zkouška zapisovatelnosti v cmt_record_to_file() (předtím neexistoval).
 *
 * Takový soubor zůstane během nahrávání prázdný (WAV se zapíše až po
 * STOP); když se nakonec nic nezapíše (zahození, záznam bez hran, chyba
 * zápisu), cmt_record_probe_remove() ho smaže. Nuluje se s cestou
 * (cmt_record_path_clear()); kopie nahrávání v checkpointu ho nenese
 * (soubor po návratu na checkpoint zůstane). Jen vlákno emulátoru.
 */
static bool s_record_probe_created = false;

/**
 * @brief Doba ticha v s, po které cmt_screen_done_period() záznam ukončila (0 = ukončil ho jiný STOP).
 *
 * Nastaví se těsně před cmt_stop() automatického ukončení, cmt_record_finish()
 * ji použije do zprávy a vynuluje. Jen vlákno emulátoru.
 */
static unsigned s_record_autostop_s = 0;

/**
 * @brief Zapomene cestu souboru záznamu (s_record_path).
 */
static void cmt_record_path_clear(void)
{
    g_free(s_record_path);
    s_record_path = NULL;
    s_record_probe_created = false;
}

/**
 * @brief Smaže prázdný soubor, který vytvořila zkouška zapisovatelnosti tohoto nahrávání.
 *
 * Jen když soubor vytvořila zkouška (s_record_probe_created) a je pořád
 * prázdný; soubor, který existoval předem nebo mezitím dostal obsah,
 * zůstane. Cestu nezapomíná (to dělá cmt_record_path_clear()).
 *
 * @pre Vlákno emulátoru.
 */
static void cmt_record_probe_remove(void)
{
    if (!s_record_path || !s_record_probe_created)
        return;
    GStatBuf st;
    if ((0 == g_stat(s_record_path, &st)) && (st.st_size == 0))
        (void)g_remove(s_record_path);
    s_record_probe_created = false;
}

/**
 * @brief Zastaví přehrávání nebo nahrávání (tlačítko STOP).
 *
 * S vloženou páskou zavolá cb_stop rozšíření, přejde do STOP a vypne MAX
 * SPEED zapnutý přes cpu_boost. Při nahrávání (RECORD) záznam dokončí
 * cmt_record_finish(): dekódování, připojení bloků na konec pásky, otevření
 * prvního nového bloku (transport stojí na jeho začátku), u cesty .wav
 * z cmt_record_to_file() zápis WAV. Konec RECORD vrátí do ROM patch
 * zápisu CMT hacku (cmthack_save_patch_sync(), Direct Save). Pásku nikdy
 * nevysune. Bez vložené pásky transport také uvede do
 * STOP (cmt_reset_transport_without_tape) - dříve to byl no-op, takže
 * stav PLAY obnovený snapshotem nešlo zastavit, přestože MCP hlásilo
 * úspěch.
 *
 * @pre Voláno z emulátorového vlákna.
 * @post CMT_TEST_STOP, paused == user_paused == motor_off == 0, playsts == STOP, output == 0.
 */
void cmt_stop(void)
{
    if (!CMT_TEST_FILLED)
    {
        cmt_reset_transport_without_tape();
        return;
    };
    if (CMT_TEST_STOP)
        return;

    bool was_record = CMT_TEST_RECORD;

    if (g_cmt.ext->cb_stop)
        g_cmt.ext->cb_stop();

    if (!s_cmt_quiet)
        printf("Virtual CMT is stopped.\n");

    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.user_paused = 0;
    g_cmt.motor_off = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    g_cmt.output = 0;
    g_cmt.recording_to_stream = 0;
    /* konec nahrávání: patch zápisu (Direct Save) se vrátí do ROM */
    cmthack_save_patch_sync();

    /* záznam zahozený dřív (cmt_record_discard()) buffer nemá */
    if (was_record && cmttmz_rec_is_active())
        cmt_record_finish();

    cmt_transport_stopped_notify();
}

/**
 * @brief Srovná zdroje pauzy s příznakem paused před změnou jednoho z nich.
 *
 * Bez pauzy uživatele nese stav motoru přímo paused (motor_off se odvodí:
 * motor stojí == paused); tak zůstávají platné i stavy nastavené jen přes
 * paused (snapshot bez nových elementů, testy, cmt_play_paused()). Ve STOP
 * se oba zdroje nulují. Při pauze uživatele se nic nemění (motor_off je
 * tehdy jediný nositel stavu motoru).
 *
 * @pre Vlákno emulátoru.
 * @post Ve STOP user_paused == motor_off == 0; při user_paused == 0
 *       motor_off == paused.
 */
static void cmt_pause_sources_sync(void)
{
    if (CMT_TEST_STOP)
    {
        g_cmt.user_paused = 0;
        g_cmt.motor_off = 0;
        return;
    };
    if (!g_cmt.user_paused)
        g_cmt.motor_off = g_cmt.paused ? 1 : 0;
}

/**
 * @brief Vedlejší efekty změny pohybu pásky (pauza / pokračování) po nastavení paused.
 *
 * CPU boost, výpis, při pokračování nová časová základna (start_time
 * a recording_last_event = teď - paused_time), BP eventy a obnova okna.
 *
 * @pre Vlákno emulátoru; paused a při pauze paused_time už nastavené.
 */
static void cmt_pause_changed(void);

/**
 * @brief Nastaví paused podle zdrojů (pauza uživatele, motor) a při změně provede vedlejší efekty.
 *
 * @pre Vlákno emulátoru, !CMT_TEST_STOP.
 * @post paused == (user_paused || motor_off).
 */
static void cmt_pause_apply_sources(void)
{
    int value = (g_cmt.user_paused || g_cmt.motor_off) ? 1 : 0;
    if (value == g_cmt.paused)
        return;
    g_cmt.paused = value;
    if (value)
        g_cmt.paused_time = gdg_get_total_ticks() - g_cmt.start_time;
    cmt_pause_changed();
}

/* Doxygen viz cmt.h. */
void cmt_motor_toggle(void)
{
    if (CMT_TEST_STOP)
        return;
    cmt_pause_sources_sync();
    g_cmt.motor_off = g_cmt.motor_off ? 0 : 1;
    cmt_pause_apply_sources();
}

/* Doxygen viz cmt.h. */
void cmt_pause(int value)
{
    if (!value)
    {
        if (CMT_TEST_STOP)
            return;
        cmt_pause_sources_sync();
        if (g_cmt.user_paused)
        {
            /* pauza uživatele pryč, motor zůstane, jak ho nastavil počítač */
            g_cmt.user_paused = 0;
        }
        else
        {
            /* bez pauzy uživatele = dosavadní "rozjeď čekající pásku": motor se rozběhne */
            g_cmt.motor_off = 0;
        };
        cmt_pause_apply_sources();
        return;
    };

    if (!CMT_TEST_STOP)
    {
        cmt_pause_sources_sync();
        g_cmt.user_paused = 1;
        cmt_pause_apply_sources();
        return;
    };

    /* STOP: PLAY čekající na motor (cmt_play_paused(): motor_off = 1, bez
     * pauzy uživatele), jako dřív - první hrana PC3 pásku rozjede */
    cmt_play_paused();
    if (CMT_TEST_STOP)
        g_cmt.paused = 0;
    g_cmt.paused_time = gdg_get_total_ticks() - g_cmt.start_time;
    cmt_pause_changed();
}

static void cmt_pause_changed(void)
{
    /* Dříve se při vypnutém cpu_boost (nebo pauze) volalo
     * emulator_max_speed(false) bezpodmínečně, takže pauza pásky
     * vypnula i MAX SPEED zvolenou uživatelem. */
    cmt_cpu_boost_apply();

    if (CMT_TEST_PAUSED)
    {
        printf("Virtual CMT - %s is paused.\n", (CMT_TEST_PLAY) ? "playing" : "recording");
    }
    else
    {
        if (CMT_TEST_PLAY)
        {
            printf("Virtual CMT is playing.\n");
        }
        else
        { // if ( TEST_CMT_RECORD ) {
            printf("Virtual CMT is recording.\n");
        };
        g_cmt.start_time = g_cmt.recording_last_event = gdg_get_total_ticks() - g_cmt.paused_time;
    };

    cmt_bp_event_mstate_fire ( );
    cmt_bp_event_state_change_fire ( );

    ui_cmt_window_update();
}

/**
 * @brief Nastaví g_cmt.last_filename (cesta vložené pásky, jak ji zadal uživatel).
 *
 * Nový řetězec se alokuje dřív, než se starý uvolní; při selhání alokace
 * zůstane původní hodnota. Cesta se nekanonizuje.
 *
 * @param path Cesta (UTF-8); "" = bez pásky. NULL se bere jako "".
 * Souběh s UI: UI vlákno čte last_filename v akcích okna kazety
 * (cmt_tape_actions.cpp: výchozí cesta dialogu otevření - kopii si pořídí
 * pod device_state_lock(), vezme si ho, nedrží-li ho volající -, Save As
 * a export, pod týmž zámkem).
 * Zapisovatelé (cmt_eject(), cmt_open_file()) běží v emu
 * vlákně pod týmž zámkem: dbgapi CMT příkazy (dbgapi_cmd_needs_device_lock()),
 * požadavky z UI (device_request_poll()), replay_media_cmt_execute(),
 * replay_media_prepare_cmt() / replay_media_restore_cmt(), cmt_cli_apply()
 * a load snapshotu (snap_cmt.c). Mimo zámek jen cmt_init() a cmt_exit()
 * při startu a ukončení (cmt_exit() z emulator_exit_cleanup(); UI vlákno
 * v té chvíli ještě běží, ale akce okna kazety volá jen při zásahu
 * uživatele do menu nebo okna kazety [neověřeno, že během ukončení k němu
 * nemůže dojít]).
 *
 * @pre Emulátorové vlákno, volající drží device_state_lock() (kromě
 *      cmt_init() / cmt_exit()).
 * @post g_cmt.last_filename != NULL.
 */
static void cmt_set_last_filename(const char *path)
{
    if (!path)
        path = "";
    size_t len = strlen(path) + 1;
    char *copy = (char *)baseui_tools_mem_alloc0(len);
    if (!copy)
        return;
    memcpy(copy, path, len);
    char *old = g_cmt.last_filename;
    g_cmt.last_filename = copy;
    if (old)
        baseui_tools_mem_free(old);
}

/**
 * @brief Vysune pásku (zastaví transport, zavolá cb_eject, ext = NULL).
 *
 * Bez vložené pásky jen uvede transport do STOP s vynulovanou časovou
 * základnou (cmt_reset_transport_without_tape). cmt_open_file_by_extension
 * volá eject před otevřením nové pásky, takže tím nová páska vždy startuje
 * z čistého STOP: následný cmt_play nastaví start_time a zavolá cb_play
 * bloku, přehrávání začne od začátku pásky. Dříve eject bez pásky nechal
 * stav PLAY/paused ze snapshotu, cmt_play byl pak no-op a cmt_pause(0)
 * dopočítal pozici pásky ze staré paused_time (za koncem nové pásky) -
 * program čekal na hranu signálu, která nikdy nepřišla.
 *
 * Probíhající záznam (RECORD) se zahodí bez dekódování (cmt_record_discard()
 * s důvodem @p why, jednou): nic se nepřipojí ani nezapíše. Neuložené změny
 * pásky (Modified) se ztratí; dotaz dělá UI, MCP vrací varování.
 *
 * @param why Důvod zahození nahrávání do zprávy a logu (anglicky).
 * @pre Voláno z emulátorového vlákna.
 * @post !CMT_TEST_FILLED, CMT_TEST_STOP, ui_base_filename == NULL, last_filename == "" (u pásky).
 */
static void cmt_eject_why(const char *why)
{
    if (!CMT_TEST_FILLED)
    {
        cmt_record_path_clear();
        cmt_reset_transport_without_tape();
        return;
    };
    if (CMT_TEST_RECORD && cmttmz_rec_is_active())
        cmt_record_discard(why);
    if (!CMT_TEST_STOP)
        cmt_stop();
    cmt_record_path_clear();
    /* Vysunutá páska = změna média (media_writes.h): checkpoint záznamu
     * vstupů ji zpět vložit neumí. */
    MEDIA_WRITES_NOTE();
    if (g_cmt.ext->cb_eject)
        g_cmt.ext->cb_eject();
    g_cmt.ext = NULL;

    /* V1.5 fáze 2.3: legacy BP_EVENT_CMT_IN fire pri eject odstranen
     * (= duplicate vuci edge-fire v cmt_update_output). Lifecycle
     * eject neni "edge na vstupnim signalu z pasky" - je to HW event
     * jineho typu. V1.6+ dat samostatny BP_EVENT_CMT_STATE_CHANGE
     * (TODO 4.6 vocabulary expand). */

    /* Eject implicitně přechod do STOP -> mstate může změnit hranu. */
    cmt_bp_event_mstate_fire ( );
    cmt_bp_event_state_change_fire ( );

    if (g_cmt.ui_base_filename)
    {
        baseui_tools_mem_free(g_cmt.ui_base_filename);
        g_cmt.ui_base_filename = NULL;
    };
    cmt_set_last_filename("");

    ui_cmt_window_update();
}

/**
 * @brief Vysune pásku (tlačítko EJECT, MCP, replay): cmt_eject_why("the tape was ejected").
 * @pre Voláno z emulátorového vlákna.
 * @post Jako cmt_eject_why().
 */
void cmt_eject(void)
{
    cmt_eject_why("the tape was ejected");
}

bool cmt_has_unsaved_changes(void)
{
    return CMT_TEST_FILLED && (CMT_TEST_RECORD || cmttmz_is_modified());
}

/**
 * @brief Uvede stav transportu do souladu s vloženou páskou (bez vedlejších efektů).
 *
 * Určeno pro snapshot loader: snapshot obnoví stav transportu (state,
 * paused, časy, playsts), ale obraz pásky neukládá ani neotevírá. Pokud
 * obnovený stav neodpovídá aktuálně vložené pásce, transport se přepne do
 * klidového stavu STOP:
 * - bez vložené pásky (ext == NULL) při libovolném stavu,
 * - PLAY nad páskou, kterou nelze přehrát (cmtext_is_playable selže), nebo
 *   bez otevřeného bloku (páska jen s řídicími bloky, blok, který player
 *   neotevře; snapshot bez elementu tape nebo z verze 2.0.6),
 * - RECORD bez aktivního bufferu záznamu (cmttmz_rec_is_active(); snapshot
 *   pořízený během nahrávání buffer hran nenese).
 *
 * Klidový stav: state = STOP, paused = user_paused = motor_off = 0,
 * playsts = STOP, output = 0, start_time = paused_time = 0,
 * recording_to_stream = 0. Ve stavu STOP (i bez přepnutí) se navíc srovná
 * paused = 0 a playsts = STOP. Zdroje pauzy se srovnají s paused (pauza
 * uživatele jen s paused, motor_off bez pauzy uživatele = paused,
 * cmt_pause_sources_sync()).
 *
 * Funkce záměrně nevolá cb_stop rozšíření, nemění rychlost emulace (to po
 * načtení snapshotu dělá cmt_cpu_boost_apply()), nevolá BP eventy
 * ani aktualizaci UI - jen opravuje hodnoty právě načtené ze snapshotu.
 * Nakonec srovná patch zápisu CMT hacku se stavem transportu
 * (cmthack_save_patch_sync(): v RECORD v ROM není).
 *
 * @return true, pokud byl transport z PLAY/RECORD přepnut do STOP; jinak false.
 *
 * @pre Emulace je pozastavená nebo voláno z emulátorového vlákna.
 * @post Platí invarianty st_CMT (bez pásky STOP, ve STOP paused == user_paused == motor_off == 0,
 *       user_paused => paused).
 */
bool cmt_sanitize_state(void)
{
    bool reset = false;

    if (!CMT_TEST_FILLED)
    {
        reset = true;
    }
    else if (CMT_TEST_PLAY && ((EXIT_SUCCESS != cmtext_is_playable(g_cmt.ext)) || (!g_cmt.ext->block)))
    {
        reset = true;
    }
    else if (CMT_TEST_RECORD && !cmttmz_rec_is_active())
    {
        reset = true;
    };

    bool was_active = !CMT_TEST_STOP;

    if (reset)
    {
        g_cmt.state = CMT_STATE_STOP;
        g_cmt.output = 0;
        g_cmt.start_time = 0;
        g_cmt.paused_time = 0;
        g_cmt.recording_to_stream = 0;
    };

    if (CMT_TEST_STOP)
    {
        g_cmt.paused = 0;
        g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    };
    /* zdroje pauzy: pauza uživatele jen s paused, ve STOP nuly */
    if (!g_cmt.paused)
        g_cmt.user_paused = 0;
    cmt_pause_sources_sync();
    /* stav transportu ze snapshotu: patch zápisu jen mimo RECORD */
    cmthack_save_patch_sync();

    return (reset && was_active);
}

static uint64_t cmt_block_length_ticks(st_CMTEXT_BLOCK *block);

/**
 * @brief Spustí přehrávání vložené pásky (tlačítko PLAY).
 *
 * Bez vložené pásky, mimo STOP, u pásky, kterou nelze přehrát, a u pásky
 * bez otevřeného bloku (páska jen s bloky bez signálu, rozšíření cmt_tmz)
 * nedělá nic - transport zůstane ve STOP.
 *
 * Přehrávání začne na pozici bloku st_CMTEXT_BLOCK::start_gdgticks
 * (nastavené ve STOP přes cmt_tape_set_position(), jinak 0 = začátek
 * bloku); pozice se tím spotřebuje (vynuluje). Pozice za koncem streamu
 * bloku se pro jistotu ořízne na konec (invariant start_gdgticks <= délka
 * streamu udržuje cmt_tape_set_position() a změna rychlosti bloku).
 *
 * @pre Voláno z emulátorového vlákna.
 * @post Při spuštění CMT_TEST_PLAY, playsts = BODY, start_time = teď mínus
 *       pozice (v pauze paused_time = pozice), čtení streamu bloku od
 *       začátku (cb_play; stream se pak čte dopředu do pozice)
 *       a start_gdgticks bloku = 0.
 */
void cmt_play(void)
{
    if (!CMT_TEST_FILLED)
        return;
    if (!CMT_TEST_STOP)
        return;
    if (EXIT_SUCCESS != cmtext_is_playable(g_cmt.ext))
        return;
    if (!g_cmt.ext->block)
        return;
    /* start z pozice nastavené ve STOP (cmt_tape_set_position()), jinak od začátku bloku;
     * pozice se spotřebuje, další STOP + PLAY hraje zase od začátku */
    uint64_t start_ticks = g_cmt.ext->block->start_gdgticks;
    uint64_t block_end = cmt_block_length_ticks(g_cmt.ext->block);
    if (start_ticks > block_end)
        start_ticks = block_end;
    g_cmt.ext->block->start_gdgticks = 0;
    g_cmt.state = CMT_STATE_PLAY;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_BODY;
    g_cmt.start_time = gdg_get_total_ticks() - start_ticks;
    // printf ( "CMT start: %ul\n", gdg_get_total_ticks ( ) );
    g_cmt.ui_player_update = 0;
    ui_cmt_window_update();
    cmt_cpu_boost_apply();
    if (!CMT_TEST_PAUSED)
    {
        printf("Virtual CMT is playing.\n");
    }
    else
    {
        g_cmt.paused_time = start_ticks;
    };
    g_cmt.ext->block->cb_play(g_cmt.ext);

    /* V1.5 fáze 2.3: legacy BP_EVENT_CMT_IN fire pri play odstranen
     * (= duplicate vuci edge-fire v cmt_update_output). Skutecny
     * "play start = signal level 1" zachyti edge fire prvnim
     * cmt_update_output po startu. */

    /* Play -> mstate change (STOP -> RUN). */
    cmt_bp_event_mstate_fire ( );
    cmt_bp_event_state_change_fire ( );
}

/**
 * @brief Spustí přehrávání v pauze se zadanými zdroji pauzy (jádro cmt_play_paused()).
 *
 * Jako cmt_play_paused(), ale pauza uživatele a stav motoru se nastaví
 * na @p user_paused a @p motor_off (výměna bloku v pauze je zachová,
 * cmt_tape_step_block(), cmt_tape_select_block()). Aspoň jeden z nich má
 * být 1, jinak by páska stála s motorem běžícím a bez pauzy uživatele.
 *
 * @param user_paused Pauza uživatele (0/1).
 * @param motor_off Motor stojí (0/1).
 * @pre Voláno z emulátorového vlákna.
 * @post Jako cmt_play_paused(); po startu user_paused a motor_off podle parametrů.
 */
static void cmt_play_paused_keep(int user_paused, int motor_off);

/**
 * @brief Spustí přehrávání v pauze (PLAY + paused, páska stojí, motor stojí).
 *
 * Páska čeká na motor (motor_off = 1, user_paused = 0): rozjede ji hrana
 * PC3 (cmt_motor_toggle()) nebo cmt_pause(0). Volá cmt_play_paused_keep(0, 1).
 *
 * Nastaví paused = 1 a zavolá cmt_play(). Pokud cmt_play() přehrávání
 * nespustí, příznak pauzy se vrátí:
 * - transport zůstal ve STOP (bez pásky, nepřehratelná páska) -> paused =
 *   user_paused = motor_off = 0;
 *   jinak by vznikl stav STOP + paused, který porušuje invariant st_CMT
 *   a následný stop/eject by ho považoval za aktivní transport,
 * - transport už předtím nebyl ve STOP (PLAY/RECORD, cmt_play je no-op)
 *   -> původní hodnoty paused, user_paused a motor_off
 *   (cmt_play_paused_keep(); bez přepočtu paused_time by vynucená pauza
 *   rozbila pozici pásky).
 *
 * @pre Voláno z emulátorového vlákna.
 * @post Buď start z STOP do PLAY s paused == 1, nebo stav transportu beze
 *       změny; ve STOP je vždy paused == 0.
 */
void cmt_play_paused(void)
{
    cmt_play_paused_keep(0, 1);
}

/* Doxygen viz výše (deklarace). */
static void cmt_play_paused_keep(int user_paused, int motor_off)
{
    bool was_stop = CMT_TEST_STOP;
    int prev_paused = g_cmt.paused;
    int prev_user = g_cmt.user_paused;
    int prev_motor_off = g_cmt.motor_off;

    g_cmt.paused = 1;
    g_cmt.user_paused = user_paused ? 1 : 0;
    g_cmt.motor_off = motor_off ? 1 : 0;
    cmt_play();

    if (CMT_TEST_STOP)
    {
        g_cmt.paused = 0;
        g_cmt.user_paused = 0;
        g_cmt.motor_off = 0;
    }
    else if (!was_stop)
    {
        g_cmt.paused = prev_paused;
        g_cmt.user_paused = prev_user;
        g_cmt.motor_off = prev_motor_off;
    };
}

/** @brief Frekvence WAV, který zapisuje cmt_record_to_file() s příponou .wav (Hz). */
#define CMT_RECORD_WAV_RATE 44100u

/**
 * @brief Vezme device_state_lock(), pokud ho volající vlákno ještě nedrží.
 *
 * Změny kontejneru pásky (nová páska, připojené bloky) musí běžet pod
 * zámkem, který čte UI. Zásahy GUI a MCP ho už drží (device_request_poll(),
 * dbgapi), vlastní činnost emulátoru (mez záznamu, testy) ne; zámek není
 * rekurzivní.
 *
 * @return true = zámek vzat, volající ho uvolní device_state_unlock();
 *         false = zámek už drží volající vlákno.
 */
static bool cmt_lock_if_needed(void)
{
    if (device_state_lock_held())
        return false;
    device_state_lock();
    return true;
}


int cmt_record(void)
{
    if (!CMT_TEST_STOP)
        return EXIT_FAILURE;

    if (!CMT_TEST_FILLED)
    {
        bool taken = cmt_lock_if_needed();
        int rc = cmttmz_new_tape();
        if (rc == EXIT_SUCCESS)
        {
            g_cmt.ext = g_cmt_tmz_extension;
            cmt_set_last_filename("");
        };
        if (taken)
            device_state_unlock();
        if (rc != EXIT_SUCCESS)
        {
            fprintf(stderr, "Virtual CMT: can't create a new tape for recording\n");
            ui_cmt_window_update();
            return EXIT_FAILURE;
        };
    };

    /* Nahrávání jen připojuje bloky na konec pásky: checkpoint záznamu
     * vstupů ho vrátí zkrácením pásky (cmttmz_tape_mark_restore()), proto
     * se tu zápis na médium nepočítá. Nahrávání se souborem ho počítá
     * (cmt_record_to_file(): zápis souboru a změnu cesty vrátit nejde). */

    /* nový záznam bez cesty (cmt_record_to_file() ji nastaví až po návratu) */
    cmt_record_path_clear();
    cmttmz_rec_begin();

    /* začíná v pauze se stojícím motorem, rozběh motoru (PC3) ji zruší */
    g_cmt.paused = 1;
    g_cmt.user_paused = 0;
    g_cmt.motor_off = 1;
    g_cmt.state = CMT_STATE_RECORD;
    /* Record = nahrát signál i rutinami ROM: patch zápisu (Direct Save)
     * z ROM pryč, hlavička WHEAD bez těla se zapomene */
    cmthack_save_patch_sync();
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_BODY;
    g_cmt.start_time = gdg_get_total_ticks();
    g_cmt.recording_last_event = g_cmt.start_time;
    g_cmt.paused_time = 0;
    g_cmt.ui_player_update = 0;
    g_cmt.recording_to_stream = 0;
    /* PC4 teď čte stav decku (0) i se zapnutým CMT hackem: změna proti
     * podvrženému bitu = hrana cmt:mstate */
    cmt_bp_event_mstate_fire_if_changed();
    ui_cmt_window_update();

    printf("Virtual CMT - recording is paused.\n");
    return EXIT_SUCCESS;
}


/**
 * @brief Výsledek posledního přímého uložení (cmt_direct_save_get_result()).
 *
 * Zapisuje cmt_direct_save_mzf() pod device_state_lock(), UI čte pod týmž
 * zámkem. Vysunutí ani vložení pásky výsledek nemaže (nové poznáš podle
 * čítače serial).
 */
static st_CMT_DIRECT_SAVE_RESULT s_direct_save_result = {0, false, -1, 0, ""};


void cmt_direct_save_get_result(st_CMT_DIRECT_SAVE_RESULT *out)
{
    *out = s_direct_save_result;
}


/**
 * @brief Zapíše výsledek přímého uložení pro okno Virtual CMT (pod zámkem stavu zařízení).
 * @param ok Úspěch. @param block_id Nový blok (-1 při chybě).
 * @param mzf Uložené MZF (jméno a délka). @param size Délka těla.
 * @pre Vlákno emulátoru.
 */
static void cmt_direct_save_note(bool ok, int block_id, const st_MZF *mzf, uint32_t size)
{
    bool taken = cmt_lock_if_needed();
    s_direct_save_result.serial++;
    if (s_direct_save_result.serial == 0)
        s_direct_save_result.serial = 1; /* 0 = žádné uložení */
    s_direct_save_result.ok = ok;
    s_direct_save_result.block_id = ok ? block_id : -1;
    s_direct_save_result.size = size;
    mzf_tools_get_fname(&mzf->header, s_direct_save_result.name);
    if (taken)
        device_state_unlock();
}


int cmt_direct_save_mzf(const st_MZF *mzf, int *block_id)
{
    if (block_id)
        *block_id = -1;
    if (!mzf)
        return EXIT_FAILURE;

    char name[CMT_DIRECT_SAVE_NAME_SIZE];
    mzf_tools_get_fname(&mzf->header, name);
    uint32_t size = mzf->header.fsize;

    if (!CMT_TEST_FILLED)
    {
        /* bez pásky: nová prázdná páska jako u cmt_record() */
        bool taken = cmt_lock_if_needed();
        int rc = cmttmz_new_tape();
        if (rc == EXIT_SUCCESS)
        {
            g_cmt.ext = g_cmt_tmz_extension;
            cmt_set_last_filename("");
        };
        if (taken)
            device_state_unlock();
        if (rc != EXIT_SUCCESS)
        {
            fprintf(stderr, "CMT hack: can't create a new tape to save \"%s\"\n", name);
            cmt_direct_save_note(false, -1, mzf, size);
            ui_cmt_window_update();
            return EXIT_FAILURE;
        };
    }
    else if (CMT_TEST_PLAY)
    {
        /* přímé uložení pásku přebírá jako přímé načtení (cmthack.c) */
        cmt_stop();
    };

    const bool recording = CMT_TEST_RECORD;

    st_CMTTMZ_IMPORT_OPTS opts;
    cmttmz_import_opts_init(&opts);
    opts.default_speed = g_cmt.mz_cmtspeed;
    opts.custom_pulses = g_cmt.mz_custom_pulses;
    const uint16_t pause_ms = cmttmz_rec_mzf_pause(&opts, 0);

    st_TZX_BLOCK block;
    memset(&block, 0, sizeof(block));
    int rc = cmttmz_rec_build_mzf(mzf, &opts, pause_ms, &block);
    int id = -1;
    if (rc == EXIT_SUCCESS)
    {
        /* záznam rychlosti jako blok MZF z nahrávání (cmt_record_finish()) */
        st_CMTTMZ_BLOCK_SPEED speed;
        memset(&speed, 0, sizeof(speed));
        speed.origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
        speed.speed = opts.default_speed;
        if (cmtspeed_is_custom(opts.default_speed))
            speed.pulses = opts.custom_pulses;
        speed.pause_ms = pause_ms;

        bool taken = cmt_lock_if_needed();
        int old = cmtext_container_get_count_blocks(g_cmt.ext->container);
        rc = cmttmz_append_blocks(&block, &speed, 1);
        if (rc == EXIT_SUCCESS)
        {
            id = old;
            /* mimo RECORD páska stojí na novém bloku (VERIFY ho porovná, LOAD
             * ho načte); otevřený blok se změnil, stav hacku nad páskou pryč */
            if (!recording && (EXIT_SUCCESS == cmttmz_seek_block(id)))
                cmthack_tape_state_clear();
        };
        if (taken)
            device_state_unlock();
    };
    /* po úspěšném připojení data převzala páska (block.data == NULL) */
    free(block.data);

    if (rc != EXIT_SUCCESS)
    {
        fprintf(stderr, "CMT hack: \"%s\" could not be added to the tape\n", name);
        cmt_direct_save_note(false, -1, mzf, size);
        ui_cmt_window_update();
        return EXIT_FAILURE;
    };

    printf("CMT hack: saved \"%s\", %u bytes, as tape block %d\n", name, (unsigned)size, id);
    cmt_direct_save_note(true, id, mzf, size);
    if (block_id)
        *block_id = id;
    ui_cmt_window_update();
    return EXIT_SUCCESS;
}


void cmt_record_discard(const char *why)
{
    cmttmz_rec_discard();
    /* nic se nezapíše: prázdný soubor ze zkoušky zapisovatelnosti pryč */
    cmt_record_probe_remove();
    cmt_record_path_clear();
    gchar *msg = g_strdup_printf("Recording discarded (%s)", why);
    bool taken = cmt_lock_if_needed();
    cmttmz_set_record_discarded(msg);
    if (taken)
        device_state_unlock();
    printf("Virtual CMT: %s\n", msg);
    g_free(msg);
}


void cmt_record_drop(void)
{
    cmttmz_rec_discard();
    cmt_record_path_clear();
}


void cmt_eject_for_checkpoint(void)
{
    if (CMT_TEST_RECORD && cmttmz_rec_is_active())
        cmt_record_drop();
    uint32_t writes = g_media_writes;
    cmt_quiet_begin();
    cmt_eject();
    cmt_quiet_end();
    /* návrat do stavu checkpointu není zápis na médium (media_writes.h) */
    g_media_writes = writes;
}


int cmt_new_tape(void)
{
    cmt_eject_why("a new tape was inserted");
    bool taken = cmt_lock_if_needed();
    int rc = cmttmz_new_tape();
    if (rc == EXIT_SUCCESS)
    {
        g_cmt.ext = g_cmt_tmz_extension;
        cmt_set_last_filename("");
    };
    if (taken)
        device_state_unlock();
    ui_cmt_window_update();
    if (rc != EXIT_SUCCESS)
        baseui_error("Virtual CMT: a new tape could not be created\n");
    return rc;
}


/**
 * @brief Zapíše záznam z bufferu hran do WAV CMT_RECORD_WAV_RATE Hz.
 *
 * Úseky se převedou na vzorky kumulativně: konec úseku i =
 * round ( součet taktů 0 .. i * CMT_RECORD_WAV_RATE / GDGCLK_BASE ), takže
 * se zaokrouhlovací chyba nesčítá (dříve se zaokrouhloval každý úsek
 * zvlášť). Úsek kratší než půl vzorku nedá žádný vzorek. Úroveň vzorku
 * je úroveň PC1 (vstream s normální polaritou jako dřívější nahrávání do
 * WAV). Doba před první hranou se nezapisuje.
 *
 * @param path Cílový soubor (UTF-8).
 * @return EXIT_SUCCESS; EXIT_FAILURE bez úseků, při selhání alokace nebo zápisu.
 * @pre Vlákno emulátoru, buffer záznamu ještě nezahozený.
 */
static int cmt_record_write_wav(const char *path)
{
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    if (!rec->count)
        return EXIT_FAILURE;
    st_CMT_STREAM *stream = cmt_stream_new(CMT_STREAM_TYPE_VSTREAM);
    if (!stream)
        return EXIT_FAILURE;
    stream->str.vstream = cmt_vstream_new(CMT_RECORD_WAV_RATE, CMT_VSTREAM_BYTELENGTH8, rec->first_level & 1, CMT_STREAM_POLARITY_NORMAL);
    if (!stream->str.vstream)
    {
        cmt_stream_destroy(stream);
        return EXIT_FAILURE;
    };
    uint64_t cum = 0;
    uint64_t prev = 0;
    int ok = 1;
    for (uint32_t i = 0; ok && i < rec->count; i++)
    {
        cum += rec->ticks[i];
        uint64_t end = (cum * CMT_RECORD_WAV_RATE + GDGCLK_BASE / 2) / GDGCLK_BASE;
        if (end > prev)
        {
            int level = (rec->first_level ^ (int)(i & 1u)) & 1;
            ok = (EXIT_SUCCESS == cmt_vstream_add_value_long(stream->str.vstream, level, end - prev));
            prev = end;
        };
    };
    int rc = ok ? cmt_stream_save_wav(stream, CMT_RECORD_WAV_RATE, (char *)path) : EXIT_FAILURE;
    cmt_stream_destroy(stream);
    return rc;
}


/**
 * @brief Dokončí záznam po přechodu z RECORD do STOP (volá cmt_stop()).
 *
 * 1. cmttmz_rec_decode() s volbami importu a výchozí rychlostí Virtual CMT
 *    (g_cmt.mz_cmtspeed, g_cmt.mz_custom_pulses), stejně jako otevření pásky.
 * 2. Bloky se připojí na konec pásky (cmttmz_append_blocks(); MZF jako
 *    CMTTMZ_SPEED_ORIGIN_DEFAULT s pauzou z dekodéru, surový blok
 *    ORIGIN_NONE) pod device_state_lock() a otevře se první nově přidaný
 *    blok, který jde otevřít (transport stojí na jeho začátku). Bez nových
 *    bloků zůstane otevřený dosavadní blok.
 * 3. Cesta z cmt_record_to_file() s příponou .wav: zápis WAV z bufferu
 *    (bez úseků nic; prázdný soubor ze zkoušky zapisovatelnosti se pak
 *    smaže); s příponou pásky uložení celé pásky, jen když se něco
 *    připojilo.
 * 4. Zpráva (cmttmz_set_record_message(), log): text dekodéru, při mezi
 *    bufferu s předponou "Recording stopped: length limit reached; ", při
 *    automatickém ukončení "Recording stopped automatically after N s of
 *    silence; ",
 *    případně neúspěch připojení a výsledek zápisu WAV.
 * 5. Buffer záznamu se zahodí.
 *
 * @pre Vlákno emulátoru, transport už ve STOP, páska vložená (cmt_tmz).
 * @post Buffer hran neaktivní (cmttmz_rec_discard()), s_record_path NULL.
 */
static void cmt_record_finish(void)
{
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    GString *msg = g_string_new(cmttmz_rec_limit_reached() ? "Recording stopped: length limit reached; " : "");
    if (s_record_autostop_s)
        g_string_append_printf(msg, "Recording stopped automatically after %u s of silence; ", s_record_autostop_s);
    s_record_autostop_s = 0;

    st_CMTTMZ_IMPORT_OPTS opts;
    cmttmz_import_opts_init(&opts);
    opts.default_speed = g_cmt.mz_cmtspeed;
    opts.custom_pulses = g_cmt.mz_custom_pulses;

    st_TZX_BLOCK *blocks = NULL;
    st_CMTTMZ_REC_BLOCK_INFO *info = NULL;
    uint32_t count = 0;
    st_CMTTMZ_REC_REPORT rep;
    int rc = cmttmz_rec_decode(rec, &opts, &blocks, &info, &count, &rep);
    g_string_append(msg, rep.text);

    uint32_t added_mzf = 0, added_raw = 0;
    if ((rc == EXIT_SUCCESS) && count)
    {
        st_CMTTMZ_BLOCK_SPEED *speeds = (st_CMTTMZ_BLOCK_SPEED *)calloc(count, sizeof(st_CMTTMZ_BLOCK_SPEED));
        if (!speeds)
        {
            rc = EXIT_FAILURE;
        }
        else
        {
            for (uint32_t i = 0; i < count; i++)
            {
                if (info[i].kind != CMTTMZ_REC_BLOCK_MZF)
                    continue; /* surový blok: ORIGIN_NONE, rychlost ze souboru */
                speeds[i].origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
                speeds[i].speed = opts.default_speed;
                if (cmtspeed_is_custom(opts.default_speed))
                    speeds[i].pulses = opts.custom_pulses;
                speeds[i].pause_ms = info[i].pause_ms;
            };
            bool taken = cmt_lock_if_needed();
            int old = cmtext_container_get_count_blocks(g_cmt.ext->container);
            rc = cmttmz_append_blocks(blocks, speeds, count);
            if (rc == EXIT_SUCCESS)
            {
                added_mzf = rep.mzf_blocks;
                added_raw = rep.raw_blocks;
                for (int k = old; k < old + (int)count; k++)
                {
                    if (EXIT_SUCCESS == cmttmz_seek_block(k))
                        break;
                };
            };
            if (taken)
                device_state_unlock();
            free(speeds);
        };
        if (rc != EXIT_SUCCESS)
            g_string_append(msg, "; the recording could not be added to the tape");
    };
    /* po úspěšném připojení jsou data bloků NULL, uvolní se jen pole */
    cmttmz_rec_blocks_free(blocks, count);
    free(info);

    if (s_record_path)
    {
        const char *fext = cmtext_get_filename_extension(s_record_path);
        if (fext && (0 == strcasecmp(fext, "wav")))
        {
            if (!rec->count)
                g_string_append_printf(msg, "; nothing recorded, %s not written", s_record_path);
            else if (EXIT_SUCCESS == cmt_record_write_wav(s_record_path))
                g_string_append_printf(msg, "; WAV saved: %s", s_record_path);
            else
                g_string_append_printf(msg, "; can't write WAV file %s", s_record_path);
            /* WAV nevznikl: prázdný soubor ze zkoušky nenechat (zapsaný už prázdný není) */
            cmt_record_probe_remove();
        }
        else if (!rec->count)
        {
            /* jako u .wav: bez záznamu se páska neuloží */
            g_string_append_printf(msg, "; nothing recorded, %s not written", s_record_path);
        }
        else if (!added_mzf && !added_raw)
        {
            /* hrany byly, ale nic se nepřipojilo (méně než 64 pulzů, chyba
             * připojení): páska je stejná jako před nahráváním, neukládá se */
            g_string_append_printf(msg, "; nothing was added to the tape, %s not written", s_record_path);
        }
        else
        {
            /* jiná přípona: uložit celou pásku (mění cestu pásky, pod zámkem) */
            bool taken = cmt_lock_if_needed();
            (void)cmttmz_save(s_record_path);
            g_string_append_printf(msg, "; %s", cmttmz_get_save_message());
            if (taken)
                device_state_unlock();
        };
        cmt_record_path_clear();
    };

    /* zprávu čte UI pod zámkem stavu zařízení (cmttmz_get_record_message()) */
    bool msg_taken = cmt_lock_if_needed();
    cmttmz_set_record_result(msg->str, added_mzf, added_raw);
    if (msg_taken)
        device_state_unlock();
    printf("Virtual CMT: %s\n", msg->str);
    g_string_free(msg, TRUE);
    cmttmz_rec_discard();
    ui_cmt_window_update();
}

void cmt_stop_button(void)
{
    /* RECORD i PLAY: cmt_stop() (záznam se připojí k pásce, páska zůstane) */
    cmt_stop();
}

int cmt_tape_step_block(int forward)
{
    /* při nahrávání otevřený blok neměnit (jako cmt_tape_seek_block()) */
    if (!CMT_TEST_FILLED || !g_cmt.ext->container || CMT_TEST_RECORD)
        return EXIT_FAILURE;
    cmtext_container_cb_next_block step = forward ? g_cmt.ext->container->cb_next_block
                                                  : g_cmt.ext->container->cb_previous_block;
    if (!step)
        return EXIT_FAILURE;

    gboolean fl_play = CMT_TEST_PLAY;
    gboolean fl_paused = CMT_TEST_PAUSED;
    cmt_pause_sources_sync();
    int user_paused = g_cmt.user_paused;
    int motor_off = g_cmt.motor_off;
    int pos_block = -1;
    uint64_t pos_ticks = 0;
    if (fl_play && (EXIT_SUCCESS != cmt_tape_get_position(&pos_block, &pos_ticks)))
        pos_block = -1;
    if (fl_play)
        cmt_stop();
    if (EXIT_FAILURE == step())
    {
        /* krok nejde (tím směrem není blok): přehrávání na původním místě
         * pokračuje, pauza uživatele ani stav motoru se neztratí */
        if (fl_play)
        {
            if (pos_block >= 0)
                (void)cmt_tape_set_position(pos_block, pos_ticks);
            if (fl_paused)
                cmt_play_paused_keep(user_paused, motor_off);
            else
                cmt_play();
        };
        return EXIT_FAILURE;
    };
    if (fl_play)
    {
        if (fl_paused)
            cmt_play_paused_keep(user_paused, motor_off);
        else
            cmt_play();
    };
    ui_cmt_window_update();
    return EXIT_SUCCESS;
}

int cmt_tape_select_block(int block_id, int forced)
{
    /* při nahrávání otevřený blok neměnit ani s forced (jako cmt_tape_seek_block()) */
    if (!CMT_TEST_FILLED || !g_cmt.ext->container || !g_cmt.ext->container->cb_open_block || CMT_TEST_RECORD)
        return EXIT_FAILURE;

    gboolean fl_stop = CMT_TEST_STOP;
    if ((!fl_stop) && (!forced))
        return EXIT_FAILURE;

    int old_block = cmtext_block_get_block_id(g_cmt.ext->block);
    gboolean fl_play = CMT_TEST_PLAY;
    gboolean fl_paused = CMT_TEST_PAUSED;
    cmt_pause_sources_sync();
    int user_paused = g_cmt.user_paused;
    int motor_off = g_cmt.motor_off;

    if (old_block != block_id)
    {
        if (fl_play)
            cmt_stop();
        if (EXIT_SUCCESS != g_cmt.ext->container->cb_open_block(block_id))
            return EXIT_FAILURE;
        if (forced)
        {
            /* dvojklik na jiný blok: v pauze zůstane pauza (oba její zdroje), jinak play */
            if (fl_paused)
                cmt_play_paused_keep(user_paused, motor_off);
            else
                cmt_play();
        };
    }
    else
    {
        if (!forced)
            return EXIT_SUCCESS;
        /* dvojklik na stejný blok v pauze uživatele: blok znovu od začátku
         * a pauza zůstane (oba její zdroje), jako dvojklik na jiný blok
         * (dřív pokračoval z místa pauzy) */
        if (fl_play && fl_paused && user_paused)
        {
            cmt_stop();
            if (EXIT_SUCCESS != g_cmt.ext->container->cb_open_block(block_id))
                return EXIT_FAILURE;
            cmt_play_paused_keep(user_paused, motor_off);
            ui_cmt_window_update();
            return EXIT_SUCCESS;
        };
        /* jinak: čekání na motor -> rozjede (cmt_pause(0)), play -> pauza, stop -> play */
        if (fl_paused)
            cmt_pause(0);
        else if (fl_play)
            cmt_pause(1);
        else
            cmt_play();
    };
    ui_cmt_window_update();
    return EXIT_SUCCESS;
}

int cmt_tape_seek_block(int block_id)
{
    if ((!CMT_TEST_FILLED) || (!g_cmt.ext->container) || (!g_cmt.ext->container->cb_open_block) || CMT_TEST_RECORD)
        return EXIT_FAILURE;

    /* při selhání zůstane otevřený blok i transport beze změny */
    if (EXIT_SUCCESS != g_cmt.ext->container->cb_open_block(block_id))
        return EXIT_FAILURE;

    st_CMTEXT_BLOCK *block = g_cmt.ext->block;
    if (CMT_TEST_PLAY && block)
    {
        /* Časová základna dosud patřila starému bloku: nový blok hraje od
         * začátku (dříve se četl od času uplynulého ve starém bloku). */
        g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_BODY;
        block->cb_play(g_cmt.ext);
        if (CMT_TEST_PAUSED)
        {
            g_cmt.paused_time = 0;
            int output = g_cmt.output;
            uint64_t transferred = 0;
            if (CMTEXT_BLOCK_PLAYSTS_BODY == cmtext_block_get_output(block, 0, &output, &transferred))
                g_cmt.output = output;
        }
        else
        {
            g_cmt.start_time = gdg_get_total_ticks();
        };
    };
    ui_cmt_window_update();
    return EXIT_SUCCESS;
}

int cmt_tape_block_status(int block_id, en_CMT_BLOCK_STATUS *status)
{
    *status = CMT_BLOCK_STATUS_OK;
    st_CMTEXT_CONTAINER *c = (CMT_TEST_FILLED && g_cmt.ext) ? cmtext_get_container(g_cmt.ext) : NULL;
    if (!c || !c->tape || (block_id < 0) || (block_id >= cmtext_container_get_count_blocks(c)))
        return EXIT_FAILURE;
    const st_CMTEXT_TAPE_INDEX *ix = &c->tape->index[block_id];
    if (ix->bltype == CMTEXT_BLOCK_TYPE_CONTROL)
    {
        *status = CMT_BLOCK_STATUS_CONTROL;
        return EXIT_SUCCESS;
    };
    /* nepřehratelnost zná jen rozšíření cmt_tmz; jinde je blok přehratelný */
    st_CMTTMZ_BLOCK_UI bui;
    if ((EXIT_SUCCESS == cmttmz_get_block_ui(block_id, &bui)) && bui.unplayable)
    {
        *status = CMT_BLOCK_STATUS_CANNOT_PLAY;
        return EXIT_SUCCESS;
    };
    if ((ix->bltype == CMTEXT_BLOCK_TYPE_WAV)
        || ((ix->bltype == CMTEXT_BLOCK_TYPE_TZX) && (ix->item.tzx.id == TZX_BLOCK_ID_CSW_RECORDING)))
        *status = CMT_BLOCK_STATUS_NOT_DECODED;
    return EXIT_SUCCESS;
}

int cmt_tape_first_playable_block(void)
{
    st_CMTEXT_CONTAINER *c = (CMT_TEST_FILLED && g_cmt.ext) ? cmtext_get_container(g_cmt.ext) : NULL;
    int count = c ? cmtext_container_get_count_blocks(c) : 0;
    for (int i = 0; i < count; i++)
    {
        en_CMT_BLOCK_STATUS st;
        if (EXIT_SUCCESS != cmt_tape_block_status(i, &st))
            return -1;
        if ((st == CMT_BLOCK_STATUS_OK) || (st == CMT_BLOCK_STATUS_NOT_DECODED))
            return i;
    };
    return -1;
}

int cmt_record_to_file(const char *path)
{
    if (!path || path[0] == '\0')
        return EXIT_FAILURE;

    /* Nahrávat lze jen ze stavu STOP (= shodně s cmt_record()). */
    if (!CMT_TEST_STOP)
        return EXIT_FAILURE;

    const char *fext = cmtext_get_filename_extension(path);
    bool is_wav = fext && (0 == strcasecmp(fext, "wav"));
    bool is_tape = fext && ((0 == strcasecmp(fext, "tmz")) || (0 == strcasecmp(fext, "tzx")) || (0 == strcasecmp(fext, "mzt")) ||
                            (0 == strcasecmp(fext, "mzf")) || (0 == strcasecmp(fext, "m12")));
    if (!is_wav && !is_tape)
    {
        baseui_error("CMT recording: unsupported file type '%s'\n", path);
        return EXIT_FAILURE;
    };

    /* Zapisovatelnost cesty hned (MCP chce deterministické selhání), bez
     * zkrácení existujícího souboru: zápis až po STOP. Neexistující soubor
     * .wav zkouška vytvoří prázdný a nechá ho po dobu nahrávání (MCP smoke
     * test čeká soubor hned po zahájení); když se nakonec nic nezapíše,
     * smaže ho cmt_record_probe_remove(). */
    bool existed = g_file_test(path, G_FILE_TEST_EXISTS);
    FILE *wtst = baseui_tools_file_open(path, "ab");
    if (!wtst)
    {
        baseui_error("CMT recording: path not writable: '%s'\n", path);
        return EXIT_FAILURE;
    };
    baseui_tools_file_close(wtst);
    /* pásku zapíše až cmttmz_save() po STOP; prázdný soubor ze zkoušky nenechat */
    if (is_tape && !existed)
        g_remove(path);

    if (EXIT_SUCCESS != cmt_record())
    {
        if (is_wav && !existed)
            g_remove(path);
        return EXIT_FAILURE;
    };

    /* soubor po STOP (a u pásky změna její cesty) checkpoint nevrátí (media_writes.h) */
    MEDIA_WRITES_NOTE();
    s_record_path = g_strdup(path);
    /* WAV ze zkoušky smaže zahození nebo konec bez zápisu (cmt_record_probe_remove()) */
    s_record_probe_created = is_wav && !existed;
    return EXIT_SUCCESS;
}


/**
 * @brief Kopie probíhajícího nahrávání pro checkpoint záznamu vstupů.
 * @invariant rec != NULL.
 */
struct st_CMT_RECORD_CKPT
{
    st_CMTTMZ_REC_COPY *rec; /**< kopie bufferu hran (vlastní) */
    char *path;              /**< cesta souboru záznamu (s_record_path), NULL = bez souboru */
};


st_CMT_RECORD_CKPT *cmt_record_ckpt_take(size_t max_bytes, bool *too_big)
{
    if (too_big)
        *too_big = false;
    if (!CMT_TEST_RECORD)
        return NULL;
    int big = 0;
    st_CMTTMZ_REC_COPY *rec = cmttmz_rec_copy_take(max_bytes, &big);
    if (too_big)
        *too_big = (big != 0);
    if (!rec)
        return NULL;
    st_CMT_RECORD_CKPT *c = g_new0(st_CMT_RECORD_CKPT, 1);
    c->rec = rec;
    c->path = s_record_path ? g_strdup(s_record_path) : NULL;
    return c;
}


size_t cmt_record_ckpt_bytes(const st_CMT_RECORD_CKPT *c)
{
    return c ? cmttmz_rec_copy_bytes(c->rec) : 0;
}


int cmt_record_ckpt_install(const st_CMT_RECORD_CKPT *c)
{
    if (!c || !CMT_TEST_RECORD)
        return EXIT_FAILURE;
    if (EXIT_SUCCESS != cmttmz_rec_copy_install(c->rec))
        return EXIT_FAILURE;
    cmt_record_path_clear();
    s_record_path = c->path ? g_strdup(c->path) : NULL;
    return EXIT_SUCCESS;
}


void cmt_record_ckpt_free(st_CMT_RECORD_CKPT *c)
{
    if (!c)
        return;
    cmttmz_rec_copy_free(c->rec);
    g_free(c->path);
    g_free(c);
}


int cmt_open_image(const uint8_t *data, size_t size, const char *path, const uint8_t *wav_blocks, size_t wav_count)
{
    if (!path)
        path = "";
    cmt_eject();
    if (!data || size == 0 || size > UINT32_MAX)
        return EXIT_FAILURE;
    if (wav_count > UINT32_MAX)
        wav_count = UINT32_MAX;
    bool taken = cmt_lock_if_needed();
    int rc = cmttmz_open_image(data, (uint32_t)size, path, wav_blocks, (uint32_t)wav_count);
    if (rc == EXIT_SUCCESS)
    {
        g_cmt.ext = g_cmt_tmz_extension;
        g_cmt.ui_base_filename = path[0] ? g_path_get_basename(path) : NULL;
        cmt_set_last_filename(path);
    };
    if (taken)
        device_state_unlock();
    ui_cmt_window_update();
    return rc;
}


int cmt_tape_mark_restore(const struct st_CMTTMZ_TAPE_MARK *mark)
{
    if (!CMT_TEST_FILLED || (g_cmt.ext != g_cmt_tmz_extension) || !mark)
        return EXIT_FAILURE;
    bool taken = cmt_lock_if_needed();
    int rc = cmttmz_tape_mark_restore(mark);
    if (taken)
        device_state_unlock();
    ui_cmt_window_update();
    return rc;
}


int cmt_tape_edit_allowed(void)
{
    if (!CMT_TEST_FILLED || (g_cmt.ext != g_cmt_tmz_extension) || !g_cmt.ext->container)
        return 0;
    if (!CMT_TEST_STOP || cmttmz_rec_is_active())
        return 0;
    /* značka checkpointu záznamu vstupů úpravy vrátit neumí */
    if (replay_is_active())
        return 0;
    return 1;
}


/**
 * @brief Ověří, že pásku jde teď upravit; jinak nastaví text chyby úpravy.
 *
 * Na začátku vždy vymaže varování nástroje nad páskou
 * (cmttmz_get_edit_warning()): každá úprava pásky (i odmítnutá) začíná bez
 * varování, takže varování dřívějšího zápisu informací nebo převodu profilu
 * se nepřipíše jiné operaci.
 *
 * @return true = cmt_tape_edit_allowed(); false = text v cmttmz_get_edit_error()
 *         ("No tape inserted", "The tape cannot be edited while recording",
 *         "The tape can be edited only when the deck is stopped", "The tape
 *         cannot be edited while an input replay is recorded or played").
 * @post cmttmz_get_edit_warning() je "".
 * @pre Vlákno emulátoru.
 */
static bool cmt_tape_edit_check(void)
{
    bool taken = cmt_lock_if_needed();
    cmttmz_set_edit_warning(NULL);
    if (taken)
        device_state_unlock();
    if (cmt_tape_edit_allowed())
        return true;
    taken = cmt_lock_if_needed();
    if (!CMT_TEST_FILLED || (g_cmt.ext != g_cmt_tmz_extension) || !g_cmt.ext->container)
        cmttmz_set_edit_error("No tape inserted");
    else if (CMT_TEST_RECORD || cmttmz_rec_is_active())
        cmttmz_set_edit_error("The tape cannot be edited while recording");
    else if (CMT_TEST_STOP && replay_is_active())
        cmttmz_set_edit_error("The tape cannot be edited while an input replay is recorded or played");
    else
        cmttmz_set_edit_error("The tape can be edited only when the deck is stopped");
    if (taken)
        device_state_unlock();
    return false;
}


int cmt_tape_edit_verify(void)
{
    return cmt_tape_edit_check() ? 1 : 0;
}


/**
 * @brief Po úspěšné úpravě pásky obnoví okno Virtual CMT.
 *
 * Stav CMT hacku nad páskou smaže už jádro (cmttmz_edit_apply()).
 *
 * @pre Vlákno emulátoru, mimo device_state_lock() (zámek pustil volající).
 */
static void cmt_tape_edit_done(void)
{
    ui_cmt_window_update();
}


int cmt_tape_remove_blocks(const int *block_ids, uint32_t count)
{
    if (!cmt_tape_edit_check())
        return EXIT_FAILURE;
    bool taken = cmt_lock_if_needed();
    int rc = cmttmz_remove_blocks(block_ids, count);
    if (taken)
        device_state_unlock();
    if ((rc == EXIT_SUCCESS) && count)
        cmt_tape_edit_done();
    return rc;
}


int cmt_tape_import_file(const char *path, int at, uint32_t *imported)
{
    if (imported)
        *imported = 0;
    bool empty_deck = !CMT_TEST_FILLED;
    if (empty_deck)
    {
        /* bez pásky neprochází cmt_tape_edit_check(): varování vymazat zde */
        cmttmz_set_edit_warning(NULL);
        /* bez pásky: jádro soubor nejdřív převede a novou pásku vloží až po
         * úspěšném převodu: neúspěšný import nezanechá stopu
         * (žádné vysunutí, události breakpointů ani zápis na médium) */
        if (replay_is_active())
        {
            cmttmz_set_edit_error("The tape cannot be edited while an input replay is recorded or played");
            return EXIT_FAILURE;
        };
        if (!CMT_TEST_STOP)
        {
            cmttmz_set_edit_error("The tape can be edited only when the deck is stopped");
            return EXIT_FAILURE;
        };
    }
    else if (!cmt_tape_edit_check())
    {
        return EXIT_FAILURE;
    };
    bool taken = cmt_lock_if_needed();
    int rc = cmttmz_import_file_at(path, at, imported);
    if ((rc == EXIT_SUCCESS) && empty_deck)
    {
        /* páska vznikla importem (jako RECORD do prázdného decku) */
        g_cmt.ext = g_cmt_tmz_extension;
        cmt_set_last_filename("");
    };
    if (taken)
        device_state_unlock();
    if (rc == EXIT_SUCCESS)
        cmt_tape_edit_done();
    return rc;
}


int cmt_tape_analysis_apply(const struct st_CMTTMZ_PULSES *p, const struct st_CMTTMZ_ANALYSIS *a, const uint8_t *use,
                            int replace, uint32_t *added)
{
    if (added)
        *added = 0;
    if (!cmt_tape_edit_check())
        return EXIT_FAILURE;
    bool taken = cmt_lock_if_needed();
    uint32_t n = 0;
    int rc = cmttmz_analysis_apply(p, a, use, replace, &n);
    if (taken)
        device_state_unlock();
    if (added)
        *added = n;
    if ((rc == EXIT_SUCCESS) && n)
        cmt_tape_edit_done();
    return rc;
}


int cmt_tape_info_set(const struct st_CMTTMZ_TAPE_INFO *in, int *changed)
{
    if (changed)
        *changed = 0;
    if (!cmt_tape_edit_check())
        return EXIT_FAILURE;
    bool taken = cmt_lock_if_needed();
    int ch = 0;
    int rc = cmttmz_tape_info_set(in, &ch);
    if (taken)
        device_state_unlock();
    if (changed)
        *changed = ch;
    if (ch)
        cmt_tape_edit_done();
    return rc;
}


int cmt_tape_convert_block(int block_id, const char *profile_id, en_CMTSPEED speed)
{
    if (!cmt_tape_edit_check())
        return EXIT_FAILURE;
    bool taken = cmt_lock_if_needed();
    int rc = cmttmz_block_convert_profile(block_id, profile_id, speed);
    if (taken)
        device_state_unlock();
    if (rc == EXIT_SUCCESS)
        cmt_tape_edit_done();
    return rc;
}


int cmt_tape_move_block(int block_id, int to)
{
    if (!cmt_tape_edit_check())
        return EXIT_FAILURE;
    bool taken = cmt_lock_if_needed();
    int rc = cmttmz_move_block(block_id, to);
    if (taken)
        device_state_unlock();
    if ((rc == EXIT_SUCCESS) && (block_id != to))
        cmt_tape_edit_done();
    return rc;
}

/**
 * @brief Změní výchozí rychlost Virtual CMT (bloky s CMTEXT_BLOCK_SPEED_DEFAULT).
 *
 * Při zastaveném transportu se v souboru vložené pásky hned přestaví
 * všechny bloky s výchozí rychlostí (cmttmz_apply_default_speed(); když
 * některý nejde, soubor se nemění) a otevřený blok s výchozí rychlostí
 * dostane nový stream (cb_set_speed) a převine se na začátek (pozice
 * nastavená ve STOP přes cmt_tape_set_position() se zruší, nový stream má
 * jinou délku). Mimo STOP se mění jen výchozí
 * rychlost; bloky se přestaví až při otevření. CMTSPEED_CUSTOM znamená
 * výchozí vlastní délky pulzů g_cmt.mz_custom_pulses (nastavují se přes
 * cmt_change_custom_pulses()).
 *
 * Délky bloků pro okno Virtual CMT: otevřený blok má délku svého nového
 * streamu (zadarmo), délky ostatních bloků s výchozí rychlostí se jen
 * označí jako neznámé (cmttmz_lengths_invalidate(), nic se negeneruje).
 *
 * @param cmtspeed Poměr nebo CMTSPEED_CUSTOM.
 * @return EXIT_SUCCESS; EXIT_FAILURE, pokud se bloky nepodařilo přestavět
 *         nebo blok přegenerovat (výchozí rychlost se pak nemění; když
 *         selže až přegenerování otevřeného bloku, ostatní bloky už jsou
 *         přestavěné a při otevření se vrátí podle výchozí rychlosti).
 * @pre Voláno z emulátorového vlákna (zásah z GUI přes cmt_request_set_speed()).
 */
int cmt_change_speed(en_CMTSPEED cmtspeed)
{
    int ret = EXIT_SUCCESS;

    if (CMT_TEST_FILLED && CMT_TEST_STOP && (g_cmt.ext == g_cmt_tmz_extension))
    {
        if (EXIT_SUCCESS != cmttmz_apply_default_speed(cmtspeed, &g_cmt.mz_custom_pulses))
            ret = EXIT_FAILURE;
    };

    if ((ret == EXIT_SUCCESS) && CMT_TEST_FILLED && CMT_TEST_STOP)
    {
        if (cmtext_block_get_block_speed(g_cmt.ext->block) == CMTEXT_BLOCK_SPEED_DEFAULT)
        {
            assert(g_cmt.ext->block->cb_set_speed != NULL);
            if (g_cmt.ext->block->cb_set_speed != NULL)
            {
                if (EXIT_SUCCESS != g_cmt.ext->block->cb_set_speed(g_cmt.ext, cmtspeed))
                {
                    ret = EXIT_FAILURE;
                };
            };
        };
    };

    if (ret == EXIT_SUCCESS)
    {
        g_cmt.mz_cmtspeed = cmtspeed;
    }

    /* délky bloků s výchozí rychlostí (okno Virtual CMT): neplatné označit, nepočítat */
    cmttmz_note_change();
    cmttmz_lengths_invalidate();
    ui_cmt_window_update();

    return ret;
}


/**
 * @brief Režim a rychlost bloku v indexu pásky (porovnání před a po změně rychlosti bloku).
 *
 * @invariant Struktura je před plněním vynulovaná (porovnává se memcmp()).
 */
typedef struct st_CMT_BLOCK_SPEED_SNAP
{
    int blspeed;                    /**< en_CMTEXT_BLOCK_SPEED bloku */
    int cmtspeed;                   /**< en_CMTSPEED bloku */
    int has_pulses;                 /**< 1 = blok MZF s vlastními pulzy (pulses platí) */
    st_MZTAPE_PULSES_LENGTH pulses; /**< vlastní pulzy bloku */
} st_CMT_BLOCK_SPEED_SNAP;


/**
 * @brief Zapamatuje režim a rychlost bloku z indexu.
 * @param c Kontejner (SIMPLE_TAPE). @param block_id Blok (v rozsahu). @param[out] s Údaje.
 * @pre Vlákno emulátoru (nebo pod device_state_lock()).
 */
static void cmt_block_speed_snap(st_CMTEXT_CONTAINER *c, int block_id, st_CMT_BLOCK_SPEED_SNAP *s)
{
    memset(s, 0, sizeof(*s));
    s->blspeed = (int)cmtext_container_get_block_speed(c, block_id);
    s->cmtspeed = (int)cmtext_container_get_block_cmt_speed(c, block_id);
    s->has_pulses = (EXIT_SUCCESS == cmtext_container_get_block_pulses(c, block_id, &s->pulses)) ? 1 : 0;
    if (!s->has_pulses)
        memset(&s->pulses, 0, sizeof(s->pulses));
}


/**
 * @brief Změnila-li se rychlost bloku, nastaví pásce Modified (uložení ji pak zapíše).
 *
 * Rychlost bloku je obsah pásky: uložení do TMZ / MZT blok přestaví
 * rychlostí z indexu (cmttmz_save()). Beze změny se Modified nemění.
 *
 * @param c Kontejner. @param block_id Blok. @param before Údaje před změnou (cmt_block_speed_snap()).
 * @pre Vlákno emulátoru.
 */
static void cmt_block_speed_note_modified(st_CMTEXT_CONTAINER *c, int block_id, const st_CMT_BLOCK_SPEED_SNAP *before)
{
    st_CMT_BLOCK_SPEED_SNAP after;
    cmt_block_speed_snap(c, block_id, &after);
    if (0 != memcmp(before, &after, sizeof(after)))
        cmttmz_set_modified(1);
}


/**
 * @brief Nastaví vlastní rychlost jednoho bloku vložené pásky (per-blok override).
 *
 * Blok se přepne na vlastní rychlost (blspeed = CMTEXT_BLOCK_SPEED_SET) a
 * uloží se mu cmtspeed. Bez SET by otevření bloku rychlost z indexu
 * ignorovalo a použilo globální g_cmt.mz_cmtspeed (dříve chyba MCP
 * cmt_tape_block_speed - nastavení nemělo účinek). GUI (kontextové menu
 * Set speed tabulky okna Virtual CMT, cmt_tape_change_block_speed())
 * nastavuje obojí stejně.
 *
 * Změna se projeví při příštím otevření bloku; právě otevřený blok se
 * nepřegeneruje. Délka bloku pro okno Virtual CMT se jen označí jako
 * neznámá (cmttmz_lengths_invalidate()). Změnila-li se rychlost, páska je
 * Modified (uložení blok přestaví rychlostí z indexu, cmttmz_save()).
 *
 * @param block_id Index bloku v páskovém kontejneru (0..count-1).
 * @param cmtspeed Rychlost (platná en_CMTSPEED, viz cmtspeed_is_valid()).
 * @return 0 při úspěchu; -1 bez vložené pásky, u pásky bez indexu bloků
 *         (jiný kontejner než CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE), při
 *         block_id mimo rozsah nebo u bloku, jehož rychlost změnit nejde
 *         (cmttmz_block_speed_changeable(): blok s rychlostí ze souboru,
 *         WAV, CONTROL); -2 při neplatné rychlosti. Při chybě se nic
 *         nemění.
 *
 * @pre Voláno z emulátorového vlákna.
 * @post Při úspěchu cmtext_container_get_block_speed() == SET a
 *       cmtext_container_get_block_cmt_speed() == cmtspeed pro block_id;
 *       seznam bloků v GUI je označený k obnovení.
 */
int cmt_tape_set_block_cmt_speed(int block_id, en_CMTSPEED cmtspeed)
{
    /* varování nástroje nad páskou patří jen operaci, která ho nastavila */
    cmttmz_set_edit_warning(NULL);
    if ((!CMT_TEST_FILLED) || (!g_cmt.ext))
        return -1;
    if (!cmtspeed_is_valid(cmtspeed))
        return -2;
    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    if (!container)
        return -1;
    /* Per-blok rychlost má smysl jen pro SIMPLE_TAPE (má index bloků);
     * SINGLE container index nemá a settery by dereferencovaly NULL
     * container->tape. Settery mají na rozsah block_id jen assert. */
    if ((cmtext_container_get_type(container) != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE)
        || (block_id < 0)
        || (block_id >= cmtext_container_get_count_blocks(container))
        || (!cmttmz_block_speed_changeable(block_id, cmtspeed)))
        return -1;
    st_CMT_BLOCK_SPEED_SNAP before;
    cmt_block_speed_snap(container, block_id, &before);
    cmtext_container_set_block_speed(container, block_id, CMTEXT_BLOCK_SPEED_SET);
    cmtext_container_set_block_cmt_speed(container, block_id, cmtspeed);
    cmt_block_speed_note_modified(container, block_id, &before);
    cmttmz_note_change();
    cmttmz_lengths_invalidate();
    ui_cmt_window_update();
    return 0;
}

int cmt_change_custom_pulses(const st_MZTAPE_PULSES_LENGTH *pulses)
{
    /* cb_set_speed bloku čte výchozí vlastní pulzy z g_cmt - nastavit
     * předem a při neúspěchu vrátit */
    st_MZTAPE_PULSES_LENGTH saved = g_cmt.mz_custom_pulses;
    g_cmt.mz_custom_pulses = *pulses;
    if (EXIT_SUCCESS != cmt_change_speed(CMTSPEED_CUSTOM))
    {
        g_cmt.mz_custom_pulses = saved;
        ui_cmt_window_update();
        return EXIT_FAILURE;
    };
    return EXIT_SUCCESS;
}

int cmt_tape_set_block_pulses(int block_id, const st_MZTAPE_PULSES_LENGTH *pulses)
{
    /* varování nástroje nad páskou patří jen operaci, která ho nastavila */
    cmttmz_set_edit_warning(NULL);
    if ((!CMT_TEST_FILLED) || (!g_cmt.ext))
        return -1;
    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    if (!container)
        return -1;
    /* stejné meze jako cmt_tape_set_block_cmt_speed() */
    if ((cmtext_container_get_type(container) != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE)
        || (block_id < 0)
        || (block_id >= cmtext_container_get_count_blocks(container))
        || (!cmttmz_block_speed_changeable(block_id, CMTSPEED_CUSTOM)))
        return -1;
    st_CMT_BLOCK_SPEED_SNAP before;
    cmt_block_speed_snap(container, block_id, &before);
    if (EXIT_SUCCESS != cmtext_container_set_block_pulses(container, block_id, pulses))
        return -1;
    cmtext_container_set_block_speed(container, block_id, CMTEXT_BLOCK_SPEED_SET);
    cmtext_container_set_block_cmt_speed(container, block_id, CMTSPEED_CUSTOM);
    cmt_block_speed_note_modified(container, block_id, &before);
    cmttmz_note_change();
    cmttmz_lengths_invalidate();
    ui_cmt_window_update();
    return 0;
}

int cmt_tape_change_block_speed(int block_id, en_CMTSPEED cmtspeed, bool is_mz, const st_MZTAPE_PULSES_LENGTH *pulses)
{
    /* varování nástroje nad páskou patří jen operaci, která ho nastavila */
    cmttmz_set_edit_warning(NULL);
    if ((!CMT_TEST_FILLED) || (!g_cmt.ext))
        return EXIT_FAILURE;
    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    /* stejné meze jako cmt_tape_set_block_cmt_speed(); CMTSPEED_NONE
     * (= výchozí rychlost Virtual CMT) jde jen u bloku MZF */
    if ((!container)
        || (cmtext_container_get_type(container) != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE)
        || (block_id < 0)
        || (block_id >= cmtext_container_get_count_blocks(container))
        || (!cmttmz_block_speed_changeable(block_id, cmtspeed)))
        return EXIT_FAILURE;

    bool is_ready = (block_id == cmtext_block_get_block_id(g_cmt.ext->block));

    /* právě přehrávaný blok se nemění */
    if (is_ready && CMT_TEST_PLAY)
        return EXIT_SUCCESS;

    st_CMT_BLOCK_SPEED_SNAP before;
    cmt_block_speed_snap(container, block_id, &before);
    if (cmtspeed_is_custom(cmtspeed))
    {
        if ((!pulses) || (EXIT_SUCCESS != cmtext_container_set_block_pulses(container, block_id, pulses)))
            return EXIT_FAILURE;
    };

    if (is_mz)
    {
        en_CMTEXT_BLOCK_SPEED blspeed = (cmtspeed == CMTSPEED_NONE) ? CMTEXT_BLOCK_SPEED_DEFAULT : CMTEXT_BLOCK_SPEED_SET;
        cmtext_container_set_block_speed(container, block_id, blspeed);
    };

    cmtext_container_set_block_cmt_speed(container, block_id, cmtspeed);
    cmt_block_speed_note_modified(container, block_id, &before);

    int ret = EXIT_SUCCESS;
    if (is_ready)
    {
        if (EXIT_SUCCESS != g_cmt.ext->container->cb_open_block(block_id))
            ret = EXIT_FAILURE;
    };

    cmttmz_note_change();
    cmttmz_lengths_invalidate();
    ui_cmt_window_update();
    return ret;
}

int cmt_get_speed_pulses(en_CMTSPEED cmtspeed, const st_MZTAPE_PULSES_LENGTH *custom, st_MZTAPE_PULSES_LENGTH *pulses)
{
    if (cmtspeed_is_custom(cmtspeed))
    {
        if (!custom)
            return EXIT_FAILURE;
        *pulses = *custom;
        return EXIT_SUCCESS;
    };
    return mztape_get_speed_pulses(CMTTMZ_FORMATSET, cmtspeed, pulses);
}

int cmt_tape_get_block_pulses(int block_id, st_MZTAPE_PULSES_LENGTH *pulses)
{
    if ((!CMT_TEST_FILLED) || (!g_cmt.ext))
        return -1;
    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    if (!container)
        return -1;
    /* kontejner bez indexu (SINGLE, dnes ho nic nevytváří) - bez pulzů */
    if (cmtext_container_get_type(container) != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE)
        return -1;
    if ((block_id < 0) || (block_id >= cmtext_container_get_count_blocks(container)))
        return -1;
    if (cmtext_container_get_block_type(container, block_id) != CMTEXT_BLOCK_TYPE_MZF)
        return -1;
    /* blok MZF z TMZ/TZX: rychlost dává soubor, emulátor ji neurčuje */
    if (cmtext_container_get_block_speed(container, block_id) == CMTEXT_BLOCK_SPEED_NONE)
        return -1;
    int ret;
    if (cmtext_container_get_block_speed(container, block_id) == CMTEXT_BLOCK_SPEED_SET)
    {
        en_CMTSPEED cmtspeed = cmtext_container_get_block_cmt_speed(container, block_id);
        if (cmtspeed_is_custom(cmtspeed))
            ret = cmtext_container_get_block_pulses(container, block_id, pulses);
        else
            ret = cmt_get_speed_pulses(cmtspeed, NULL, pulses);
    }
    else
    {
        ret = cmt_get_speed_pulses(g_cmt.mz_cmtspeed, &g_cmt.mz_custom_pulses, pulses);
    };
    return (ret == EXIT_SUCCESS) ? 0 : -1;
}

void cmt_exit(void)
{
    /* Konec emulátoru: záznam se zahodí (jednou, při vysunutí), neuložené
     * změny pásky jen do logu. GUI se před zavřením okna zeptá
     * (imgui_cmt_quit_requested()); tohle je záloha pro ukončení bez dotazu
     * (headless, MCP shutdown, automatizace --exit-*, SDL_EVENT_QUIT). */
    if (cmttmz_is_modified())
        fprintf(stderr, "Virtual CMT: warning: the tape has unsaved changes, discarded at exit\n");
    cmt_eject_why("emulator exit");
    if (g_cmt.last_filename)
        baseui_tools_mem_free(g_cmt.last_filename);

    if (g_ui_cmt_filters)
        baseui_tools_mem_free(g_ui_cmt_filters);

    cmthack_exit();
    cmtext_exit();
    /* nevyzvednutá kopie pulzů a čekající kopie formulářů zásahů */
    cmt_request_exit();
}

void cmt_propagatecfg_cmt_speed(void *e, void *data)
{
    (void)data;
    g_cmt.mz_cmtspeed = cfgelement_get_keyword_value((CFGELM *)e);
    ui_cmt_window_update();
}

/**
 * @brief Výchozí vlastní délky pulzů v µs: LONG high, LONG low, SHORT high, SHORT low.
 *
 * Sada 1xspeed.mzf zařízení UniCMT, shodná s nominálem ROM ze servisního
 * manuálu (báze hw/25-unicmt.md kap. 2.1).
 */
static const double g_cmt_default_custom_pulses_us[4] = {470.0, 494.0, 240.0, 278.0};

/** @brief Názvy INI klíčů vlastních délek pulzů (pořadí jako g_cmt_cfg_custom_pulses_us). */
static char *g_cmt_cfg_custom_pulses_name[4] = {
    "mz_custom_pulse_long_high_us",
    "mz_custom_pulse_long_low_us",
    "mz_custom_pulse_short_high_us",
    "mz_custom_pulse_short_low_us",
};

/**
 * @brief Zrcadlo g_cmt.mz_custom_pulses v µs pro cfg elementy (save handler ukazuje sem).
 *
 * Synchronizuje ho cmt_cfg_custom_pulses_sync() před uložením INI
 * (cfgmodule save callback) a propagate callback po načtení.
 */
static float g_cmt_cfg_custom_pulses_us[4];

/** @brief Elementy vlastních délek pulzů (pro propagate callback). */
static CFGELM *g_cmt_cfg_custom_pulses_elm[4];

/**
 * @brief Přepíše zrcadlo g_cmt_cfg_custom_pulses_us z g_cmt.mz_custom_pulses.
 */
static void cmt_cfg_custom_pulses_sync(void)
{
    g_cmt_cfg_custom_pulses_us[0] = (float)(g_cmt.mz_custom_pulses.long_pulse.high * 1e6);
    g_cmt_cfg_custom_pulses_us[1] = (float)(g_cmt.mz_custom_pulses.long_pulse.low * 1e6);
    g_cmt_cfg_custom_pulses_us[2] = (float)(g_cmt.mz_custom_pulses.short_pulse.high * 1e6);
    g_cmt_cfg_custom_pulses_us[3] = (float)(g_cmt.mz_custom_pulses.short_pulse.low * 1e6);
}

/**
 * @brief Propagate callback klíčů mz_custom_pulse_*_us: složí výchozí vlastní délky pulzů.
 *
 * Volá se pro každý ze 4 klíčů; čte vždy všechny čtyři elementy, takže
 * po posledním z nich platí hodnoty z INI. Neplatná kombinace (nemůže
 * nastat díky rozsahu elementů) ponechá předchozí délky.
 *
 * @param e Měněný element (nepoužívá se).
 * @param data Nepoužívá se.
 */
static void cmt_propagatecfg_custom_pulses(void *e, void *data)
{
    (void)e;
    (void)data;
    double us[4];
    for (int i = 0; i < 4; i++)
    {
        us[i] = (g_cmt_cfg_custom_pulses_elm[i]) ? cfgelement_get_float_value(g_cmt_cfg_custom_pulses_elm[i])
                                                 : g_cmt_default_custom_pulses_us[i];
    };
    mztape_pulses_set_us(&g_cmt.mz_custom_pulses, us[0], us[1], us[2], us[3]);
    cmt_cfg_custom_pulses_sync();
    ui_cmt_window_update();
}

/**
 * @brief Save callback modulu CMT: před zápisem INI srovná zrcadlo vlastních pulzů.
 *
 * @param m Modul (nepoužívá se).
 * @param data Nepoužívá se.
 */
static void cmt_cfg_save_cb(void *m, void *data)
{
    (void)m;
    (void)data;
    cmt_cfg_custom_pulses_sync();
}

void cmt_rear_dip_switch_cmt_inverted_polarity(unsigned value)
{
    value &= 1;
    if (value == g_cmt.polarity)
        return;
    g_cmt.polarity = value;
    /* páska bez otevřeného bloku: polarita se uplatní při otevření bloku WAV */
    if ((!CMT_TEST_FILLED) || (!g_cmt.ext->block))
        return;
    if (g_cmt.ext->block->cb_set_polarity != NULL)
        g_cmt.ext->block->cb_set_polarity(g_cmt.ext, value);
}

void cmt_propagatecfg_inverted_polarity(void *e, void *data)
{
    (void)data;
    (void)e;
}

void cmt_propagatecfg_cpu_boost(void *e, void *data)
{
    (void)e;
    (void)data;
    cmt_cpu_boost_set(cfgelement_get_bool_value((CFGELM *)e));
}

void cmt_propagatecfg_mzfsize_check(void *e, void *data)
{
    (void)e;
    (void)data;
    cmt_mzfsize_check_set(cfgelement_get_bool_value((CFGELM *)e));
}

void cmt_init(void)
{

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "CMT");

    /* výchozí vlastní délky pulzů platí, i když INI klíče chybí */
    mztape_pulses_set_us(&g_cmt.mz_custom_pulses,
                         g_cmt_default_custom_pulses_us[0], g_cmt_default_custom_pulses_us[1],
                         g_cmt_default_custom_pulses_us[2], g_cmt_default_custom_pulses_us[3]);
    cmt_cfg_custom_pulses_sync();
    cfgmodule_set_save_cb(cmod, cmt_cfg_save_cb, NULL);

    CFGELM *elm;
    elm = cfgmodule_register_new_element(cmod, "mz_cmtspeed", CFGENTYPE_KEYWORD, CMTSPEED_1_1,
                                         CMTSPEED_1_1, "SPEED_1/1",
                                         CMTSPEED_2_1, "SPEED_2/1",
                                         CMTSPEED_2_1_CPM, "SPEED_2/1_CPM",
                                         CMTSPEED_3_2, "SPEED_3/2",
                                         CMTSPEED_7_3, "SPEED_7/3",
                                         CMTSPEED_8_3, "SPEED_8/3",
                                         CMTSPEED_3_1, "SPEED_3/1",
                                         CMTSPEED_9_7, "SPEED_9/7",
                                         CMTSPEED_25_14, "SPEED_25/14",
                                         CMTSPEED_CUSTOM, "SPEED_CUSTOM",
                                         -1);
    cfgelement_set_propagate_cb(elm, cmt_propagatecfg_cmt_speed, NULL);
    cfgelement_set_handlers(elm, NULL, (void *)&g_cmt.mz_cmtspeed);

    /* vlastní délky pulzů pro SPEED_CUSTOM (µs, rozsah jako hlavička CMTSPEED) */
    for (int i = 0; i < 4; i++)
    {
        elm = cfgmodule_register_new_element(cmod, g_cmt_cfg_custom_pulses_name[i], CFGENTYPE_FLOAT,
                                             g_cmt_default_custom_pulses_us[i], 0.1, MZTAPE_PULSE_US_MAX);
        g_cmt_cfg_custom_pulses_elm[i] = elm;
        cfgelement_set_propagate_cb(elm, cmt_propagatecfg_custom_pulses, NULL);
        cfgelement_set_handlers(elm, NULL, (void *)&g_cmt_cfg_custom_pulses_us[i]);
    };

    elm = cfgmodule_register_new_element(cmod, "cmt_polarity_inverted", CFGENTYPE_BOOL, CMT_STREAM_POLARITY_NORMAL);
    // cfgelement_set_propagate_cb(elm, cmt_propagatecfg_inverted_polarity, NULL);
    cfgelement_set_handlers(elm, (void *)&g_cmt.polarity, (void *)&g_cmt.polarity);

    elm = cfgmodule_register_new_element(cmod, "cpu_boost", CFGENTYPE_BOOL, CMT_CPU_BOOST_ENABLED);
    cfgelement_set_propagate_cb(elm, cmt_propagatecfg_cpu_boost, NULL);
    cfgelement_set_handlers(elm, (void *)&g_cmt.cpu_boost, (void *)&g_cmt.cpu_boost);

    elm = cfgmodule_register_new_element(cmod, "mzfsize_check", CFGENTYPE_BOOL, CMT_MZFSIZE_CHECK_ENABLED);
    cfgelement_set_propagate_cb(elm, cmt_propagatecfg_mzfsize_check, NULL);
    cfgelement_set_handlers(elm, (void *)&g_cmt.mzfsize_check, (void *)&g_cmt.mzfsize_check);

    /* automatická oprava velikosti MZF bez dotazu (cmt_fix_mzfsize.cpp), výchozí vypnuto */
    elm = cfgmodule_register_new_element(cmod, "mzfsize_autofix", CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm, (void *)&g_cmt.mzfsize_autofix, (void *)&g_cmt.mzfsize_autofix);

    /* echo: signál pásky do reproduktoru (cmt_echo_frame()), výchozí vypnuto */
    elm = cfgmodule_register_new_element(cmod, "echo", CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm, (void *)&g_cmt.echo, (void *)&g_cmt.echo);

    /* automatické převinutí po konci pásky (cmt_tape_advance()), výchozí vypnuto */
    elm = cfgmodule_register_new_element(cmod, "autorewind", CFGENTYPE_BOOL, CMT_AUTOREWIND_DISABLED);
    cfgelement_set_handlers(elm, (void *)&g_cmt.autorewind, (void *)&g_cmt.autorewind);

    /* automatické ukončení záznamu po N s ticha (cmt_screen_done_period()), výchozí vypnuto */
    elm = cfgmodule_register_new_element(cmod, "rec_autostop_s", CFGENTYPE_UNSIGNED, CMT_REC_AUTOSTOP_DEFAULT_S, 0,
                                         CMT_REC_AUTOSTOP_MAX_S);
    cfgelement_set_handlers(elm, (void *)&g_cmt.rec_autostop_s, (void *)&g_cmt.rec_autostop_s);

    /* kód zařízení v BC při přímém spuštění MZF (run_mzf_device, výchozí cmt);
     * stav drží bootstrap_device.c, sekce [CMT] je jen místo v INI */
    bootdev_cfg_register(cmod);

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);

    g_cmt.output = 0;
    g_cmt.start_time = 0;
    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.user_paused = 0;
    g_cmt.motor_off = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    g_cmt.ext = NULL;
    g_cmt.last_filename = baseui_tools_mem_alloc0(1);
    g_cmt.ui_player_update = 0;
    g_cmt.ui_base_filename = NULL;

    cmtext_init();

    /* Filtr dialogu (syntaxe ImGuiFileDialog "popis{.přípona,...}"): první
     * položka nabízí všechny přehratelné pásky (cmtext_is_playable_filename(),
     * .wave jako .wav), pak jednotlivé přípony a všechny soubory. */
    g_ui_cmt_filters = g_strdup("Tape (*.tmz *.tzx *.mzf *.m12 *.mzt *.tap *.wav){.tmz,.tzx,.mzf,.m12,.mzt,.tap,.wav,.wave}"
                                ", .tmz, .tzx, .mzf, .m12, .mzt, .tap, .wav, .wave, .*");

    ui_cmt_window_update();

    cmthack_init();
}

void cmt_tape_path_changed(const char *path)
{
    if (!path)
        path = "";
    if (g_cmt.ui_base_filename)
        baseui_tools_mem_free(g_cmt.ui_base_filename);
    g_cmt.ui_base_filename = path[0] ? g_path_get_basename(path) : NULL;
    cmt_set_last_filename(path);
    ui_cmt_window_update();
}

int cmt_open_file_by_extension(char *filename)
{
    return cmt_open_file(filename, NULL);
}

int cmt_open_file(const char *filename, bool *discarded)
{
    if (discarded)
        *discarded = false;

    st_CMTEXT *ext = filename ? cmtext_get_extension((char *)filename) : NULL;

    if (!ext)
    {
        baseui_error("Unknown CMT file extension '%s'\n", filename ? filename : "");
        return EXIT_FAILURE;
    }

    /* Nejdřív načíst a ověřit nový soubor: neúspěšné otevření nesmí
     * vysunout vloženou pásku ani zahodit nahrávání (2.0.6 to dělalo). */
    st_CMTTMZ_PREPARED *prep = NULL;
    if (ext == g_cmt_tmz_extension)
    {
        prep = cmttmz_open_prepare(filename);
        if (!prep)
        {
            /* jedno hlášení: důvod z importu i jméno souboru */
            const char *why = cmttmz_get_prepare_error();
            baseui_error("%s can't open file '%s'%s%s\n", cmtext_get_description(ext), filename, why[0] ? ": " : "", why);
            return EXIT_FAILURE;
        };

    };

    if (discarded)
        *discarded = cmt_has_unsaved_changes();
    cmt_eject_why("another tape was opened");

    int ret = prep ? cmttmz_open_prepared(prep) : ext->cb_open((char *)filename);
    if (ret == EXIT_SUCCESS)
    {
        g_cmt.ext = ext;
        g_cmt.ui_base_filename = g_path_get_basename(filename);
        cmt_set_last_filename(filename);
        /* Jediné místo, kde vložení pásky ze souboru uspěje (menu, okno
         * Virtual CMT, overlay menu, příkazová řádka, MCP): zápis do historie
         * naposledy vložených médií. Páska, kterou vrací načtení snapshotu
         * (tichý režim), nové vložení není. */
        if (!s_cmt_quiet)
            recent_media_add(RECENT_MEDIA_CMT, filename);
    }
    else
    {
        baseui_error("%s can't open file '%s'\n", cmtext_get_description(ext), filename);
    };

    ui_cmt_window_update();

    /* v tichém režimu (načtení snapshotu) dialog kontroly velikosti MZF nevyskočí */
    if ((ret == EXIT_SUCCESS) && (CMT_TEST_MZFSIZE_CHECK_ENABLED) && !s_cmt_quiet)
    {
        /* kontrola délky jen u souborů .mzf a .m12 (blok 0 = MZF z hlavičky souboru) */
        en_CMTTMZ_SOURCE source;
        if ((EXIT_SUCCESS == cmttmz_source_from_filename(filename, &source)) && (source == CMTTMZ_SOURCE_MZF))
        {
            st_MZF *mzf = cmttmz_block_get_mzf(0);
            if (mzf)
            {
                unsigned mzf_size = mzf->header.fsize + sizeof(st_MZF_HEADER);
                mzf_free(mzf);
                baseui_cmt_check_mzf_filesize(filename, mzf_size);
            };
        };
    };

    return ret;
}

const char *cmt_ui_open_filters(void)
{
    return g_ui_cmt_filters ? g_ui_cmt_filters : "";
}

/**
 * @brief Konec streamu bloku pásky rozšíření cmt_tmz: další blok podle playeru.
 *
 * Zavolá cmttmz_advance() a podle výsledku:
 * - CMTTMZ_ADVANCE_BLOCK: hraje nový blok, start_time posune tak, aby
 *   přesah za koncem předchozího bloku (transferred_ticks) patřil novému,
 * - CMTTMZ_ADVANCE_STOP_TAPE: páska stojí na bloku za příkazem zastavení
 *   (Pause 0, 0x2A) - start_time = teď (blok od začátku) a zastavení
 *   motoru (motor_off = 1, cmt_pause_apply_sources()), ne pauza uživatele:
 *   stav zůstane PLAY, paused = 1 (Waiting for motor), PC4 (motor sense)
 *   čte 0, CPU boost se vypne, odpálí se BP eventy CMT_MSTATE
 *   a CMT_STATE_CHANGE; pokračuje náběžná hrana PC3 (motor) nebo
 *   cmt_pause(0) bez pauzy uživatele (Stop a Play okna blok přehraje od
 *   začátku); tlačítko Pause okna pásku jen pozastaví (pauza uživatele),
 *   nerozjede ji,
 * - CMTTMZ_ADVANCE_END: cmt_stop(); při g_cmt.autorewind navíc
 *   cmttmz_rewind() (první blok se signálem, stav STOP),
 * - CMTTMZ_ADVANCE_ERROR: cmt_stop() (příčinu vypsal cmt_tmz do logu).
 *
 * @param transferred_ticks Takty GDG, o které čas přehrávání přesáhl konec bloku.
 * Kdyby hrálo jiné rozšíření než cmt_tmz (dnes nemožné), cmttmz_advance()
 * bez vložené pásky cmt_tmz vrátí CMTTMZ_ADVANCE_END a výsledkem je STOP.
 *
 * @pre Emulátorové vlákno, g_cmt.ext == g_cmt_tmz_extension, PLAY bez pauzy,
 *      cmtext_block_get_output() právě hlásil konec bloku.
 */
static void cmt_tape_advance(uint64_t transferred_ticks)
{
    switch (cmttmz_advance())
    {
    case CMTTMZ_ADVANCE_BLOCK:
        g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_BODY;
        g_cmt.start_time = gdg_get_total_ticks() - transferred_ticks;
        ui_cmt_window_update();
        break;

    case CMTTMZ_ADVANCE_STOP_TAPE:
        g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_BODY;
        g_cmt.start_time = gdg_get_total_ticks();
        printf("Virtual CMT: the tape stopped (stop-the-tape block).\n");
        /* jako zastavení motoru: pokračuje hrana PC3 nebo cmt_pause(0) */
        cmt_pause_sources_sync();
        g_cmt.motor_off = 1;
        cmt_pause_apply_sources();
        break;

    case CMTTMZ_ADVANCE_END:
        cmt_stop();
        if (g_cmt.autorewind && (EXIT_SUCCESS == cmttmz_rewind()))
        {
            printf("Virtual CMT: the tape is rewound.\n");
            ui_cmt_window_update();
        };
        break;

    case CMTTMZ_ADVANCE_ERROR:
    default:
        cmt_stop();
        break;
    };
}

/**
 * @brief Přepočítá výstupní signál z pásky (g_cmt.output) k aktuálnímu GDG času.
 *
 * Z rozdílu gdg_get_total_ticks() - g_cmt.start_time zjistí od cmtext bloku
 * aktuální úroveň signálu a stav přehrávání bloku. Na konci bloku pásky
 * rozšíření cmt_tmz pokračuje podle playeru (cmt_tape_advance(): další
 * blok, zastavení pásky = pauza, konec pásky = STOP, případně převinutí).
 * Jiné rozšíření Virtual CMT neexistuje (záznam jde do téže pásky).
 * Volá se z čtení
 * PC5 8255 (cmt_read_data), z cmt_screen_done_period() a z mzarch při
 * opuštění pauzy emulace.
 *
 * Se zapnutým echem (g_cmt.echo) navíc ohlásí hrany signálu zvukovému
 * logu (cmt_echo_play_walk(), audio_cmt_echo_changed()); na g_cmt.output
 * ani na polohu pásky to vliv nemá.
 *
 * Bez vložené pásky (ext == NULL) se nic nepočítá a output se nemění.
 * Tento test je defenzivní: platný stav bez pásky je vždy STOP (viz
 * invariant st_CMT), ale funkce se volá bezpodmínečně (mzarch.c při
 * opuštění pauzy), takže nesmí dereferencovat NULL ani při porušeném
 * invariantu (dříve pád po načtení snapshotu pořízeného během přehrávání).
 * Stejně bez otevřeného bloku (páska jen s řídicími bloky): nic se
 * nepočítá.
 *
 * @pre Voláno z emulátorového vlákna.
 * @post Při PLAY bez pauzy s vloženou páskou odpovídá g_cmt.output poloze
 *       pásky; může dojít k přechodu na další blok, do pauzy (zastavení
 *       pásky) nebo do STOP.
 */
/**
 * @brief Echo: ohlásí hrany přehrávaného bloku od čtecí pozice streamu do času @p now_ticks.
 *
 * U bloku s vstreamem projde události streamu od jeho čtecí pozice a pro
 * každou hranu do @p now_ticks zavolá audio_cmt_echo_changed() s časem
 * hrany (začátek přehrávání bloku + poloha hrany). Čtecí pozici posune
 * stejně jako cmt_vstream_get_value() pro tentýž čas, takže následné
 * cmtext_block_get_output() vrátí totéž co bez echa. Blok s bitstreamem
 * se neprochází (úroveň ohlásí cmt_update_output() po svém čtení).
 *
 * @param now_ticks Aktuální čas GDG (gdg_get_total_ticks()).
 * @pre Vlákno emulátoru, PLAY bez pauzy, g_cmt.ext->block != NULL, echo zapnuté.
 */
static void cmt_echo_play_walk(uint64_t now_ticks)
{
    st_CMT_STREAM *stream = g_cmt.ext->block->stream;
    if (!stream || (stream->stream_type != CMT_STREAM_TYPE_VSTREAM))
        return;
    st_CMT_VSTREAM *vs = stream->str.vstream;
    if (!vs || !vs->rate || (now_ticks < g_cmt.start_time))
        return;
    /* stejný převod času na vzorky jako cmtext_block_get_output() */
    uint64_t play_scans = round((double)(now_ticks - g_cmt.start_time) * vs->rate / GDGCLK_BASE);
    /* úroveň na čtecí pozici (začátek bloku, po převinutí): echo se s ní srovná dřív než hrany */
    audio_cmt_echo_changed(vs->last_read_value != 0,
                           g_cmt.start_time + (uint64_t)((double)vs->last_read_position_sample * GDGCLK_BASE / vs->rate));
    while (vs->last_read_position < vs->size)
    {
        int event_byte_length = 0;
        uint64_t end = cmt_vstream_get_last_read_event(vs, &event_byte_length) + vs->last_read_position_sample;
        if (play_scans < end)
            break;
        vs->last_read_value = ~vs->last_read_value & 1;
        vs->last_read_position += event_byte_length;
        vs->last_read_position_sample = end;
        audio_cmt_echo_changed(vs->last_read_value != 0,
                               g_cmt.start_time + (uint64_t)((double)end * GDGCLK_BASE / vs->rate));
    };
}

/* Doxygen viz cmt.h. */
void cmt_echo_frame(void)
{
    const uint64_t now_ticks = gdg_get_total_ticks();
    if (!g_cmt.echo || (!CMT_TEST_FILLED) || CMT_TEST_STOP || CMT_TEST_PAUSED)
    {
        audio_cmt_echo_changed(false, now_ticks);
        return;
    };
    if (CMT_TEST_PLAY && g_cmt.ext->block)
        cmt_echo_play_walk(now_ticks);
}

void cmt_update_output(void)
{

    if ((!CMT_TEST_FILLED) || (!CMT_TEST_PLAY) || (CMT_TEST_PAUSED))
        return;
    /* pojistka: PLAY bez otevřeného bloku (cmt_sanitize_state() ho po snapshotu
     * srovná, jiná cesta ho vytvořit nemá) nemá co přehrávat */
    if (!g_cmt.ext->block)
        return;

    /* echo: hrany, které čtení níž přeskočí, nejdřív do zvukového logu */
    if (g_cmt.echo)
        cmt_echo_play_walk(gdg_get_total_ticks());

    uint64_t play_ticks = gdg_get_total_ticks() - g_cmt.start_time;
    uint64_t transferred_ticks = 0;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* V1.5 fáze 2.2 + 2.3: HWE BP_EVENT_CMT_IN edge-based fire.
     * Snapshot prev output, refresh, fire jen pokud se output reálně
     * změnil (= edge na vstupním signálu z pásky). Edge detekce v
     * breakpoints_enforce_hw_event eliminuje duplicate fire pro stable
     * level, ale fire only-on-change zde redukuje overhead. */
    int prev_output = g_cmt.output;
#endif

    en_CMTEXT_BLOCK_PLAYSTS playsts = cmtext_block_get_output(g_cmt.ext->block, play_ticks, &g_cmt.output, &transferred_ticks);

    /* echo: úroveň po čtení (u vstreamu už ohlášená, u bitstreamu jediný zdroj) */
    if (g_cmt.echo)
        audio_cmt_echo_changed((g_cmt.output & 1) != 0, g_cmt.start_time + play_ticks);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if ( ( g_cmt.output != prev_output ) && g_bp_event_active[ BP_EVENT_CMT_IN ] ) {
        bp_event_fire ( BP_EVENT_CMT_IN, (int32_t) ( g_cmt.output & 1 ) );
    }
#endif

    if (playsts != g_cmt.playsts)
    {
        if (CMTEXT_BLOCK_PLAYSTS_STOP == playsts)
        {
            /* Přehrávat umí jen rozšíření cmt_tmz (registr v cmtext.c, jediné). */
            cmt_tape_advance(transferred_ticks);
        }
        else if (CMTEXT_BLOCK_PLAYSTS_PAUSE == playsts)
        {
            printf("CMT: Playing a gap space of %d ms\n", cmtext_block_get_pause_after(g_cmt.ext->block));
            g_cmt.playsts = playsts;
            ui_cmt_window_update();
        };
    };
}

/**
 * @brief Práce kazety na konci snímku (cmt_on_screen_done_event()).
 *
 * Mimo STOP s vloženou páskou: při RECORD ukončí záznam jako STOP, když
 * buffer hran dosáhl meze (i v pauze), když čas záznamu bez pauzy přesáhl
 * CMTTMZ_REC_MAX_SECONDS i bez hrany (cmttmz_rec_check_time(), zpráva jako
 * u meze bufferu), nebo když od poslední přijaté hrany
 * uplynulo g_cmt.rec_autostop_s s emulovaného času při běžícím motoru
 * (doba pauzy se nepočítá, bez hrany se neukončí; zpráva "Recording
 * stopped automatically after N s of silence"). Bez pauzy dál srovná
 * výstup přehrávání (cmt_update_output()) a jednou za 50 snímků řídí MAX
 * SPEED cpu_boost při nahrávání.
 *
 * @pre Vlákno emulátoru, konec snímku.
 * @post Při splněné podmínce ukončení je transport ve STOP a záznam
 *       připojený k pásce (cmt_stop()).
 */
void cmt_screen_done_period(void)
{

    if ((!CMT_TEST_FILLED) || (CMT_TEST_STOP))
        return;

    /* mez bufferu hran: záznam se ukončí jako STOP (i v pauze, hrany už nepřibudou) */
    if (CMT_TEST_RECORD && cmttmz_rec_limit_reached())
    {
        cmt_stop();
        return;
    };

    if (CMT_TEST_PAUSED)
        return;

    /* mez délky záznamu i bez hran (motor běží, program nezapisuje): hrany
     * by ji ověřily až při příchodu, tedy nikdy */
    /* čas záznamu: start_time z jiné časové osy (např. po načtení staršího
     * stavu) nesmí podtéct na obří hodnotu */
    const uint64_t now_ticks = gdg_get_total_ticks();
    if (CMT_TEST_RECORD
        && cmttmz_rec_check_time((now_ticks > g_cmt.start_time) ? (now_ticks - g_cmt.start_time) : 0))
    {
        cmt_stop();
        return;
    };

    /* Automatické ukončení záznamu: ticho při běžícím motoru (start_time
     * vylučuje dobu pauzy) po aspoň jedné hraně. Emulovaný čas, ne čas
     * hostitele: stejné při přehrání záznamu vstupů i s CPU Boost. */
    if (CMT_TEST_RECORD && g_cmt.rec_autostop_s)
    {
        uint64_t last = 0;
        uint64_t now = (now_ticks > g_cmt.start_time) ? (now_ticks - g_cmt.start_time) : 0;
        if (cmttmz_rec_last_edge(&last) && (now > last)
            && ((now - last) >= (uint64_t)g_cmt.rec_autostop_s * (uint64_t)GDGCLK_BASE))
        {
            s_record_autostop_s = g_cmt.rec_autostop_s;
            cmt_stop();
            return;
        };
    };

    cmt_update_output();

    if (g_cmt.ui_player_update++ < 49)
        return;

    g_cmt.ui_player_update = 0;

    if (CMT_TEST_RECORD)
    {
        if (g_cmt.recording_to_stream == 0)
        {
            /* první zaznamenaný úsek = program opravdu zapisuje */
            if (cmttmz_rec_get()->count > 0)
            {
                g_cmt.recording_to_stream = 1;
                if (g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED)
                {
                    emulator_max_speed_boost(true);
                };
            };
        }
        else if (g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED)
        {
            /* Program 5 s nezapisuje -> MAX SPEED od boostu dočasně pryč,
             * při další aktivitě zpět. Uživatelskou MAX SPEED nemění. */
            if ((gdg_get_total_ticks() - g_cmt.recording_last_event) > (5 * GDGCLK_BASE))
            {
                emulator_max_speed_boost(false);
            }
            else
            {
                emulator_max_speed_boost(true);
            };
        };
    };
}

int cmt_read_data(void)
{
    cmt_update_output();
    return g_cmt.output;
}

/**
 * @brief Předá zápis signálu na pásku (PC1 8255) bufferu hran záznamu.
 *
 * Mimo nahrávání, v pauze (stojící motor) nebo bez vložené pásky
 * (defenzivní test, viz invariant st_CMT) nedělá nic. Čas hrany je od
 * začátku záznamu bez doby stojícího motoru (start_time posouvá
 * cmt_pause(0)); buffer dostane úroveň PC1 po hraně (cmttmz_rec_edge(),
 * konvence úrovně v cmt_tmz_record.h).
 *
 * @param value Úroveň PC1 po zápisu (bit 0).
 *
 * Se zapnutým echem (g_cmt.echo) hranu ohlásí i zvukovému logu
 * (audio_cmt_echo_changed()).
 *
 * @pre Voláno z emulátorového vlákna.
 * @post Při RECORD bez pauzy je zaznamenán čas události (recording_last_event).
 */
void cmt_write_data(int value)
{
    if ((!CMT_TEST_FILLED) || (!CMT_TEST_RECORD) || (CMT_TEST_PAUSED))
        return;
    g_cmt.recording_last_event = gdg_get_total_ticks();
    uint64_t play_ticks = g_cmt.recording_last_event - g_cmt.start_time;
    cmttmz_rec_edge(play_ticks, value & 1);
    /* echo: nahrávaný signál do reproduktoru */
    if (g_cmt.echo)
        audio_cmt_echo_changed((value & 1) != 0, g_cmt.recording_last_event);
}

/**
 * @brief Nastaví volbu cpu_boost (automatická MAX SPEED během přehrávání).
 *
 * Změnu hned promítne do rychlosti emulace přes cmt_cpu_boost_apply():
 * zapnutí během aktivního transportu (PLAY/RECORD bez pauzy) zapne MAX
 * SPEED, vypnutí vypne MAX SPEED jen tehdy, když ji zapnul cpu_boost.
 * Dříve se při vypnutí volalo emulator_max_speed(false) a pauza transportu
 * se neřešila (zapnutí v pauze hned zapnulo MAX SPEED).
 *
 * @param cpu_boost Nová hodnota volby.
 *
 * @pre Voláno z emulátorového vlákna nebo při pozastavené emulaci
 *      (propagace konfigurace).
 */
void cmt_cpu_boost_set(en_CMT_CPU_BOOST cpu_boost)
{
    g_cmt.cpu_boost = cpu_boost;
    cmt_cpu_boost_apply();
}

void cmt_mzfsize_check_set(en_CMT_MZFSIZE_CHECK mzfsize_check)
{
    g_cmt.mzfsize_check = mzfsize_check;
}

/* Doxygen viz cmt.h. */
void cmt_mzfsize_autofix_set(int autofix)
{
    g_cmt.mzfsize_autofix = autofix ? 1 : 0;
}

/* Doxygen viz cmt.h. */
void cmt_echo_set(int echo)
{
    g_cmt.echo = echo ? 1 : 0;
}

/**
 * @brief Délka streamu bloku v taktech GDGCLK_BASE.
 *
 * @param block Blok (nesmí být NULL, se streamem).
 * @return round ( počet vzorků * GDGCLK_BASE / frekvence ); u vstreamu
 *         s frekvencí GDGCLK_BASE přímo počet vzorků.
 */
static uint64_t cmt_block_length_ticks(st_CMTEXT_BLOCK *block)
{
    uint32_t rate = cmtext_block_get_rate(block);
    uint64_t scans = cmtext_block_get_count_scans(block);
    if (rate == GDGCLK_BASE)
        return scans;
    if (!rate)
        return 0;
    return (uint64_t)llround((double)scans * GDGCLK_BASE / rate);
}

/**
 * @brief Pozice pásky: otevřený blok a takty GDGCLK_BASE od začátku jeho streamu.
 *
 * Pozici nese dosavadní časová základna transportu, žádné další počítadlo:
 * - STOP: st_CMTEXT_BLOCK::start_gdgticks (odkud začne příští PLAY;
 *   po otevření bloku a po každém cmt_play() 0),
 * - PLAY v pauze (i zastavení pásky): g_cmt.paused_time,
 * - PLAY bez pauzy: gdg_get_total_ticks() - g_cmt.start_time.
 * Hodnota za koncem streamu (PLAY před zpracováním konce bloku) se ořízne
 * na délku streamu včetně ticha za blokem, které přidal player.
 *
 * Bez vedlejších efektů a bez výpisu.
 *
 * @param[out] block_id Číslo otevřeného bloku (index v souboru pásky).
 * @param[out] ticks_in_block Pozice v taktech GDGCLK_BASE.
 * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, bez otevřeného bloku,
 *         u pásky jiného rozšíření než cmt_tmz, při nahrávání nebo
 *         s NULL parametrem (výstupy se pak nemění).
 * @pre Volá se z vlákna emulátoru.
 */
int cmt_tape_get_position(int *block_id, uint64_t *ticks_in_block)
{
    if ((!block_id) || (!ticks_in_block))
        return EXIT_FAILURE;
    if ((!CMT_TEST_FILLED) || (g_cmt.ext != g_cmt_tmz_extension) || (!g_cmt.ext->block) || CMT_TEST_RECORD)
        return EXIT_FAILURE;
    st_CMTEXT_BLOCK *block = g_cmt.ext->block;
    uint64_t ticks;
    if (CMT_TEST_STOP)
        ticks = block->start_gdgticks;
    else if (CMT_TEST_PAUSED)
        ticks = g_cmt.paused_time;
    else
        ticks = gdg_get_total_ticks() - g_cmt.start_time;
    uint64_t end = cmt_block_length_ticks(block);
    *block_id = cmtext_block_get_block_id(block);
    *ticks_in_block = (ticks > end) ? end : ticks;
    return EXIT_SUCCESS;
}

void cmt_motor_get_state(bool *known, bool *on)
{
    bool k = false;
    bool o = false;
    if (CMTHACK_TEST_FAKES_MOTOR)
    {
        /* hack mimo nahrávání: hrana PC3 přepíná PC4 (pio8255_write()), PC4 = stav motoru */
        k = true;
        o = (g_pio8255.signal_pc04 & 1) ? true : false;
    }
    else if (CMT_TEST_FILLED && !CMT_TEST_STOP)
    {
        /* hrana PC3 přepíná motor (cmt_motor_toggle()): bez pauzy uživatele
         * motor běží = bez pauzy, s ní nese stav motoru motor_off */
        k = true;
        o = g_cmt.user_paused ? !g_cmt.motor_off : !CMT_TEST_PAUSED;
    };
    if (known)
        *known = k;
    if (on)
        *on = o;
}

int cmt_tape_get_ui_position(int *block_id, double *pos_s)
{
    if (block_id)
        *block_id = -1;
    if (pos_s)
        *pos_s = 0;
    if ((!block_id) || (!pos_s))
        return EXIT_FAILURE;
    if ((!CMT_TEST_FILLED) || (g_cmt.ext != g_cmt_tmz_extension) || CMT_TEST_RECORD)
        return EXIT_FAILURE;
    int id = cmttmz_get_open_block_id();
    if (id < 0)
        return EXIT_FAILURE;
    uint64_t ticks;
    if (CMT_TEST_STOP)
    {
        /* ve STOP blok vyměňují jen zásahy pod device_state_lock() */
        st_CMTEXT_BLOCK *block = g_cmt.ext->block;
        if (!block)
            return EXIT_FAILURE;
        ticks = block->start_gdgticks;
    }
    else if (CMT_TEST_PAUSED)
    {
        ticks = g_cmt.paused_time;
    }
    else
    {
        uint64_t now = gdg_get_total_ticks();
        ticks = (now > g_cmt.start_time) ? (now - g_cmt.start_time) : 0;
    };
    double pos = (double)ticks / (double)GDGCLK_BASE;
    st_CMTTMZ_BLOCK_UI bui;
    if ((EXIT_SUCCESS == cmttmz_get_block_ui(id, &bui)) && (bui.length_state == CMTTMZ_LENGTH_KNOWN) && (pos > bui.length_s))
        pos = bui.length_s;
    *block_id = id;
    *pos_s = pos;
    return EXIT_SUCCESS;
}

int cmt_record_get_progress(double *time_s, uint32_t *pulses)
{
    if (time_s)
        *time_s = 0;
    if (pulses)
        *pulses = 0;
    if ((!CMT_TEST_FILLED) || (!CMT_TEST_RECORD))
        return EXIT_FAILURE;
    uint64_t ticks;
    if (CMT_TEST_PAUSED)
    {
        ticks = g_cmt.paused_time;
    }
    else
    {
        uint64_t now = gdg_get_total_ticks();
        ticks = (now > g_cmt.start_time) ? (now - g_cmt.start_time) : 0;
    };
    if (time_s)
        *time_s = (double)ticks / (double)GDGCLK_BASE;
    if (pulses)
        *pulses = cmttmz_rec_get()->count / 2u;
    return EXIT_SUCCESS;
}

/**
 * @brief Nastaví pozici pásky: blok a takty GDGCLK_BASE od začátku jeho streamu.
 *
 * Jiný blok než otevřený se otevře přímo (cmttmz_seek_block(): player se
 * přestaví na blok s prázdnými zásobníky a výchozí úrovní signálu, jako
 * cb_open_block). Pozice za koncem streamu se ořízne na konec. Ve STOP se
 * pozice uloží do st_CMTEXT_BLOCK::start_gdgticks a příští cmt_play()
 * začne odtud. V pauze (PLAY + paused, i po zastavení pásky) se nastaví
 * g_cmt.paused_time, čtení streamu se vrátí na začátek (cb_play bloku,
 * cmt_vstream_read_reset()) a g_cmt.output dostane úroveň signálu v daném
 * místě; pokračování (cmt_pause(0), motor PC3) hraje odtud.
 *
 * Kromě pozice (blok, start_gdgticks, paused_time, playsts = BODY
 * v pauze, output v pauze) nic nemění a nic nevypisuje.
 *
 * Ve STOP se g_cmt.output nemění a zůstává 0 (zastavený magnetofon nic
 * nevydává a cmt_update_output() ve STOP nic nepočítá); úroveň signálu
 * v nastaveném místě se objeví prvním cmt_update_output() po PLAY. To je
 * definované chování, ne opomenutí.
 *
 * Změna rychlosti otevřeného bloku (cmt_change_speed(),
 * cmt_change_custom_pulses()) nastavenou pozici ve STOP zruší: blok se
 * převine na začátek.
 *
 * @param block_id Index bloku v souboru pásky.
 * @param ticks_in_block Pozice v taktech GDGCLK_BASE.
 * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, u pásky jiného rozšíření,
 *         při nahrávání, při PLAY bez pauzy, pro blok mimo rozsah, blok
 *         CONTROL nebo blok, který se přeskakuje (pak se nic nemění).
 * @pre Volá se z vlákna emulátoru. Při jiném block_id než otevřeném (ve
 *      STOP i v pauze) funkce nahradí g_cmt.ext->block (starý blok uvolní).
 *      UI vlákno blok čte pod device_state_lock(); funkce sama zámek
 *      nebere. Volající, který běží mimo zásah GUI/MCP (ten už zámek drží:
 *      cmt_request, dbgapi_cmd_needs_device_lock()), musí zámek držet sám,
 *      jinak jde o stejnou výměnu bloku bez zámku jako při přechodu na další
 *      blok během PLAY (cmttmz_advance()) [neověřeno, že je v praxi bezpečná].
 */
int cmt_tape_set_position(int block_id, uint64_t ticks_in_block)
{
    if ((!CMT_TEST_FILLED) || (g_cmt.ext != g_cmt_tmz_extension) || CMT_TEST_RECORD)
        return EXIT_FAILURE;
    if (CMT_TEST_PLAY && (!CMT_TEST_PAUSED))
        return EXIT_FAILURE;
    if ((!g_cmt.ext->block) || (cmtext_block_get_block_id(g_cmt.ext->block) != block_id))
    {
        if (EXIT_SUCCESS != cmttmz_seek_block(block_id))
            return EXIT_FAILURE;
        ui_cmt_window_update();
    };
    st_CMTEXT_BLOCK *block = g_cmt.ext->block;
    if (!block)
        return EXIT_FAILURE;

    uint64_t end = cmt_block_length_ticks(block);
    uint64_t ticks = (ticks_in_block > end) ? end : ticks_in_block;

    if (CMT_TEST_STOP)
    {
        block->start_gdgticks = ticks;
        return EXIT_SUCCESS;
    };

    /* PLAY v pauze */
    g_cmt.paused_time = ticks;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_BODY;
    block->cb_play(g_cmt.ext); /* čtení streamu od začátku (stream se čte jen dopředu) */
    int output = g_cmt.output;
    uint64_t transferred = 0;
    if (CMTEXT_BLOCK_PLAYSTS_BODY == cmtext_block_get_output(block, ticks, &output, &transferred))
        g_cmt.output = output;
    return EXIT_SUCCESS;
}
