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

#include "mzarch/mzcommon_config.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <glib.h>
#include <assert.h>

// Lokalizace
#include "i18n.h"

#include "cmt.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/bp_event.h"
#endif
#include "hw-generic/gdg/gdg_state.h"
#include "mzarch/mzhal.h"

#include "baseui/baseui.h"
#include "baseui/baseui_filechooser.h"

#define NOT_HAVE_CMT_UI

#include "ui-imgui/cmt/imgui_cmt.h"

#define ui_cmt_window_update() imgui_cmt_tape_update_filelist()

#include "cfgmain.h"

#include "cmthack.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "cmtext.h"
#include "cmtext_block.h"
#include "cmt_mzf.h"
#include "libs/mzf/mzf.h"
#include "emulator.h"

st_CMT g_cmt;
char *g_ui_cmt_filters = NULL;


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/**
 * V1.5 fáze 2.2: HWE BP_EVENT_CMT_MSTATE edge fire helper pro
 * non-CMTHACK případ. Mstate je odvozený z CMT_TEST_STOP/PAUSED:
 * mstate = (STOP || PAUSED) ? 0 : 1. Voláno z cmt_stop/cmt_pause/
 * cmt_play/cmt_eject po update state.
 *
 * Edge detection v breakpoints_enforce_hw_event eliminuje duplicate
 * fire na stejný level; helper jen forwarduje aktuální mstate
 * do enforce vrstvy.
 */
