/* 
 * File:   cmtext_container.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. května 2018, 13:22
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
 * @file cmtext_container.h
 * @brief Kontejner pásky Virtual CMT: cesta, typ a index bloků.
 */

#ifndef CMTEXT_CONTAINER_H
#define CMTEXT_CONTAINER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mztape/mztape.h"

#include "cmtext_block_defs.h"


    /**
     * @brief Typ kontejneru pásky.
     */
    typedef enum en_CMTEXT_CONTAINER_TYPE {
        CMTEXT_CONTAINER_TYPE_SINGLE = 0,  /**< jeden blok bez indexu (tape == NULL); dnes ho žádné rozšíření nevytváří (dřív nahrávání do WAV) */
        CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE, /**< páska s indexem bloků; každá přehrávaná páska (rozšíření cmt_tmz), i jediné MZF nebo WAV */
    } en_CMTEXT_CONTAINER_TYPE;


    /**
     * @brief Položka indexu pásky pro MZF blok.
     *
     * cmtspeed platí jen pro blok s CMTEXT_BLOCK_SPEED_SET; pulses jen
     * pro cmtspeed == CMTSPEED_CUSTOM (např. z hlavičky CMTSPEED UniCMT).
     */
    typedef struct st_CMTEXT_TAPE_ITEM_MZF {
        char *fname;
        uint8_t ftype;
        uint16_t fsize;
        uint16_t fexec;
        uint16_t fstrt;
        en_CMTSPEED cmtspeed;
        st_MZTAPE_PULSES_LENGTH pulses; /**< vlastní délky pulzů (jen CMTSPEED_CUSTOM) */
    } st_CMTEXT_TAPE_ITEM_MZF;


    typedef struct st_CMTEXT_TAPE_ITEM_TAPHDR {
        uint16_t size; // skutecna delka bloku (flag + data + checksum)
        char *fname;
        uint8_t code;
        uint16_t data_size;
        uint16_t param1;
        uint16_t param2;
        en_CMTSPEED cmtspeed;
    } st_CMTEXT_TAPE_ITEM_TAPHDR;


    typedef struct st_CMTEXT_TAPE_ITEM_TAPDATA {
        uint16_t size; // skutecna delka bloku (flag + data + checksum)
        en_CMTSPEED cmtspeed;
    } st_CMTEXT_TAPE_ITEM_TAPDATA;


    /**
     * @brief Položka indexu pásky pro blok typu CMTEXT_BLOCK_TYPE_TZX nebo CMTEXT_BLOCK_TYPE_CONTROL.
     *
     * Vlastnictví: name je statický řetězec (tmz_block_id_name()), index
     * ho neuvolňuje.
     */
    typedef struct st_CMTEXT_TAPE_ITEM_TZX {
        const char *name; /**< jméno typu bloku pro UI a MCP (statický řetězec, nikdy NULL) */
        uint8_t id;       /**< ID bloku TZX/TMZ */
    } st_CMTEXT_TAPE_ITEM_TZX;


    /**
     * @brief Údaje položky indexu podle typu bloku (st_CMTEXT_TAPE_INDEX::bltype).
     */
    typedef union un_CMTEXT_TAPE_ITEM {
        st_CMTEXT_TAPE_ITEM_MZF mzf;         /**< CMTEXT_BLOCK_TYPE_MZF */
        st_CMTEXT_TAPE_ITEM_TAPHDR taphdr;   /**< CMTEXT_BLOCK_TYPE_TAPHEADER */
        st_CMTEXT_TAPE_ITEM_TAPDATA tapdata; /**< CMTEXT_BLOCK_TYPE_TAPDATA */
        st_CMTEXT_TAPE_ITEM_TZX tzx;         /**< CMTEXT_BLOCK_TYPE_TZX, CMTEXT_BLOCK_TYPE_CONTROL */
    } un_CMTEXT_TAPE_ITEM;


    /**
     * @brief Položka indexu pásky (jeden blok).
     *
     * U pásky rozšíření cmt_tmz je block_id index bloku v st_TZX_FILE
     * (včetně řídicích bloků) a pause_after je vždy 0 (pauzu nese stream
     * bloku).
     *
     * @invariant item odpovídá bltype; fname položek MZF a TAPHEADER vlastní
     *            index (cmtext_container_tapeindex_destroy()).
     */
    typedef struct st_CMTEXT_TAPE_INDEX {
        int block_id;                  /**< číslo bloku na pásce */
        uint32_t offset;               /**< poloha bloku ve zdroji (u cmt_tmz index v st_TZX_FILE) */
        en_CMTEXT_BLOCK_TYPE bltype;   /**< typ bloku */
        en_CMTEXT_BLOCK_SPEED blspeed; /**< režim rychlosti bloku */
        un_CMTEXT_TAPE_ITEM item;      /**< údaje podle typu */
        uint16_t pause_after;          /**< pauza za blokem v ms, kterou přidává přehrávání (u cmt_tmz 0) */
    } st_CMTEXT_TAPE_INDEX;


    typedef struct st_CMTEXT_CONTAINER_TAPE {
        st_HANDLER *h;
        st_CMTEXT_TAPE_INDEX *index;
    } st_CMTEXT_CONTAINER_TAPE;


    typedef int ( *cmtext_container_cb_next_block ) (void);
    typedef int ( *cmtext_container_cb_previous_block ) (void);
    typedef int ( *cmtext_container_cb_open_block ) (int block_id);


    typedef struct st_CMTEXT_CONTAINER {
        char *filepath;
        char *name; // basename filepath
        en_CMTEXT_CONTAINER_TYPE type;
        int count_blocks;
        st_CMTEXT_CONTAINER_TAPE *tape;
        cmtext_container_cb_next_block cb_next_block;
        cmtext_container_cb_previous_block cb_previous_block;
        cmtext_container_cb_open_block cb_open_block;
    } st_CMTEXT_CONTAINER;


    extern void cmtext_container_tapeindex_destroy ( st_CMTEXT_TAPE_INDEX *index, int count_blocks );

    extern st_CMTEXT_CONTAINER_TAPE* cmtext_container_tape_new ( st_HANDLER *h, st_CMTEXT_TAPE_INDEX *index );
    extern void cmtext_container_tape_destroy ( st_CMTEXT_CONTAINER_TAPE *tape, int count_blocks );

    extern st_CMTEXT_TAPE_INDEX* cmtext_container_tape_index_aloc ( st_CMTEXT_TAPE_INDEX *index, int count_items );

    extern st_CMTEXT_CONTAINER* cmtext_container_new (
                                                       en_CMTEXT_CONTAINER_TYPE type,
                                                       char *filepath,
                                                       int count_blocks,
                                                       st_CMTEXT_CONTAINER_TAPE *tape,
                                                       cmtext_container_cb_next_block cb_next_block,
                                                       cmtext_container_cb_previous_block cb_previous_block,
                                                       cmtext_container_cb_open_block cb_open_block
                                                       );

    extern void cmtext_container_destroy ( st_CMTEXT_CONTAINER *container );

    extern const char* cmtext_container_get_name ( st_CMTEXT_CONTAINER *container );
    extern const char* cmtext_container_get_filepath ( st_CMTEXT_CONTAINER *container );

    /**
     * @brief Změní cestu a jméno kontejneru (pásku uloženou pod jiným jménem).
     *
     * @param container Kontejner. @param filepath Nová cesta (UTF-8, kopíruje se);
     *        jméno je její basename.
     * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace (kontejner beze změny).
     * @pre Volající drží device_state_lock(), čte-li kontejner UI.
     */
    extern int cmtext_container_set_filepath ( st_CMTEXT_CONTAINER *container, const char *filepath );
    extern en_CMTEXT_CONTAINER_TYPE cmtext_container_get_type ( st_CMTEXT_CONTAINER *container );
    extern int cmtext_container_get_count_blocks ( st_CMTEXT_CONTAINER *container );

    extern en_CMTEXT_BLOCK_TYPE cmtext_container_get_block_type ( st_CMTEXT_CONTAINER *container, int block_id );
    extern en_CMTEXT_BLOCK_SPEED cmtext_container_get_block_speed ( st_CMTEXT_CONTAINER *container, int block_id );
    extern void cmtext_container_set_block_speed ( st_CMTEXT_CONTAINER *container, int block_id, en_CMTEXT_BLOCK_SPEED blspeed );
    /**
     * @brief Jméno bloku pro UI a MCP.
     *
     * MZF: jméno z hlavičky; TAPHEADER: jméno z hlavičky ZX; TAPDATA: "";
     * WAV: "WAV"; TZX a CONTROL: název typu bloku (tmz_block_id_name()).
     *
     * @param container Kontejner s indexem (SIMPLE_TAPE).
     * @param block_id Index bloku (v rozsahu, kontroluje jen assert).
     * @return Řetězec vlastněný indexem nebo statický; platí do vysunutí pásky.
     */
    extern const char* cmtext_container_get_block_fname ( st_CMTEXT_CONTAINER *container, int block_id );
    extern int cmtext_container_get_block_ftype ( st_CMTEXT_CONTAINER *container, int block_id );
    extern int cmtext_container_get_block_fsize ( st_CMTEXT_CONTAINER *container, int block_id );
    extern int cmtext_container_get_block_fstrt ( st_CMTEXT_CONTAINER *container, int block_id );
    extern int cmtext_container_get_block_fexec ( st_CMTEXT_CONTAINER *container, int block_id );
    extern en_CMTSPEED cmtext_container_get_block_cmt_speed ( st_CMTEXT_CONTAINER *container, int block_id );
    extern void cmtext_container_set_block_cmt_speed ( st_CMTEXT_CONTAINER *container, int block_id, en_CMTSPEED cmtspeed );

    /**
     * @brief Vrátí uložené vlastní délky pulzů MZF bloku.
     *
     * @param container Kontejner s indexem bloků (SIMPLE_TAPE).
     * @param block_id Index bloku (v rozsahu, kontroluje jen assert).
     * @param[out] pulses Délky pulzů.
     * @return EXIT_SUCCESS, pokud je blok MZF s cmtspeed CMTSPEED_CUSTOM;
     *         jinak EXIT_FAILURE a *pulses se nemění.
     */
    extern int cmtext_container_get_block_pulses ( st_CMTEXT_CONTAINER *container, int block_id, st_MZTAPE_PULSES_LENGTH *pulses );

    /**
     * @brief Uloží MZF bloku vlastní délky pulzů (nemění blspeed ani cmtspeed).
     *
     * @param container Kontejner s indexem bloků (SIMPLE_TAPE).
     * @param block_id Index bloku (v rozsahu, kontroluje jen assert).
     * @param pulses Délky pulzů (kopírují se).
     * @return EXIT_SUCCESS pro MZF blok, jinak EXIT_FAILURE (nic se nemění).
     */
    extern int cmtext_container_set_block_pulses ( st_CMTEXT_CONTAINER *container, int block_id, const st_MZTAPE_PULSES_LENGTH *pulses );

#ifdef __cplusplus
}
#endif

#endif /* CMTEXT_CONTAINER_H */

