/* 
 * File:   cmtext_block.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. května 2018, 13:24
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


/**
 * @file cmtext_block.h
 * @brief Blok pásky Virtual CMT: stream signálu a callbacky rozšíření.
 */

#ifndef CMTEXT_BLOCK_H
#define CMTEXT_BLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mzf/mzf.h"
#include "libs/cmt_stream/cmt_stream.h"

#include "cmtext_block_defs.h"


    typedef void ( *cmtext_block_cb_play ) (void *cmtext );
    typedef void ( *cmtext_block_cb_set_polarity ) (void *cmtext, en_CMT_STREAM_POLARITY polarity );
    typedef const char* ( *cmtext_block_cb_get_playname ) (void *cmtext );
    typedef int ( *cmtext_block_cb_set_speed ) (void *cmtext, en_CMTSPEED mztape_speed );
    typedef uint16_t ( *cmtext_block_cb_get_bdspeed ) (void *cmtext );


    /**
     * @brief Otevřený blok pásky: stream signálu a callbacky rozšíření.
     *
     * @invariant start_gdgticks <= délka streamu v taktech GDG.
     * @par Vlastnictví: stream vlastní blok (cmtext_block_destroy()),
     *      spec vlastní rozšíření, které blok vytvořilo.
     */
    typedef struct st_CMTEXT_BLOCK {
        int block_id;
        en_CMTEXT_BLOCK_TYPE type;
        st_CMT_STREAM *stream;
        en_CMTEXT_BLOCK_SPEED block_speed;
        uint16_t pause_after; // Delka pauzy po prehrani bloku (ms)
        /** Pozice ve STOP: takty GDGCLK_BASE od začátku streamu, odkud začne
         *  příští PLAY (cmt_tape_set_position()); nový blok 0, cmt_play() ji
         *  spotřebuje a vynuluje. */
        uint64_t start_gdgticks;
        void *spec;
        cmtext_block_cb_play cb_play;
        cmtext_block_cb_get_playname cb_get_playname;
        cmtext_block_cb_set_polarity cb_set_polarity;
        cmtext_block_cb_set_speed cb_set_speed;
        cmtext_block_cb_get_bdspeed cb_get_bdspeed;
    } st_CMTEXT_BLOCK;

    extern st_CMTEXT_BLOCK* cmtext_block_new ( int block_id,
                                               en_CMTEXT_BLOCK_TYPE type,
                                               st_CMT_STREAM *stream,
                                               en_CMTEXT_BLOCK_SPEED block_speed,
                                               uint16_t pause_after,
                                               void *spec );

    /**
     * @brief Uvolní blok včetně jeho streamu.
     *
     * @param block Blok k uvolnění; NULL je přípustné (nic se nestane).
     *
     * @pre block->spec == NULL - blockspec musí dřív uvolnit a vynulovat
     *      rozšíření, které jej vytvořilo (např. cmttmz_block_close()
     *      v cmt_tmz.c); kontroluje se přes assert.
     * @post Stream bloku (pokud existuje) je uvolněn cmt_stream_destroy()
     *       a blok je uvolněn; ukazatel block je po návratu neplatný.
     */
    extern void cmtext_block_destroy ( st_CMTEXT_BLOCK *block );
    extern void cmtext_block_play ( void *cmtext );
    extern const char* cmtext_block_get_playname ( void *cmtext );
    extern void cmtext_block_set_polarity ( void *cmtext, en_CMT_STREAM_POLARITY polarity );
    extern en_CMTEXT_BLOCK_TYPE cmtext_block_get_type ( st_CMTEXT_BLOCK *block );
    extern uint32_t cmtext_block_get_rate ( st_CMTEXT_BLOCK *block );

    /*
     * Následující gettery přijímají i block == NULL: vložená páska nemusí mít
     * otevřený blok (páska rozšíření cmt_tmz jen s bloky bez signálu), UI
     * a MCP se pak ptají bez zvláštní kontroly.
     */

    /**
     * @brief Velikost dat streamu bloku v bajtech.
     * @param block Blok (NULL = 0).
     * @return Velikost; 0 bez bloku nebo bez streamu.
     */
    extern uint32_t cmtext_block_get_size ( st_CMTEXT_BLOCK *block );

    /**
     * @brief Pauza za blokem v ms, kterou přidává přehrávání (cmtext_block_get_output()).
     * @param block Blok (NULL = 0).
     * @return Pauza v ms; 0 bez bloku.
     */
    extern uint16_t cmtext_block_get_pause_after ( st_CMTEXT_BLOCK *block );
    extern st_CMT_STREAM* cmtext_block_get_stream ( st_CMTEXT_BLOCK *block );
    extern en_CMT_STREAM_TYPE cmtext_block_get_stream_type ( st_CMTEXT_BLOCK *block );

    /**
     * @brief Číslo bloku na pásce.
     * @param block Blok (NULL = -1).
     * @return Číslo bloku; -1 bez bloku.
     */
    extern int cmtext_block_get_block_id ( st_CMTEXT_BLOCK *block );

    /**
     * @brief Počet vzorků streamu bloku.
     * @param block Blok (NULL = 0).
     * @return Počet vzorků; 0 bez bloku nebo bez streamu.
     */
    extern uint64_t cmtext_block_get_count_scans ( st_CMTEXT_BLOCK *block );

    /**
     * @brief Délka jednoho vzorku streamu bloku v sekundách.
     * @param block Blok (NULL = 0).
     * @return Délka vzorku; 0 bez bloku nebo bez streamu.
     */
    extern double cmtext_block_get_scantime ( st_CMTEXT_BLOCK *block );

    /**
     * @brief Režim rychlosti otevřeného bloku.
     * @param block Blok (NULL = CMTEXT_BLOCK_SPEED_NONE).
     * @return Režim rychlosti; CMTEXT_BLOCK_SPEED_NONE bez bloku.
     */
    extern en_CMTEXT_BLOCK_SPEED cmtext_block_get_block_speed ( st_CMTEXT_BLOCK *block );


    typedef enum en_CMTEXT_BLOCK_PLAYSTS {
        CMTEXT_BLOCK_PLAYSTS_BODY = 0,
        CMTEXT_BLOCK_PLAYSTS_PAUSE,
        CMTEXT_BLOCK_PLAYSTS_STOP
    } en_CMTEXT_BLOCK_PLAYSTS;

    extern en_CMTEXT_BLOCK_PLAYSTS cmtext_block_get_output ( st_CMTEXT_BLOCK *block, uint64_t play_gdgticks, int *output, uint64_t *transferred_gdgticks );

#ifdef __cplusplus
}
#endif

#endif /* CMTEXT_BLOCK_H */