static void cmt_bp_event_mstate_fire ( void ) {
    if ( !g_bp_event_active[ BP_EVENT_CMT_MSTATE ] ) return;
    /* CMTHACK případ má fire v pio8255_write (toggle signal_pc04);
     * tady fire jen pro non-CMTHACK = derived ze STOP/PAUSED. */
    if ( CMTHACK_TEST_IS_INSTALLED ) return;
    int mstate = ( ( CMT_TEST_STOP ) || ( CMT_TEST_PAUSED ) ) ? 0 : 1;
    bp_event_fire ( BP_EVENT_CMT_MSTATE, (int32_t) mstate );
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
#define cmt_bp_event_state_change_fire() ((void)0)
#endif


void cmt_stop(void)
{
    if (!CMT_TEST_FILLED)
        return;
    if (CMT_TEST_STOP)
        return;

    if (g_cmt.ext->cb_stop)
        g_cmt.ext->cb_stop();

    printf("Virtual CMT is stopped.\n");

    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    g_cmt.output = 0;

    if (g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED)
    {
        emulator_max_speed(false);
    };

    cmt_bp_event_mstate_fire ( );
    cmt_bp_event_state_change_fire ( );

    ui_cmt_window_update();
}

void cmt_pause(int value)
{

    if (value == g_cmt.paused)
        return;

    if (value)
    {
        if (CMT_TEST_STOP)
        {
            cmt_play_paused();
        }
        else
        {
            g_cmt.paused = 1;
        };
        if (CMT_TEST_STOP)
        {
            g_cmt.paused = 0;
        };
        g_cmt.paused_time = gdg_get_total_ticks() - g_cmt.start_time;
    }
    else
    {
        g_cmt.paused = 0;
    };

    if ((g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED) && (!CMT_TEST_PAUSED) && (!CMT_TEST_STOP))
    {
        emulator_max_speed(true);
    }
    else
    {
        emulator_max_speed(false);
    };

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

void cmt_eject(void)
{
    if (!CMT_TEST_FILLED)
        return;
    if (!CMT_TEST_STOP)
        cmt_stop();
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

    ui_cmt_window_update();
}

void cmt_play(void)
{
    if (!CMT_TEST_FILLED)
        return;
    if (!CMT_TEST_STOP)
        return;
    if (EXIT_SUCCESS != cmtext_is_playable(g_cmt.ext))
        return;
    g_cmt.state = CMT_STATE_PLAY;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_BODY;
    g_cmt.start_time = gdg_get_total_ticks();
    // printf ( "CMT start: %ul\n", gdg_get_total_ticks ( ) );
    g_cmt.ui_player_update = 0;
    ui_cmt_window_update();
    if ((g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED) && (!CMT_TEST_PAUSED))
    {
        emulator_max_speed(true);
    };
    if (!CMT_TEST_PAUSED)
    {
        printf("Virtual CMT is playing.\n");
    }
    else
    {
        g_cmt.paused_time = 0;
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

void cmt_play_paused(void)
{
    g_cmt.paused = 1;
    cmt_play();
}

static void cmt_record(void)
{
    // implicitne zacneme v pauze
    g_cmt.paused = 1;

    g_cmt.state = CMT_STATE_RECORD;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_BODY;
    g_cmt.start_time = gdg_get_total_ticks();
    // printf ( "CMT start: %ul\n", gdg_get_total_ticks ( ) );
    g_cmt.ui_player_update = 0;
    g_cmt.recording_to_stream = 0;
    ui_cmt_window_update();
    /*
        if ( g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED ) {
            mz800_switch_emulation_speed ( 1 );
        };
     */

    if (!CMT_TEST_PAUSED)
    {
        printf("Virtual CMT is recording.\n");
    }
    else
    {
        printf("Virtual CMT - recording is paused.\n");
        g_cmt.paused_time = 0;
    };
}

static void cmt_ui_record_cb(baseui_fchooser_t *fch)
{
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        ui_cmt_window_update();
        return;
    };

    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        baseui_filechooser_destroy(fch);
        ui_cmt_window_update();
        return;
    };

    char *filepath = fch->selected_filePathName;
    fch->selected_filePathName = NULL;
    baseui_filechooser_destroy(fch);

    const char *fileext = baseui_filechooser_get_extension_ptr(filepath);
    if (fileext != NULL)
    {
        if (0 != strcasecmp(fileext, ".wav"))
            fileext = NULL;
    };

    if (fileext == NULL)
    {
        GString *gs = g_string_new(0);
        int i = 0;

        FILE *tst_fh = NULL;

        do
        {
            if (i > 100)
                break;
            g_string_printf(gs, "%s", filepath);
            if (i != 0)
            {
                g_string_append_printf(gs, "_%d", i);
            };
            i++;
            g_string_append(gs, ".wav");
            tst_fh = baseui_tools_file_open(gs->str, "rb+");
        } while (tst_fh);

        if (tst_fh)
        {
            g_string_free(gs, TRUE);
            baseui_tools_file_close(tst_fh);
            printf("Can't save to file: %s.wav\n", filepath);
            free(filepath);
            ui_cmt_window_update();
            return;
        }
        else
        {
            free(filepath);
            filepath = g_string_free(gs, FALSE);
        };
    };

    st_CMTEXT *ext = cmtext_get_recording_extension();

    if (EXIT_SUCCESS != ext->cb_open(filepath))
    {
        baseui_error("%s can't open file '%s'\n", cmtext_get_description(ext), filepath);
        baseui_tools_mem_free(filepath);
        ui_cmt_window_update();
        return;
    };

    baseui_tools_mem_free(filepath);
    g_cmt.ext = ext;
    cmt_record();
}

void cmt_ui_record(void)
{
    if (!CMT_TEST_STOP)
        return;

    if ((CMT_TEST_FILLED) && (EXIT_SUCCESS != cmtext_is_recordable(g_cmt.ext)))
    {
        cmt_eject();
    };

    if (!CMT_TEST_FILLED)
    {

        baseui_fchooser_t *fch = baseui_filechooser_save_file(_("Create a new WAV file"), ".wav", NULL, NULL, _("newfile.wav"), cmt_ui_record_cb, NULL);
        if (!fch)
        {
            fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        };
    }
    else
    {
        cmt_record();
    };
}

/**
 * @brief Non-UI vstupní bod pro zahájení nahrávání do WAV souboru.
 *
 * Spustí RECORD bez file dialogu (= ekvivalent cmt_ui_record minus
 * baseui_filechooser). Určeno pro programové ovládání (MCP server,
 * testy). Output formát je WAV (= recording extension vrácená
 * cmtext_get_recording_extension).
 *
 * Sekvence zrcadlí cmt_ui_record + cmt_ui_record_cb:
 *   1. Pokud CMT není ve STOP, nahrávání nelze zahájit (= EXIT_FAILURE).
 *   2. Pokud je nahrán nerecordable obraz, je vysunut (cmt_eject).
 *   3. Pokud po kroku 2 zůstává nahrán recordable obraz, nahrává se do
 *      něj (cmt_record bez nového cb_open).
 *   4. Jinak se otevře recording extension nad zadanou cestou a spustí
 *      se cmt_record.
 *
 * Nahrávání startuje v pauze (= cmt_record nastaví g_cmt.paused = 1),
 * shodně s UI variantou. Pro reálný zápis dat musí klient následně
 * zrušit pauzu (cmt_pause(0)).
 *
 * @param path Cesta k cílovému WAV souboru. Nesmí být NULL/prázdná.
 *             Funkce parametr nemodifikuje (cast na char* kvůli legacy
 *             signatuře cb_open).
 * @return EXIT_SUCCESS při úspěšném zahájení nahrávání, jinak
 *         EXIT_FAILURE (= špatný stav, neplatná cesta nebo selhání
 *         otevření souboru).
 *
 * @pre Musí být voláno z emulátorového vlákna (manipuluje g_cmt a
 *      volá cmtext cb_open, stejně jako ostatní cmt_* funkce).
 * @post Při úspěchu je g_cmt.state == CMT_STATE_RECORD a g_cmt.paused == 1.
 */
int cmt_record_to_file(const char *path)
{
    if (!path || path[0] == '\0')
        return EXIT_FAILURE;

    /* Nahrávat lze jen ze stavu STOP (= shodně s cmt_ui_record). */
    if (!CMT_TEST_STOP)
        return EXIT_FAILURE;

    /* Pokud je nahrán obraz, do kterého nelze nahrávat, vysunout ho. */
    if ((CMT_TEST_FILLED) && (EXIT_SUCCESS != cmtext_is_recordable(g_cmt.ext)))
    {
        cmt_eject();
    };

    /* Recordable obraz už nahrán -> nahrávat do něj bez nového cb_open. */
    if (CMT_TEST_FILLED)
    {
        cmt_record();
        return EXIT_SUCCESS;
    };

    /* Pre-check zapisovatelnosti cílové cesty.
     *
     * Recording cb_open (cmtsave_container_open) si název jen
     * zapamatuje a fyzicky soubor otevírá až líně při prvním zápisu
     * dat - tedy sám neselže nad neexistujícím adresářem / read-only
     * cestou. To je v pořádku pro UI flow (file dialog cestu validuje),
     * ale pro programové volání (MCP) potřebujeme deterministické
     * selhání hned. Otevřeme proto cestu pro zápis (= vytvoří/zkrátí
     * soubor, do kterého se stejně bude nahrávat) a hned zavřeme;
     * pokud open selže, cesta není zapisovatelná. */
    FILE *wtst = baseui_tools_file_open(path, "wb");
    if (!wtst)
    {
        baseui_error("CMT recording: path not writable: '%s'\n", path);
        return EXIT_FAILURE;
    };
    baseui_tools_file_close(wtst);

    /* Otevřít recording extension nad cílovou cestou. */
    st_CMTEXT *ext = cmtext_get_recording_extension();

    if (EXIT_SUCCESS != ext->cb_open((char *) path))
    {
        baseui_error("%s can't open file '%s'\n", cmtext_get_description(ext), path);
        ui_cmt_window_update();
        return EXIT_FAILURE;
    };

    g_cmt.ext = ext;
    cmt_record();
    return EXIT_SUCCESS;
}

int cmt_change_speed(en_CMTSPEED cmtspeed)
{
    int ret = EXIT_SUCCESS;

    if (CMT_TEST_FILLED && CMT_TEST_STOP)
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

    ui_cmt_window_update();

    return ret;
}

void cmt_exit(void)
{

    cmt_eject();
    if (g_cmt.last_filename)
        baseui_tools_mem_free(g_cmt.last_filename);

    if (g_ui_cmt_filters)
        baseui_tools_mem_free(g_ui_cmt_filters);

    cmthack_exit();
    cmtext_exit();
}

void cmt_propagatecfg_cmt_speed(void *e, void *data)
{
    (void)data;
    g_cmt.mz_cmtspeed = cfgelement_get_keyword_value((CFGELM *)e);
    ui_cmt_window_update();
}

void cmt_rear_dip_switch_cmt_inverted_polarity(unsigned value)
{
    value &= 1;
    if (value == g_cmt.polarity)
        return;
    g_cmt.polarity = value;
    if (!CMT_TEST_FILLED)
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

    CFGELM *elm;
    elm = cfgmodule_register_new_element(cmod, "mz_cmtspeed", CFGENTYPE_KEYWORD, CMTSPEED_1_1,
                                         CMTSPEED_1_1, "SPEED_1/1",
                                         CMTSPEED_2_1, "SPEED_2/1",
                                         CMTSPEED_7_3, "SPEED_7/3",
                                         CMTSPEED_8_3, "SPEED_8/3",
                                         CMTSPEED_3_1, "SPEED_3/1",
                                         -1);
    cfgelement_set_propagate_cb(elm, cmt_propagatecfg_cmt_speed, NULL);
    cfgelement_set_handlers(elm, NULL, (void *)&g_cmt.mz_cmtspeed);

    elm = cfgmodule_register_new_element(cmod, "cmt_polarity_inverted", CFGENTYPE_BOOL, CMT_STREAM_POLARITY_NORMAL);
    // cfgelement_set_propagate_cb(elm, cmt_propagatecfg_inverted_polarity, NULL);
    cfgelement_set_handlers(elm, (void *)&g_cmt.polarity, (void *)&g_cmt.polarity);

    elm = cfgmodule_register_new_element(cmod, "cpu_boost", CFGENTYPE_BOOL, CMT_CPU_BOOST_ENABLED);
    cfgelement_set_propagate_cb(elm, cmt_propagatecfg_cpu_boost, NULL);
    cfgelement_set_handlers(elm, (void *)&g_cmt.cpu_boost, (void *)&g_cmt.cpu_boost);

    elm = cfgmodule_register_new_element(cmod, "mzfsize_check", CFGENTYPE_BOOL, CMT_MZFSIZE_CHECK_ENABLED);
    cfgelement_set_propagate_cb(elm, cmt_propagatecfg_mzfsize_check, NULL);
    cfgelement_set_handlers(elm, (void *)&g_cmt.mzfsize_check, (void *)&g_cmt.mzfsize_check);

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);

    g_cmt.output = 0;
    g_cmt.start_time = 0;
    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    g_cmt.ext = NULL;
    g_cmt.last_filename = baseui_tools_mem_alloc0(1);
    g_cmt.ui_player_update = 0;
    g_cmt.ui_base_filename = NULL;

    cmtext_init();

    GString *gs = g_string_new(0);
    g_string_append_printf(gs, "%s{.mzf,.m12,.mzt,.tap,.wav,.wave}", _("All Supported CMT Files"));
    g_string_append_printf(gs, ", %s{.mzt,.tap}", _("Tape Files"));
    g_string_append_printf(gs, ", .mzf, .m12, .mzt, .tap, .wav, .wave, .*");
    g_ui_cmt_filters = g_string_free(gs, FALSE);

    ui_cmt_window_update();

    cmthack_init();
}

int cmt_open_file_by_extension(char *filename)
{

    st_CMTEXT *ext = cmtext_get_extension(filename);

    if (!ext)
    {
        baseui_error("Unknown CMT file extension '%s'\n", filename);
        return EXIT_FAILURE;
    }

    cmt_eject();

    int ret = ext->cb_open(filename);
    if (ret == EXIT_SUCCESS)
    {
        g_cmt.ext = ext;
        g_cmt.ui_base_filename = g_path_get_basename(filename);
    }
    else
    {
        baseui_error("%s can't open file '%s'\n", cmtext_get_description(ext), filename);
    };

    ui_cmt_window_update();

    if ((ret == EXIT_SUCCESS) && (CMT_TEST_MZFSIZE_CHECK_ENABLED))
    {
        if (0 == strcmp(cmtext_get_name(g_cmt.ext), "MZF"))
        {
            st_CMTEXT_BLOCK *mzfblk = cmtext_get_block(g_cmt.ext);
            st_CMTMZF_BLOCKSPEC *blspec = mzfblk->spec;
            st_MZF_HEADER *mzfhdr = blspec->hdr;
            unsigned mzf_size = mzfhdr->fsize + sizeof(st_MZF_HEADER);
            baseui_cmt_check_mzf_filesize(filename, mzf_size);
        };
    };

    return ret;
}

static void cmt_ui_open_cb(baseui_fchooser_t *fch)
{
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        return;
    };

    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        baseui_filechooser_destroy(fch);
        return;
    };

    char *filename = fch->selected_filePathName;
    cmt_open_file_by_extension(filename);
    baseui_filechooser_destroy(fch);

    bool play_immediately = GPOINTER_TO_INT(fch->user_data);
    if (play_immediately)
    {
        cmt_play();
    };
}

