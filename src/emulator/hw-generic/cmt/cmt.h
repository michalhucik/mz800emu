/* 
 * File:   cmt.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 11. srpna 2015, 12:08
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

#ifndef CMT_H
#define CMT_H


#include <stdint.h>
#include <stdbool.h>

#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/gdg.h"

#include "cmtext.h"

#include "libs/cmtspeed/cmtspeed.h"
#include "libs/cmt_stream/cmt_stream.h"

    typedef enum en_CMT_STATE {
        CMT_STATE_STOP = 0,
        CMT_STATE_PLAY,
        CMT_STATE_RECORD,
    } en_CMT_STATE;


    typedef enum en_CMT_CPU_BOOST {
        CMT_CPU_BOOST_DISABLED = 0,
        CMT_CPU_BOOST_ENABLED = 1
    } en_CMT_CPU_BOOST;


    typedef enum en_CMT_MZFSIZE_CHECK {
        CMT_MZFSIZE_CHECK_DISABLED = 0,
        CMT_MZFSIZE_CHECK_ENABLED = 1
    } en_CMT_MZFSIZE_CHECK;


    /**
     * @brief Stav virtuálního kazetového magnetofonu (globální instance g_cmt).
     *
     * Drží vloženou pásku (ext), stav transportu (state, paused), stav
     * přehrávání aktuálního bloku (playsts), výstupní signál z pásky (output)
     * a časovou základnu přehrávání v GDG ticích (start_time, paused_time).
     *
     * Invarianty:
     * - Bez vložené pásky (ext == NULL) je transport vždy ve stavu
     *   CMT_STATE_STOP s paused == 0 a playsts == CMTEXT_BLOCK_PLAYSTS_STOP.
     *   Funkce, které ve stavu PLAY/RECORD čtou ext (cmt_update_output,
     *   cmt_write_data), na tento invariant spoléhají a navíc ho samy
     *   defenzivně testují. Za běhu ho udržují cmt_stop() a cmt_eject()
     *   (i bez vložené pásky přepnou do STOP), po načtení snapshotu ho
     *   obnoví cmt_sanitize_state().
     * - Ve stavu CMT_STATE_STOP je paused == 0.
     *
     * Ownership: ext ukazuje na statickou instanci cmtext rozšíření (nikdy
     * se neuvolňuje přes g_cmt), last_filename a ui_base_filename vlastní
     * g_cmt (uvolňují se přes baseui_tools_mem_free).
     *
     * Synchronizace: přístup jen z emulátorového vlákna (nebo s pozastavenou
     * emulací, např. při načítání snapshotu).
     */
    typedef struct st_CMT {
        st_CMTEXT *ext;
        char *last_filename;
        char *ui_base_filename;
        en_CMT_STREAM_POLARITY polarity;
        en_CMTSPEED mz_cmtspeed;
        en_CMT_STATE state;
        int paused;
        en_CMTEXT_BLOCK_PLAYSTS playsts;
        int output;
        uint64_t start_time;
        uint64_t paused_time;
        int ui_player_update;
        /** Uživatelská preference "MAX SPEED během přehrávání" (cfg CMT/cpu_boost,
         *  cfg element ukazuje přímo sem). Snapshot ji zapisuje, ale při
         *  načtení neobnovuje (viz snap_cmt_load). */
        en_CMT_CPU_BOOST cpu_boost;
        en_CMT_MZFSIZE_CHECK mzfsize_check;
        int recording_to_stream; // pri RECORD identifikuje, zda uz mame zalozen stream
        uint64_t recording_last_event;
    } st_CMT;

    extern st_CMT g_cmt;


#define CMT_TEST_FILLED (g_cmt.ext != NULL)
#define CMT_TEST_STOP (g_cmt.state == CMT_STATE_STOP)
#define CMT_TEST_PLAY (g_cmt.state == CMT_STATE_PLAY)
#define CMT_TEST_RECORD (g_cmt.state == CMT_STATE_RECORD)
#define CMT_TEST_PAUSED (g_cmt.paused == 1)
#define CMT_TEST_MZFSIZE_CHECK_ENABLED (g_cmt.mzfsize_check == CMT_MZFSIZE_CHECK_ENABLED)

#define CMT_TEST_MZ_CMTSPEED(speed) (g_cmt.mz_cmtspeed == speed)
#define CMT_TEST_CPU_BOOST_ENABLED (g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED)
#define CMT_TEST_MZF_SIZE_CHECK_ENABLED (g_cmt.mzfsize_check == CMT_MZFSIZE_CHECK_ENABLED)

