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

#include "mzarch/mzhal.h"
#include "hw-generic/gdg/gdg_state.h"

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
        if ( CMT_TEST_PAUSED ) return g_cmt.paused_time * ( 1 / (double) g_mzhal.gdgclk_base );
        uint64_t now_ticks = gdg_get_total_ticks ( );
        uint64_t play_ticks = ( now_ticks - g_cmt.start_time );
        double play_time = play_ticks * ( 1 / (double) g_mzhal.gdgclk_base );
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
    int cmt_change_speed ( en_CMTSPEED cmtspeed );

    void cmt_screen_done_period ( void );
    int cmt_read_data ( void );
    void cmt_update_output ( void );
    void cmt_write_data ( int value );

    void cmt_cpu_boost_set ( en_CMT_CPU_BOOST cpu_boost );
    void cmt_mzfsize_check_set ( en_CMT_MZFSIZE_CHECK mzfsize_check );

#ifdef __cplusplus
}
#endif

#endif /* CMT_H */