void cmt_ui_open(bool play_immediately)
{
    baseui_fchooser_t *fch = baseui_filechooser_open_file(_("Select CMT file to open"), g_ui_cmt_filters, NULL, NULL, g_cmt.last_filename, cmt_ui_open_cb, GINT_TO_POINTER(play_immediately));
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    };
}

void cmt_update_output(void)
{

    if ((!CMT_TEST_PLAY) || (CMT_TEST_PAUSED))
        return;

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

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if ( ( g_cmt.output != prev_output ) && g_bp_event_active[ BP_EVENT_CMT_IN ] ) {
        bp_event_fire ( BP_EVENT_CMT_IN, (int32_t) ( g_cmt.output & 1 ) );
    }
#endif

    if (playsts != g_cmt.playsts)
    {
        if (CMTEXT_BLOCK_PLAYSTS_STOP == playsts)
        {
            int total_blocks = cmtext_container_get_count_blocks(g_cmt.ext->container);
            int play_block = cmtext_block_get_block_id(g_cmt.ext->block) + 1;

            if (play_block < total_blocks)
            {
                assert(g_cmt.ext->container->cb_next_block);

                if ((!g_cmt.ext->container->cb_next_block) || (EXIT_FAILURE == g_cmt.ext->container->cb_next_block()))
                {
                    cmt_stop();
                }
                else
                {
                    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_BODY;
                    g_cmt.start_time = gdg_get_total_ticks() - transferred_ticks;
                    ui_cmt_window_update();
                };
            }
            else
            {
                cmt_stop();
            };
        }
        else if (CMTEXT_BLOCK_PLAYSTS_PAUSE == playsts)
        {
            printf("CMT: Playing a gap space of %d ms\n", cmtext_block_get_pause_after(g_cmt.ext->block));
            g_cmt.playsts = playsts;
            ui_cmt_window_update();
        };
    };
}