#define CMT_SET_MZF_SIZE_CHECK(value) (g_cmt.mzfsize_check = value)

#define CMT_TEST_POLARITY_NORMAL (g_cmt.polarity == CMT_STREAM_POLARITY_NORMAL)
#define CMT_TEST_POLARITY_INVERTED (g_cmt.polarity == CMT_STREAM_POLARITY_INVERTED)

#define cmt_on_screen_done_event( ) { if ( !CMT_TEST_STOP ) cmt_screen_done_period (); }

    static inline double cmt_get_playtime ( void ) {
        if ( !CMT_TEST_FILLED ) return 0;
        if ( CMT_TEST_STOP ) return 0;
        if ( CMT_TEST_PAUSED ) return g_cmt.paused_time * ( 1 / (double) GDGCLK_BASE );
        uint64_t now_ticks = gdg_get_total_ticks ( );
        uint64_t play_ticks = ( now_ticks - g_cmt.start_time );
        double play_time = play_ticks * ( 1 / (double) GDGCLK_BASE );
        return play_time;
    }

#define cmt_get_ui_base_filename() (g_cmt.ui_base_filename ? g_cmt.ui_base_filename : "")

#ifdef __cplusplus
extern "C" {
#endif
    
    void cmt_init ( void );
    void cmt_exit ( void );
    void cmt_rear_dip_switch_cmt_inverted_polarity ( unsigned value );

    int cmt_open_file_by_extension ( char *filename );
    void cmt_ui_open(bool play_immediately);
    void cmt_play ( void );
    void cmt_play_paused ( void );
    void cmt_ui_record ( void );
    int cmt_record_to_file ( const char *path );
    void cmt_pause ( int value );
    void cmt_stop ( void );
    void cmt_eject ( void );
    bool cmt_sanitize_state ( void );
    int cmt_change_speed ( en_CMTSPEED cmtspeed );

    void cmt_screen_done_period ( void );
    int cmt_read_data ( void );
    void cmt_update_output ( void );
    void cmt_write_data ( int value );

    void cmt_cpu_boost_set ( en_CMT_CPU_BOOST cpu_boost );

    /**
     * @brief Srovná MAX SPEED se stavem transportu podle volby cpu_boost.
     *
     * Sémantika cpu_boost: automatická MAX SPEED po dobu, kdy páska běží.
     * - cpu_boost zapnutý, páska vložená, PLAY nebo RECORD bez pauzy
     *   -> emulator_max_speed_boost(true) (zapne MAX SPEED, pokud neběží),
     * - jinak -> emulator_max_speed_boost(false): vypne MAX SPEED jen tehdy,
     *   když ji zapnul cpu_boost; MAX SPEED zvolenou uživatelem nechá.
     *
     * Volá se při každé změně transportu (play, pauza, stop), při změně
     * volby a po načtení snapshotu (snap_cmt_load) - snapshot obnoví stav
     * transportu, ale dříve rychlost emulace nesrovnal. Samotnou volbu
     * cpu_boost snapshot neobnovuje (uživatelská preference, do snapshotu
     * se zapisuje jen kvůli kompatibilitě); platí aktuální volba uživatele.
     * Při RECORD pak MAX SPEED dál řídí cmt_screen_done_period() podle
     * aktivity zápisu: po 5 s bez zápisu na pásku MAX SPEED od boostu
     * vypne, při další aktivitě (kontrola jednou za 50 snímků) ji zapne.
     * Vypne-li uživatel MAX SPEED během boostu, automatika ji znovu zapne
     * při nejbližší pauze/obnovení transportu nebo změně volby cpu_boost
     * (při RECORD i při další aktivitě zápisu).
     *
     * @pre Voláno z emulátorového vlákna nebo při pozastavené emulaci;
     *      platí invarianty st_CMT.
     * @post Bez aktivního transportu (nebo s vypnutým cpu_boost) neběží
     *       MAX SPEED zapnutá automatikou (g_emulator.max_speed_boost == false).
     */
    void cmt_cpu_boost_apply ( void );
    void cmt_mzfsize_check_set ( en_CMT_MZFSIZE_CHECK mzfsize_check );

#ifdef __cplusplus
}
#endif

#endif /* CMT_H */