void cmt_screen_done_period(void)
{

    if ((!CMT_TEST_FILLED) || (CMT_TEST_STOP) || (CMT_TEST_PAUSED))
        return;

    cmt_update_output();

    if (g_cmt.ui_player_update++ < 49)
        return;

    g_cmt.ui_player_update = 0;

    if (CMT_TEST_RECORD)
    {
        if (g_cmt.recording_to_stream == 0)
        {
            st_CMTEXT_BLOCK *block = cmtext_get_block(g_cmt.ext);
            st_CMT_STREAM *stream = cmtext_block_get_stream(block);
            if (stream != NULL)
            {
                g_cmt.recording_to_stream = 1;
                if (g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED)
                {
                    emulator_max_speed(true);
                };
            };
        }
        else
        {
            if ((gdg_get_total_ticks() - g_cmt.recording_last_event) > (5 * g_mzhal.gdgclk_base))
            {
                if ((g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED) && (EMULATOR_TEST_MAX_SPEED))
                {
                    emulator_max_speed(false);
                };
            }
            else
            {
                if ((g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED) && (!EMULATOR_TEST_MAX_SPEED))
                {
                    emulator_max_speed(true);
                };
            };
        };
    };
}

int cmt_read_data(void)
{
    cmt_update_output();
    return g_cmt.output;
}

void cmt_write_data(int value)
{
    if ((!CMT_TEST_RECORD) || (CMT_TEST_PAUSED))
        return;
    g_cmt.recording_last_event = gdg_get_total_ticks();
    uint64_t play_ticks = g_cmt.recording_last_event - g_cmt.start_time;
    if (g_cmt.ext->cb_write)
        g_cmt.ext->cb_write(play_ticks, ~value);
}

void cmt_cpu_boost_set(en_CMT_CPU_BOOST cpu_boost)
{

    g_cmt.cpu_boost = cpu_boost;

    if (!CMT_TEST_STOP)
    {
        if (g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED)
        {
            emulator_max_speed(true);
        }
        else
        {
            emulator_max_speed(false);
        };
    };
}

void cmt_mzfsize_check_set(en_CMT_MZFSIZE_CHECK mzfsize_check)
{
    g_cmt.mzfsize_check = mzfsize_check;
}
