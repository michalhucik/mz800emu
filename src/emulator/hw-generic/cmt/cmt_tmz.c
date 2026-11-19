/**
 * @file   cmt_tmz.c
 * @brief  Rozšíření Virtual CMT: každá páska jako TMZ v paměti přehrávaná přes tmz_player.
 *
 * Vlastnictví (vše drží g_cmttmz a g_cmt_tmz_extension_def, vlákno emulátoru):
 * - st_TZX_FILE vložené pásky (g_cmttmz.file): vzniká importem při
 *   otevření, uvolní se při vysunutí nebo otevření jiné pásky,
 * - pole rychlostí bloků z importu (g_cmttmz.speeds, viz
 *   st_CMTTMZ_BLOCK_SPEED) a příznaky přeskočení bloků (g_cmttmz.skip):
 *   obě se stejným počtem položek jako bloků souboru,
 * - údaje bloků pro UI (g_cmttmz.info, st_CMTTMZ_BLOCK_INFO: délka,
 *   "vznikl záznamem", CSW, jméno v Sharp MZ ASCII) se stejným počtem
 *   položek; UI je čte jen přes cmttmz_get_block_ui(),
 * - příznaky neuloženo bloků (g_cmttmz.unsaved, cmttmz_block_is_unsaved())
 *   se stejným počtem položek,
 * - stav playeru (g_cmttmz.player): odkazuje na file; snapshot ho čte
 *   a po ověření nastavuje přes cmttmz_get_player_state() /
 *   cmttmz_set_player_state() (snap_cmt.c),
 * - kontejner s indexem (g_cmt_tmz_extension_def.container) a otevřený
 *   blok (g_cmt_tmz_extension_def.block, stream vlastní blok, blockspec
 *   st_CMTTMZ_BLOCKSPEC vlastní rozšíření a uvolní ho cmttmz_block_close()).
 *
 * Index kontejneru (a vedlejší pole) se mění jen v úpravách pásky
 * (cmttmz_edit_apply(): cmttmz_append_blocks() - připojení bloků záznamu,
 * cmttmz_insert_blocks(), cmttmz_remove_blocks(), cmttmz_move_block()),
 * při návratu na značku (cmttmz_tape_mark_restore()) a v cmttmz_new_tape(),
 * vždy ve vlákně emulátoru pod
 * device_state_lock(); UI vlákno index čte pod týmž zámkem a ukazatele do
 * něj drží jen po dobu zámku (okno Virtual CMT, seznam bloků, viz
 * main_window.cpp). Emu vlákno ale otevírá další blok (cmt_update_output()
 * -> cmttmz_advance()) bez tohoto zámku, proto se blok, který player
 * odmítne, neoznačuje v indexu, ale příznakem v g_cmttmz.skip.
 *
 * Dva způsoby, jak se blok otevře:
 * - Přímé otevření (cmttmz_block_open(): cb_open_block, cb_next_block,
 *   cb_previous_block, tedy uživatel, MCP, záznam vstupů, snapshot;
 *   cmttmz_set_speed()): player se přestaví na blok (current_block =
 *   číslo bloku, prázdné zásobníky smyček a volání, výchozí úroveň signálu
 *   TZX_LEVEL_AFTER_PAUSE). Stream bloku je funkcí jen čísla bloku a jeho
 *   rychlosti, opakované přímé otevření dá stejný signál.
 * - Postupné přehrávání (cmttmz_advance(), volá ho cmt.c na konci streamu
 *   bloku; cmttmz_open_from_start() při otevření pásky a převinutí):
 *   tmz_player_play_next() na stejném stavu playeru, takže úroveň signálu
 *   mezi bloky, smyčky, skoky, volání a zastavení pásky určuje player;
 *   číslo otevřeného bloku je state.last_played_block.
 *
 * Rychlost bloku (pravidlo na jednom místě: cmttmz_block_speed_kind(),
 * navenek cmttmz_block_speed_changeable()): měnit jde bloky 0x46
 * MZ800EMU-CMT z importu MZF a MZT nebo z TMZ s čitelnými PROFILE_ARGS
 * (poměr, výchozí rychlost, vlastní pulzy) a bloky 0x11 z importu TAP
 * (poměr, časování zxtape vydělené poměrem). Režim DEFAULT / SET drží
 * index (blspeed), rychlost, kterou je blok v souboru právě sestaven,
 * vedlejší pole g_cmttmz.speeds (nic z toho není v souboru). Změna
 * výchozí rychlosti ve STOP přestaví hned všechny bloky DEFAULT
 * (cmttmz_apply_default_speed()); jinak se blok přestaví při otevření,
 * pokud se požadovaná rychlost (index, výchozí rychlost g_cmt) liší od
 * sestavené. Ostatní bloky mají rychlost ze souboru a nemění se.
 *
 * Výpisy do logu jsou anglicky (stdout informace, stderr varování).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#ifdef MZTEST_HEADLESS
#include <malloc.h>     /* _msize() / malloc_usable_size() pro cmttmz_test_get_side_alloc() */
#endif

#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_player.h"
#include "libs/tmz/tmz_mzt.h"
#include "libs/mzcmt_fm/mzcmt_fm.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/mztape/mztape.h"
#include "libs/zxtape/zxtape.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "hw-generic/gdg/gdgclk.h"
#include "baseui/baseui.h"

#include "cmt.h"
#include "cmtext.h"
#include "cmt_tmz.h"
#include "cmt_tmz_import.h"
#include "cmthack.h"


/** @brief Přípony souborů, které rozšíření otevírá (stejná sada jako cmttmz_source_from_filename()). */
static char *g_cmt_tmz_fileext[] = {
                                    "tmz",
                                    "tzx",
                                    "mzf",
                                    "m12",
                                    "mzt",
                                    "tap",
                                    "wav",
                                    "wave",
                                    NULL
};

/** @brief Popis rozšíření v registru (jméno "TMZ", jen přehrávání). */
static st_CMTEXT_INFO g_cmt_tmz_info = {
                                        "TMZ",
                                        g_cmt_tmz_fileext,
                                        "TMZ cmt extension",
                                        CMTEXT_TYPE_PLAYABLE
};


/**
 * @brief Údaje bloku pro UI, které index kontejneru nenese (vedlejší pole g_cmttmz.info).
 *
 * Vyplňuje je cmttmz_info_fill() při vložení bloku (otevření pásky,
 * připojení záznamu). Délku zapisuje stream otevřeného bloku
 * (cmttmz_info_set_stream_length(): otevření bloku, změna rychlosti
 * otevřeného bloku), levný výpočet z hlavičky nebo dat bloku
 * (cmttmz_length_cheap(): připojení záznamu, návrat na značku, zásah UI)
 * a výpočet generováním streamu jen v zásahu UI
 * (cmttmz_lengths_update_budget()). Délka nikdy neovlivní emulaci,
 * snapshot, otisk stroje ani záznam vstupů.
 *
 * @invariant length_state == CMTTMZ_LENGTH_KNOWN => length_s je délka
 *            streamu bloku sestaveného rychlostí length_speed (u bloku
 *            s měnitelnou rychlostí; jinak length_speed nemá význam).
 * @invariant unplayable == 1 po cmttmz_info_fill() <=> druh nebo data
 *            bloku player odmítne (cmttmz_block_known_unplayable()).
 */
typedef struct st_CMTTMZ_BLOCK_INFO {
    en_CMTTMZ_LENGTH_STATE length_state;   /**< stav délky */
    double length_s;                       /**< délka streamu bloku v s (jen CMTTMZ_LENGTH_KNOWN) */
    en_CMTSPEED length_speed;              /**< rychlost, pro kterou délka platí (bloky s měnitelnou rychlostí) */
    st_MZTAPE_PULSES_LENGTH length_pulses; /**< pulzy k length_speed CMTSPEED_CUSTOM, jinak nulové */
    uint8_t unplayable;                    /**< 1 = player blok přehrát neumí (druh bloku nebo chyba při výpočtu délky) */
    uint8_t recorded;                      /**< 1 = blok připojil záznam (cmttmz_append_blocks()) */
    uint8_t imported;                      /**< 1 = blok vložil import souboru (cmttmz_import_file_at()); ne bloky analýzy, informací o pásce a převodu profilu; přesun ho nese, otevření pásky a snapshot ho nenastaví */
    uint32_t csw_rate;                     /**< blok 0x18: vzorkovací frekvence v Hz, jinak 0 */
    uint32_t csw_pulses;                   /**< blok 0x18: počet pulzů, jinak 0 */
    uint8_t mzf_name[MZF_FILE_NAME_LENGTH]; /**< jméno MZF v kódech Sharp MZ ASCII (bez terminátoru) */
    uint8_t mzf_name_len;                  /**< počet platných bajtů mzf_name */
} st_CMTTMZ_BLOCK_INFO;


/**
 * @brief Stav vložené pásky rozšíření.
 *
 * @invariant file == NULL <=> speeds == NULL <=> skip == NULL <=> info ==
 *            NULL <=> unsaved == NULL <=> bez vložené pásky
 *            (g_cmt_tmz_extension_def.container == NULL).
 * @invariant speeds, skip, info a unsaved mají aspoň file->block_count
 *            položek ve stejném pořadí jako bloky souboru; speeds[i].speed
 *            a pulses popisují rychlost, se kterou je blok i v souboru právě
 *            sestaven (přestavění je aktualizuje).
 * @invariant index kontejneru: count_blocks == file->block_count, položka i
 *            má block_id == offset == i.
 * @invariant player.file == file, nebo player je vynulovaný.
 * @invariant unsaved[i] == 1 pro některé i => modified.
 */
typedef struct st_CMTTMZ {
    st_TZX_FILE *file;              /**< vložená páska (vlastní rozšíření) */
    st_CMTTMZ_BLOCK_SPEED *speeds;  /**< rychlost bloků z importu (vlastní rozšíření, free()) */
    uint8_t *skip;                  /**< 1 = blok player odmítl (UNSUPPORTED, UNREADABLE) nebo nedal signál, next/previous/open ho přeskakují (vlastní rozšíření, free()) */
    st_CMTTMZ_BLOCK_INFO *info;     /**< údaje bloků pro UI (délka, záznam, CSW, jméno; vlastní rozšíření, free()) */
    uint8_t *unsaved;               /**< 1 = blok není v souboru pásky (připojil ho záznam, vložila úprava, páska bez souboru); 0 po otevření a uložení (vlastní rozšíření, free()) */
    char edit_error[CMTTMZ_EDIT_ERROR_SIZE]; /**< text chyby poslední úpravy pásky (anglicky, "" po úspěchu) */
    char edit_warning[CMTTMZ_WARNING_SIZE]; /**< varování poslední úpravy pásky a nástroje nad páskou (cmttmz_get_edit_warning()): změna účinného taktu CPU bloku 0x46 po mazání a přesunu, informace o pásce, převod profilu; anglicky, "" = žádné */
    char import_warning[CMTTMZ_WARNING_SIZE]; /**< varování posledního cmttmz_import_file_at() (import, nepřehratelné bloky, změna taktu CPU u bloků 0x46; "" = žádné) */
    en_CMTTMZ_SOURCE source;        /**< zdroj importu vložené pásky (CMTTMZ_SOURCE_TMZ u nové pásky a obrazu) */
    unsigned change_serial;         /**< čítač změn údajů bloků pro UI (cmttmz_get_change_serial()), jen roste */
    int open_block_id;              /**< číslo otevřeného bloku (-1 bez bloku); zapisuje se s každou výměnou bloku */
    st_TMZ_PLAYER_STATE player;     /**< stav playeru posledního otevření bloku */
    char warning[CMTTMZ_WARNING_SIZE]; /**< varování naposledy otevřené pásky (import, bloky, které player neumí, žádný otevřený blok) */
    int quiet;                      /**< 1 = otevření bloku bez informačního výpisu (cmttmz_seek_block()) */
    unsigned open_serial;           /**< čítač výměn otevřeného bloku (cmttmz_block_replace(), otevření a vysunutí pásky, připojení bloků, každá úprava pásky), jen roste; viz cmttmz_get_open_serial() */
    unsigned tape_serial;           /**< čítač výměn pásky (otevření, vysunutí, nová páska, zkrácení návratem na značku, každá úprava pásky kromě připojení záznamu, uplatnění analýzy a nástroje nad páskou), jen roste; viz cmttmz_get_tape_serial() */
    int modified;                   /**< 1 = páska změněná proti souboru (připojené bloky), 0 po otevření, vysunutí, nové pásce a uložení */
    char record_message[CMTTMZ_RECORD_MESSAGE_SIZE]; /**< výsledek posledního záznamu pro log, UI a MCP (anglicky) */
    uint32_t record_mzf;            /**< bloky 0x46 posledního záznamu připojené k pásce */
    uint32_t record_raw;            /**< bloky 0x18 posledního záznamu připojené k pásce */
    unsigned record_serial;         /**< čítač dokončených záznamů (cmttmz_set_record_result()), jen roste */
    int record_discarded;           /**< 1 = poslední záznam se zahodil (cmttmz_set_record_discarded()), nic se nepřipojilo */
    char prepare_error[256];        /**< důvod posledního neúspěšného cmttmz_open_prepare() ("" = žádný) */
    char save_message[CMTTMZ_SAVE_MESSAGE_SIZE]; /**< výsledek posledního uložení pásky nebo exportu bloku (anglicky) */
    char save_warning[CMTTMZ_SAVE_MESSAGE_SIZE]; /**< varování posledního úspěšného uložení (anglicky, "; " mezi varováními), jinak prázdné */
    char export_message[CMTTMZ_SAVE_MESSAGE_SIZE]; /**< výsledek posledního exportu celé pásky (cmttmz_export_tape_wav(), anglicky) */
    char export_warning[CMTTMZ_SAVE_MESSAGE_SIZE]; /**< varování posledního úspěšného exportu celé pásky, jinak prázdné */
} st_CMTTMZ;

/** @brief Stav vložené pásky (jen vlákno emulátoru). */
static st_CMTTMZ g_cmttmz;


/**
 * @brief Data otevřeného bloku (st_CMTEXT_BLOCK::spec).
 *
 * @invariant cmtspeed a pulses popisují rychlost streamu bloku; pulses
 *            platí jen při CMTSPEED_CUSTOM.
 * @par Vlastnictví: alokuje cmttmz_block_open(), uvolní cmttmz_block_close().
 */
typedef struct st_CMTTMZ_BLOCKSPEC {
    en_CMTSPEED cmtspeed;           /**< rychlost streamu (poměr, CMTSPEED_CUSTOM; CMTSPEED_NONE = blok bez řízené rychlosti) */
    st_MZTAPE_PULSES_LENGTH pulses; /**< vlastní délky pulzů (jen CMTSPEED_CUSTOM) */
} st_CMTTMZ_BLOCKSPEC;


st_CMTEXT *g_cmt_tmz_extension = &g_cmt_tmz_extension_def;


/* ------------------------------------------------------------------------- */
/*  Pomocné funkce souboru a indexu                                          */
/* ------------------------------------------------------------------------- */


/**
 * @brief Přečte 32bitové číslo little endian.
 * @param p Ukazatel na 4 bajty.
 * @return Hodnota.
 */
static uint32_t cmttmz_rd32 ( const uint8_t *p ) {
    return ( uint32_t ) p[0] | ( ( uint32_t ) p[1] << 8 ) | ( ( uint32_t ) p[2] << 16 ) | ( ( uint32_t ) p[3] << 24 );
}


/**
 * @brief Takt CPU z posledního bloku 0x43 (MZ Machine Info) před blokem.
 *
 * Čte stejné místo jako stavový automat playeru (tmz_player.c, větev
 * TMZ_BLOCK_ID_MZ_MACHINE_INFO v tmz_player_play_next(): data + 4 + 1,
 * 32 bitů LE, při délce aspoň 9 B). tmz_block_parse_mz_machine_info()
 * se nepoužívá, protože mění data bloku (konverze endianity).
 *
 * @param block_id Index bloku v g_cmttmz.file.
 * @return Takt v Hz, nebo 0 (výchozí takt playeru), když před blokem žádný
 *         platný blok 0x43 není.
 */
static uint32_t cmttmz_cpu_clock_before ( uint32_t block_id ) {
    uint32_t clock = 0;
    for ( uint32_t i = 0; i < block_id && i < g_cmttmz.file->block_count; i++ ) {
        const st_TZX_BLOCK *b = &g_cmttmz.file->blocks[i];
        if ( b->id == TMZ_BLOCK_ID_MZ_MACHINE_INFO && b->data && b->length >= 4 + 1 + 4 ) {
            clock = cmttmz_rd32 ( b->data + 4 + 1 );
        }
    }
    return clock;
}


/**
 * @brief Zařadí blok do položky indexu (typ, jméno, rychlost) bez čtení stavu pásky.
 *
 * Typy: 0x40, 0x41, 0x46 s čitelným MZF = MZF; 0x10, 0x11 s 19 B dat
 * a flagem 00h = TAPHEADER, jiné 0x10, 0x11 = TAPDATA (0x11 delší než
 * 65535 B = TZX); 0x18 z importu WAV nebo s příznakem původu WAV
 * (@p wav_origin, obraz pásky ze snapshotu nebo ze záznamu vstupů) = WAV;
 * ostatní bloky se signálem (0x12-0x15, 0x18, 0x20 s nenulovou pauzou,
 * 0x45 a 0x40, 0x41, 0x46 bez čitelného MZF) = TZX; zbytek (řídicí
 * a informační bloky, Pause 0, nepodporované bloky) = CONTROL.
 *
 * Rychlost podle původu z importu: DEFAULT -> CMTEXT_BLOCK_SPEED_DEFAULT,
 * FIXED -> CMTEXT_BLOCK_SPEED_SET, NONE -> CMTEXT_BLOCK_SPEED_NONE.
 *
 * Blok ani záznam rychlosti nemusí být ještě na pásce (vložení bloků
 * připraví položky dřív, než pásku změní, cmttmz_edit_apply()).
 *
 * @param b Blok. @param sp Záznam rychlosti bloku.
 * @param i Číslo bloku na pásce (block_id a offset položky).
 * @param source Zdroj importu.
 * @param wav_origin 1 = blok 0x18 pochází z importu WAV (typ WAV i při
 *        @p source jiném než CMTTMZ_SOURCE_WAV); u jiných bloků se nepoužije.
 * @param[out] idx Položka indexu (přepíše se celá).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace jména (položka
 *         pak nevlastní žádnou paměť).
 */
static int cmttmz_index_fill_block ( const st_TZX_BLOCK *b, const st_CMTTMZ_BLOCK_SPEED *sp, uint32_t i,
                                     en_CMTTMZ_SOURCE source, int wav_origin, st_CMTEXT_TAPE_INDEX *idx ) {
    memset ( idx, 0, sizeof ( *idx ) );
    idx->block_id = ( int ) i;
    idx->offset = i;
    idx->pause_after = 0; /* pauzu nese stream z playeru */
    idx->bltype = CMTEXT_BLOCK_TYPE_CONTROL;
    switch ( sp->origin ) {
        case CMTTMZ_SPEED_ORIGIN_DEFAULT:
            idx->blspeed = CMTEXT_BLOCK_SPEED_DEFAULT;
            break;
        case CMTTMZ_SPEED_ORIGIN_FIXED:
            idx->blspeed = CMTEXT_BLOCK_SPEED_SET;
            break;
        case CMTTMZ_SPEED_ORIGIN_NONE:
        default:
            idx->blspeed = CMTEXT_BLOCK_SPEED_NONE;
            break;
    }

    switch ( b->id ) {
        case TMZ_BLOCK_ID_MZ_STANDARD_DATA:
        case TMZ_BLOCK_ID_MZ_TURBO_DATA:
        case TMZ_BLOCK_ID_MZ_FILE:
        {
            st_MZF *mzf = tmz_block_to_mzf ( b, NULL );
            if ( !mzf ) {
                idx->bltype = CMTEXT_BLOCK_TYPE_TZX;
                break;
            }
            char ascii_filename[MZF_FNAME_FULL_LENGTH];
            mzf_tools_get_fname ( &mzf->header, ascii_filename );
            st_CMTEXT_TAPE_ITEM_MZF *it = &idx->item.mzf;
            it->ftype = mzf->header.ftype;
            it->fsize = mzf->header.fsize;
            it->fstrt = mzf->header.fstrt;
            it->fexec = mzf->header.fexec;
            it->cmtspeed = sp->speed;
            if ( cmtspeed_is_custom ( sp->speed ) ) it->pulses = sp->pulses;
            mzf_free ( mzf );
            it->fname = ( char* ) baseui_tools_mem_alloc0 ( strlen ( ascii_filename ) + 1 );
            if ( !it->fname ) {
                memset ( &idx->item, 0, sizeof ( idx->item ) );
                return EXIT_FAILURE;
            }
            strcpy ( it->fname, ascii_filename );
            idx->bltype = CMTEXT_BLOCK_TYPE_MZF;
            return EXIT_SUCCESS;
        }

        case TZX_BLOCK_ID_STANDARD_SPEED:
        case TZX_BLOCK_ID_TURBO_SPEED:
        {
            const uint8_t *data = NULL;
            uint32_t len = 0;
            if ( b->id == TZX_BLOCK_ID_STANDARD_SPEED ) {
                st_TZX_STANDARD_SPEED ss;
                if ( TZX_OK == tzx_parse_standard_speed ( b->data, b->length, &ss ) ) {
                    data = ss.data;
                    len = ss.data_length;
                }
            } else {
                st_TZX_TURBO_SPEED ts;
                if ( TZX_OK == tzx_parse_turbo_speed ( b->data, b->length, &ts ) ) {
                    data = ts.data;
                    len = ts.data_length;
                }
            }
            if ( !data || len == 0 || len > 0xFFFF ) {
                idx->bltype = CMTEXT_BLOCK_TYPE_TZX;
                break;
            }
            en_CMTSPEED cmtspeed = sp->speed;
            if ( len == 19 && data[0] == 0x00 ) {
                /* hlavička ZX: flag, kód, jméno 10 B, délka, param1, param2, checksum */
                st_CMTEXT_TAPE_ITEM_TAPHDR *it = &idx->item.taphdr;
                it->size = ( uint16_t ) len;
                it->code = data[1];
                it->data_size = ( uint16_t ) ( data[12] | ( data[13] << 8 ) );
                it->param1 = ( uint16_t ) ( data[14] | ( data[15] << 8 ) );
                it->param2 = ( uint16_t ) ( data[16] | ( data[17] << 8 ) );
                it->cmtspeed = cmtspeed;
                it->fname = ( char* ) baseui_tools_mem_alloc0 ( 10 + 1 );
                if ( !it->fname ) {
                    memset ( &idx->item, 0, sizeof ( idx->item ) );
                    return EXIT_FAILURE;
                }
                memcpy ( it->fname, &data[2], 10 );
                idx->bltype = CMTEXT_BLOCK_TYPE_TAPHEADER;
            } else {
                idx->item.tapdata.size = ( uint16_t ) len;
                idx->item.tapdata.cmtspeed = cmtspeed;
                idx->bltype = CMTEXT_BLOCK_TYPE_TAPDATA;
            }
            return EXIT_SUCCESS;
        }

        case TZX_BLOCK_ID_CSW_RECORDING:
            idx->bltype = ( source == CMTTMZ_SOURCE_WAV || wav_origin ) ? CMTEXT_BLOCK_TYPE_WAV : CMTEXT_BLOCK_TYPE_TZX;
            break;

        case TZX_BLOCK_ID_PURE_TONE:
        case TZX_BLOCK_ID_PULSE_SEQUENCE:
        case TZX_BLOCK_ID_PURE_DATA:
        case TZX_BLOCK_ID_DIRECT_RECORDING:
        case TMZ_BLOCK_ID_MZ_BASIC_DATA:
            idx->bltype = CMTEXT_BLOCK_TYPE_TZX;
            break;

        case TZX_BLOCK_ID_PAUSE:
            /* Pause 0 = zastavení pásky, signál nemá */
            if ( b->data && b->length >= 2 && ( b->data[0] | ( b->data[1] << 8 ) ) != 0 ) {
                idx->bltype = CMTEXT_BLOCK_TYPE_TZX;
            }
            break;

        default:
            break;
    }

    if ( idx->bltype != CMTEXT_BLOCK_TYPE_WAV ) {
        idx->item.tzx.name = tmz_block_id_name ( b->id );
        idx->item.tzx.id = b->id;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Zařadí blok vložené pásky do indexu (cmttmz_index_fill_block() nad blokem souboru).
 *
 * @param i Index bloku v g_cmttmz.file (i záznam rychlosti g_cmttmz.speeds[i]).
 * @param source Zdroj importu. @param wav_origin 1 = blok 0x18 z importu WAV.
 * @param[out] idx Položka indexu.
 * @return Jako cmttmz_index_fill_block().
 */
static int cmttmz_index_fill ( uint32_t i, en_CMTTMZ_SOURCE source, int wav_origin, st_CMTEXT_TAPE_INDEX *idx ) {
    return cmttmz_index_fill_block ( &g_cmttmz.file->blocks[i], &g_cmttmz.speeds[i], i, source, wav_origin, idx );
}


/**
 * @brief Sestaví index všech bloků vložené pásky.
 *
 * @param source Zdroj importu.
 * @param wav_blocks Příznaky původu WAV bloků (cmttmz_get_wav_blocks()),
 *        NULL = žádný; položky za @p wav_count se berou jako 0.
 * @param wav_count Počet položek @p wav_blocks.
 * @return Nový index (g_cmttmz.file->block_count položek, vlastník je
 *         volající, uvolnit cmtext_container_tapeindex_destroy()), nebo NULL
 *         při selhání alokace.
 */
static st_CMTEXT_TAPE_INDEX* cmttmz_index_new ( en_CMTTMZ_SOURCE source, const uint8_t *wav_blocks, uint32_t wav_count ) {
    uint32_t count = g_cmttmz.file->block_count;
    st_CMTEXT_TAPE_INDEX *index = cmtext_container_tape_index_aloc ( NULL, ( int ) count );
    if ( !index ) return NULL;
    for ( uint32_t i = 0; i < count; i++ ) {
        int wav = ( wav_blocks && i < wav_count && wav_blocks[i] ) ? 1 : 0;
        if ( EXIT_SUCCESS != cmttmz_index_fill ( i, source, wav, &index[i] ) ) {
            fprintf ( stderr, "%s():%d - Can't allocate memory\n", __func__, __LINE__ );
            cmtext_container_tapeindex_destroy ( index, ( int ) i );
            return NULL;
        }
    }
    return index;
}


/**
 * @brief Položka indexu vložené pásky.
 * @param block_id Index bloku (v rozsahu).
 * @return Položka (vlastní kontejner).
 */
static st_CMTEXT_TAPE_INDEX* cmttmz_idx ( int block_id ) {
    return &g_cmt_tmz_extension->container->tape->index[block_id];
}


/**
 * @brief Zjistí, zda se blok při otevírání přeskakuje.
 *
 * Přeskakuje se blok typu CONTROL a blok s příznakem g_cmttmz.skip.
 *
 * @param block_id Index bloku (v rozsahu).
 * @return 1 = přeskočit, 0 = blok se zkusí otevřít.
 */
static int cmttmz_block_skipped ( int block_id ) {
    return ( cmttmz_idx ( block_id )->bltype == CMTEXT_BLOCK_TYPE_CONTROL ) || g_cmttmz.skip[block_id];
}


/* ------------------------------------------------------------------------- */
/*  Rychlost bloků (MZF 0x46 MZ800EMU-CMT, TAP 0x11)                         */
/* ------------------------------------------------------------------------- */


/**
 * @brief Druh bloku z hlediska změny rychlosti.
 */
typedef enum en_CMTTMZ_SPEED_KIND {
    CMTTMZ_SPEED_KIND_NONE = 0, /**< rychlost dává soubor, měnit nejde */
    CMTTMZ_SPEED_KIND_MZF,      /**< blok 0x46 MZ800EMU-CMT (import MZF/MZT, nebo z TMZ s čitelnými PROFILE_ARGS) */
    CMTTMZ_SPEED_KIND_TAP,      /**< blok 0x11 z importu TAP (časování zxtape) */
} en_CMTTMZ_SPEED_KIND;


/**
 * @brief Jediné místo pravidla, zda a jak jde blok přestavět jinou rychlostí.
 *
 * MZF: původ rychlosti z importu (st_CMTTMZ_BLOCK_SPEED::origin) různý od
 * NONE, blok v souboru 0x46 a v indexu typ MZF. TAP: původ různý od NONE,
 * blok 0x11 a v indexu TAPHEADER nebo TAPDATA. Ostatní bloky (TMZ s jinými
 * profily, 0x10-0x18 ze souboru, WAV, CONTROL) mají rychlost ze souboru.
 *
 * @param block_id Index bloku (smí být mimo rozsah).
 * @return Druh; CMTTMZ_SPEED_KIND_NONE i bez vložené pásky a mimo rozsah.
 */
static en_CMTTMZ_SPEED_KIND cmttmz_block_speed_kind ( int block_id ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) return CMTTMZ_SPEED_KIND_NONE;
    if ( block_id < 0 || block_id >= g_cmt_tmz_extension->container->count_blocks ) return CMTTMZ_SPEED_KIND_NONE;
    if ( g_cmttmz.speeds[block_id].origin == CMTTMZ_SPEED_ORIGIN_NONE ) return CMTTMZ_SPEED_KIND_NONE;
    uint8_t id = g_cmttmz.file->blocks[block_id].id;
    en_CMTEXT_BLOCK_TYPE type = cmttmz_idx ( block_id )->bltype;
    if ( id == TMZ_BLOCK_ID_MZ_FILE && type == CMTEXT_BLOCK_TYPE_MZF ) return CMTTMZ_SPEED_KIND_MZF;
    if ( id == TZX_BLOCK_ID_TURBO_SPEED && ( type == CMTEXT_BLOCK_TYPE_TAPHEADER || type == CMTEXT_BLOCK_TYPE_TAPDATA ) ) {
        return CMTTMZ_SPEED_KIND_TAP;
    }
    return CMTTMZ_SPEED_KIND_NONE;
}


int cmttmz_block_speed_changeable ( int block_id, en_CMTSPEED cmtspeed ) {
    switch ( cmttmz_block_speed_kind ( block_id ) ) {
        case CMTTMZ_SPEED_KIND_MZF:
            /* CMTSPEED_NONE = výchozí rychlost Virtual CMT (cmt_tape_change_block_speed()) */
            return ( cmtspeed == CMTSPEED_NONE ) || cmtspeed_is_valid ( cmtspeed ) || cmtspeed_is_custom ( cmtspeed );
        case CMTTMZ_SPEED_KIND_TAP:
            return cmtspeed_is_valid ( cmtspeed ) ? 1 : 0;
        case CMTTMZ_SPEED_KIND_NONE:
        default:
            return 0;
    }
}


int cmttmz_get_block_speed_record ( int block_id, st_CMTTMZ_BLOCK_SPEED *out ) {
    if ( !out || !g_cmttmz.file || block_id < 0 || ( uint32_t ) block_id >= g_cmttmz.file->block_count ) return EXIT_FAILURE;
    *out = g_cmttmz.speeds[block_id];
    return EXIT_SUCCESS;
}


/**
 * @brief Zjistí, zda blok smí rozšíření přestavět jinou rychlostí (cmttmz_block_speed_kind() != NONE).
 * @param block_id Index bloku.
 * @return 1 = přestavitelný, 0 = ne.
 */
static int cmttmz_block_speed_rebuildable ( int block_id ) {
    return cmttmz_block_speed_kind ( block_id ) != CMTTMZ_SPEED_KIND_NONE;
}


/**
 * @brief Rychlost, kterou má blok hrát podle indexu a výchozí rychlosti Virtual CMT.
 *
 * MZF: CMTEXT_BLOCK_SPEED_SET = rychlost a pulzy z indexu, jinak
 * g_cmt.mz_cmtspeed (a g_cmt.mz_custom_pulses). TAP: rychlost z indexu
 * (CMTSPEED_NONE = 1:1).
 *
 * @param block_id Index přestavitelného bloku (cmttmz_block_speed_rebuildable()).
 * @param[out] speed Rychlost.
 * @param[out] pulses Vlastní délky pulzů (platí jen při CMTSPEED_CUSTOM, jinak nulové).
 */
static void cmttmz_block_wanted_speed ( int block_id, en_CMTSPEED *speed, st_MZTAPE_PULSES_LENGTH *pulses ) {
    const st_CMTEXT_TAPE_INDEX *idx = cmttmz_idx ( block_id );
    memset ( pulses, 0, sizeof ( *pulses ) );
    if ( cmttmz_block_speed_kind ( block_id ) == CMTTMZ_SPEED_KIND_TAP ) {
        *speed = cmtext_container_get_block_cmt_speed ( g_cmt_tmz_extension->container, block_id );
        if ( *speed == CMTSPEED_NONE ) *speed = CMTSPEED_1_1;
        return;
    }
    if ( idx->blspeed == CMTEXT_BLOCK_SPEED_SET ) {
        *speed = idx->item.mzf.cmtspeed;
        if ( cmtspeed_is_custom ( *speed ) ) *pulses = idx->item.mzf.pulses;
    } else {
        *speed = g_cmt.mz_cmtspeed;
        if ( cmtspeed_is_custom ( *speed ) ) *pulses = g_cmt.mz_custom_pulses;
    }
}


/**
 * @brief Zjistí, zda je blok v souboru sestaven danou rychlostí.
 * @param block_id Index bloku. @param speed Rychlost. @param pulses Pulzy (jen CMTSPEED_CUSTOM).
 * @return 1 = shodná rychlost, 0 = jiná.
 */
static int cmttmz_block_built_with ( int block_id, en_CMTSPEED speed, const st_MZTAPE_PULSES_LENGTH *pulses ) {
    const st_CMTTMZ_BLOCK_SPEED *sp = &g_cmttmz.speeds[block_id];
    if ( sp->speed != speed ) return 0;
    if ( !cmtspeed_is_custom ( speed ) ) return 1;
    return ( 0 == memcmp ( &sp->pulses, pulses, sizeof ( *pulses ) ) );
}


/**
 * @brief Sestaví nový blok z bloku souboru jinou rychlostí.
 *
 * MZF: MZF (hlavička a tělo) z dosavadního bloku (tmz_block_to_mzf()),
 * cmttmz_build_mzf_block() s pauzou z importu (st_CMTTMZ_BLOCK_SPEED::pause_ms).
 * TAP: data a pauza z dosavadního bloku 0x11 (tzx_parse_turbo_speed()),
 * cmttmz_build_tap_block() (časování zxtape vydělené rychlostí).
 *
 * @param block_id Index přestavitelného bloku.
 * @param speed Nová rychlost. @param pulses Pulzy (jen CMTSPEED_CUSTOM u MZF).
 * @return Nový blok (uvolnit tmz_block_free()), nebo NULL (chyba vypsána do logu).
 */
static st_TZX_BLOCK* cmttmz_block_build ( int block_id, en_CMTSPEED speed, const st_MZTAPE_PULSES_LENGTH *pulses ) {
    const st_TZX_BLOCK *old = &g_cmttmz.file->blocks[block_id];
    st_TZX_BLOCK *nb = NULL;
    if ( cmttmz_block_speed_kind ( block_id ) == CMTTMZ_SPEED_KIND_TAP ) {
        st_TZX_TURBO_SPEED ts;
        if ( TZX_OK == tzx_parse_turbo_speed ( old->data, old->length, &ts ) && ts.data_length > 0 && ts.data_length <= 0xFFFF ) {
            nb = cmttmz_build_tap_block ( ts.data, ( uint16_t ) ts.data_length, speed, ts.pause_ms );
        }
    } else {
        st_MZF *mzf = tmz_block_to_mzf ( old, NULL );
        if ( !mzf ) {
            fprintf ( stderr, "%s: warning: block %d: can't read MZF of the block\n", cmtext_get_description ( g_cmt_tmz_extension ), block_id );
            return NULL;
        }
        nb = cmttmz_build_mzf_block ( &mzf->header, mzf->body, mzf->header.fsize, speed, pulses,
                                      g_cmttmz.speeds[block_id].pause_ms );
        mzf_free ( mzf );
    }
    if ( !nb ) {
        fprintf ( stderr, "%s: warning: block %d: can't build the block at the requested speed\n",
                  cmtext_get_description ( g_cmt_tmz_extension ), block_id );
    }
    return nb;
}


/**
 * @brief Vymění data bloku souboru za data nového bloku na témže indexu.
 *
 * Výsledek je stejný jako tzx_file_remove_block() + tzx_file_insert_block()
 * na témže indexu, jen bez posunu pole a bez uvolnění starých dat: ta
 * dostane volající (pro vrácení změny, nebo je uvolní free(); data bloků
 * alokuje výchozí alokátor knihoven tzx/tmz, malloc, viz cmt_tmz_import.c).
 *
 * @param block_id Index bloku.
 * @param nb Nový blok; funkce převezme jeho data a uvolní strukturu.
 * @param[out] old Původní blok (id, délka, data; vlastník je volající).
 */
static void cmttmz_file_swap_block ( int block_id, st_TZX_BLOCK *nb, st_TZX_BLOCK *old ) {
    *old = g_cmttmz.file->blocks[block_id];
    g_cmttmz.file->blocks[block_id].id = nb->id;
    g_cmttmz.file->blocks[block_id].length = nb->length;
    g_cmttmz.file->blocks[block_id].data = nb->data;
    nb->data = NULL; /* data převzal soubor */
    tmz_block_free ( nb );
}


/**
 * @brief Zapíše do vedlejšího pole, jakou rychlostí je blok v souboru sestaven.
 * @param block_id Index bloku. @param speed Rychlost. @param pulses Pulzy (jen CMTSPEED_CUSTOM).
 */
static void cmttmz_note_built_speed ( int block_id, en_CMTSPEED speed, const st_MZTAPE_PULSES_LENGTH *pulses ) {
    st_CMTTMZ_BLOCK_SPEED *sp = &g_cmttmz.speeds[block_id];
    sp->speed = speed;
    memset ( &sp->pulses, 0, sizeof ( sp->pulses ) );
    if ( cmtspeed_is_custom ( speed ) && pulses ) sp->pulses = *pulses;
}


int cmttmz_apply_default_speed ( en_CMTSPEED speed, const st_MZTAPE_PULSES_LENGTH *pulses ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) return EXIT_SUCCESS;
    st_MZTAPE_PULSES_LENGTH p;
    memset ( &p, 0, sizeof ( p ) );
    if ( cmtspeed_is_custom ( speed ) ) {
        if ( !pulses ) return EXIT_FAILURE;
        p = *pulses;
    }
    int count = g_cmt_tmz_extension->container->count_blocks;
    st_TZX_BLOCK **built = ( st_TZX_BLOCK** ) calloc ( count ? ( size_t ) count : 1, sizeof ( st_TZX_BLOCK* ) );
    if ( !built ) return EXIT_FAILURE;

    /* 1. všechny nové bloky sestavit; při chybě se soubor nemění */
    int ok = 1;
    for ( int i = 0; ok && i < count; i++ ) {
        if ( cmttmz_block_speed_kind ( i ) != CMTTMZ_SPEED_KIND_MZF ) continue;
        if ( cmttmz_idx ( i )->blspeed != CMTEXT_BLOCK_SPEED_DEFAULT ) continue;
        if ( cmttmz_block_built_with ( i, speed, &p ) ) continue;
        built[i] = cmttmz_block_build ( i, speed, &p );
        if ( !built[i] ) ok = 0;
    }
    /* 2. vyměnit */
    for ( int i = 0; i < count; i++ ) {
        if ( !built[i] ) continue;
        if ( !ok ) {
            tmz_block_free ( built[i] );
            continue;
        }
        st_TZX_BLOCK old;
        cmttmz_file_swap_block ( i, built[i], &old );
        free ( old.data );
        cmttmz_note_built_speed ( i, speed, &p );
    }
    free ( built );
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}


/* ------------------------------------------------------------------------- */
/*  Přehrání bloku přes player                                               */
/* ------------------------------------------------------------------------- */


/**
 * @brief Výsledek pokusu o stream bloku (cmttmz_play_from()).
 */
typedef enum en_CMTTMZ_STREAM_RESULT {
    CMTTMZ_STREAM_OK = 0,        /**< stream je vytvořený */
    CMTTMZ_STREAM_NO_STREAM,     /**< player stream nevrátil: chyba playeru (err), zastavení pásky, konec pásky, nebo (při požadovaném bloku) blok bez signálu; co z toho, rozhodne volající podle err a stavu playeru */
    CMTTMZ_STREAM_SPEED_ERROR,   /**< blok nejde přestavět požadovanou rychlostí (soubor beze změny) */
} en_CMTTMZ_STREAM_RESULT;


/**
 * @brief Rozpočet iterací playeru na jedno volání cmttmz_seq_next() (hledání dalšího bloku se signálem).
 *
 * Jeden rozpočet (tmz_player_play_next_budget()) pro všechna volání
 * playeru při hledání: každý zpracovaný blok (řídicí, přeskočený, zastavení
 * pásky) stojí 1. Omezuje tak i smyčku v pásce, která opakuje jen
 * přeskakované bloky, a vnořené prázdné smyčky kolem nich (dřív rozpočet
 * platil pro každé volání zvlášť a volání bylo až 100 000). Vyčerpání =
 * chyba "control flow does not reach a playable block". Hodnota je táž
 * jako pojistka jednoho volání playeru (TMZ_PLAYER_MAX_ITERATIONS).
 */
#define CMTTMZ_SEQ_MAX_ITERATIONS TMZ_PLAYER_MAX_ITERATIONS


/**
 * @brief Stav playeru pro přímé otevření bloku: blok block_id, prázdné zásobníky, výchozí úroveň.
 *
 * Config: vzorkovací frekvence GDGCLK_BASE (konstanta sestavení), vstream,
 * převod bloků v1.0, takt CPU z posledního bloku 0x43 před blokem.
 *
 * @param[out] st Stav.
 * @param block_id Blok, od kterého player začne (0 = začátek pásky).
 */
static void cmttmz_player_init_at ( st_TMZ_PLAYER_STATE *st, uint32_t block_id ) {
    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init ( &cfg );
    cfg.sample_rate = GDGCLK_BASE;
    cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
    cfg.v1_convert = 1;
    cfg.mz_cpu_clock = cmttmz_cpu_clock_before ( block_id );
    tmz_player_state_init ( st, g_cmttmz.file, &cfg );
    st->current_block = block_id;
}


/**
 * @brief Přehraje další blok ze stavu playeru base; blok MZF z importu požadovanou rychlostí.
 *
 * g_cmttmz.player se nastaví na kopii base a zavolá se
 * tmz_player_play_next(). Vrátí-li player stream bloku, který jde
 * přestavět (cmttmz_block_speed_rebuildable()) a v souboru je sestavený
 * jinou než požadovanou rychlostí, stream se zahodí, do souboru se
 * dočasně vloží přestavěný blok a player se pustí znovu ze stejného stavu
 * base (stejný blok díky deterministickému průchodu); po úspěchu nový blok
 * v souboru zůstane (aktualizuje se g_cmttmz.speeds), po neúspěchu se vrátí
 * původní.
 *
 * @param base Stav playeru před přehráním (smí ukazovat na g_cmttmz.player).
 * @param want_id Požadovaný blok (přímé otevření), nebo -1 = jakýkoli další blok
 *        (postupné přehrávání). Stream jiného bloku se zahodí a vrátí se
 *        CMTTMZ_STREAM_NO_STREAM s err TMZ_PLAYER_OK.
 * @param force_speed NULL = rychlost podle indexu (cmttmz_block_wanted_speed()),
 *        jinak tato rychlost (cmttmz_set_speed()).
 * @param force_pulses Pulzy k force_speed CMTSPEED_CUSTOM (jinak smí být NULL).
 * @param[out] stream Stream (vlastník je volající), jinak NULL.
 * @param[out] id state.last_played_block po přehrání (blok streamu, blok
 *        chyby nebo blok zastavení pásky).
 * @param[out] err Kód chyby playeru.
 * @param[in,out] budget Sdílený rozpočet iterací playeru
 *        (tmz_player_play_next_budget()), nebo NULL = každé volání playeru
 *        s vlastní pojistkou TMZ_PLAYER_MAX_ITERATIONS.
 * @return Výsledek.
 * @post g_cmttmz.player je stav po přehrání (při CMTTMZ_STREAM_SPEED_ERROR
 *       neurčitý stav za blokem; volající ho obnoví nebo končí).
 */
static en_CMTTMZ_STREAM_RESULT cmttmz_play_from ( const st_TMZ_PLAYER_STATE *base, int want_id,
                                                  const en_CMTSPEED *force_speed, const st_MZTAPE_PULSES_LENGTH *force_pulses,
                                                  st_CMT_STREAM **stream, int *id, en_TMZ_PLAYER_ERROR *err,
                                                  uint32_t *budget ) {
    st_TMZ_PLAYER_STATE b = *base;
    *stream = NULL;
    *err = TMZ_PLAYER_OK;
    uint32_t own_budget = TMZ_PLAYER_MAX_ITERATIONS;

    g_cmttmz.player = b;
    st_CMT_STREAM *s = tmz_player_play_next_budget ( &g_cmttmz.player, budget ? budget : &own_budget, err );
    *id = ( int ) g_cmttmz.player.last_played_block;
    if ( !s ) return CMTTMZ_STREAM_NO_STREAM;
    if ( want_id >= 0 && *id != want_id ) {
        /* blok signál nedal a player pokračoval dalším blokem */
        cmt_stream_destroy ( s );
        *err = TMZ_PLAYER_OK;
        return CMTTMZ_STREAM_NO_STREAM;
    }
    if ( !cmttmz_block_speed_rebuildable ( *id ) ) {
        *stream = s;
        return CMTTMZ_STREAM_OK;
    }

    en_CMTSPEED speed;
    st_MZTAPE_PULSES_LENGTH pulses;
    memset ( &pulses, 0, sizeof ( pulses ) );
    if ( force_speed ) {
        speed = *force_speed;
        if ( cmtspeed_is_custom ( speed ) && force_pulses ) pulses = *force_pulses;
    } else {
        cmttmz_block_wanted_speed ( *id, &speed, &pulses );
    }
    if ( cmttmz_block_built_with ( *id, speed, &pulses ) ) {
        *stream = s;
        return CMTTMZ_STREAM_OK;
    }
    cmt_stream_destroy ( s );

    st_TZX_BLOCK *nb = cmttmz_block_build ( *id, speed, &pulses );
    if ( !nb ) return CMTTMZ_STREAM_SPEED_ERROR;
    st_TZX_BLOCK saved;
    cmttmz_file_swap_block ( *id, nb, &saved );

    g_cmttmz.player = b;
    own_budget = TMZ_PLAYER_MAX_ITERATIONS;
    s = tmz_player_play_next_budget ( &g_cmttmz.player, budget ? budget : &own_budget, err );
    if ( !s || ( int ) g_cmttmz.player.last_played_block != *id ) {
        if ( s ) cmt_stream_destroy ( s );
        free ( g_cmttmz.file->blocks[*id].data );
        g_cmttmz.file->blocks[*id] = saved;
        fprintf ( stderr, "%s: warning: block %d: the block rebuilt at the requested speed can't be played\n",
                  cmtext_get_description ( g_cmt_tmz_extension ), *id );
        return CMTTMZ_STREAM_SPEED_ERROR;
    }
    /* data bloků alokuje výchozí alokátor knihoven tzx/tmz (malloc), viz cmt_tmz_import.c */
    free ( saved.data );
    cmttmz_note_built_speed ( *id, speed, &pulses );
    *stream = s;
    return CMTTMZ_STREAM_OK;
}


/**
 * @brief Zapíše délku bloku pro UI ze streamu, který blok právě hraje (zadarmo, bez výpočtu).
 *
 * Délka platí pro rychlost, se kterou je blok v souboru sestaven
 * (g_cmttmz.speeds): stream otevřeného bloku vzniká vždy z bloku souboru.
 * Čítač změn se nemění (volající mění čítač otevření nebo volá
 * cmttmz_note_change()).
 *
 * @param block_id Blok streamu (v rozsahu).
 * @param stream Stream bloku (nesmí být NULL; vlastnictví se nemění).
 * @post g_cmttmz.info[block_id].length_state == CMTTMZ_LENGTH_KNOWN.
 * @pre Vlákno emulátoru (otevření bloku za PLAY i bez device_state_lock();
 *      hodnoty jsou skalární, UI je čte nanejvýš o snímek staré).
 */
static void cmttmz_info_set_stream_length ( int block_id, st_CMT_STREAM *stream ) {
    st_CMTTMZ_BLOCK_INFO *inf = &g_cmttmz.info[block_id];
    inf->length_s = cmt_stream_get_length ( stream );
    inf->length_speed = g_cmttmz.speeds[block_id].speed;
    memset ( &inf->length_pulses, 0, sizeof ( inf->length_pulses ) );
    if ( cmtspeed_is_custom ( inf->length_speed ) ) inf->length_pulses = g_cmttmz.speeds[block_id].pulses;
    inf->length_state = CMTTMZ_LENGTH_KNOWN;
}


/**
 * @brief Uvolní blok otevřený cmttmz_block_wrap() včetně blockspec a streamu.
 * @param block Blok (NULL je přípustné).
 * @post block je neplatný.
 */
static void cmttmz_block_close ( st_CMTEXT_BLOCK *block ) {
    if ( !block ) return;
    baseui_tools_mem_free ( block->spec );
    block->spec = NULL;
    cmtext_block_destroy ( block );
}


/**
 * @brief Callback bloku: přenosová rychlost v Bd.
 *
 * Blok MZF z importu: poměr krát 1200 Bd, u vlastních pulzů ekvivalent
 * vůči 1:1 (1200 Bd krát mztape_pulses_get_ratio()), jako dosavadní
 * rozšíření MZF. Blok TAP: poměr rychlosti krát 1400 Bd (ZXTAPE_DEFAULT_BDSPEED).
 *
 * Vlákna: UI vrstva ji dnes nevolá (žádný volající v src/ui-imgui ani src/ui;
 * kdyby ji UI vlákno volalo, pak jen
 * pod device_state_lock()). Čte jen blockspec
 * bloku; ten zapisuje cmttmz_block_wrap() před zveřejněním bloku
 * a cmttmz_set_speed(), kterou emu vlákno volá jen ze zásahů GUI/MCP
 * (cmt_request, dbgapi CMT_SET_PROPERTY) pod týmž zámkem, takže zápis
 * a čtení blockspec se nepřekrývají. Samotný ukazatel g_cmt.ext->block
 * ale emu vlákno vyměňuje při přechodu na další blok během PLAY
 * (cmt_update_output() -> cmttmz_advance()) bez tohoto zámku - to platilo
 * stejně pro dřívější rozšíření MZF/TAP [neověřeno, že je to v praxi
 * bezpečné; mimo rozsah tohoto kroku].
 *
 * @param cmtext Rozšíření s otevřeným blokem (st_CMTEXT*).
 * @return Rychlost v Bd (zaokrouhlená).
 */
static uint16_t cmttmz_get_bdspeed ( void *cmtext ) {
    assert ( cmtext != NULL );
    st_CMTEXT *cext = ( st_CMTEXT* ) cmtext;
    assert ( cext->block != NULL );
    st_CMTTMZ_BLOCKSPEC *blspec = ( st_CMTTMZ_BLOCKSPEC* ) cext->block->spec;
    assert ( blspec != NULL );
    en_CMTEXT_BLOCK_TYPE type = cext->block->type;
    if ( ( type == CMTEXT_BLOCK_TYPE_TAPHEADER ) || ( type == CMTEXT_BLOCK_TYPE_TAPDATA ) ) {
        en_CMTSPEED tap_speed = ( blspec->cmtspeed == CMTSPEED_NONE ) ? CMTSPEED_1_1 : blspec->cmtspeed;
        return cmtspeed_get_bdspeed ( tap_speed, ZXTAPE_DEFAULT_BDSPEED );
    }
    if ( cmtspeed_is_custom ( blspec->cmtspeed ) ) {
        return ( uint16_t ) round ( MZTAPE_DEFAULT_BDSPEED * mztape_pulses_get_ratio ( CMTTMZ_FORMATSET, &blspec->pulses ) );
    }
    en_CMTSPEED speed = ( blspec->cmtspeed == CMTSPEED_NONE ) ? CMTSPEED_1_1 : blspec->cmtspeed;
    return ( uint16_t ) round ( MZTAPE_DEFAULT_BDSPEED * g_cmtspeed_divisor[speed] );
}


/**
 * @brief Callback bloku: přegeneruje blok MZF na novou výchozí rychlost Virtual CMT.
 *
 * Volá ho cmt_change_speed() / cmt_change_custom_pulses() jen pro blok
 * s CMTEXT_BLOCK_SPEED_DEFAULT; CMTSPEED_CUSTOM bere pulzy
 * g_cmt.mz_custom_pulses (volající je nastaví předem). Jde o přímou
 * operaci: player se přestaví na blok (cmttmz_player_init_at()), jako při
 * přímém otevření bloku.
 *
 * @param cmtext Rozšíření, jehož aktuální blok se mění (st_CMTEXT*).
 * @param cmtspeed Nová rychlost: poměr nebo CMTSPEED_CUSTOM.
 * @return EXIT_SUCCESS (i beze změny); EXIT_FAILURE, pokud blok nemá
 *         výchozí rychlost nebo ho nejde přestavět či přehrát (blok, jeho
 *         stream, soubor i stav playeru pak zůstanou beze změny).
 * @post Při úspěchu má blok nový stream (čtení od začátku), blockspec
 *       novou rychlost a pozici ve STOP st_CMTEXT_BLOCK::start_gdgticks = 0:
 *       změna rychlosti otevřeného bloku ho převine na začátek.
 */
static int cmttmz_set_speed ( void *cmtext, en_CMTSPEED cmtspeed ) {
    assert ( cmtext != NULL );
    st_CMTEXT *cext = ( st_CMTEXT* ) cmtext;
    st_CMTEXT_BLOCK *block = cext->block;
    assert ( block != NULL );
    if ( block->block_speed != CMTEXT_BLOCK_SPEED_DEFAULT ) return EXIT_FAILURE;
    if ( !cmttmz_block_speed_rebuildable ( block->block_id ) ) return EXIT_FAILURE;

    st_MZTAPE_PULSES_LENGTH pulses;
    memset ( &pulses, 0, sizeof ( pulses ) );
    if ( cmtspeed_is_custom ( cmtspeed ) ) pulses = g_cmt.mz_custom_pulses;
    st_CMTTMZ_BLOCKSPEC *blspec = ( st_CMTTMZ_BLOCKSPEC* ) block->spec;
    if ( ( blspec->cmtspeed == cmtspeed ) && ( !cmtspeed_is_custom ( cmtspeed ) || ( 0 == memcmp ( &blspec->pulses, &pulses, sizeof ( pulses ) ) ) ) ) {
        return EXIT_SUCCESS;
    }

    st_TMZ_PLAYER_STATE saved = g_cmttmz.player;
    st_TMZ_PLAYER_STATE base;
    cmttmz_player_init_at ( &base, ( uint32_t ) block->block_id );
    st_CMT_STREAM *stream = NULL;
    en_TMZ_PLAYER_ERROR err = TMZ_PLAYER_OK;
    int id = -1;
    if ( CMTTMZ_STREAM_OK != cmttmz_play_from ( &base, block->block_id, &cmtspeed, &pulses, &stream, &id, &err, NULL ) ) {
        g_cmttmz.player = saved;
        return EXIT_FAILURE;
    }
    cmt_stream_destroy ( block->stream );
    block->stream = stream;
    /* nový stream má jinou délku: pozice ve STOP by mohla být za jeho koncem; blok začne od začátku */
    block->start_gdgticks = 0;
    blspec->cmtspeed = cmtspeed;
    blspec->pulses = pulses;
    /* délka pro UI zadarmo ze streamu, který blok teď hraje (soubor je sestavený touto rychlostí) */
    cmttmz_info_set_stream_length ( block->block_id, stream );
    return EXIT_SUCCESS;
}


/**
 * @brief Zabalí stream bloku do nového st_CMTEXT_BLOCK (blockspec, callbacky, polarita WAV).
 *
 * Blockspec dostane rychlost, se kterou je blok v souboru sestaven
 * (g_cmttmz.speeds). Blok WAV dostane aktuální polaritu zadního
 * přepínače (g_cmt.polarity). Délku streamu zapíše do údajů bloku pro UI
 * (g_cmttmz.info, CMTTMZ_LENGTH_KNOWN pro rychlost sestavení).
 *
 * @param block_id Blok streamu.
 * @param stream Stream; funkce ho převezme (i při chybě, pak ho uvolní).
 * @return Nový blok (uvolnit cmttmz_block_close()), nebo NULL při selhání alokace.
 */
static st_CMTEXT_BLOCK* cmttmz_block_wrap ( int block_id, st_CMT_STREAM *stream ) {
    const st_CMTEXT_TAPE_INDEX *idx = cmttmz_idx ( block_id );
    st_CMTTMZ_BLOCKSPEC *blspec = ( st_CMTTMZ_BLOCKSPEC* ) baseui_tools_mem_alloc0 ( sizeof ( st_CMTTMZ_BLOCKSPEC ) );
    if ( !blspec ) {
        fprintf ( stderr, "%s():%d - Can't allocate memory\n", __func__, __LINE__ );
        cmt_stream_destroy ( stream );
        return NULL;
    }
    blspec->cmtspeed = g_cmttmz.speeds[block_id].speed;
    if ( cmtspeed_is_custom ( blspec->cmtspeed ) ) blspec->pulses = g_cmttmz.speeds[block_id].pulses;

    if ( idx->bltype == CMTEXT_BLOCK_TYPE_WAV ) {
        /* import WAV prahuje vždy normální polaritou, obrácená se uplatní na stream */
        cmt_stream_set_polarity ( stream, g_cmt.polarity );
    }

    st_CMTEXT_BLOCK *block = cmtext_block_new ( block_id, idx->bltype, stream, idx->blspeed, 0, blspec );
    if ( !block ) {
        cmt_stream_destroy ( stream );
        baseui_tools_mem_free ( blspec );
        return NULL;
    }

    int rebuildable = cmttmz_block_speed_rebuildable ( block_id );
    block->cb_play = cmtext_block_play;
    block->cb_get_playname = cmtext_block_get_playname;
    block->cb_set_polarity = ( idx->bltype == CMTEXT_BLOCK_TYPE_WAV ) ? cmtext_block_set_polarity : ( cmtext_block_cb_set_polarity ) NULL;
    block->cb_set_speed = rebuildable ? cmttmz_set_speed : ( cmtext_block_cb_set_speed ) NULL;
    int has_bdspeed = rebuildable || ( idx->bltype == CMTEXT_BLOCK_TYPE_TAPHEADER ) || ( idx->bltype == CMTEXT_BLOCK_TYPE_TAPDATA );
    block->cb_get_bdspeed = has_bdspeed ? cmttmz_get_bdspeed : ( cmtext_block_cb_get_bdspeed ) NULL;

    /* délka pro UI zadarmo: stream otevřeného bloku je sestavený rychlostí, kterou blok hraje */
    cmttmz_info_set_stream_length ( block_id, stream );

    if ( !g_cmttmz.quiet && !cmt_quiet_active ( ) ) printf ( "%s block %d: %s '%s', %s stream %0.2f kB, %d Hz, %1.2f s\n", cmtext_get_name ( g_cmt_tmz_extension ), block_id,
             tmz_block_id_name ( g_cmttmz.file->blocks[block_id].id ),
             cmtext_container_get_block_fname ( g_cmt_tmz_extension->container, block_id ),
             cmt_stream_get_stream_type_txt ( stream ), ( float ) cmt_stream_get_size ( stream ) / 1024,
             cmt_stream_get_rate ( stream ), cmt_stream_get_length ( stream ) );
    return block;
}


/**
 * @brief Výsledek pokusu o otevření bloku (cmttmz_block_open(), cmttmz_seq_next()).
 */
typedef enum en_CMTTMZ_OPEN_RESULT {
    CMTTMZ_OPEN_OK = 0,      /**< blok je otevřený */
    CMTTMZ_OPEN_SKIPPED,     /**< blok se přeskakuje (CONTROL nebo příznak skip), nic se nezkoušelo */
    CMTTMZ_OPEN_UNPLAYABLE,  /**< player blok odmítl (UNSUPPORTED, UNREADABLE) nebo blok nedal signál; nastaven příznak skip */
    CMTTMZ_OPEN_SPEED_ERROR, /**< blok nejde přestavět požadovanou rychlostí (soubor beze změny) */
    CMTTMZ_OPEN_ERROR,       /**< jiná chyba (mimo rozsah, alokace, jiná chyba playeru); příznak skip se nemění */
    CMTTMZ_OPEN_NONE_LEFT,   /**< ve směru (nebo v průchodu playeru) už žádný blok se signálem není */
} en_CMTTMZ_OPEN_RESULT;


/**
 * @brief Zpracuje chybu playeru u bloku: trvalé odmítnutí označí příznakem skip.
 *
 * TMZ_PLAYER_ERROR_UNSUPPORTED a TMZ_PLAYER_ERROR_UNREADABLE (a blok bez
 * signálu, err == TMZ_PLAYER_OK) nastaví g_cmttmz.skip[block_id] a vrátí
 * CMTTMZ_OPEN_UNPLAYABLE; ostatní chyby jen vypíše a vrátí
 * CMTTMZ_OPEN_ERROR. Vše jde do logu (stderr) anglicky s číslem bloku.
 *
 * @param block_id Blok (v rozsahu).
 * @param err Kód chyby playeru.
 * @return CMTTMZ_OPEN_UNPLAYABLE nebo CMTTMZ_OPEN_ERROR.
 */
static en_CMTTMZ_OPEN_RESULT cmttmz_player_failure ( int block_id, en_TMZ_PLAYER_ERROR err ) {
    const char *desc = cmtext_get_description ( g_cmt_tmz_extension );
    const char *bname = tmz_block_id_name ( g_cmttmz.file->blocks[block_id].id );
    if ( err == TMZ_PLAYER_OK ) {
        fprintf ( stderr, "%s: warning: block %d (%s) has no signal, skipped\n", desc, block_id, bname );
    } else if ( ( err == TMZ_PLAYER_ERROR_UNSUPPORTED ) || ( err == TMZ_PLAYER_ERROR_UNREADABLE ) ) {
        fprintf ( stderr, "%s: warning: block %d (%s) can't be played: %s, skipped\n", desc, block_id, bname,
                  tmz_player_error_string ( err ) );
    } else {
        fprintf ( stderr, "%s: error: block %d (%s) can't be opened now: %s\n", desc, block_id, bname,
                  tmz_player_error_string ( err ) );
        return CMTTMZ_OPEN_ERROR;
    }
    g_cmttmz.skip[block_id] = 1;
    g_cmttmz.change_serial++; /* UI: blok je teď nepřehratelný */
    return CMTTMZ_OPEN_UNPLAYABLE;
}


/**
 * @brief Přímé otevření bloku (uživatel, MCP, záznam vstupů, změna bloku tlačítkem).
 *
 * Player se přestaví na blok: current_block = block_id, prázdné zásobníky
 * smyček a volání, výchozí úroveň signálu (cmttmz_player_init_at()).
 * Stream bloku je proto funkcí jen čísla bloku a jeho rychlosti; opakované
 * přímé otevření (např. checkpoint záznamu vstupů) dá stejný signál.
 * Postupné přehrávání pak pokračuje ze stavu za tímto blokem
 * (cmttmz_advance()).
 *
 * Trvalé odmítnutí playerem a blok bez signálu nastaví příznak skip
 * (cmttmz_player_failure()). Při neúspěchu se stav playeru vrátí na stav
 * před voláním. Index kontejneru se nemění.
 *
 * @param block_id Index bloku v souboru.
 * @param[out] res Výsledek (nesmí být NULL).
 * @return Nový blok (uvolnit cmttmz_block_close()), nebo NULL podle *res.
 */
static st_CMTEXT_BLOCK* cmttmz_block_open ( int block_id, en_CMTTMZ_OPEN_RESULT *res ) {
    *res = CMTTMZ_OPEN_ERROR;
    st_CMTEXT_CONTAINER *container = g_cmt_tmz_extension->container;
    if ( !container || block_id < 0 || block_id >= container->count_blocks ) return NULL;
    if ( cmttmz_block_skipped ( block_id ) ) {
        *res = CMTTMZ_OPEN_SKIPPED;
        return NULL;
    }

    st_TMZ_PLAYER_STATE saved = g_cmttmz.player;
    st_TMZ_PLAYER_STATE base;
    cmttmz_player_init_at ( &base, ( uint32_t ) block_id );
    st_CMT_STREAM *stream = NULL;
    en_TMZ_PLAYER_ERROR err = TMZ_PLAYER_OK;
    int id = -1;
    en_CMTTMZ_STREAM_RESULT sres = cmttmz_play_from ( &base, block_id, NULL, NULL, &stream, &id, &err, NULL );
    if ( sres != CMTTMZ_STREAM_OK ) {
        g_cmttmz.player = saved;
        *res = ( sres == CMTTMZ_STREAM_SPEED_ERROR ) ? CMTTMZ_OPEN_SPEED_ERROR : cmttmz_player_failure ( block_id, err );
        return NULL;
    }
    st_CMTEXT_BLOCK *block = cmttmz_block_wrap ( block_id, stream );
    if ( !block ) {
        g_cmttmz.player = saved;
        return NULL;
    }
    *res = CMTTMZ_OPEN_OK;
    return block;
}


/**
 * @brief Přímo otevře první blok se signálem ve směru step počínaje from (včetně).
 *
 * Bloky CONTROL, bloky s příznakem skip a bloky, které player právě
 * trvale odmítl (cmttmz_block_open() jim příznak nastaví), přeskočí.
 * Hledání skončí na prvním bloku, který otevřít nejde z jiného důvodu
 * (chyba přestavění rychlosti, jiná chyba playeru, alokace).
 *
 * @param from První zkoušený index (smí být mimo rozsah, pak nic).
 * @param step +1 dopředu, -1 dozadu.
 * @param[out] res CMTTMZ_OPEN_OK, CMTTMZ_OPEN_NONE_LEFT (ve směru není
 *        blok se signálem), nebo výsledek bloku, na kterém hledání skončilo.
 * @param[out] failed_id Blok, na kterém hledání skončilo chybou (jinak -1).
 * @return Nový blok (uvolnit cmttmz_block_close()), nebo NULL.
 */
static st_CMTEXT_BLOCK* cmttmz_block_open_first ( int from, int step, en_CMTTMZ_OPEN_RESULT *res, int *failed_id ) {
    *failed_id = -1;
    int count = g_cmt_tmz_extension->container->count_blocks;
    for ( int id = from; id >= 0 && id < count; id += step ) {
        st_CMTEXT_BLOCK *block = cmttmz_block_open ( id, res );
        if ( block ) return block;
        if ( ( *res == CMTTMZ_OPEN_SKIPPED ) || ( *res == CMTTMZ_OPEN_UNPLAYABLE ) ) continue;
        *failed_id = id;
        return NULL;
    }
    *res = CMTTMZ_OPEN_NONE_LEFT;
    return NULL;
}


/**
 * @brief Postupné přehrávání: další blok se signálem z aktuálního stavu playeru.
 *
 * Volá tmz_player_play_next() na g_cmttmz.player (stejný stav, jaký zbyl
 * po předchozím bloku), takže úroveň signálu, smyčky, skoky a volání
 * určuje player. Blok s příznakem skip se zahodí a pokračuje se; blok,
 * který player trvale odmítne (UNSUPPORTED, UNREADABLE), dostane příznak
 * skip a pokračuje se také. Zastavení pásky (Pause 0, 0x2A) se poznamená
 * do *stop_seen a pokračuje se k dalšímu bloku se signálem (páska pak
 * stojí na něm).
 *
 * @param[out] stop_seen 1, pokud cestou player zastavil pásku (jinak se nemění).
 * @param[out] res CMTTMZ_OPEN_OK; CMTTMZ_OPEN_NONE_LEFT (player došel na
 *        konec pásky); CMTTMZ_OPEN_SPEED_ERROR; CMTTMZ_OPEN_ERROR (jiná
 *        chyba playeru, alokace, vyčerpaný rozpočet CMTTMZ_SEQ_MAX_ITERATIONS).
 * @param[out] failed_id Blok, u kterého nastala chyba (jinak -1).
 * @return Nový blok (uvolnit cmttmz_block_close()), nebo NULL.
 */
static st_CMTEXT_BLOCK* cmttmz_seq_next ( int *stop_seen, en_CMTTMZ_OPEN_RESULT *res, int *failed_id ) {
    const char *desc = cmtext_get_description ( g_cmt_tmz_extension );
    *failed_id = -1;
    /* jeden rozpočet iterací na celé hledání; každé volání playeru ho sníží
     * aspoň o 1, nebo skončí průchodem (finished), smyčka tedy vždy skončí */
    uint32_t budget = CMTTMZ_SEQ_MAX_ITERATIONS;
    for ( ;; ) {
        st_CMT_STREAM *stream = NULL;
        en_TMZ_PLAYER_ERROR err = TMZ_PLAYER_OK;
        int id = -1;
        en_CMTTMZ_STREAM_RESULT sres = cmttmz_play_from ( &g_cmttmz.player, -1, NULL, NULL, &stream, &id, &err, &budget );
        if ( sres == CMTTMZ_STREAM_SPEED_ERROR ) {
            *res = CMTTMZ_OPEN_SPEED_ERROR;
            *failed_id = id;
            return NULL;
        }
        if ( sres == CMTTMZ_STREAM_OK ) {
            if ( cmttmz_block_skipped ( id ) ) {
                cmt_stream_destroy ( stream );
                continue;
            }
            st_CMTEXT_BLOCK *block = cmttmz_block_wrap ( id, stream );
            if ( !block ) {
                *res = CMTTMZ_OPEN_ERROR;
                *failed_id = id;
                return NULL;
            }
            *res = CMTTMZ_OPEN_OK;
            return block;
        }
        /* bez streamu: zastavení pásky, konec pásky nebo chyba playeru */
        if ( err == TMZ_PLAYER_OK ) {
            if ( tmz_player_state_stopped ( &g_cmttmz.player ) ) {
                printf ( "%s: block %d stops the tape\n", desc, id );
                *stop_seen = 1;
                continue;
            }
            *res = CMTTMZ_OPEN_NONE_LEFT;
            return NULL;
        }
        if ( tmz_player_state_finished ( &g_cmttmz.player ) ) {
            /* player ukončil průchod chybou: vyčerpaný rozpočet iterací (řídicí bloky se točí bez
             * bloku se signálem, např. skok na sebe, nebo smyčka jen přeskakovaných bloků) */
            fprintf ( stderr, "%s: error: control flow does not reach a playable block after block %d "
                      "(possible infinite loop, player stopped at block %u)\n",
                      desc, id, ( unsigned ) g_cmttmz.player.current_block );
            *res = CMTTMZ_OPEN_ERROR;
            *failed_id = id;
            return NULL;
        }
        if ( CMTTMZ_OPEN_UNPLAYABLE == cmttmz_player_failure ( id, err ) ) continue;
        *res = CMTTMZ_OPEN_ERROR;
        *failed_id = id;
        return NULL;
    }
}


/**
 * @brief Nahradí otevřený blok rozšíření novým (starý uvolní).
 *
 * Zapíše i číslo otevřeného bloku pro UI (cmttmz_get_open_block_id()).
 *
 * @param block Nový blok (nesmí být NULL); převezme ho rozšíření.
 */
static void cmttmz_block_replace ( st_CMTEXT_BLOCK *block ) {
    cmttmz_block_close ( g_cmt_tmz_extension->block );
    g_cmt_tmz_extension->block = block;
    g_cmttmz.open_block_id = block->block_id;
    g_cmttmz.open_serial++;
}


/**
 * @brief Otevře první blok pásky průchodem playeru od začátku (bez náhrady bloku).
 *
 * Player se nastaví na blok 0 s prázdnými zásobníky a výchozí úrovní
 * a cmttmz_seq_next() najde první blok se signálem; řídicí bloky na
 * začátku (smyčka, skok, Pause 0 ...) tedy zpracuje player. Při neúspěchu
 * se stav playeru vrátí.
 *
 * @param[out] res Výsledek (viz cmttmz_seq_next()).
 * @param[out] failed_id Blok chyby (jinak -1).
 * @return Nový blok (uvolnit cmttmz_block_close()), nebo NULL.
 */
static st_CMTEXT_BLOCK* cmttmz_open_from_start ( en_CMTTMZ_OPEN_RESULT *res, int *failed_id ) {
    st_TMZ_PLAYER_STATE saved = g_cmttmz.player;
    cmttmz_player_init_at ( &g_cmttmz.player, 0 );
    int stop_seen = 0;
    st_CMTEXT_BLOCK *block = cmttmz_seq_next ( &stop_seen, res, failed_id );
    if ( !block ) g_cmttmz.player = saved;
    return block;
}


/* ------------------------------------------------------------------------- */
/*  Callbacky kontejneru (přímé otevření bloku)                              */
/* ------------------------------------------------------------------------- */


/**
 * @brief Callback kontejneru: přímo otevře další blok se signálem podle čísla (přeskakované bloky vynechá).
 *
 * Jde o přímé otevření (tlačítko Next, cmt_tape_step_block()): bere blok
 * s číslem o jedna vyšším, ne další blok podle průchodu playeru (ten dává
 * cmttmz_advance()). Bez otevřeného bloku hledá od začátku pásky.
 *
 * @return EXIT_SUCCESS; EXIT_FAILURE, když za aktuálním blokem žádný
 *         blok otevřít nejde (aktuální blok pak zůstává).
 */
static int cmttmz_container_next_block ( void ) {
    assert ( g_cmt_tmz_extension->container != NULL );
    int from = cmtext_block_get_block_id ( g_cmt_tmz_extension->block ) + 1;
    en_CMTTMZ_OPEN_RESULT res;
    int failed_id;
    st_CMTEXT_BLOCK *block = cmttmz_block_open_first ( from, +1, &res, &failed_id );
    if ( !block ) return EXIT_FAILURE;
    cmttmz_block_replace ( block );
    return EXIT_SUCCESS;
}


/**
 * @brief Callback kontejneru: přímo otevře předchozí blok se signálem (přeskakované bloky vynechá).
 *
 * @return EXIT_SUCCESS; EXIT_FAILURE, když před aktuálním blokem žádný
 *         blok otevřít nejde nebo není otevřený žádný blok.
 */
static int cmttmz_container_previous_block ( void ) {
    assert ( g_cmt_tmz_extension->container != NULL );
    if ( !g_cmt_tmz_extension->block ) return EXIT_FAILURE;
    int from = cmtext_block_get_block_id ( g_cmt_tmz_extension->block ) - 1;
    en_CMTTMZ_OPEN_RESULT res;
    int failed_id;
    st_CMTEXT_BLOCK *block = cmttmz_block_open_first ( from, -1, &res, &failed_id );
    if ( !block ) return EXIT_FAILURE;
    cmttmz_block_replace ( block );
    return EXIT_SUCCESS;
}


/**
 * @brief Callback kontejneru: přímo otevře blok block_id (cmttmz_block_open()).
 *
 * Blok CONTROL a blok s příznakem skip (i blok, který player teď trvale
 * odmítne) otevřít nejde. Otevření téhož bloku znovu vytvoří nový stream
 * (např. po změně rychlosti bloku). Index kontejneru se nemění.
 *
 * @param block_id Index bloku v souboru.
 * @return EXIT_SUCCESS; EXIT_FAILURE (aktuální blok i stav playeru se nemění).
 */
static int cmttmz_container_open_block ( int block_id ) {
    assert ( g_cmt_tmz_extension->container != NULL );
    en_CMTTMZ_OPEN_RESULT res;
    st_CMTEXT_BLOCK *block = cmttmz_block_open ( block_id, &res );
    if ( !block ) return EXIT_FAILURE;
    cmttmz_block_replace ( block );
    return EXIT_SUCCESS;
}


/* ------------------------------------------------------------------------- */
/*  Postupné přehrávání (volá cmt.c)                                         */
/* ------------------------------------------------------------------------- */


en_CMTTMZ_ADVANCE cmttmz_advance ( void ) {
    if ( !g_cmt_tmz_extension->container || !g_cmttmz.file || !g_cmt_tmz_extension->block ) return CMTTMZ_ADVANCE_END;
    /* při chybě se stav playeru vrátí: otevřený blok zůstává a další PLAY zkusí přechod znovu */
    st_TMZ_PLAYER_STATE saved = g_cmttmz.player;
    int stop_seen = 0;
    en_CMTTMZ_OPEN_RESULT res;
    int failed_id;
    st_CMTEXT_BLOCK *block = cmttmz_seq_next ( &stop_seen, &res, &failed_id );
    if ( block ) {
        cmttmz_block_replace ( block );
        return stop_seen ? CMTTMZ_ADVANCE_STOP_TAPE : CMTTMZ_ADVANCE_BLOCK;
    }
    if ( res == CMTTMZ_OPEN_NONE_LEFT ) return CMTTMZ_ADVANCE_END;
    g_cmttmz.player = saved;
    if ( res == CMTTMZ_OPEN_SPEED_ERROR ) {
        fprintf ( stderr, "%s: warning: block %d could not be rebuilt at the requested speed, playback stopped\n",
                  cmtext_get_description ( g_cmt_tmz_extension ), failed_id );
    }
    return CMTTMZ_ADVANCE_ERROR;
}


int cmttmz_seek_block ( int block_id ) {
    if ( !g_cmt_tmz_extension->container || !g_cmttmz.file ) return EXIT_FAILURE;
    g_cmttmz.quiet = 1;
    int ret = cmttmz_container_open_block ( block_id );
    g_cmttmz.quiet = 0;
    return ret;
}


int cmttmz_rewind ( void ) {
    if ( !g_cmt_tmz_extension->container || !g_cmttmz.file ) return EXIT_FAILURE;
    en_CMTTMZ_OPEN_RESULT res;
    int failed_id;
    st_CMTEXT_BLOCK *block = cmttmz_open_from_start ( &res, &failed_id );
    if ( !block ) return EXIT_FAILURE;
    cmttmz_block_replace ( block );
    return EXIT_SUCCESS;
}


/* ------------------------------------------------------------------------- */
/*  Callbacky rozšíření                                                      */
/* ------------------------------------------------------------------------- */


/**
 * @brief Callback rozšíření: vysune pásku (blok, kontejner, soubor, stav playeru).
 *
 * @post g_cmt_tmz_extension->block == NULL, container == NULL,
 *       g_cmttmz.file == NULL, g_cmttmz.speeds == NULL, g_cmttmz.skip == NULL,
 *       g_cmttmz.info == NULL, g_cmttmz.unsaved == NULL, g_cmttmz.modified == 0,
 *       source == CMTTMZ_SOURCE_TMZ, open_block_id == -1.
 *       Varování nástroje nad páskou (cmttmz_get_edit_warning()) je "".
 */
static void cmttmz_eject ( void ) {
    cmttmz_set_edit_warning ( NULL );
    cmttmz_block_close ( g_cmt_tmz_extension->block );
    g_cmt_tmz_extension->block = NULL;
    g_cmttmz.open_block_id = -1;
    g_cmttmz.open_serial++;
    g_cmttmz.tape_serial++;
    cmtext_container_destroy ( g_cmt_tmz_extension->container );
    g_cmt_tmz_extension->container = NULL;
    tzx_free ( g_cmttmz.file );
    g_cmttmz.file = NULL;
    free ( g_cmttmz.speeds );
    g_cmttmz.speeds = NULL;
    free ( g_cmttmz.skip );
    g_cmttmz.skip = NULL;
    free ( g_cmttmz.info );
    g_cmttmz.info = NULL;
    free ( g_cmttmz.unsaved );
    g_cmttmz.unsaved = NULL;
    memset ( &g_cmttmz.player, 0, sizeof ( g_cmttmz.player ) );
    g_cmttmz.modified = 0;
    g_cmttmz.source = CMTTMZ_SOURCE_TMZ;
}


/**
 * @brief Připojí text k varování otevřené pásky (g_cmttmz.warning, oddělovač "; ").
 *
 * Co se do bufferu nevejde, se ořízne (snprintf); buffer zůstane ukončený nulou.
 *
 * @param fmt Formát (printf), text anglicky.
 */
static void cmttmz_warning_append ( const char *fmt, ... ) {
    size_t len = strlen ( g_cmttmz.warning );
    if ( len + 3 >= sizeof ( g_cmttmz.warning ) ) return;
    if ( len ) {
        memcpy ( g_cmttmz.warning + len, "; ", 3 );
        len += 2;
    }
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( g_cmttmz.warning + len, sizeof ( g_cmttmz.warning ) - len, fmt, ap );
    va_end ( ap );
}


/**
 * @brief Je blok druhem, který player přehrát neumí (zjištěno bez generování signálu)?
 *
 * Zrcadlí větvení tmz_player_play_block() a sekvenčního playeru: signál
 * dávají bloky MZ 0x40, 0x41, 0x45, 0x46 a audio bloky TZX 0x10 až 0x15,
 * 0x18, 0x20; řídicí a informační bloky (0x21 až 0x2B, 0x30 až 0x35 kromě
 * 0x34, 0x5A, 0x42 až 0x44, rezervované 0x47 až 0x4F) signál nedávají, ale
 * chybou nejsou. Ostatní ID (např. 0x16, 0x17, 0x19, 0x34) player odmítne
 * jako nepodporované. Blok 0x18 se navíc ověří tzx_csw_recording_check():
 * komprese jiná než RLE (Z-RLE, neznámá), neplatná data nebo signál delší
 * než mez bloku (CMT_VSTREAM_MAX_BLOCK_SECONDS) = nepřehratelný. Jiné chyby
 * dat (např. meze generátorů FM a MANCHESTER, mez bloku 0x15 a 0x46) se
 * poznají až při přehrávání.
 *
 * @param b Blok souboru.
 * @return true = player blok odmítne; false = blok přehrát umí, nebo není
 *         zvukový (řídicí, informační).
 */
static bool cmttmz_block_known_unplayable ( const st_TZX_BLOCK *b ) {
    switch ( b->id ) {
        case TZX_BLOCK_ID_CSW_RECORDING: {
            /* komprese, data a mez délky bloku jedním průchodem daty (bez generování) */
            st_TZX_CSW_RECORDING p;
            if ( TZX_OK != tzx_parse_csw_recording ( b->data, b->length, &p ) ) return true;
            return TZX_OK != tzx_csw_recording_check ( &p );
        }
        case TZX_BLOCK_ID_STANDARD_SPEED:
        case TZX_BLOCK_ID_TURBO_SPEED:
        case TZX_BLOCK_ID_PURE_TONE:
        case TZX_BLOCK_ID_PULSE_SEQUENCE:
        case TZX_BLOCK_ID_PURE_DATA:
        case TZX_BLOCK_ID_DIRECT_RECORDING:
        case TZX_BLOCK_ID_PAUSE:
        case TZX_BLOCK_ID_GROUP_START:
        case TZX_BLOCK_ID_GROUP_END:
        case TZX_BLOCK_ID_JUMP:
        case TZX_BLOCK_ID_LOOP_START:
        case TZX_BLOCK_ID_LOOP_END:
        case TZX_BLOCK_ID_CALL_SEQUENCE:
        case TZX_BLOCK_ID_RETURN_FROM_SEQ:
        case TZX_BLOCK_ID_SELECT_BLOCK:
        case TZX_BLOCK_ID_STOP_48K:
        case TZX_BLOCK_ID_SET_SIGNAL_LEVEL:
        case TZX_BLOCK_ID_TEXT_DESCRIPTION:
        case TZX_BLOCK_ID_MESSAGE:
        case TZX_BLOCK_ID_ARCHIVE_INFO:
        case TZX_BLOCK_ID_HARDWARE_TYPE:
        case TZX_BLOCK_ID_CUSTOM_INFO:
        case TZX_BLOCK_ID_GLUE:
        case TMZ_BLOCK_ID_MZ_STANDARD_DATA:
        case TMZ_BLOCK_ID_MZ_TURBO_DATA:
        case TMZ_BLOCK_ID_MZ_EXTRA_BODY:
        case TMZ_BLOCK_ID_MZ_MACHINE_INFO:
        case TMZ_BLOCK_ID_MZ_LOADER:
        case TMZ_BLOCK_ID_MZ_BASIC_DATA:
        case TMZ_BLOCK_ID_MZ_FILE:
            return false;
        default:
            return !( b->id >= TMZ_BLOCK_ID_RESERVED_FIRST && b->id <= TMZ_BLOCK_ID_RESERVED_LAST );
    }
}


/* ------------------------------------------------------------------------- */
/*  Údaje bloků pro UI (délka, záznam, neuloženo, CSW, jméno)                */
/* ------------------------------------------------------------------------- */


/**
 * @brief Vyplní údaje bloku pro UI z bloku souboru (bez délky).
 *
 * Nepřehratelnost podle druhu bloku (cmttmz_block_known_unplayable(), bez
 * generování signálu; jediné ověření bloku při otevření pásky, varování
 * cmttmz_warn_unplayable_blocks() bere výsledek odsud), u bloku 0x18
 * vzorkovací frekvence a počet pulzů (tzx_parse_csw_recording()), u bloku
 * s MZF jméno v kódech Sharp MZ ASCII (tmz_block_to_mzf()). Řídicí blok
 * (v indexu CONTROL) a nepřehratelný blok mají délku
 * CMTTMZ_LENGTH_NO_SIGNAL, ostatní CMTTMZ_LENGTH_UNKNOWN.
 *
 * @param i Index bloku v souboru (položka indexu už vyplněná).
 * @param recorded 1 = blok připojil záznam.
 * @pre g_cmttmz.info má aspoň i + 1 položek.
 */
static void cmttmz_info_fill ( uint32_t i, int recorded ) {
    st_CMTTMZ_BLOCK_INFO *inf = &g_cmttmz.info[i];
    const st_TZX_BLOCK *b = &g_cmttmz.file->blocks[i];
    memset ( inf, 0, sizeof ( *inf ) );
    inf->recorded = recorded ? 1 : 0;
    inf->unplayable = cmttmz_block_known_unplayable ( b ) ? 1 : 0;
    if ( b->id == TZX_BLOCK_ID_CSW_RECORDING ) {
        st_TZX_CSW_RECORDING p;
        if ( TZX_OK == tzx_parse_csw_recording ( b->data, b->length, &p ) ) {
            inf->csw_rate = p.sample_rate;
            inf->csw_pulses = p.pulse_count;
        }
    }
    if ( cmttmz_idx ( ( int ) i )->bltype == CMTEXT_BLOCK_TYPE_MZF ) {
        st_MZF *mzf = tmz_block_to_mzf ( b, NULL );
        if ( mzf ) {
            uint8_t len = mzf_tools_get_fname_length ( &mzf->header );
            memcpy ( inf->mzf_name, mzf->header.fname.name, len );
            inf->mzf_name_len = len;
            mzf_free ( mzf );
        }
    }
    if ( cmttmz_idx ( ( int ) i )->bltype == CMTEXT_BLOCK_TYPE_CONTROL || inf->unplayable ) {
        inf->length_state = CMTTMZ_LENGTH_NO_SIGNAL;
    }
}


/**
 * @brief Je délka bloku neznámá nebo zastaralá (blok by teď hrál jinou rychlostí)?
 * @param i Index bloku.
 * @return 1 = délku je třeba zjistit znovu (cmttmz_lengths_pass()).
 */
static int cmttmz_length_stale ( int i ) {
    const st_CMTTMZ_BLOCK_INFO *inf = &g_cmttmz.info[i];
    if ( inf->length_state == CMTTMZ_LENGTH_NO_SIGNAL ) return 0;
    if ( inf->length_state == CMTTMZ_LENGTH_UNKNOWN ) return 1;
    if ( !cmttmz_block_speed_rebuildable ( i ) ) return 0;
    en_CMTSPEED speed;
    st_MZTAPE_PULSES_LENGTH pulses;
    cmttmz_block_wanted_speed ( i, &speed, &pulses );
    if ( speed != inf->length_speed ) return 1;
    return cmtspeed_is_custom ( speed ) && memcmp ( &pulses, &inf->length_pulses, sizeof ( pulses ) );
}


/**
 * @brief Rychlost, pro kterou platí délka bloku spočítaná teď (kterou by blok teď hrál).
 *
 * Blok s měnitelnou rychlostí: cmttmz_block_wanted_speed(); jiný blok:
 * rychlost, se kterou je v souboru (g_cmttmz.speeds, u něj bez významu).
 *
 * @param i Index bloku.
 * @param[out] speed Rychlost. @param[out] pulses Pulzy (jen CMTSPEED_CUSTOM, jinak nulové).
 */
static void cmttmz_length_speed ( int i, en_CMTSPEED *speed, st_MZTAPE_PULSES_LENGTH *pulses ) {
    memset ( pulses, 0, sizeof ( *pulses ) );
    if ( cmttmz_block_speed_rebuildable ( i ) ) {
        cmttmz_block_wanted_speed ( i, speed, pulses );
        return;
    }
    *speed = g_cmttmz.speeds[i].speed;
    if ( cmtspeed_is_custom ( *speed ) ) *pulses = g_cmttmz.speeds[i].pulses;
}


/** @brief Výsledek levného výpočtu délky bloku (cmttmz_length_cheap()). */
typedef enum en_CMTTMZ_CHEAP {
    CMTTMZ_CHEAP_NONE = 0,  /**< levně spočítat nejde (blok MZ, přestavba jinou rychlostí, 0x15, chyba dat); zbývá stream */
    CMTTMZ_CHEAP_KNOWN,     /**< délka spočítaná ve vzorcích GDGCLK_BASE */
    CMTTMZ_CHEAP_NO_SIGNAL, /**< blok signál nedá (0x20 s pauzou 0, player vrátí NULL bez chyby) */
} en_CMTTMZ_CHEAP;


/**
 * @brief Počet výstupních vzorků jednoho pulzu TZX, stejně jako generátor knihovny tzx.
 *
 * Generátor (tzx_add_pulse() v tzx.c) převádí T-states výrazem
 * tzx_tstates_to_samples() a dává nejméně 1 vzorek. Player pro bloky TZX
 * nechává takt TZX_DEFAULT_CPU_CLOCK (tmz_player_to_tzx_config()); výstup
 * má frekvenci GDGCLK_BASE jako stream přehrávání.
 *
 * @param tstates Délka pulzu v T-states.
 * @return Vzorky (>= 1).
 */
static uint64_t cmttmz_tzx_pulse_samples ( uint32_t tstates ) {
    uint32_t s = tzx_tstates_to_samples ( tstates, ( uint32_t ) GDGCLK_BASE, TZX_DEFAULT_CPU_CLOCK );
    return s ? s : 1;
}


/**
 * @brief Počet výstupních vzorků pauzy TZX, stejně jako tzx_add_pause_to_vstream() v tzx.c.
 *
 * Rozdělení pauzy na 1 ms vysoko a zbytek nízko (podle úrovně) celkový
 * počet vzorků nemění.
 *
 * @param pause_ms Pauza v ms (0 = žádné vzorky).
 * @return Vzorky (pauza > 0 aspoň 1).
 */
static uint64_t cmttmz_tzx_pause_samples ( uint16_t pause_ms ) {
    if ( pause_ms == 0 ) return 0;
    uint32_t total = ( uint32_t ) ( ( double ) pause_ms * ( uint32_t ) GDGCLK_BASE / 1000.0 + 0.5 );
    return total ? total : 1;
}


/**
 * @brief Počet výstupních vzorků datových bitů TZX, stejně jako tzx_add_data_bits() v tzx.c.
 *
 * Každý bit jsou dva pulzy délky podle hodnoty bitu; bajty od MSb, v posledním
 * bajtu jen used_bits_last bitů (hodnota nad 8 dává nulové bity jako
 * generátor). Jeden průchod daty bez alokace.
 *
 * @param data Data (smí být NULL jen při len 0). @param len Počet bajtů.
 * @param used_bits_last Bity posledního bajtu. @param zero Pulz bitu 0 (T-states). @param one Pulz bitu 1.
 * @return Vzorky.
 */
static uint64_t cmttmz_tzx_data_samples ( const uint8_t *data, uint32_t len, uint8_t used_bits_last,
                                          uint16_t zero, uint16_t one ) {
    if ( len == 0 ) return 0;
    uint64_t ones = 0, zeros = 0;
    for ( uint32_t i = 0; i + 1 < len; i++ ) {
        unsigned n = ( unsigned ) __builtin_popcount ( data[i] );
        ones += n;
        zeros += 8u - n;
    }
    uint8_t byte = data[len - 1];
    for ( int b = 0; b < used_bits_last; b++ ) {
        if ( byte & 0x80 ) ones++;
        else zeros++;
        byte = ( uint8_t ) ( byte << 1 );
    }
    return 2u * ( ones * cmttmz_tzx_pulse_samples ( one ) + zeros * cmttmz_tzx_pulse_samples ( zero ) );
}


/**
 * @brief Počet výstupních vzorků bloku 0x18 jedním průchodem daty RLE (bez alokace).
 *
 * Stejný výpočet jako generátor knihovny (gen_csw_recording() v tzx.c):
 * konec k-tého pulzu je zaokrouhlený převod součtu prvních k délek
 * z frekvence záznamu na GDGCLK_BASE, pulz dostane nejméně 1 vzorek; za
 * pulzy pauza bloku. Platnost bloku (komprese, data, mez délky) ověřila
 * knihovna při vložení bloku (tzx_csw_recording_check() v
 * cmttmz_block_known_unplayable()); průchod tu jen nečte za koncem dat.
 *
 * @param p Naparsovaný blok (tzx_parse_csw_recording()).
 * @param[out] samples Vzorky bloku včetně pauzy.
 * @return 1 = spočítáno; 0 = data neodpovídají počtu pulzů (spočítat nejde).
 */
static int cmttmz_csw_samples ( const st_TZX_CSW_RECORDING *p, uint64_t *samples ) {
    const uint64_t dst = ( uint32_t ) GDGCLK_BASE;
    const uint64_t src = p->sample_rate;
    if ( src == 0 || ( p->data_length && !p->data ) ) return 0;
    const uint8_t *d = p->data;
    uint32_t pos = 0;
    uint64_t src_end = 0, emitted = 0;
    for ( uint32_t i = 0; i < p->pulse_count; i++ ) {
        if ( pos >= p->data_length ) return 0;
        uint32_t len = d[pos++];
        if ( len == 0 ) {
            if ( p->data_length - pos < 4 ) return 0;
            len = cmttmz_rd32 ( d + pos );
            pos += 4;
            if ( len == 0 ) return 0;
        }
        src_end += len;
        /* round ( src_end * dst / src ) bez přetečení (jako tzx_csw_scale()) */
        uint64_t target = ( src_end / src ) * dst + ( ( src_end % src ) * dst + src / 2 ) / src;
        emitted += ( target > emitted ) ? target - emitted : 1;
    }
    if ( pos != p->data_length ) return 0;
    *samples = emitted + cmttmz_tzx_pause_samples ( p->pause_ms );
    return 1;
}


/**
 * @brief Délka bloku bez generování signálu: z parametrů bloku, nebo jedním průchodem dat 0x18.
 *
 * Počítá totéž, co by dal stream tmz_player_play_block() při GDGCLK_BASE
 * (do 1 výstupního vzorku, ověřují testy proti streamu):
 * - 0x10: pilot TZX_PILOT_COUNT_HEADER / _DATA pulzů podle flag bajtu,
 *   dva synchronizační pulzy, datové bity, pauza;
 * - 0x11: pilot, synchronizace, datové bity (used_bits), pauza; blok TAP
 *   s měnitelnou rychlostí jen tehdy, je-li v souboru sestaven rychlostí,
 *   kterou by hrál;
 * - 0x12, 0x13: součet pulzů; 0x14: datové bity a pauza;
 * - 0x18: průchod daty RLE (cmttmz_csw_samples());
 * - 0x20: pauza (pauza 0 = blok bez signálu).
 * Ostatní bloky (MZ 0x40 až 0x46 a bloky MZF s měnitelnou rychlostí,
 * 0x15 kvůli mezi délky bloku, kterou knihovna ověřuje až při generování)
 * levně spočítat nejde.
 *
 * @param i Index bloku.
 * @param[out] samples Vzorky při CMTTMZ_CHEAP_KNOWN (jinak 0).
 * @return Výsledek.
 */
static en_CMTTMZ_CHEAP cmttmz_length_cheap ( int i, uint64_t *samples ) {
    const st_TZX_BLOCK *b = &g_cmttmz.file->blocks[i];
    *samples = 0;
    if ( cmttmz_block_speed_rebuildable ( i ) ) {
        en_CMTSPEED speed;
        st_MZTAPE_PULSES_LENGTH pulses;
        cmttmz_block_wanted_speed ( i, &speed, &pulses );
        if ( cmttmz_block_speed_kind ( i ) != CMTTMZ_SPEED_KIND_TAP || !cmttmz_block_built_with ( i, speed, &pulses ) ) {
            return CMTTMZ_CHEAP_NONE;
        }
    }
    if ( !b->data && b->length ) return CMTTMZ_CHEAP_NONE;
    switch ( b->id ) {
        case TZX_BLOCK_ID_STANDARD_SPEED: {
            st_TZX_STANDARD_SPEED p;
            if ( TZX_OK != tzx_parse_standard_speed ( b->data, b->length, &p ) ) return CMTTMZ_CHEAP_NONE;
            uint32_t pilot = ( p.data_length > 0 && p.data[0] < 128 ) ? TZX_PILOT_COUNT_HEADER : TZX_PILOT_COUNT_DATA;
            *samples = pilot * cmttmz_tzx_pulse_samples ( TZX_PILOT_PULSE_LENGTH )
                       + cmttmz_tzx_pulse_samples ( TZX_SYNC1_PULSE_LENGTH ) + cmttmz_tzx_pulse_samples ( TZX_SYNC2_PULSE_LENGTH )
                       + cmttmz_tzx_data_samples ( p.data, p.data_length, 8, TZX_ZERO_BIT_PULSE_LENGTH, TZX_ONE_BIT_PULSE_LENGTH )
                       + cmttmz_tzx_pause_samples ( p.pause_ms );
            return CMTTMZ_CHEAP_KNOWN;
        }
        case TZX_BLOCK_ID_TURBO_SPEED: {
            st_TZX_TURBO_SPEED p;
            if ( TZX_OK != tzx_parse_turbo_speed ( b->data, b->length, &p ) ) return CMTTMZ_CHEAP_NONE;
            *samples = ( uint64_t ) p.pilot_count * cmttmz_tzx_pulse_samples ( p.pilot_pulse )
                       + cmttmz_tzx_pulse_samples ( p.sync1_pulse ) + cmttmz_tzx_pulse_samples ( p.sync2_pulse )
                       + cmttmz_tzx_data_samples ( p.data, p.data_length, p.used_bits, p.zero_pulse, p.one_pulse )
                       + cmttmz_tzx_pause_samples ( p.pause_ms );
            return CMTTMZ_CHEAP_KNOWN;
        }
        case TZX_BLOCK_ID_PURE_TONE: {
            st_TZX_PURE_TONE p;
            if ( TZX_OK != tzx_parse_pure_tone ( b->data, b->length, &p ) ) return CMTTMZ_CHEAP_NONE;
            *samples = ( uint64_t ) p.pulse_count * cmttmz_tzx_pulse_samples ( p.pulse_length );
            return CMTTMZ_CHEAP_KNOWN;
        }
        case TZX_BLOCK_ID_PULSE_SEQUENCE: {
            st_TZX_PULSE_SEQUENCE p;
            if ( TZX_OK != tzx_parse_pulse_sequence ( b->data, b->length, &p ) ) return CMTTMZ_CHEAP_NONE;
            const uint8_t *raw = ( const uint8_t* ) p.pulses;
            uint64_t s = 0;
            for ( uint32_t k = 0; k < p.pulse_count; k++ ) {
                s += cmttmz_tzx_pulse_samples ( ( uint32_t ) raw[2 * k] | ( ( uint32_t ) raw[2 * k + 1] << 8 ) );
            }
            *samples = s;
            return CMTTMZ_CHEAP_KNOWN;
        }
        case TZX_BLOCK_ID_PURE_DATA: {
            st_TZX_PURE_DATA p;
            if ( TZX_OK != tzx_parse_pure_data ( b->data, b->length, &p ) ) return CMTTMZ_CHEAP_NONE;
            *samples = cmttmz_tzx_data_samples ( p.data, p.data_length, p.used_bits, p.zero_pulse, p.one_pulse )
                       + cmttmz_tzx_pause_samples ( p.pause_ms );
            return CMTTMZ_CHEAP_KNOWN;
        }
        case TZX_BLOCK_ID_CSW_RECORDING: {
            st_TZX_CSW_RECORDING p;
            if ( TZX_OK != tzx_parse_csw_recording ( b->data, b->length, &p ) ) return CMTTMZ_CHEAP_NONE;
            if ( p.compression_type != TZX_CSW_COMPRESSION_RLE ) return CMTTMZ_CHEAP_NONE;
            return cmttmz_csw_samples ( &p, samples ) ? CMTTMZ_CHEAP_KNOWN : CMTTMZ_CHEAP_NONE;
        }
        case TZX_BLOCK_ID_PAUSE: {
            st_TZX_PAUSE p;
            if ( TZX_OK != tzx_parse_pause ( b->data, b->length, &p ) ) return CMTTMZ_CHEAP_NONE;
            if ( p.pause_ms == 0 ) return CMTTMZ_CHEAP_NO_SIGNAL;
            *samples = cmttmz_tzx_pause_samples ( p.pause_ms );
            return CMTTMZ_CHEAP_KNOWN;
        }
        default:
            return CMTTMZ_CHEAP_NONE;
    }
}


/**
 * @brief Zapíše délku bloku levným výpočtem (cmttmz_length_cheap()), jde-li to.
 * @param i Index bloku.
 * @return 1 = délka zapsaná (KNOWN nebo NO_SIGNAL); 0 = levně nejde, údaje beze změny.
 */
static int cmttmz_length_try_cheap ( int i ) {
    uint64_t samples = 0;
    en_CMTTMZ_CHEAP r = cmttmz_length_cheap ( i, &samples );
    if ( r == CMTTMZ_CHEAP_NONE ) return 0;
    st_CMTTMZ_BLOCK_INFO *inf = &g_cmttmz.info[i];
    if ( r == CMTTMZ_CHEAP_NO_SIGNAL ) {
        inf->length_s = 0;
        inf->length_state = CMTTMZ_LENGTH_NO_SIGNAL;
        return 1;
    }
    inf->length_s = ( double ) samples / ( double ) GDGCLK_BASE;
    cmttmz_length_speed ( i, &inf->length_speed, &inf->length_pulses );
    inf->length_state = CMTTMZ_LENGTH_KNOWN;
    return 1;
}


#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: počet streamů vygenerovaných pro délky (cmttmz_length_stream()). */
static unsigned s_test_length_streams = 0;
#endif


/**
 * @brief Délka bloku generováním streamu, který by blok teď dal (drahá cesta, jen zásah UI).
 *
 * Blok s měnitelnou rychlostí, sestavený jinou rychlostí, než by hrál, se
 * sestaví do dočasné kopie (jako export WAV); soubor, player, otevřený
 * blok ani údaje bloků se nemění.
 *
 * @param i Index bloku.
 * @param[out] len_s Délka v s (0 bez signálu).
 * @param[out] unplayable 1 = player blok odmítl (UNSUPPORTED, UNREADABLE).
 * @return CMTTMZ_LENGTH_KNOWN, nebo CMTTMZ_LENGTH_NO_SIGNAL (blok nejde
 *         sestavit rychlostí, kterou by hrál, nebo player signál nedal).
 */
static en_CMTTMZ_LENGTH_STATE cmttmz_length_stream ( int i, double *len_s, int *unplayable ) {
    const st_TZX_BLOCK *src = &g_cmttmz.file->blocks[i];
    st_TZX_BLOCK *built = NULL;
    *len_s = 0;
    *unplayable = 0;
    en_CMTSPEED speed;
    st_MZTAPE_PULSES_LENGTH pulses;
    cmttmz_length_speed ( i, &speed, &pulses );
    if ( cmttmz_block_speed_rebuildable ( i ) && !cmttmz_block_built_with ( i, speed, &pulses ) ) {
        built = cmttmz_block_build ( i, speed, &pulses );
        /* blok nejde sestavit rychlostí, kterou by hrál: signál nedá (chybu ohlásí jeho otevření) */
        if ( !built ) return CMTTMZ_LENGTH_NO_SIGNAL;
        src = built;
    }
    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init ( &cfg );
    cfg.sample_rate = GDGCLK_BASE;
    cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
    cfg.v1_convert = 1;
    cfg.mz_cpu_clock = cmttmz_cpu_clock_before ( ( uint32_t ) i );
    en_TMZ_PLAYER_ERROR perr = TMZ_PLAYER_OK;
    st_CMT_STREAM *stream = tmz_player_play_block ( src, &cfg, &perr );
    if ( built ) tmz_block_free ( built );
    if ( !stream ) {
        if ( perr == TMZ_PLAYER_ERROR_UNSUPPORTED || perr == TMZ_PLAYER_ERROR_UNREADABLE ) *unplayable = 1;
        return CMTTMZ_LENGTH_NO_SIGNAL;
    }
#ifdef MZTEST_HEADLESS
    s_test_length_streams++;
#endif
    *len_s = cmt_stream_get_length ( stream );
    cmt_stream_destroy ( stream );
    return CMTTMZ_LENGTH_KNOWN;
}


/**
 * @brief Zapíše délku bloku generováním streamu (cmttmz_length_stream()).
 *
 * Blok, který player odmítne, dostane CMTTMZ_LENGTH_NO_SIGNAL a příznak
 * unplayable (příznak přeskočení g_cmttmz.skip, kterým se řídí
 * přehrávání, se nemění).
 *
 * @param i Index bloku.
 */
static void cmttmz_length_compute ( int i ) {
    st_CMTTMZ_BLOCK_INFO *inf = &g_cmttmz.info[i];
    double len = 0;
    int unplayable = 0;
    en_CMTTMZ_LENGTH_STATE st = cmttmz_length_stream ( i, &len, &unplayable );
    inf->length_s = len;
    inf->length_state = st;
    if ( unplayable ) inf->unplayable = 1;
    if ( st == CMTTMZ_LENGTH_KNOWN ) cmttmz_length_speed ( i, &inf->length_speed, &inf->length_pulses );
}


/** @brief Co průchod cmttmz_lengths_pass() s bloky dělá. */
typedef enum en_CMTTMZ_LENGTHS_MODE {
    CMTTMZ_LENGTHS_INVALIDATE = 0, /**< zastaralou délku jen označí jako neznámou (nic nepočítá) */
    CMTTMZ_LENGTHS_CHEAP,          /**< v rozsahu bloků navíc levný výpočet (cmttmz_length_cheap()) */
    CMTTMZ_LENGTHS_STEP,           /**< navíc výpočet generováním streamu; levný i drahý výpočet v rozpočtu času */
} en_CMTTMZ_LENGTHS_MODE;


/**
 * @brief Projde bloky se zastaralou nebo neznámou délkou (cmttmz_length_stale()).
 *
 * Blok v rozsahu [from, to) podle režimu spočítá; ostatní zastaralé bloky
 * označí CMTTMZ_LENGTH_UNKNOWN (UI je pak nepočítá do délky pásky). Režim
 * CMTTMZ_LENGTHS_STEP počítá, dokud nevyprší budget_us času hostitele
 * (kontrola před každým blokem, aspoň jeden blok se spočítá vždy, je-li co).
 * Při změně zvedne čítač cmttmz_get_change_serial().
 *
 * @param mode Režim. @param from První blok rozsahu. @param to Blok za rozsahem.
 * @param budget_us Rozpočet v µs (jen CMTTMZ_LENGTHS_STEP).
 * @pre Vlákno emulátoru pod device_state_lock(), pokud UI pole může číst.
 */
static void cmttmz_lengths_pass ( en_CMTTMZ_LENGTHS_MODE mode, int from, int to, int64_t budget_us ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container || !g_cmttmz.info ) return;
    int count = g_cmt_tmz_extension->container->count_blocks;
    gint64 start = g_get_monotonic_time ( );
    int changed = 0;
    int done = 0;
    for ( int i = 0; i < count; i++ ) {
        if ( !cmttmz_length_stale ( i ) ) continue;
        int may = ( mode != CMTTMZ_LENGTHS_INVALIDATE ) && ( i >= from ) && ( i < to );
        if ( may && mode == CMTTMZ_LENGTHS_STEP && done > 0 && ( g_get_monotonic_time ( ) - start ) > budget_us ) may = 0;
        if ( may && cmttmz_length_try_cheap ( i ) ) {
            done++;
            changed = 1;
            continue;
        }
        if ( may && mode == CMTTMZ_LENGTHS_STEP ) {
            cmttmz_length_compute ( i );
            done++;
            changed = 1;
            continue;
        }
        /* délka zůstane neznámá do dalšího zásahu UI nebo otevření bloku */
        if ( g_cmttmz.info[i].length_state != CMTTMZ_LENGTH_UNKNOWN ) {
            g_cmttmz.info[i].length_state = CMTTMZ_LENGTH_UNKNOWN;
            g_cmttmz.info[i].length_s = 0;
            changed = 1;
        }
    }
    if ( changed ) g_cmttmz.change_serial++;
}


void cmttmz_lengths_invalidate ( void ) {
    cmttmz_lengths_pass ( CMTTMZ_LENGTHS_INVALIDATE, 0, 0, 0 );
}


void cmttmz_lengths_update_budget ( int64_t budget_us ) {
    if ( !g_cmt_tmz_extension->container ) return;
    cmttmz_lengths_pass ( CMTTMZ_LENGTHS_STEP, 0, g_cmt_tmz_extension->container->count_blocks, budget_us );
}


#ifdef MZTEST_HEADLESS
unsigned cmttmz_test_get_length_streams ( void ) {
    return s_test_length_streams;
}


int cmttmz_test_length_paths ( int block_id, int *cheap_known, double *cheap_s, int *stream_known, double *stream_s ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container || block_id < 0
         || block_id >= g_cmt_tmz_extension->container->count_blocks ) return EXIT_FAILURE;
    uint64_t samples = 0;
    en_CMTTMZ_CHEAP r = cmttmz_length_cheap ( block_id, &samples );
    *cheap_known = ( r == CMTTMZ_CHEAP_KNOWN ) ? 1 : ( ( r == CMTTMZ_CHEAP_NO_SIGNAL ) ? 0 : -1 );
    *cheap_s = ( double ) samples / ( double ) GDGCLK_BASE;
    int unplayable = 0;
    *stream_known = ( CMTTMZ_LENGTH_KNOWN == cmttmz_length_stream ( block_id, stream_s, &unplayable ) ) ? 1 : 0;
    return EXIT_SUCCESS;
}
#endif


int cmttmz_get_block_ui ( int block_id, st_CMTTMZ_BLOCK_UI *out ) {
    if ( !out ) return EXIT_FAILURE;
    memset ( out, 0, sizeof ( *out ) );
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container || !g_cmttmz.info ) return EXIT_FAILURE;
    if ( block_id < 0 || block_id >= g_cmt_tmz_extension->container->count_blocks ) return EXIT_FAILURE;
    const st_CMTTMZ_BLOCK_INFO *inf = &g_cmttmz.info[block_id];
    out->length_state = inf->length_state;
    out->length_s = ( inf->length_state == CMTTMZ_LENGTH_KNOWN ) ? inf->length_s : 0;
    out->unplayable = ( inf->unplayable || g_cmttmz.skip[block_id] ) ? 1 : 0;
    out->recorded = inf->recorded;
    out->imported = inf->imported;
    out->unsaved = g_cmttmz.unsaved[block_id] ? 1 : 0;
    out->csw_rate = inf->csw_rate;
    out->csw_pulses = inf->csw_pulses;
    memcpy ( out->mzf_name, inf->mzf_name, sizeof ( out->mzf_name ) );
    out->mzf_name_len = inf->mzf_name_len;
    return EXIT_SUCCESS;
}


unsigned cmttmz_get_change_serial ( void ) {
    return g_cmttmz.change_serial + g_cmttmz.open_serial;
}


void cmttmz_note_change ( void ) {
    g_cmttmz.change_serial++;
}


int cmttmz_get_open_block_id ( void ) {
    if ( !g_cmt_tmz_extension->container || !g_cmt_tmz_extension->block ) return -1;
    return g_cmttmz.open_block_id;
}


en_CMTTMZ_SOURCE cmttmz_get_source ( void ) {
    return g_cmttmz.file ? g_cmttmz.source : CMTTMZ_SOURCE_TMZ;
}


/** @brief Nejvýš kolik nepřehratelných bloků varování otevření vyjmenuje (zbytek jako počet). */
#define CMTTMZ_WARN_MAX_LISTED 8


/**
 * @brief Do varování otevření připíše bloky, které player přehrát neumí.
 *
 * Text "Blocks the player cannot play (skipped): 3 (ID 0x19), 5 (ID 0x18)",
 * nejvýš CMTTMZ_WARN_MAX_LISTED bloků, zbytek "and N more". Totéž jde do
 * logu (stderr). Které bloky, bere z příznaku unplayable údajů bloků, který
 * právě vyplnila cmttmz_info_fill() (cmttmz_block_known_unplayable()).
 *
 * @pre g_cmttmz.file je otevřený soubor; cmttmz_info_fill() proběhla pro
 *      všechny bloky a nic jiného příznak unplayable zatím nenastavilo.
 */
static void cmttmz_warn_unplayable_blocks ( void ) {
    const st_TZX_FILE *f = g_cmttmz.file;
    char list[CMTTMZ_WARNING_SIZE];
    size_t len = 0;
    int listed = 0, more = 0;
    list[0] = '\0';
    for ( uint32_t i = 0; i < f->block_count; i++ ) {
        /* výsledek cmttmz_block_known_unplayable() z cmttmz_info_fill(): data bloku 0x18 se znovu neprocházejí */
        if ( !g_cmttmz.info[i].unplayable ) continue;
        if ( listed >= CMTTMZ_WARN_MAX_LISTED ) {
            more++;
            continue;
        }
        int n = snprintf ( list + len, sizeof ( list ) - len, "%s%u (ID 0x%02X)", listed ? ", " : "", i, f->blocks[i].id );
        if ( n > 0 && ( size_t ) n < sizeof ( list ) - len ) len += ( size_t ) n;
        listed++;
    }
    if ( !listed ) return;
    if ( more ) {
        cmttmz_warning_append ( "Blocks the player cannot play (skipped): %s and %d more", list, more );
        fprintf ( stderr, "%s: warning: blocks the player cannot play (skipped): %s and %d more\n",
                  cmtext_get_description ( g_cmt_tmz_extension ), list, more );
    } else {
        cmttmz_warning_append ( "Blocks the player cannot play (skipped): %s", list );
        fprintf ( stderr, "%s: warning: blocks the player cannot play (skipped): %s\n",
                  cmtext_get_description ( g_cmt_tmz_extension ), list );
    }
}


/**
 * @brief Callback rozšíření: otevře soubor pásky.
 *
 * Soubor převede import (cmttmz_import_file_speeds()) s výchozí rychlostí
 * a vlastními pulzy z g_cmt a normální polaritou WAV, sestaví index všech
 * bloků a otevře první blok se signálem průchodem playeru od začátku
 * pásky (cmttmz_open_from_start()). Páska bez takového bloku se
 * otevře také (block zůstane NULL, přehrávání se nespustí).
 *
 * Varování otevření (cmttmz_get_open_warning(), MCP emu_cmt_open pole
 * warning) a log (stderr): varování importu; bloky, jejichž druh player
 * přehrát neumí (cmttmz_warn_unplayable_blocks(), bez generování signálu);
 * žádný otevřený blok (páska bez přehratelného bloku, blok nejde otevřít
 * nebo přestavět požadovanou rychlostí). Že blok jiného druhu nedá signál
 * (např. meze generátoru), se pozná až při přehrávání - jen do logu.
 *
 * @param filename Cesta k souboru (UTF-8).
 * @return EXIT_SUCCESS; EXIT_FAILURE při chybě importu (text v logu přes
 *         baseui_error) nebo alokace; rozšíření pak nemá vloženou pásku.
 * @pre Volá se z vlákna emulátoru.
 */
static int cmttmz_container_install ( st_TZX_FILE *file, st_CMTTMZ_BLOCK_SPEED *speeds, en_CMTTMZ_SOURCE source,
                                      const char *filename, const uint8_t *wav_blocks, uint32_t wav_count );
static int cmttmz_mem_open ( st_HANDLER *h );


/**
 * @brief Volby importu podle aktuálního nastavení Virtual CMT (výchozí rychlost a pulzy).
 * @param[out] opts Volby.
 */
static void cmttmz_open_opts ( st_CMTTMZ_IMPORT_OPTS *opts ) {
    cmttmz_import_opts_init ( opts );
    opts->default_speed = g_cmt.mz_cmtspeed;
    opts->custom_pulses = g_cmt.mz_custom_pulses;
    opts->wav_polarity = CMT_STREAM_POLARITY_NORMAL;
}


/**
 * @brief Soubor pásky po importu, před vložením (první fáze otevření).
 *
 * @invariant file != NULL; speeds má file->block_count položek nebo je NULL.
 * Vlastnictví: vše patří struktuře, uvolní cmttmz_prepared_free() nebo
 * převezme cmttmz_open_prepared().
 */
struct st_CMTTMZ_PREPARED {
    st_TZX_FILE *file;                   /**< převedený soubor */
    st_CMTTMZ_BLOCK_SPEED *speeds;       /**< vedlejší pole rychlostí z importu (free()) */
    en_CMTTMZ_SOURCE source;             /**< zdrojový formát (index bloků) */
    char *filename;                      /**< cesta kontejneru (g_free()) */
    char warning[CMTTMZ_WARNING_SIZE];   /**< varování importu ("" = žádné) */
};


const char* cmttmz_get_prepare_error ( void ) {
    return g_cmttmz.prepare_error;
}


st_CMTTMZ_PREPARED* cmttmz_open_prepare ( const char *filename ) {
    g_cmttmz.prepare_error[0] = '\0';
    if ( !filename ) return NULL;

    printf ( "%s\nOpen: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), filename );

    st_CMTTMZ_IMPORT_OPTS opts;
    cmttmz_open_opts ( &opts );

    st_CMTTMZ_IMPORT_REPORT rep;
    st_CMTTMZ_BLOCK_SPEED *speeds = NULL;
    st_TZX_FILE *file = cmttmz_import_file_speeds ( filename, &opts, &rep, &speeds );
    if ( rep.warning[0] ) {
        fprintf ( stderr, "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), rep.warning );
    }
    if ( !file ) {
        free ( speeds );
        /* jedno hlášení uživateli dá volající (cmt_open_file(): důvod i jméno souboru) */
        snprintf ( g_cmttmz.prepare_error, sizeof ( g_cmttmz.prepare_error ), "%s", rep.error );
        fprintf ( stderr, "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), rep.error );
        return NULL;
    }
    st_CMTTMZ_PREPARED *prep = g_new0 ( st_CMTTMZ_PREPARED, 1 );
    prep->file = file;
    prep->speeds = speeds;
    prep->source = rep.source;
    prep->filename = g_strdup ( filename );
    snprintf ( prep->warning, sizeof ( prep->warning ), "%s", rep.warning );
    return prep;
}


void cmttmz_prepared_free ( st_CMTTMZ_PREPARED *prep ) {
    if ( !prep ) return;
    tzx_free ( prep->file );
    free ( prep->speeds );
    g_free ( prep->filename );
    g_free ( prep );
}


int cmttmz_open_prepared ( st_CMTTMZ_PREPARED *prep ) {
    cmttmz_eject ( );
    g_cmttmz.warning[0] = '\0';
    if ( !prep ) return EXIT_FAILURE;
    snprintf ( g_cmttmz.warning, sizeof ( g_cmttmz.warning ), "%s", prep->warning );
    /* soubor a rychlosti převezme vložená páska */
    st_TZX_FILE *file = prep->file;
    st_CMTTMZ_BLOCK_SPEED *speeds = prep->speeds;
    prep->file = NULL;
    prep->speeds = NULL;
    int rc = cmttmz_container_install ( file, speeds, prep->source, prep->filename, NULL, 0 );
    cmttmz_prepared_free ( prep );
    return rc;
}


/**
 * @brief Callback rozšíření cb_open: obě fáze otevření najednou.
 *
 * Bez rozdělení na fáze (cmt_open_file() je používá samo, aby neúspěšné
 * otevření nevysunulo pásku): cmttmz_open_prepare() a pak
 * cmttmz_open_prepared(). Při neúspěšném importu rozšíření pásku vysune
 * (dřívější chování callbacku).
 *
 * @param filename Cesta k souboru (UTF-8).
 * @return EXIT_SUCCESS; EXIT_FAILURE při chybě importu nebo alokace.
 * @pre Vlákno emulátoru.
 */
static int cmttmz_container_open ( char *filename ) {
    st_CMTTMZ_PREPARED *prep = cmttmz_open_prepare ( filename );
    if ( !prep ) {
        cmttmz_eject ( );
        g_cmttmz.warning[0] = '\0';
        return EXIT_FAILURE;
    }
    return cmttmz_open_prepared ( prep );
}


int cmttmz_open_image ( const uint8_t *data, uint32_t size, const char *path, const uint8_t *wav_blocks, uint32_t wav_count ) {
    cmttmz_eject ( );
    g_cmttmz.warning[0] = '\0';
    if ( !path ) path = "";
    if ( !data || !size ) return EXIT_FAILURE;

    printf ( "%s\nOpen embedded tape: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), path[0] ? path : "(new tape)" );

    /* jen hlavička (10 B: signatura, 1Ah, verze) = páska bez bloků; import
     * takovou odmítá ("No playable block"), staví ji cmttmz_new_tape() */
    if ( size == 10 && data[7] == 0x1A && ( !memcmp ( data, "TapeMZ!", 7 ) || !memcmp ( data, "ZXTape!", 7 ) ) ) {
        if ( EXIT_SUCCESS != cmttmz_new_tape ( ) ) return EXIT_FAILURE;
        /* hlavička tak, jak byla uložená (i ZXTape! a jiná verze), ne TMZ 1.1 nové pásky */
        memcpy ( &g_cmttmz.file->header, data, sizeof ( g_cmttmz.file->header ) );
        g_cmttmz.file->is_tmz = ( 0 == memcmp ( data, "TapeMZ!", 7 ) );
        if ( path[0] ) cmtext_container_set_filepath ( g_cmt_tmz_extension->container, path );
        g_cmttmz.modified = 1;
        return EXIT_SUCCESS;
    }

    st_CMTTMZ_IMPORT_OPTS opts;
    cmttmz_open_opts ( &opts );
    st_CMTTMZ_IMPORT_REPORT rep;
    st_CMTTMZ_BLOCK_SPEED *speeds = NULL;
    st_TZX_FILE *file = cmttmz_import_buffer_speeds ( CMTTMZ_SOURCE_TMZ, data, size, &opts, &rep, &speeds );
    if ( rep.warning[0] ) {
        snprintf ( g_cmttmz.warning, sizeof ( g_cmttmz.warning ), "%s", rep.warning );
        fprintf ( stderr, "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), rep.warning );
    }
    if ( !file ) {
        fprintf ( stderr, "%s: embedded tape: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), rep.error );
        return EXIT_FAILURE;
    }
    /* zdroj TMZ (obraz je tzx_save()); bloky 0x18 z importu WAV pozná jen příznak volajícího */
    if ( EXIT_SUCCESS != cmttmz_container_install ( file, speeds, CMTTMZ_SOURCE_TMZ, path, wav_blocks, wav_count ) ) return EXIT_FAILURE;
    if ( !path[0] ) {
        /* páska bez cesty: jméno prázdné jako u nové pásky (g_path_get_basename("") by dalo ".") */
        char *empty = ( char* ) baseui_tools_mem_alloc0 ( 1 );
        if ( empty ) {
            baseui_tools_mem_free ( g_cmt_tmz_extension->container->name );
            g_cmt_tmz_extension->container->name = empty;
        }
        /* páska bez souboru: žádný blok není uložený */
        memset ( g_cmttmz.unsaved, 1, file->block_count ? file->block_count : 1 );
    }
    /* páska se souborem: které bloky soubor nese, obraz neříká; bere se, že
     * všechny [neověřeno]; snapshot příznaky obnoví sám (cmttmz_set_block_unsaved()) */
    g_cmttmz.modified = 1;
    return EXIT_SUCCESS;
}


uint8_t* cmttmz_get_wav_blocks ( uint32_t *count ) {
    if ( count ) *count = 0;
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) return NULL;
    int n = g_cmt_tmz_extension->container->count_blocks;
    const st_CMTEXT_TAPE_INDEX *ix = g_cmt_tmz_extension->container->tape->index;
    int any = 0;
    for ( int i = 0; i < n && !any; i++ ) any = ( ix[i].bltype == CMTEXT_BLOCK_TYPE_WAV );
    if ( !any ) return NULL;
    uint8_t *flags = ( uint8_t* ) g_malloc0 ( ( gsize ) n );
    for ( int i = 0; i < n; i++ ) flags[i] = ( ix[i].bltype == CMTEXT_BLOCK_TYPE_WAV ) ? 1 : 0;
    if ( count ) *count = ( uint32_t ) n;
    return flags;
}


int cmttmz_save_image ( uint8_t **data, size_t *size ) {
    if ( !data || !size ) return EXIT_FAILURE;
    *data = NULL;
    *size = 0;
    if ( !g_cmttmz.file ) return EXIT_FAILURE;
    st_HANDLER h;
    if ( EXIT_SUCCESS != cmttmz_mem_open ( &h ) ) return EXIT_FAILURE;
    int rc = EXIT_FAILURE;
    if ( TZX_OK == tzx_save ( &h, g_cmttmz.file ) ) {
        uint8_t *copy = ( uint8_t* ) g_malloc ( h.spec.memspec.size ? h.spec.memspec.size : 1 );
        memcpy ( copy, h.spec.memspec.ptr, h.spec.memspec.size );
        *data = copy;
        *size = h.spec.memspec.size;
        rc = EXIT_SUCCESS;
    }
    generic_driver_close ( &h );
    return rc;
}


/**
 * @brief Vloží soubor pásky jako vloženou pásku rozšíření (index, kontejner, první blok).
 *
 * Převezme @p file a @p speeds (i při chybě, pak je uvolní cmttmz_eject()).
 * Sestaví i údaje bloků pro UI (cmttmz_info_fill(); všechny bloky uložené,
 * příznaky neuloženo 0, zdroj @p source). Délky bloků se při vložení
 * nepočítají (otevření pásky není pomalejší než bez okna Virtual CMT):
 * známá je jen délka otevřeného bloku (jeho stream, zadarmo), ostatní
 * doplní zásahy UI ve STOP (cmttmz_lengths_update_budget()) nebo otevření
 * bloku.
 *
 * @param file Soubor pásky (z importu). @param speeds Vedlejší pole rychlostí (smí být NULL).
 * @param source Zdrojový formát (index bloků). @param filename Cesta kontejneru ("" = bez cesty).
 * @param wav_blocks Příznaky původu WAV bloků 0x18 (obraz pásky, cmttmz_open_image()), NULL = podle @p source.
 * @param wav_count Počet položek @p wav_blocks.
 * @return EXIT_SUCCESS (i bez bloku se signálem, s varováním); EXIT_FAILURE při selhání alokace.
 * @pre Rozšíření bez vložené pásky (po cmttmz_eject()).
 */
static int cmttmz_container_install ( st_TZX_FILE *file, st_CMTTMZ_BLOCK_SPEED *speeds, en_CMTTMZ_SOURCE source,
                                      const char *filename, const uint8_t *wav_blocks, uint32_t wav_count ) {
    /* varování nástroje nad páskou patří pásce, při které vzniklo */
    cmttmz_set_edit_warning ( NULL );
    g_cmttmz.file = file;
    g_cmttmz.speeds = speeds;
    g_cmttmz.skip = ( uint8_t* ) calloc ( file->block_count ? file->block_count : 1, 1 );
    g_cmttmz.info = ( st_CMTTMZ_BLOCK_INFO* ) calloc ( file->block_count ? file->block_count : 1, sizeof ( st_CMTTMZ_BLOCK_INFO ) );
    /* všechny bloky jsou v souboru: příznaky neuloženo 0 */
    g_cmttmz.unsaved = ( uint8_t* ) calloc ( file->block_count ? file->block_count : 1, 1 );
    if ( !g_cmttmz.skip || !g_cmttmz.info || !g_cmttmz.unsaved ) {
        baseui_error ( "%s: Not enough memory\n", cmtext_get_description ( g_cmt_tmz_extension ) );
        cmttmz_eject ( );
        return EXIT_FAILURE;
    }
    g_cmttmz.source = source;

    int count = ( int ) file->block_count;
    st_CMTEXT_TAPE_INDEX *index = cmttmz_index_new ( source, wav_blocks, wav_count );
    st_CMTEXT_CONTAINER_TAPE *tape = index ? cmtext_container_tape_new ( NULL, index ) : NULL;
    if ( !tape ) {
        if ( index ) cmtext_container_tapeindex_destroy ( index, count );
        cmttmz_eject ( );
        return EXIT_FAILURE;
    }
    st_CMTEXT_CONTAINER *container = cmtext_container_new ( CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE, ( char* ) filename, count, tape,
                                                            cmttmz_container_next_block, cmttmz_container_previous_block,
                                                            cmttmz_container_open_block );
    if ( !container ) {
        cmtext_container_tape_destroy ( tape, count );
        cmttmz_eject ( );
        return EXIT_FAILURE;
    }
    g_cmt_tmz_extension->container = container;
    for ( int i = 0; i < count; i++ ) cmttmz_info_fill ( ( uint32_t ) i, 0 );

    printf ( "%s blocks: %d\n", cmtext_get_name ( g_cmt_tmz_extension ), count );

    en_CMTTMZ_OPEN_RESULT res;
    int failed_id;
    cmttmz_warn_unplayable_blocks ( );
    g_cmt_tmz_extension->block = cmttmz_open_from_start ( &res, &failed_id );
    g_cmttmz.open_block_id = g_cmt_tmz_extension->block ? g_cmt_tmz_extension->block->block_id : -1;
    g_cmttmz.open_serial++;
    g_cmttmz.tape_serial++;
    if ( !g_cmt_tmz_extension->block ) {
        const char *desc = cmtext_get_description ( g_cmt_tmz_extension );
        if ( res == CMTTMZ_OPEN_SPEED_ERROR ) {
            fprintf ( stderr, "%s: warning: block %d could not be rebuilt at the requested speed, no block is open\n",
                      desc, failed_id );
            cmttmz_warning_append ( "Block %d could not be rebuilt at the requested speed, no block is open", failed_id );
        } else if ( res == CMTTMZ_OPEN_NONE_LEFT ) {
            fprintf ( stderr, "%s: warning: the tape has no playable block\n", desc );
            cmttmz_warning_append ( "The tape has no playable block" );
        } else {
            fprintf ( stderr, "%s: warning: block %d could not be opened, no block is open\n", desc, failed_id );
            cmttmz_warning_append ( "Block %d could not be opened, no block is open", failed_id );
        }
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Callback rozšíření: konec emulátoru, vysune pásku.
 */
static void cmttmz_exit ( void ) {
    cmttmz_eject ( );
}


st_CMTEXT g_cmt_tmz_extension_def = {
                                     &g_cmt_tmz_info,
                                     ( st_CMTEXT_CONTAINER* ) NULL,
                                     ( st_CMTEXT_BLOCK* ) NULL,
                                     ( cmtext_cb_init ) NULL,
                                     cmttmz_exit,
                                     cmttmz_container_open,
                                     ( cmtext_cb_stop ) NULL,
                                     cmttmz_eject,
                                     ( cmtext_cb_write_data ) NULL,
};


/* ------------------------------------------------------------------------- */
/*  Nová páska, připojení bloků, příznak Modified, zpráva záznamu            */
/* ------------------------------------------------------------------------- */


/**
 * @brief Pauza, kterou dostane dosavadní poslední blok 0x46 před připojenými bloky (ms).
 *
 * Stejná hodnota jako pauza mezi bloky importu MZF / MZT
 * (cmttmz_import_opts_init(), pause_ms 1000).
 */
#define CMTTMZ_APPEND_GAP_MS 1000u


int cmttmz_new_tape ( void ) {
    cmttmz_eject ( );
    g_cmttmz.warning[0] = '\0';
    g_cmttmz.modified = 0;

    st_TZX_FILE *file = ( st_TZX_FILE* ) calloc ( 1, sizeof ( st_TZX_FILE ) );
    if ( !file ) return EXIT_FAILURE;
    tmz_header_init ( &file->header );   /* TMZ_VERSION_MAJOR.TMZ_VERSION_MINOR (1.1) */
    file->is_tmz = true;
    g_cmttmz.file = file;
    g_cmttmz.speeds = ( st_CMTTMZ_BLOCK_SPEED* ) calloc ( 1, sizeof ( st_CMTTMZ_BLOCK_SPEED ) );
    g_cmttmz.skip = ( uint8_t* ) calloc ( 1, 1 );
    g_cmttmz.info = ( st_CMTTMZ_BLOCK_INFO* ) calloc ( 1, sizeof ( st_CMTTMZ_BLOCK_INFO ) );
    g_cmttmz.unsaved = ( uint8_t* ) calloc ( 1, 1 );
    st_CMTEXT_TAPE_INDEX *index = cmtext_container_tape_index_aloc ( NULL, 0 );
    st_CMTEXT_CONTAINER_TAPE *tape = index ? cmtext_container_tape_new ( NULL, index ) : NULL;
    if ( !g_cmttmz.speeds || !g_cmttmz.skip || !g_cmttmz.info || !g_cmttmz.unsaved || !tape ) {
        if ( tape ) cmtext_container_tape_destroy ( tape, 0 );
        else if ( index ) cmtext_container_tapeindex_destroy ( index, 0 );
        cmttmz_eject ( );
        return EXIT_FAILURE;
    }
    st_CMTEXT_CONTAINER *container = cmtext_container_new ( CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE, "", 0, tape,
                                                            cmttmz_container_next_block, cmttmz_container_previous_block,
                                                            cmttmz_container_open_block );
    if ( !container ) {
        cmtext_container_tape_destroy ( tape, 0 );
        cmttmz_eject ( );
        return EXIT_FAILURE;
    }
    /* páska bez cesty: jméno prázdné (g_path_get_basename("") by dalo ".") */
    char *empty = ( char* ) baseui_tools_mem_alloc0 ( 1 );
    if ( empty ) {
        baseui_tools_mem_free ( container->name );
        container->name = empty;
    }
    g_cmt_tmz_extension->container = container;
    g_cmt_tmz_extension->block = NULL;
    g_cmttmz.open_block_id = -1;
    g_cmttmz.source = CMTTMZ_SOURCE_TMZ;
    g_cmttmz.open_serial++;
    g_cmttmz.tape_serial++;
    printf ( "%s\nNew empty tape\n", cmtext_get_description ( g_cmt_tmz_extension ) );
    return EXIT_SUCCESS;
}


/**
 * @brief Nastaví pauzu za blokem s měnitelnou rychlostí a blok přestaví (stejnou rychlostí).
 * @param block_id Index bloku (druh CMTTMZ_SPEED_KIND_MZF, ověřuje volající). @param pause_ms Pauza v ms.
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání přestavby (pauza zůstane původní).
 * @pre Vlákno emulátoru pod device_state_lock().
 */
static int cmttmz_set_block_pause ( int block_id, uint16_t pause_ms ) {
    st_CMTTMZ_BLOCK_SPEED *sp = &g_cmttmz.speeds[block_id];
    uint16_t before = sp->pause_ms;
    sp->pause_ms = pause_ms;
    st_TZX_BLOCK *nb = cmttmz_block_build ( block_id, sp->speed, &sp->pulses );
    if ( !nb ) {
        sp->pause_ms = before;
        return EXIT_FAILURE;
    }
    st_TZX_BLOCK old;
    cmttmz_file_swap_block ( block_id, nb, &old );
    free ( old.data );
    /* jiná pauza = jiná délka streamu; neznámou ji doplní zásah UI nebo otevření bloku */
    g_cmttmz.info[block_id].length_state = CMTTMZ_LENGTH_UNKNOWN;
    g_cmttmz.info[block_id].length_s = 0;
    return EXIT_SUCCESS;
}


/**
 * @brief Přestaví připojení bloků (cmttmz_append_gap_before()) blok, je-li dosavadní poslední?
 *
 * Jediný zásah připojení do dosavadních bloků: blok 0x46 MZ800EMU-CMT
 * (rychlost jde měnit, cmttmz_block_speed_kind() == MZF) s pauzou 0
 * dostane pauzu CMTTMZ_APPEND_GAP_MS. Podle toho značka checkpointu
 * (cmttmz_tape_mark_get()) kopíruje data posledního bloku jen tehdy, když
 * je připojení může změnit.
 *
 * @param block_id Index bloku (v rozsahu).
 * @return 1 = připojení za tento blok ho přestaví; 0 = blok zůstane bajtově stejný.
 * @pre Vlákno emulátoru (nebo pod device_state_lock()).
 */
static int cmttmz_append_rewrites ( int block_id ) {
    return ( cmttmz_block_speed_kind ( block_id ) == CMTTMZ_SPEED_KIND_MZF ) && ( g_cmttmz.speeds[block_id].pause_ms == 0 );
}


/**
 * @brief Dá dosavadnímu poslednímu bloku 0x46 MZ800EMU-CMT s pauzou 0 pauzu CMTTMZ_APPEND_GAP_MS.
 *
 * Import dává poslednímu bloku pásky pauzu 0; po připojení dalších bloků
 * by mezi ním a novým záznamem nebyla mezera. Přestaví se jen blok, jehož
 * rychlost jde měnit (cmttmz_append_rewrites()), stejnou rychlostí, se
 * kterou je v souboru sestaven; jiné bloky se nemění. Otevřený blok si do
 * dalšího otevření nechá dosavadní stream. Selhání přestavby se jen vypíše
 * (blok zůstane s pauzou 0).
 *
 * @param block_id Index dosavadního posledního bloku.
 * @pre Vlákno emulátoru pod device_state_lock().
 */
static void cmttmz_append_gap_before ( int block_id ) {
    if ( !cmttmz_append_rewrites ( block_id ) ) return;
    if ( EXIT_SUCCESS != cmttmz_set_block_pause ( block_id, CMTTMZ_APPEND_GAP_MS ) ) {
        fprintf ( stderr, "%s: warning: block %d: can't add a pause before the following blocks\n",
                  cmtext_get_description ( g_cmt_tmz_extension ), block_id );
    }
}


#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: volá se při každém vložení bloků (cmttmz_edit_apply(): připojení záznamu i cmttmz_insert_blocks()) po nasazení nového indexu a vyplnění údajů pro UI vložených bloků (NULL = nic). */
static void ( *s_test_append_hook ) ( void ) = NULL;


void cmttmz_test_set_append_hook ( void ( *hook ) ( void ) ) {
    s_test_append_hook = hook;
}


int cmttmz_test_get_side ( int block_id, st_CMTTMZ_BLOCK_SPEED *speed, int *skip ) {
    if ( !g_cmttmz.file || block_id < 0 || ( uint32_t ) block_id >= g_cmttmz.file->block_count ) return EXIT_FAILURE;
    if ( speed ) *speed = g_cmttmz.speeds[block_id];
    if ( skip ) *skip = g_cmttmz.skip[block_id];
    return EXIT_SUCCESS;
}


/**
 * @brief Skutečná velikost alokace z malloc() / g_malloc() (0 pro NULL, -1 = neumí zjistit).
 * @param p Ukazatel z malloc(), realloc() nebo g_malloc().
 */
static long long cmttmz_test_alloc_size ( void *p ) {
    if ( !p ) return 0;
#if defined(_WIN32)
    return ( long long ) _msize ( p );
#elif defined(__GLIBC__)
    return ( long long ) malloc_usable_size ( p );
#else
    return -1;
#endif
}


int cmttmz_test_get_side_alloc ( long long *speeds_bytes, long long *skip_bytes, long long *index_bytes ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) return EXIT_FAILURE;
    long long s = cmttmz_test_alloc_size ( g_cmttmz.speeds );
    long long k = cmttmz_test_alloc_size ( g_cmttmz.skip );
    long long i = cmttmz_test_alloc_size ( g_cmt_tmz_extension->container->tape->index );
    if ( s < 0 || k < 0 || i < 0 ) return EXIT_FAILURE;
    if ( speeds_bytes ) *speeds_bytes = s;
    if ( skip_bytes ) *skip_bytes = k;
    if ( index_bytes ) *index_bytes = i;
    return EXIT_SUCCESS;
}
#endif


/* ------------------------------------------------------------------------- */
/*  Úpravy pásky: vložení, mazání, přesun bloků (jedna údržba stavu)         */
/* ------------------------------------------------------------------------- */


/** @brief Druh úpravy pásky (cmttmz_edit_apply()). */
typedef enum en_CMTTMZ_EDIT_KIND {
    CMTTMZ_EDIT_INSERT = 0, /**< vložení bloků před pozici at (at == počet bloků = připojení na konec) */
    CMTTMZ_EDIT_REMOVE,     /**< smazání bloků ids (vzestupně bez opakování) */
    CMTTMZ_EDIT_MOVE,       /**< přesun bloku from na pozici to (from != to) */
    CMTTMZ_EDIT_REPLACE,    /**< nahrazení bloku at bloky blocks (count > 0; analýza surového bloku) */
} en_CMTTMZ_EDIT_KIND;


/**
 * @brief Úprava pásky pro cmttmz_edit_apply(); parametry ověřil volající.
 *
 * Vlastnictví: blocks a speeds patří volajícímu; data bloků převezme páska
 * jen při úspěchu (funkce je pak v poli nastaví na NULL).
 */
typedef struct st_CMTTMZ_EDIT {
    en_CMTTMZ_EDIT_KIND kind;            /**< druh úpravy */
    uint32_t at;                         /**< INSERT: pozice 0 .. počet bloků; REPLACE: nahrazovaný blok (< počet bloků) */
    st_TZX_BLOCK *blocks;                /**< INSERT, REPLACE: vkládané bloky (count položek) */
    const st_CMTTMZ_BLOCK_SPEED *speeds; /**< INSERT, REPLACE: záznamy rychlosti vkládaných bloků (count položek) */
    uint32_t count;                      /**< INSERT, REPLACE: počet vkládaných bloků (REPLACE > 0); REMOVE: počet mazaných bloků (> 0) */
    int recorded;                        /**< INSERT: 1 = bloky připojil záznam (st_CMTTMZ_BLOCK_UI::recorded); REPLACE 0 */
    int imported;                        /**< INSERT: 1 = bloky vložil import souboru (cmttmz_import_file_at(), st_CMTTMZ_BLOCK_UI::imported); jinak 0 (záznam, analýza, nástroje nad páskou, REPLACE) */
    en_CMTTMZ_SOURCE source;             /**< INSERT, REPLACE: zdroj vkládaných bloků pro index (CMTTMZ_SOURCE_WAV = blok 0x18 typu WAV, jinak 0x18 jako TZX) */
    const int *ids;                      /**< REMOVE: čísla mazaných bloků vzestupně bez opakování, v rozsahu */
    uint32_t from;                       /**< MOVE: číslo přesouvaného bloku */
    uint32_t to;                         /**< MOVE: pozice bloku po přesunu */
} st_CMTTMZ_EDIT;


/**
 * @brief Vedlejší pole a index pásky po úpravě, připravené dřív, než se páska změní.
 *
 * Vlastnictví: všechna pole patří struktuře (cmttmz_side_free()), dokud je
 * cmttmz_side_commit() nenasadí do g_cmttmz a kontejneru (pak tam patří
 * speeds, skip, info, unsaved a index; src a kept dál struktuře).
 *
 * @invariant src, speeds, skip, info, unsaved mají max(count, 1) položek,
 *            index count + 1 (cmtext_container_tape_index_aloc()); kept má
 *            max(dosavadní počet bloků, 1) položek.
 * @invariant Položka indexu vkládaného bloku (src < 0) vlastní své jméno;
 *            položka dosavadního bloku je mělká kopie (jméno vlastní
 *            dosavadní index, po nasazení nový).
 */
typedef struct st_CMTTMZ_SIDE {
    uint32_t count;                 /**< počet bloků po úpravě */
    int32_t *src;                   /**< původ bloku i: >= 0 = dosavadní číslo bloku, < 0 = vkládaný blok číslo -(src + 1) */
    uint8_t *kept;                  /**< 1 = dosavadní blok j zůstává na pásce, 0 = maže se */
    st_CMTTMZ_BLOCK_SPEED *speeds;  /**< nové pole rychlostí */
    uint8_t *skip;                  /**< nové pole příznaků přeskočení */
    st_CMTTMZ_BLOCK_INFO *info;     /**< nové pole údajů pro UI */
    uint8_t *unsaved;               /**< nové pole příznaků neuloženo */
    st_CMTEXT_TAPE_INDEX *index;    /**< nový index kontejneru */
} st_CMTTMZ_SIDE;


/**
 * @brief Uvolní jméno, které vlastní položka indexu (blok MZF, hlavička TAP).
 * @param e Položka indexu.
 * @post Položka žádnou paměť nevlastní (jméno NULL).
 */
static void cmttmz_index_entry_free_name ( st_CMTEXT_TAPE_INDEX *e ) {
    if ( e->bltype == CMTEXT_BLOCK_TYPE_MZF ) {
        baseui_tools_mem_free ( e->item.mzf.fname );
        e->item.mzf.fname = NULL;
    } else if ( e->bltype == CMTEXT_BLOCK_TYPE_TAPHEADER ) {
        baseui_tools_mem_free ( e->item.taphdr.fname );
        e->item.taphdr.fname = NULL;
    }
}


/**
 * @brief Uvolní připravená pole úpravy (cmttmz_side_prepare()).
 * @param s Připravená pole.
 * @param inserted_names Počet prvních pozic, jejichž položky indexu vkládaných
 *        bloků už vlastní jméno (uvolní se); 0 po nasazení (jména patří indexu).
 * @post Struktura je vynulovaná.
 */
static void cmttmz_side_free ( st_CMTTMZ_SIDE *s, uint32_t inserted_names ) {
    if ( s->index && s->src ) {
        for ( uint32_t i = 0; i < inserted_names && i < s->count; i++ ) {
            if ( s->src[i] < 0 ) cmttmz_index_entry_free_name ( &s->index[i] );
        }
    }
    free ( s->src );
    free ( s->kept );
    free ( s->speeds );
    free ( s->skip );
    free ( s->info );
    free ( s->unsaved );
    if ( s->index ) baseui_tools_mem_free ( s->index );
    memset ( s, 0, sizeof ( *s ) );
}


/**
 * @brief Připraví vedlejší pole a index pásky po úpravě (páska se nemění).
 *
 * Sestaví mapu nových pozic na dosavadní bloky (src), zkopíruje údaje
 * dosavadních bloků na nové pozice (rychlost, skip, údaje pro UI, příznak
 * neuloženo, položku indexu s jejím režimem rychlosti a typem WAV) a vyplní
 * údaje vkládaných bloků: záznam rychlosti z úpravy, skip 0, neuloženo 1,
 * položka indexu z bloku úpravy (cmttmz_index_fill_block(), blok 0x18 jako
 * TZX). Údaje pro UI vkládaných bloků vyplní až cmttmz_edit_apply() nad
 * změněnou páskou. Všem položkám indexu nastaví block_id = offset = pozice.
 *
 * @param e Úprava (ověřená). @param[out] s Připravená pole.
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace (nic se nemění, *s vynulovaná).
 * @pre g_cmttmz.file a kontejner existují.
 */
static int cmttmz_side_prepare ( const st_CMTTMZ_EDIT *e, st_CMTTMZ_SIDE *s ) {
    memset ( s, 0, sizeof ( *s ) );
    uint32_t old = g_cmttmz.file->block_count;
    uint32_t n = old;
    if ( e->kind == CMTTMZ_EDIT_INSERT ) n = old + e->count;
    else if ( e->kind == CMTTMZ_EDIT_REMOVE ) n = old - e->count;
    else if ( e->kind == CMTTMZ_EDIT_REPLACE ) n = old - 1 + e->count;
    size_t cap = n ? n : 1;
    s->count = n;
    s->src = ( int32_t* ) malloc ( cap * sizeof ( int32_t ) );
    s->kept = ( uint8_t* ) calloc ( old ? old : 1, 1 );
    s->speeds = ( st_CMTTMZ_BLOCK_SPEED* ) malloc ( cap * sizeof ( st_CMTTMZ_BLOCK_SPEED ) );
    s->skip = ( uint8_t* ) malloc ( cap );
    s->info = ( st_CMTTMZ_BLOCK_INFO* ) malloc ( cap * sizeof ( st_CMTTMZ_BLOCK_INFO ) );
    s->unsaved = ( uint8_t* ) malloc ( cap );
    s->index = cmtext_container_tape_index_aloc ( NULL, ( int ) n );
    if ( !s->src || !s->kept || !s->speeds || !s->skip || !s->info || !s->unsaved || !s->index ) {
        cmttmz_side_free ( s, 0 );
        return EXIT_FAILURE;
    }

    /* mapa nových pozic na dosavadní bloky */
    switch ( e->kind ) {
        case CMTTMZ_EDIT_INSERT:
            for ( uint32_t i = 0; i < n; i++ ) {
                if ( i < e->at ) s->src[i] = ( int32_t ) i;
                else if ( i < e->at + e->count ) s->src[i] = -( int32_t ) ( i - e->at ) - 1;
                else s->src[i] = ( int32_t ) ( i - e->count );
            }
            break;
        case CMTTMZ_EDIT_REPLACE:
            /* blok at zmizí (kept[at] zůstane 0), na jeho místě vkládané bloky */
            for ( uint32_t i = 0; i < n; i++ ) {
                if ( i < e->at ) s->src[i] = ( int32_t ) i;
                else if ( i < e->at + e->count ) s->src[i] = -( int32_t ) ( i - e->at ) - 1;
                else s->src[i] = ( int32_t ) ( i - e->count + 1 );
            }
            break;
        case CMTTMZ_EDIT_REMOVE:
        {
            uint32_t k = 0, i = 0;
            for ( uint32_t j = 0; j < old; j++ ) {
                if ( k < e->count && ( uint32_t ) e->ids[k] == j ) {
                    k++;
                    continue;
                }
                s->src[i++] = ( int32_t ) j;
            }
            break;
        }
        case CMTTMZ_EDIT_MOVE:
        default:
        {
            uint32_t j = 0;
            for ( uint32_t p = 0; p < n; p++ ) {
                if ( p == e->to ) {
                    s->src[p] = ( int32_t ) e->from;
                    continue;
                }
                if ( j == e->from ) j++;
                s->src[p] = ( int32_t ) j++;
            }
            break;
        }
    }

    const st_CMTEXT_TAPE_INDEX *ix = g_cmt_tmz_extension->container->tape->index;
    for ( uint32_t i = 0; i < n; i++ ) {
        int32_t from = s->src[i];
        if ( from >= 0 ) {
            s->kept[from] = 1;
            s->speeds[i] = g_cmttmz.speeds[from];
            s->skip[i] = g_cmttmz.skip[from];
            s->info[i] = g_cmttmz.info[from];
            s->unsaved[i] = g_cmttmz.unsaved[from];
            s->index[i] = ix[from];   /* mělká kopie: jméno převezme nový index při nasazení */
            s->index[i].block_id = ( int ) i;
            s->index[i].offset = i;
            continue;
        }
        uint32_t j = ( uint32_t ) ( -from - 1 );
        s->speeds[i] = e->speeds[j];
        s->skip[i] = 0;
        memset ( &s->info[i], 0, sizeof ( s->info[i] ) );
        s->unsaved[i] = 1;
        /* 0x18 ze záznamu jako TZX (nese úroveň PC1, přepínač polarity na něj
         * nepůsobí), z importu WAV jako WAV (jako po otevření WAV) */
        if ( EXIT_SUCCESS != cmttmz_index_fill_block ( &e->blocks[j], &e->speeds[j], i, e->source, 0, &s->index[i] ) ) {
            cmttmz_side_free ( s, i );
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Nasadí připravená pole: uvolní jména mazaných bloků a dosavadní pole, nastaví count_blocks.
 *
 * @param s Připravená pole; pole speeds, skip, info, unsaved a index přejdou
 *        na pásku (ve struktuře NULL), src a kept zůstanou struktuře.
 * @pre Soubor pásky už je změněný (g_cmttmz.file->block_count == s->count),
 *      kontejner má ještě dosavadní count_blocks a index.
 * @post Kontejner má nový index a count_blocks == s->count, g_cmttmz nová
 *       vedlejší pole; dosavadní pole a index jsou uvolněné.
 */
static void cmttmz_side_commit ( st_CMTTMZ_SIDE *s ) {
    st_CMTEXT_CONTAINER *c = g_cmt_tmz_extension->container;
    st_CMTEXT_TAPE_INDEX *oi = c->tape->index;
    for ( int j = 0; j < c->count_blocks; j++ ) {
        if ( !s->kept[j] ) cmttmz_index_entry_free_name ( &oi[j] );
    }
    baseui_tools_mem_free ( oi );
    c->tape->index = s->index;
    free ( g_cmttmz.speeds );
    free ( g_cmttmz.skip );
    free ( g_cmttmz.info );
    free ( g_cmttmz.unsaved );
    g_cmttmz.speeds = s->speeds;
    g_cmttmz.skip = s->skip;
    g_cmttmz.info = s->info;
    g_cmttmz.unsaved = s->unsaved;
    c->count_blocks = ( int ) s->count;
    s->index = NULL;
    s->speeds = NULL;
    s->skip = NULL;
    s->info = NULL;
    s->unsaved = NULL;
}


/**
 * @brief Je na vložené pásce blok 0x43 (MZ Machine Info, takt CPU bloků za ním)?
 * @return 1 = ano, 0 = ne.
 * @pre g_cmttmz.file != NULL (páska vložená).
 */
static int cmttmz_file_has_clock ( void ) {
    for ( uint32_t i = 0; i < g_cmttmz.file->block_count; i++ ) {
        if ( g_cmttmz.file->blocks[i].id == TMZ_BLOCK_ID_MZ_MACHINE_INFO ) return 1;
    }
    return 0;
}


/**
 * @brief Po úpravě s posunem čísel zavře otevřený blok a otevře blok kurzoru.
 *
 * Stav playeru se přestaví (odkazoval dosavadní čísla bloků): blok kurzoru
 * se otevře přímo (cmttmz_block_open_first(), bez informačního výpisu),
 * pozice v bloku je 0. Nejde-li blok @p target otevřít (CONTROL, skip),
 * zkusí se nejbližší další, pak nejbližší předchozí blok se signálem.
 * Bez otevřeného bloku před úpravou se páska převine (cmttmz_open_from_start()).
 * Nejde-li otevřít nic, otevřený blok chybí a player stojí na začátku pásky.
 *
 * @param had_block 1 = před úpravou byl otevřený blok.
 * @param target Pozice bloku kurzoru po úpravě (-1 = žádná, prázdná páska).
 * @post g_cmttmz.open_block_id odpovídá g_cmt_tmz_extension->block.
 */
static void cmttmz_edit_reopen ( int had_block, int target ) {
    cmttmz_block_close ( g_cmt_tmz_extension->block );
    g_cmt_tmz_extension->block = NULL;
    g_cmttmz.open_block_id = -1;
    cmttmz_player_init_at ( &g_cmttmz.player, 0 );
    st_CMTEXT_BLOCK *block = NULL;
    en_CMTTMZ_OPEN_RESULT res;
    int failed_id;
    g_cmttmz.quiet = 1;
    if ( !had_block ) {
        block = cmttmz_open_from_start ( &res, &failed_id );
    } else if ( target >= 0 ) {
        block = cmttmz_block_open_first ( target, +1, &res, &failed_id );
        if ( !block ) block = cmttmz_block_open_first ( target - 1, -1, &res, &failed_id );
    }
    g_cmttmz.quiet = 0;
    if ( block ) {
        g_cmt_tmz_extension->block = block;
        g_cmttmz.open_block_id = block->block_id;
    } else {
        cmttmz_player_init_at ( &g_cmttmz.player, 0 );
    }
}


/**
 * @brief Provede úpravu pásky: jediné místo údržby souboru, indexu, vedlejších polí a čítačů.
 *
 * Postup (při chybě v krocích 1 a 2 zůstane páska celá beze změny):
 * 1. cmttmz_side_prepare(): nová vedlejší pole a index (alokace),
 * 2. soubor: vložení (tzx_file_append_block() a posun tzx_file_move_block()),
 *    mazání (tzx_file_remove_block() od konce), přesun (tzx_file_move_block()),
 *    nahrazení (připojení nových bloků, pak bez alokace smazání
 *    nahrazovaného bloku a posun nových na jeho místo),
 * 3. cmttmz_side_commit(), převzetí dat vložených bloků, údaje pro UI
 *    vložených bloků (cmttmz_info_fill(); příznak imported jen u importu
 *    souboru, st_CMTTMZ_EDIT::imported), hlavička (blok 0x40 až 0x4F:
 *    ZXTape! -> TMZ 1.1, TMZ 1.0 -> 1.1),
 * 4. při posunu čísel a bloku 0x43 na pásce všechny známé délky neznámé
 *    (takt CPU bloku závisí na blocích před ním),
 *    u mazání, přesunu a nahrazení s blokem 0x43 navíc porovnání účinného
 *    taktu CPU dosavadních bloků 0x46 před a po úpravě: rozdíl = varování
 *    v g_cmttmz.edit_warning (cmttmz_get_edit_warning();
 *    kanál se na začátku každé úpravy vymaže - jediný kanál varování úprav
 *    i nástrojů nad páskou, nástroje přidávají svá varování až po svých úpravách),
 * 5. pauza na švu (cmttmz_append_gap_before()): vložení - blok před
 *    vloženými a poslední vložený, je-li za ním blok; přesun - blok před
 *    novou pozicí a přesunutý blok, je-li za ním blok; nahrazení - jen
 *    poslední nový blok, je-li za ním blok (blok před nahrazeným na něj
 *    navazoval už dřív); mazání nic,
 * 6. čítač otevření, Modified 1; u každé úpravy kromě připojení záznamu
 *    navíc čítač výměn pásky a změn, úklid stavu CMT hacku nad páskou
 *    (cmthack_tape_state_clear()) a kurzor (cmttmz_edit_reopen(): blok,
 *    který byl otevřený, nebo blok, který zaujal místo smazaného, nebo nový
 *    poslední; bez otevřeného bloku převinutí, takže po vložení do prázdné
 *    pásky je otevřený blok 0 a PLAY funguje),
 * 7. levný výpočet délek vložených a přestavěných bloků.
 * Připojení záznamu (cmttmz_append_blocks(), keep_numbers: vložení na konec
 * s recorded 1) nechá otevřený blok, pozici, stav playeru, stav CMT hacku
 * i čítač výměn pásky (původní chování záznamu, z doby před úpravami pásky).
 * Veřejné cmttmz_insert_blocks() je úprava s čítačem výměn pásky a kurzorem
 * pro každé at, i na konec.
 *
 * Kroky po nasazení, které alokují (přestavba švu, otevření bloku), úpravu
 * nevracejí: jejich selhání jen vypíše varování, nechá blok s pauzou 0 nebo
 * pásku bez otevřeného bloku; páska je i tak konzistentní.
 *
 * @param e Úprava (ověřená volajícím, neprázdná).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace (páska beze změny,
 *         text chyby v g_cmttmz.edit_error).
 * @pre Vlákno emulátoru pod device_state_lock(); páska vložená.
 */
static int cmttmz_edit_apply ( st_CMTTMZ_EDIT *e ) {
    st_TZX_FILE *f = g_cmttmz.file;
    uint32_t old = f->block_count;
    /* jen připojení záznamu nechává čísla, otevřený blok a stav hacku */
    int keep_numbers = ( e->kind == CMTTMZ_EDIT_INSERT ) && e->recorded && ( e->at == old );
    int had_clock = cmttmz_file_has_clock ( );
    g_cmttmz.edit_warning[0] = '\0';

    st_CMTTMZ_SIDE s;
    if ( EXIT_SUCCESS != cmttmz_side_prepare ( e, &s ) ) {
        cmttmz_set_edit_error ( "Not enough memory" );
        return EXIT_FAILURE;
    }
    uint32_t n = s.count;
    /* takt CPU bloků před mazáním, přesunem a nahrazením (vložení hlásí import sám); bez bloku 0x43
     * na pásce hrají všechny bloky výchozím taktem a nic se změnit nemůže */
    uint32_t *old_clock = NULL;
    if ( e->kind != CMTTMZ_EDIT_INSERT && had_clock && old ) {
        old_clock = ( uint32_t* ) malloc ( ( size_t ) old * sizeof ( uint32_t ) );
        if ( old_clock ) cmttmz_clock_contexts ( f, old_clock );
    }

    /* kurzor: dosavadní otevřený blok na nové pozici, nebo blok, který zaujal místo smazaného */
    int had_block = ( g_cmt_tmz_extension->block != NULL );
    int cur = had_block ? g_cmt_tmz_extension->block->block_id : -1;
    int target = -1;
    if ( cur >= 0 ) {
        for ( uint32_t p = 0; p < n && target < 0; p++ ) {
            if ( s.src[p] == cur ) target = ( int ) p;
        }
        if ( target < 0 ) {
            target = 0;
            for ( uint32_t p = 0; p < n; p++ ) {
                if ( s.src[p] >= 0 && s.src[p] < cur ) target++;
            }
            if ( ( uint32_t ) target >= n ) target = ( int ) n - 1;
        }
    }

    /* soubor pásky */
    switch ( e->kind ) {
        case CMTTMZ_EDIT_INSERT:
            for ( uint32_t j = 0; j < e->count; j++ ) {
                if ( TZX_OK != tzx_file_append_block ( f, &e->blocks[j] ) ) {
                    /* data zatím patří i volajícímu: jen zkrátit počet */
                    f->block_count = old;
                    cmttmz_side_free ( &s, n );
                    free ( old_clock );
                    cmttmz_set_edit_error ( "Not enough memory" );
                    return EXIT_FAILURE;
                }
            }
            /* připojené bloky na místo (jen posun položek pole, bez alokace) */
            for ( uint32_t j = 0; e->at < old && j < e->count; j++ ) {
                ( void ) tzx_file_move_block ( f, old + j, e->at + j );
            }
            break;
        case CMTTMZ_EDIT_REPLACE:
            for ( uint32_t j = 0; j < e->count; j++ ) {
                if ( TZX_OK != tzx_file_append_block ( f, &e->blocks[j] ) ) {
                    f->block_count = old;
                    cmttmz_side_free ( &s, n );
                    /* takt dosavadních bloků vzniká jen mimo vložení, tedy i tady */
                    free ( old_clock );
                    cmttmz_set_edit_error ( "Not enough memory" );
                    return EXIT_FAILURE;
                }
            }
            /* od tady bez alokace: smazání nahrazovaného bloku (uvolní jeho data)
             * a posun připojených bloků na jeho místo */
            ( void ) tzx_file_remove_block ( f, e->at );
            for ( uint32_t j = 0; e->at + 1 < old && j < e->count; j++ ) {
                ( void ) tzx_file_move_block ( f, old - 1 + j, e->at + j );
            }
            break;
        case CMTTMZ_EDIT_REMOVE:
            for ( uint32_t k = e->count; k-- > 0; ) ( void ) tzx_file_remove_block ( f, ( uint32_t ) e->ids[k] );
            break;
        case CMTTMZ_EDIT_MOVE:
        default:
            ( void ) tzx_file_move_block ( f, e->from, e->to );
            break;
    }

    /* index a vedlejší pole */
    cmttmz_side_commit ( &s );
    int has_mz = 0;
    int inserts = ( e->kind == CMTTMZ_EDIT_INSERT ) || ( e->kind == CMTTMZ_EDIT_REPLACE );
    for ( uint32_t j = 0; inserts && j < e->count; j++ ) {
        if ( e->blocks[j].id >= 0x40 && e->blocks[j].id <= 0x4F ) has_mz = 1;
        e->blocks[j].data = NULL; /* data převzal soubor */
    }
    for ( uint32_t i = 0; i < n; i++ ) {
        if ( s.src[i] < 0 ) {
            cmttmz_info_fill ( i, e->recorded );
            /* původ bloku pro UI: jen blok z importu souboru; bloky analýzy,
             * informací o pásce a převodu profilu (vložení i nahrazení) importované nejsou */
            g_cmttmz.info[i].imported = e->imported ? 1 : 0;
        }
    }
#ifdef MZTEST_HEADLESS
    if ( inserts && s_test_append_hook ) s_test_append_hook ( );
#endif
    if ( has_mz && !f->is_tmz ) {
        /* páska ZXTape! s blokem MZ už není TZX: hlavička TMZ (verze 1.1) */
        tmz_header_init ( &f->header );
        f->is_tmz = true;
    } else if ( has_mz && f->header.ver_major == 1 && f->header.ver_minor < 1 ) {
        f->header.ver_minor = 1;   /* blok 0x46 je až v TMZ 1.1 */
    }

    /* mazání a přesun: hraje některý blok 0x46 jiným účinným taktem než před úpravou? */
    if ( old_clock ) {
        uint32_t *new_clock = n ? ( uint32_t* ) malloc ( ( size_t ) n * sizeof ( uint32_t ) ) : NULL;
        if ( new_clock ) {
            cmttmz_clock_contexts ( f, new_clock );
            for ( uint32_t p = 0; p < n; p++ ) {
                if ( f->blocks[p].id != TMZ_BLOCK_ID_MZ_FILE || s.src[p] < 0 ) continue;
                if ( old_clock[s.src[p]] != new_clock[p] ) {
                    snprintf ( g_cmttmz.edit_warning, sizeof ( g_cmttmz.edit_warning ), "%s", CMTTMZ_CLOCK_WARNING );
                    break;
                }
            }
            free ( new_clock );
        }
        free ( old_clock );
        old_clock = NULL;
    }

    /* takt CPU z bloku 0x43 platí pro bloky za ním: po posunu čísel žádná délka nemusí platit */
    if ( !keep_numbers && ( had_clock || cmttmz_file_has_clock ( ) ) ) {
        for ( uint32_t i = 0; i < n; i++ ) {
            if ( g_cmttmz.info[i].length_state != CMTTMZ_LENGTH_KNOWN ) continue;
            g_cmttmz.info[i].length_state = CMTTMZ_LENGTH_UNKNOWN;
            g_cmttmz.info[i].length_s = 0;
        }
    }

    /* pauza na švu a rozsah levného výpočtu délek */
    int lo = 0, hi = 0;
    if ( e->kind == CMTTMZ_EDIT_INSERT ) {
        if ( e->at > 0 ) cmttmz_append_gap_before ( ( int ) e->at - 1 );
        if ( e->at + e->count < n ) cmttmz_append_gap_before ( ( int ) ( e->at + e->count ) - 1 );
        lo = ( e->at > 0 ) ? ( int ) e->at - 1 : 0;
        hi = ( int ) ( e->at + e->count );
    } else if ( e->kind == CMTTMZ_EDIT_MOVE ) {
        if ( e->to > 0 ) cmttmz_append_gap_before ( ( int ) e->to - 1 );
        if ( e->to + 1 < n ) cmttmz_append_gap_before ( ( int ) e->to );
        lo = ( e->to > 0 ) ? ( int ) e->to - 1 : 0;
        hi = ( int ) e->to + 1;
    } else if ( e->kind == CMTTMZ_EDIT_REPLACE ) {
        /* šev jen za posledním novým blokem (jako poslední vložený); blok před
         * nahrazeným blokem na něj navazoval už dřív, jeho pauza zůstává */
        if ( e->at + e->count < n ) cmttmz_append_gap_before ( ( int ) ( e->at + e->count ) - 1 );
        lo = ( int ) e->at;
        hi = ( int ) ( e->at + e->count );
    }

    g_cmttmz.open_serial++;
    g_cmttmz.modified = 1;
    if ( !keep_numbers ) {
        /* čísla bloků přestala platit: výběr v okně a zásahy s číslem bloku */
        g_cmttmz.tape_serial++;
        g_cmttmz.change_serial++;
        /* hlavička načtená hackem bez těla a konec pásky odkazují bloky číslem */
        cmthack_tape_state_clear ( );
        cmttmz_edit_reopen ( had_block, target );
    }
    /* délky vložených a přestavěných bloků jen levně: surový blok 0x18 jedním
     * průchodem daty, blok MZF zůstane neznámý pro zásah UI */
    if ( hi > lo ) cmttmz_lengths_pass ( CMTTMZ_LENGTHS_CHEAP, lo, hi, 0 );
    cmttmz_side_free ( &s, 0 );
    g_cmttmz.edit_error[0] = '\0';
    return EXIT_SUCCESS;
}


/**
 * @brief Vložení bloků (společné tělo cmttmz_insert_blocks(), cmttmz_append_blocks() a importu souboru).
 *
 * @param at Pozice. @param blocks Bloky. @param speeds Záznamy rychlosti. @param count Počet.
 * @param recorded 1 = bloky připojil záznam (údaj pro UI "vznikl záznamem").
 * @param imported 1 = bloky vložil import souboru (cmttmz_import_file_at(); údaj pro UI
 *        "Imported"); 0 = záznam a cmttmz_insert_blocks() (analýza,
 *        informace o pásce).
 * @param source Zdroj bloků pro index: CMTTMZ_SOURCE_WAV = blok 0x18 typu WAV
 *        (import WAV, přepínač polarity na něj působí), jinak CMTTMZ_SOURCE_TMZ
 *        (blok 0x18 typu TZX; záznam, cmttmz_insert_blocks()); bloky MZF a TAP
 *        typ dostanou podle obsahu.
 * @return Jako cmttmz_insert_blocks().
 */
static int cmttmz_insert_internal ( int at, st_TZX_BLOCK *blocks, const st_CMTTMZ_BLOCK_SPEED *speeds, uint32_t count,
                                    int recorded, int imported, en_CMTTMZ_SOURCE source ) {
    g_cmttmz.edit_error[0] = '\0';
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) {
        cmttmz_set_edit_error ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    if ( count && ( !blocks || !speeds ) ) {
        cmttmz_set_edit_error ( "No blocks to insert" );
        return EXIT_FAILURE;
    }
    uint32_t old = g_cmttmz.file->block_count;
    if ( at < 0 || ( uint32_t ) at > old ) {
        char msg[CMTTMZ_EDIT_ERROR_SIZE];
        snprintf ( msg, sizeof ( msg ), "Insert position %d is out of range (0 to %u)", at, old );
        cmttmz_set_edit_error ( msg );
        return EXIT_FAILURE;
    }
    if ( count == 0 ) return EXIT_SUCCESS;
    if ( ( uint64_t ) old + count > ( uint64_t ) INT32_MAX ) {
        cmttmz_set_edit_error ( "Too many blocks" );
        return EXIT_FAILURE;
    }
    st_CMTTMZ_EDIT e;
    memset ( &e, 0, sizeof ( e ) );
    e.kind = CMTTMZ_EDIT_INSERT;
    e.at = ( uint32_t ) at;
    e.blocks = blocks;
    e.speeds = speeds;
    e.count = count;
    e.recorded = recorded;
    e.imported = imported;
    e.source = source;
    return cmttmz_edit_apply ( &e );
}


int cmttmz_append_blocks ( st_TZX_BLOCK *blocks, const st_CMTTMZ_BLOCK_SPEED *speeds, uint32_t count ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) return EXIT_FAILURE;
    /* připojení záznamu není úprava uživatele: text chyby úpravy pásky nemění */
    char saved[CMTTMZ_EDIT_ERROR_SIZE];
    memcpy ( saved, g_cmttmz.edit_error, sizeof ( saved ) );
    int rc = cmttmz_insert_internal ( ( int ) g_cmttmz.file->block_count, blocks, speeds, count, 1, 0, CMTTMZ_SOURCE_TMZ );
    memcpy ( g_cmttmz.edit_error, saved, sizeof ( saved ) );
    return rc;
}


int cmttmz_insert_blocks ( int at, st_TZX_BLOCK *blocks, const st_CMTTMZ_BLOCK_SPEED *speeds, uint32_t count ) {
    return cmttmz_insert_internal ( at, blocks, speeds, count, 0, 0, CMTTMZ_SOURCE_TMZ );
}


int cmttmz_replace_block ( int block_id, st_TZX_BLOCK *blocks, const st_CMTTMZ_BLOCK_SPEED *speeds, uint32_t count,
                           int wav_type ) {
    g_cmttmz.edit_error[0] = '\0';
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) {
        cmttmz_set_edit_error ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    uint32_t old = g_cmttmz.file->block_count;
    if ( block_id < 0 || ( uint32_t ) block_id >= old ) {
        char msg[CMTTMZ_EDIT_ERROR_SIZE];
        snprintf ( msg, sizeof ( msg ), "Block %d does not exist", block_id );
        cmttmz_set_edit_error ( msg );
        return EXIT_FAILURE;
    }
    if ( !count || !blocks || !speeds ) {
        cmttmz_set_edit_error ( "No blocks to insert" );
        return EXIT_FAILURE;
    }
    if ( ( uint64_t ) old - 1u + count > ( uint64_t ) INT32_MAX ) {
        cmttmz_set_edit_error ( "Too many blocks" );
        return EXIT_FAILURE;
    }
    st_CMTTMZ_EDIT e;
    memset ( &e, 0, sizeof ( e ) );
    e.kind = CMTTMZ_EDIT_REPLACE;
    e.at = ( uint32_t ) block_id;
    e.blocks = blocks;
    e.speeds = speeds;
    e.count = count;
    e.recorded = 0;
    e.source = wav_type ? CMTTMZ_SOURCE_WAV : CMTTMZ_SOURCE_TMZ;
    return cmttmz_edit_apply ( &e );
}


int cmttmz_block_is_wav ( int block_id ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) return 0;
    if ( block_id < 0 || block_id >= g_cmt_tmz_extension->container->count_blocks ) return 0;
    return ( cmttmz_idx ( block_id )->bltype == CMTEXT_BLOCK_TYPE_WAV ) ? 1 : 0;
}


int cmttmz_remove_blocks ( const int *block_ids, uint32_t count ) {
    g_cmttmz.edit_error[0] = '\0';
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) {
        cmttmz_set_edit_error ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    if ( count == 0 ) return EXIT_SUCCESS;
    if ( !block_ids ) {
        cmttmz_set_edit_error ( "No blocks to delete" );
        return EXIT_FAILURE;
    }
    int n = g_cmt_tmz_extension->container->count_blocks;
    char msg[CMTTMZ_EDIT_ERROR_SIZE];
    for ( uint32_t k = 0; k < count; k++ ) {
        int id = block_ids[k];
        if ( id < 0 || id >= n ) {
            snprintf ( msg, sizeof ( msg ), "Block %d does not exist", id );
            cmttmz_set_edit_error ( msg );
            return EXIT_FAILURE;
        }
        if ( k > 0 && id <= block_ids[k - 1] ) {
            cmttmz_set_edit_error ( "Block numbers must be in ascending order without repetition" );
            return EXIT_FAILURE;
        }
    }
    st_CMTTMZ_EDIT e;
    memset ( &e, 0, sizeof ( e ) );
    e.kind = CMTTMZ_EDIT_REMOVE;
    e.ids = block_ids;
    e.count = count;
    return cmttmz_edit_apply ( &e );
}


int cmttmz_move_block ( int block_id, int to ) {
    g_cmttmz.edit_error[0] = '\0';
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) {
        cmttmz_set_edit_error ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    int n = g_cmt_tmz_extension->container->count_blocks;
    char msg[CMTTMZ_EDIT_ERROR_SIZE];
    if ( block_id < 0 || block_id >= n ) {
        snprintf ( msg, sizeof ( msg ), "Block %d does not exist", block_id );
        cmttmz_set_edit_error ( msg );
        return EXIT_FAILURE;
    }
    if ( to < 0 || to >= n ) {
        snprintf ( msg, sizeof ( msg ), "Target position %d is out of range (0 to %d)", to, n - 1 );
        cmttmz_set_edit_error ( msg );
        return EXIT_FAILURE;
    }
    if ( to == block_id ) return EXIT_SUCCESS;
    st_CMTTMZ_EDIT e;
    memset ( &e, 0, sizeof ( e ) );
    e.kind = CMTTMZ_EDIT_MOVE;
    e.from = ( uint32_t ) block_id;
    e.to = ( uint32_t ) to;
    return cmttmz_edit_apply ( &e );
}


int cmttmz_block_is_unsaved ( int block_id ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container || !g_cmttmz.unsaved ) return 0;
    if ( block_id < 0 || block_id >= g_cmt_tmz_extension->container->count_blocks ) return 0;
    return g_cmttmz.unsaved[block_id] ? 1 : 0;
}


int cmttmz_set_block_unsaved ( int block_id, int unsaved ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container || !g_cmttmz.unsaved ) return EXIT_FAILURE;
    if ( block_id < 0 || block_id >= g_cmt_tmz_extension->container->count_blocks ) return EXIT_FAILURE;
    uint8_t v = unsaved ? 1 : 0;
    /* čítač změn jen při skutečné změně (snapshot volá funkci pro každý blok) */
    int changed = ( g_cmttmz.unsaved[block_id] != v ) || ( v && !g_cmttmz.modified );
    if ( v ) g_cmttmz.modified = 1;
    g_cmttmz.unsaved[block_id] = v;
    if ( changed ) g_cmttmz.change_serial++;
    return EXIT_SUCCESS;
}


uint8_t* cmttmz_get_unsaved_blocks ( uint32_t *count ) {
    if ( count ) *count = 0;
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container || !g_cmttmz.unsaved ) return NULL;
    uint32_t n = ( uint32_t ) g_cmt_tmz_extension->container->count_blocks;
    int any = 0;
    for ( uint32_t i = 0; i < n && !any; i++ ) any = g_cmttmz.unsaved[i];
    if ( !any ) return NULL;
    uint8_t *flags = ( uint8_t* ) g_memdup2 ( g_cmttmz.unsaved, n );
    if ( count ) *count = n;
    return flags;
}


const char* cmttmz_get_edit_error ( void ) {
    return g_cmttmz.edit_error;
}


void cmttmz_set_edit_error ( const char *text ) {
    snprintf ( g_cmttmz.edit_error, sizeof ( g_cmttmz.edit_error ), "%s", text ? text : "" );
}


const char* cmttmz_get_edit_warning ( void ) {
    return g_cmttmz.edit_warning;
}


void cmttmz_set_edit_warning ( const char *text ) {
    snprintf ( g_cmttmz.edit_warning, sizeof ( g_cmttmz.edit_warning ), "%s", text ? text : "" );
}


int cmttmz_tape_mark_get ( st_CMTTMZ_TAPE_MARK *mark ) {
    memset ( mark, 0, sizeof ( *mark ) );
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) return EXIT_FAILURE;
    const st_TZX_FILE *f = g_cmttmz.file;
    mark->block_count = f->block_count;
    if ( f->block_count ) mark->unsaved = ( uint8_t* ) g_memdup2 ( g_cmttmz.unsaved, f->block_count );
    mark->modified = g_cmttmz.modified;
    mark->header = f->header;
    mark->is_tmz = f->is_tmz;
    mark->path = g_strdup ( cmtext_container_get_filepath ( g_cmt_tmz_extension->container ) );
    if ( mark->block_count ) {
        int last = ( int ) mark->block_count - 1;
        const st_TZX_BLOCK *b = &f->blocks[last];
        mark->last_pause_ms = g_cmttmz.speeds[last].pause_ms;
        mark->last_id = b->id;
        mark->last_len = b->length;
        /* data jen u bloku, který připojení přestaví (cmttmz_append_gap_before());
         * jiný poslední blok (např. velký 0x18 pásky WAV) se nekopíruje */
        if ( cmttmz_append_rewrites ( last ) ) {
            mark->last_data = ( uint8_t* ) g_malloc ( b->length ? b->length : 1 );
            if ( b->length ) memcpy ( mark->last_data, b->data, b->length );
            /* rychlost, kterou jsou data sestavená (po značce je může přestavět otevření
             * nebo uložení jinou rychlostí) */
            mark->last_speed = g_cmttmz.speeds[last].speed;
            mark->last_pulses = g_cmttmz.speeds[last].pulses;
        }
    }
    return EXIT_SUCCESS;
}


void cmttmz_tape_mark_free ( st_CMTTMZ_TAPE_MARK *mark ) {
    if ( !mark ) return;
    g_free ( mark->path );
    g_free ( mark->last_data );
    g_free ( mark->unsaved );
    memset ( mark, 0, sizeof ( *mark ) );
}


/**
 * @brief Nastaví cestu kontejneru vložené pásky ("" = bez souboru, jméno pak "").
 *
 * Mění i g_cmt (last_filename, jméno pro UI) přes cmt_tape_path_changed().
 *
 * @param path Cesta (UTF-8, nesmí být NULL).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace (cesta beze změny).
 * @pre Vlákno emulátoru pod device_state_lock(), páska vložená.
 */
static int cmttmz_set_container_path ( const char *path ) {
    st_CMTEXT_CONTAINER *c = g_cmt_tmz_extension->container;
    if ( EXIT_SUCCESS != cmtext_container_set_filepath ( c, path ) ) return EXIT_FAILURE;
    if ( !path[0] ) {
        /* páska bez cesty: jméno prázdné (basename("") by dalo ".") */
        char *empty = ( char* ) baseui_tools_mem_alloc0 ( 1 );
        if ( empty ) {
            baseui_tools_mem_free ( c->name );
            c->name = empty;
        }
    }
    cmt_tape_path_changed ( path );
    return EXIT_SUCCESS;
}


int cmttmz_tape_mark_restore ( const st_CMTTMZ_TAPE_MARK *mark ) {
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) return EXIT_FAILURE;
    st_TZX_FILE *f = g_cmttmz.file;
    uint32_t count = mark->block_count;
    if ( count > f->block_count ) return EXIT_FAILURE;
    st_CMTEXT_CONTAINER *container = g_cmt_tmz_extension->container;

    /* kopie dat posledního bloku předem: při selhání alokace se páska nemění;
     * značka bez dat (blok, který připojení nepřestaví) blok nemění */
    uint8_t *last = NULL;
    st_TZX_BLOCK *lb = ( count && mark->last_data ) ? &f->blocks[count - 1] : NULL;
    bool last_differs = lb && ( lb->id != mark->last_id || lb->length != mark->last_len
                                || ( mark->last_len && memcmp ( lb->data, mark->last_data, mark->last_len ) ) );
    if ( last_differs ) {
        last = ( uint8_t* ) malloc ( mark->last_len ? mark->last_len : 1 );
        if ( !last ) return EXIT_FAILURE;
        if ( mark->last_len ) memcpy ( last, mark->last_data, mark->last_len );
    }

    /* cesta (uložení po nahrávání s příponou pásky ji změnilo) ještě před
     * zkrácením: jediný další krok, který může selhat (alokace), páska je
     * pak beze změny */
    const char *mp = mark->path ? mark->path : "";
    if ( strcmp ( cmtext_container_get_filepath ( container ), mp ) ) {
        if ( EXIT_SUCCESS != cmttmz_set_container_path ( mp ) ) {
            free ( last );
            return EXIT_FAILURE;
        }
    }

    if ( count < f->block_count ) {
        /* otevřený blok za hranicí: zavřít (otevře ho volající podle checkpointu) */
        st_CMTEXT_BLOCK *blk = g_cmt_tmz_extension->block;
        if ( blk && ( blk->block_id < 0 || ( uint32_t ) blk->block_id >= count ) ) {
            cmttmz_block_close ( blk );
            g_cmt_tmz_extension->block = NULL;
            g_cmttmz.open_block_id = -1;
        }
        st_CMTEXT_TAPE_INDEX *ix = container->tape->index;
        for ( uint32_t i = count; i < f->block_count; i++ ) {
            free ( f->blocks[i].data );
            f->blocks[i].data = NULL;
            st_CMTEXT_TAPE_INDEX *e = &ix[i];
            cmttmz_index_entry_free_name ( e );
            memset ( e, 0, sizeof ( *e ) );
        }
        f->block_count = count;
        container->count_blocks = ( int ) count;
        /* stav playeru může ukazovat za konec: nastaví ho snapshot checkpointu */
        g_cmttmz.open_serial++;
        /* bloky za hranicí zmizely: výběr v okně se zruší */
        g_cmttmz.tape_serial++;
    }
    /* dosavadní poslední blok bajtově zpět (cmttmz_append_gap_before() ho přestavěl
     * s pauzou); otevřený stream bloku přestaví načtení snapshotu checkpointu */
    if ( last ) {
        free ( lb->data );
        lb->id = mark->last_id;
        lb->length = mark->last_len;
        lb->data = last;
        g_cmttmz.open_serial++;
        /* jiná data posledního bloku = jiná délka; levně ji spočítá průchod níže, jinak zůstane neznámá */
        g_cmttmz.info[count - 1].length_state = CMTTMZ_LENGTH_UNKNOWN;
        g_cmttmz.info[count - 1].length_s = 0;
    }
    if ( count ) g_cmttmz.speeds[count - 1].pause_ms = mark->last_pause_ms;
    if ( count && mark->last_data ) {
        /* záznam rychlosti k vráceným datům: data jsou sestavená touto rychlostí
         * (blok se pak přestaví, až bude hrát jinou, jako ve značce) */
        g_cmttmz.speeds[count - 1].speed = mark->last_speed;
        g_cmttmz.speeds[count - 1].pulses = mark->last_pulses;
    }
    f->header = mark->header;
    f->is_tmz = mark->is_tmz;
    g_cmttmz.modified = mark->modified ? 1 : 0;
    /* příznaky neuloženo bloků do značky jako ve značce (uložení po značce je
     * mohlo vynulovat); Modified 0 = vše uložené */
    if ( count && mark->unsaved ) memcpy ( g_cmttmz.unsaved, mark->unsaved, count );
    if ( !g_cmttmz.modified ) memset ( g_cmttmz.unsaved, 0, count ? count : 1 );
    g_cmttmz.change_serial++;
    /* návrat při přehrávání vstupů (zásahy UI se tam odmítají): jen levně a jen poslední blok */
    cmttmz_lengths_pass ( CMTTMZ_LENGTHS_CHEAP, count ? ( int ) count - 1 : 0, ( int ) count, 0 );
    return EXIT_SUCCESS;
}


int cmttmz_is_modified ( void ) {
    return ( g_cmttmz.file && g_cmttmz.modified ) ? 1 : 0;
}


void cmttmz_set_modified ( int modified ) {
    g_cmttmz.modified = ( g_cmttmz.file && modified ) ? 1 : 0;
    if ( g_cmttmz.file && !g_cmttmz.modified ) {
        memset ( g_cmttmz.unsaved, 0, g_cmttmz.file->block_count ? g_cmttmz.file->block_count : 1 );
    }
    g_cmttmz.change_serial++;
}


void cmttmz_set_record_message ( const char *text ) {
    cmttmz_set_record_result ( text, 0, 0 );
}


void cmttmz_set_record_result ( const char *text, uint32_t mzf, uint32_t raw ) {
    snprintf ( g_cmttmz.record_message, sizeof ( g_cmttmz.record_message ), "%s", text ? text : "" );
    g_cmttmz.record_mzf = mzf;
    g_cmttmz.record_raw = raw;
    g_cmttmz.record_discarded = 0;
    g_cmttmz.record_serial++;
}


void cmttmz_set_record_discarded ( const char *text ) {
    cmttmz_set_record_result ( text, 0, 0 );
    g_cmttmz.record_discarded = 1;
}


int cmttmz_get_record_discarded ( void ) {
    return g_cmttmz.record_discarded;
}


void cmttmz_get_record_result ( uint32_t *mzf, uint32_t *raw, unsigned *serial ) {
    if ( mzf ) *mzf = g_cmttmz.record_mzf;
    if ( raw ) *raw = g_cmttmz.record_raw;
    if ( serial ) *serial = g_cmttmz.record_serial;
}


const char* cmttmz_get_record_message ( void ) {
    return g_cmttmz.record_message;
}


int cmttmz_get_last_recording ( unsigned *serial, uint32_t *mzf, uint32_t *raw, int *discarded ) {
    /* od startu emulátoru: vložení pásky výsledek nesmaže (otevření jiné
     * pásky, nová páska i snapshot nahrávání nejdřív zahodí a pak pásku
     * vloží - výsledek té operace musí zůstat čitelný) */
    int have = ( g_cmttmz.record_serial != 0 );
    if ( serial ) *serial = have ? g_cmttmz.record_serial : 0;
    if ( mzf ) *mzf = have ? g_cmttmz.record_mzf : 0;
    if ( raw ) *raw = have ? g_cmttmz.record_raw : 0;
    if ( discarded ) *discarded = have ? g_cmttmz.record_discarded : 0;
    return have;
}

/* ------------------------------------------------------------------------- */
/*  Uložení pásky a export bloku                                             */
/* ------------------------------------------------------------------------- */


/** @brief Frekvence WAV exportu bloku (Hz). */
#define CMTTMZ_EXPORT_WAV_RATE 44100u


/**
 * @brief Připojí varování uložení (g_cmttmz.save_warning, oddělovač "; ").
 * @param fmt Formát (printf), text anglicky; co se nevejde, se ořízne.
 */
static void cmttmz_save_warn ( const char *fmt, ... ) {
    size_t used = strlen ( g_cmttmz.save_warning );
    if ( used + 3 >= sizeof ( g_cmttmz.save_warning ) ) return;
    if ( used ) {
        memcpy ( g_cmttmz.save_warning + used, "; ", 3 );
        used += 2;
    }
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( g_cmttmz.save_warning + used, sizeof ( g_cmttmz.save_warning ) - used, fmt, ap );
    va_end ( ap );
}


/**
 * @brief Nastaví zprávu posledního uložení nebo exportu (g_cmttmz.save_message).
 * @param fmt Formát (printf), text anglicky; delší se ořízne.
 */
static void cmttmz_save_msg ( const char *fmt, ... ) {
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( g_cmttmz.save_message, sizeof ( g_cmttmz.save_message ), fmt, ap );
    va_end ( ap );
}


#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: 1 = cmttmz_write_file_atomic() selže po zápisu dočasného souboru, před nahrazením cíle. */
static int s_test_fail_before_replace = 0;


void cmttmz_test_fail_before_replace ( int fail ) {
    s_test_fail_before_replace = fail ? 1 : 0;
}
#endif


/**
 * @brief Ověří, že existující cíl atomického zápisu smí být přepsán.
 *
 * Na POSIX systémech rename() nahradí i soubor bez práva zápisu (rozhoduje
 * právo k adresáři), na Windows nahrazení souboru jen pro čtení selže.
 * Kontrola předem (g_access() s W_OK) chování sjednocuje: soubor chráněný
 * proti zápisu se nepřepíše nikde. Neexistující cíl projde. Proces
 * s právy roota kontrolou projde vždy (access() mu zápis povolí).
 *
 * @param path Cíl (UTF-8).
 * @return 1 = cíl neexistuje nebo do něj jde zapisovat; 0 = existuje a je
 *         chráněný proti zápisu (errno nastaví g_access(), typicky EACCES).
 */
static int cmttmz_target_writable ( const char *path ) {
    if ( !g_file_test ( path, G_FILE_TEST_EXISTS ) ) return 1;
    return ( g_access ( path, W_OK ) == 0 );
}


/**
 * @brief Zapíše data do souboru atomicky.
 *
 * Existující cíl chráněný proti zápisu se odmítne předem
 * (cmttmz_target_writable()).
 * Data se zapíší do nového dočasného souboru ve stejném adresáři
 * (g_mkstemp_full() s šablonou "<cíl>.XXXXXX", binárně), soubor se zavře
 * a teprve pak g_rename() nahradí cíl (GLib na Windows MoveFileEx
 * s nahrazením existujícího souboru). Při selhání vytvoření, zápisu,
 * zavření nebo nahrazení se dočasný soubor smaže a cíl zůstane, jak byl
 * (plný disk, adresář jen pro čtení, cíl je adresář nebo soubor jen pro
 * čtení).
 *
 * @param path Cíl (UTF-8). @param data Data. @param len Délka.
 * @return EXIT_SUCCESS; EXIT_FAILURE (text do save_message).
 */
static int cmttmz_write_file_atomic ( const char *path, const uint8_t *data, size_t len ) {
    if ( !cmttmz_target_writable ( path ) ) {
        cmttmz_save_msg ( "Can't write file %s: %s", path, g_strerror ( errno ) );
        return EXIT_FAILURE;
    }
    gchar *tmp = g_strdup_printf ( "%s.XXXXXX", path );
#ifdef O_BINARY
    int fd = g_mkstemp_full ( tmp, O_RDWR | O_BINARY, 0666 );
#else
    int fd = g_mkstemp_full ( tmp, O_RDWR, 0666 );
#endif
    if ( fd < 0 ) {
        cmttmz_save_msg ( "Can't write file %s: %s", path, g_strerror ( errno ) );
        g_free ( tmp );
        return EXIT_FAILURE;
    }
    FILE *fh = fdopen ( fd, "wb" );
    int ok = ( fh != NULL );
    if ( !fh ) close ( fd );
    if ( ok && len ) ok = ( fwrite ( data, 1, len, fh ) == len );
    if ( fh && ( fclose ( fh ) != 0 ) ) ok = 0;
    if ( !ok ) {
        cmttmz_save_msg ( "Can't write file %s: %s", path, g_strerror ( errno ) );
        g_remove ( tmp );
        g_free ( tmp );
        return EXIT_FAILURE;
    }
#ifdef MZTEST_HEADLESS
    if ( s_test_fail_before_replace ) {
        cmttmz_save_msg ( "Can't write file %s: test failure before replace", path );
        g_remove ( tmp );
        g_free ( tmp );
        return EXIT_FAILURE;
    }
#endif
    if ( g_rename ( tmp, path ) != 0 ) {
        cmttmz_save_msg ( "Can't write file %s: %s", path, g_strerror ( errno ) );
        g_remove ( tmp );
        g_free ( tmp );
        return EXIT_FAILURE;
    }
    g_free ( tmp );
    return EXIT_SUCCESS;
}


/**
 * @brief Otevře prázdný paměťový handler pro zápis (roste podle potřeby).
 * @param[out] h Handler (uvolnit generic_driver_close()).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace.
 */
static int cmttmz_mem_open ( st_HANDLER *h ) {
    memset ( h, 0, sizeof ( *h ) );
    if ( !generic_driver_open_memory ( h, &g_memory_driver_realloc, 1 ) ) return EXIT_FAILURE;
    generic_driver_set_handler_readonly_status ( h, 0 );
    h->spec.memspec.swelling_enabled = 1;
    return EXIT_SUCCESS;
}


/**
 * @brief Zapíše MZF do souboru atomicky (mzf_save() do paměti, pak cmttmz_write_file_atomic()).
 * @param mzf MZF. @param path Cíl.
 * @return EXIT_SUCCESS; EXIT_FAILURE (text do save_message).
 */
static int cmttmz_write_mzf ( const st_MZF *mzf, const char *path ) {
    st_HANDLER h;
    if ( EXIT_SUCCESS != cmttmz_mem_open ( &h ) ) {
        cmttmz_save_msg ( "Not enough memory" );
        return EXIT_FAILURE;
    }
    int rc = EXIT_FAILURE;
    if ( MZF_OK != mzf_save ( &h, mzf ) ) {
        cmttmz_save_msg ( "Can't build the MZF file" );
    } else {
        rc = cmttmz_write_file_atomic ( path, h.spec.memspec.ptr, h.spec.memspec.size );
    }
    generic_driver_close ( &h );
    return rc;
}


/**
 * @brief Je blok blokem rozšíření TMZ 0x40 až 0x4F (formát ZX TZX ho nezná)?
 * @param id ID bloku. @return true pro 0x40 až 0x4F.
 */
static bool cmttmz_block_id_is_mz ( uint8_t id ) {
    return ( id >= 0x40 ) && ( id <= 0x4F );
}


/**
 * @brief Pulzy UniCMT v µs, se kterými se blok uloží do MZT.
 *
 * Blok 0x46 MZ800EMU-CMT (rychlost jde měnit): SET s vlastními pulzy =
 * pulzy indexu zaokrouhlené na celé µs (1 až 65535); SET s poměrem =
 * floor ( 1x / dělitel poměru ) - stejné pravidlo jako tmz_mzt_block_info()
 * u bloku 0x41 a MZ800EMU-CMT; SET 2:1 CPM (pulzy cmt.com, poměr je
 * nepopíše, knihovna: rom_structure 0) = rychlost nejde uložit; DEFAULT =
 * 1x (bez hlavičky CMTSPEED, výchozí rychlost se po importu znovu
 * uplatní). Jiný blok s MZF: rychlost ze souboru (tmz_mzt_block_info()),
 * blok bez struktury ROM = rychlost nejde uložit.
 *
 * @param block_id Blok. @param info Výsledek tmz_mzt_block_info() bloku.
 * @param[out] us Pulzy LONG H, LONG L, SHORT H, SHORT L (platí jen při návratu 1).
 * @return 1 = rychlost jde uložit (us platí), 0 = nejde (blok se zapíše
 *         bez hlavičky CMTSPEED v dosavadní rychlosti).
 */
static int cmttmz_mzt_block_pulses ( int block_id, const st_TMZ_MZT_BLOCK_INFO *info, uint16_t us[4] ) {
    static const uint16_t one[4] = TMZ_MZT_CMTSPEED_PULSES_1X;
    memcpy ( us, one, sizeof ( one ) );
    if ( cmttmz_block_speed_kind ( block_id ) == CMTTMZ_SPEED_KIND_MZF ) {
        const st_CMTEXT_TAPE_INDEX *idx = cmttmz_idx ( block_id );
        if ( idx->blspeed != CMTEXT_BLOCK_SPEED_SET ) return 1;
        en_CMTSPEED sp = idx->item.mzf.cmtspeed;
        if ( cmtspeed_is_custom ( sp ) ) {
            const st_MZTAPE_PULSES_LENGTH *p = &idx->item.mzf.pulses;
            const double sec[4] = { p->long_pulse.high, p->long_pulse.low, p->short_pulse.high, p->short_pulse.low };
            for ( int k = 0; k < 4; k++ ) {
                double v = sec[k] * 1e6;
                if ( !( v >= 0.5 && v < 65535.5 ) ) return 0;
                us[k] = ( uint16_t ) lround ( v );
            }
            return 1;
        }
        if ( sp == CMTSPEED_2_1_CPM || !cmtspeed_is_valid ( sp ) ) return 0;
        for ( int k = 0; k < 4; k++ ) us[k] = ( uint16_t ) floor ( one[k] / g_cmtspeed_divisor[sp] );
        return 1;
    }
    if ( !info->rom_structure ) return 0;
    memcpy ( us, info->pulses_us, sizeof ( one ) );
    return 1;
}


/**
 * @brief Sestaví obsah MZT z celé pásky.
 *
 * Každý blok musí nést MZF (tmz_mzt_block_info() == 1), jinak chyba "Tape
 * contains blocks that cannot be stored as MZT". Hlavičky a těla MZF jdou
 * bajt po bajtu z bloků. Před souborem, jehož rychlost
 * (cmttmz_mzt_block_pulses()) se liší od předchozí (na začátku 1x), se
 * vloží hlavička CMTSPEED UniCMT (tmz_mzt_build_cmtspeed()). Stejně jako
 * tmz_mzt_export() v knihovně se bez hlavičky CMTSPEED zapíše a varováním
 * "Speed of block N cannot be stored in MZT" ohlásí: první soubor, jehož
 * rychlost není 1x (UniCMT úvodní CMTSPEED nepřehraje), blok 2:1 CPM a blok
 * bez struktury ROM.
 *
 * @param[out] out Data (g_free()). @param[out] len Délka.
 * @return EXIT_SUCCESS (varování v g_cmttmz.save_warning); EXIT_FAILURE
 *         (text do save_message).
 */
static int cmttmz_build_mzt ( uint8_t **out, size_t *len ) {
    *out = NULL;
    *len = 0;
    if ( g_cmttmz.file->block_count == 0 ) {
        cmttmz_save_msg ( "Tape has no blocks" );
        return EXIT_FAILURE;
    }
    static const uint16_t one[4] = TMZ_MZT_CMTSPEED_PULSES_1X;
    uint16_t cur[4];
    memcpy ( cur, one, sizeof ( cur ) );
    GByteArray *ba = g_byte_array_new ( );
    for ( uint32_t i = 0; i < g_cmttmz.file->block_count; i++ ) {
        st_TMZ_MZT_BLOCK_INFO info;
        if ( 1 != tmz_mzt_block_info ( &g_cmttmz.file->blocks[i], &info ) ) {
            g_byte_array_free ( ba, TRUE );
            cmttmz_save_msg ( "Tape contains blocks that cannot be stored as MZT" );
            return EXIT_FAILURE;
        }
        uint16_t want[4];
        int storable = cmttmz_mzt_block_pulses ( ( int ) i, &info, want );
        if ( storable && i == 0 && memcmp ( want, one, sizeof ( one ) ) ) storable = 0;
        if ( !storable ) {
            cmttmz_save_warn ( "Speed of block %u cannot be stored in MZT", ( unsigned ) i );
        } else if ( memcmp ( want, cur, sizeof ( cur ) ) ) {
            uint8_t hdr[TMZ_MZT_HEADER_SIZE];
            tmz_mzt_build_cmtspeed ( want, hdr );
            g_byte_array_append ( ba, hdr, TMZ_MZT_HEADER_SIZE );
            memcpy ( cur, want, sizeof ( cur ) );
        }
        g_byte_array_append ( ba, info.header, TMZ_MZT_HEADER_SIZE );
        if ( info.body_size ) g_byte_array_append ( ba, info.body, info.body_size );
        tmz_mzt_block_info_free ( &info );
    }
    *len = ba->len;
    *out = g_byte_array_free ( ba, FALSE );
    return EXIT_SUCCESS;
}


/**
 * @brief Před uložením přestaví v souboru bloky, které jsou sestavené jinou rychlostí, než mají hrát.
 *
 * Blok s měnitelnou rychlostí (cmttmz_block_speed_rebuildable()) se jinak
 * přestaví až při otevření; rychlost bloku změněná v indexu (Set speed,
 * MCP) nebo výchozí rychlost změněná mimo STOP by se tak do souboru
 * uložila stará. Nejdřív se sestaví všechny (cmttmz_block_build() rychlostí
 * cmttmz_block_wanted_speed()); když některý sestavit nejde, soubor se
 * nemění. Pak se vymění na témže indexu (cmttmz_file_swap_block())
 * a vedlejší pole rychlostí dostane novou rychlost. Otevřený blok si nechá
 * svůj stream (je sestavený rychlostí, kterou hraje); délky bloků se
 * nemění (platí pro rychlost, kterou by blok hrál).
 *
 * @return EXIT_SUCCESS (i když není co přestavět); EXIT_FAILURE, když blok
 *         sestavit nejde (text do save_message, soubor beze změny).
 * @pre Vlákno emulátoru, páska vložená.
 */
static int cmttmz_rebuild_for_save ( void ) {
    int count = g_cmt_tmz_extension->container->count_blocks;
    st_TZX_BLOCK **built = ( st_TZX_BLOCK** ) calloc ( count ? ( size_t ) count : 1, sizeof ( st_TZX_BLOCK* ) );
    en_CMTSPEED *bspeed = ( en_CMTSPEED* ) calloc ( count ? ( size_t ) count : 1, sizeof ( en_CMTSPEED ) );
    st_MZTAPE_PULSES_LENGTH *bpulses = ( st_MZTAPE_PULSES_LENGTH* ) calloc ( count ? ( size_t ) count : 1, sizeof ( st_MZTAPE_PULSES_LENGTH ) );
    if ( !built || !bspeed || !bpulses ) {
        free ( built );
        free ( bspeed );
        free ( bpulses );
        cmttmz_save_msg ( "Not enough memory" );
        return EXIT_FAILURE;
    }
    int failed = -1;
    for ( int i = 0; failed < 0 && i < count; i++ ) {
        if ( !cmttmz_block_speed_rebuildable ( i ) ) continue;
        cmttmz_block_wanted_speed ( i, &bspeed[i], &bpulses[i] );
        if ( cmttmz_block_built_with ( i, bspeed[i], &bpulses[i] ) ) continue;
        built[i] = cmttmz_block_build ( i, bspeed[i], &bpulses[i] );
        if ( !built[i] ) failed = i;
    }
    for ( int i = 0; i < count; i++ ) {
        if ( !built[i] ) continue;
        if ( failed >= 0 ) {
            tmz_block_free ( built[i] );
            continue;
        }
        st_TZX_BLOCK old;
        cmttmz_file_swap_block ( i, built[i], &old );
        free ( old.data );
        cmttmz_note_built_speed ( i, bspeed[i], &bpulses[i] );
    }
    free ( built );
    free ( bspeed );
    free ( bpulses );
    if ( failed >= 0 ) {
        cmttmz_save_msg ( "Block %d cannot be rebuilt at its speed", failed );
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


int cmttmz_save ( const char *path ) {
    g_cmttmz.save_message[0] = '\0';
    g_cmttmz.save_warning[0] = '\0';
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) {
        cmttmz_save_msg ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    char *target = g_strdup ( path ? path : cmtext_container_get_filepath ( g_cmt_tmz_extension->container ) );
    if ( !target || !target[0] ) {
        g_free ( target );
        cmttmz_save_msg ( "Tape has no file name" );
        return EXIT_FAILURE;
    }
    const char *ext = cmtext_get_filename_extension ( target );
    uint8_t *data = NULL;
    size_t len = 0;
    int rc = EXIT_FAILURE;

    if ( ext && ( !g_ascii_strcasecmp ( ext, "tmz" ) || !g_ascii_strcasecmp ( ext, "tzx" ) ) ) {
        st_HANDLER h;
        if ( EXIT_SUCCESS != cmttmz_rebuild_for_save ( ) ) {
            /* text dal cmttmz_rebuild_for_save() */
        } else if ( EXIT_SUCCESS != cmttmz_mem_open ( &h ) ) {
            cmttmz_save_msg ( "Not enough memory" );
        } else {
            if ( TZX_OK != tzx_save ( &h, g_cmttmz.file ) ) {
                cmttmz_save_msg ( "Can't build the tape file" );
            } else {
                rc = cmttmz_write_file_atomic ( target, h.spec.memspec.ptr, h.spec.memspec.size );
            }
            generic_driver_close ( &h );
        }
        if ( !g_ascii_strcasecmp ( ext, "tzx" ) ) {
            for ( uint32_t i = 0; i < g_cmttmz.file->block_count; i++ ) {
                if ( !cmttmz_block_id_is_mz ( g_cmttmz.file->blocks[i].id ) ) continue;
                cmttmz_save_warn ( "Tape contains MZ blocks; file saved in TMZ format with .tzx extension" );
                break;
            }
        }
    } else if ( ext && !g_ascii_strcasecmp ( ext, "mzt" ) ) {
        if ( EXIT_SUCCESS == cmttmz_build_mzt ( &data, &len ) ) {
            rc = cmttmz_write_file_atomic ( target, data, len );
        }
    } else if ( ext && ( !g_ascii_strcasecmp ( ext, "mzf" ) || !g_ascii_strcasecmp ( ext, "m12" ) ) ) {
        st_MZF *mzf = ( g_cmttmz.file->block_count == 1 ) ? cmttmz_block_get_mzf ( 0 ) : NULL;
        if ( !mzf ) {
            cmttmz_save_msg ( "Only a tape with a single MZF block can be saved as MZF" );
        } else {
            rc = cmttmz_write_mzf ( mzf, target );
            mzf_free ( mzf );
        }
    } else {
        cmttmz_save_msg ( "Unsupported file type for saving a tape: %s", target );
    }
    g_free ( data );

    if ( rc == EXIT_SUCCESS ) {
        cmtext_container_set_filepath ( g_cmt_tmz_extension->container, target );
        cmt_tape_path_changed ( target );
        g_cmttmz.modified = 0;
        memset ( g_cmttmz.unsaved, 0, g_cmttmz.file->block_count ? g_cmttmz.file->block_count : 1 );
        g_cmttmz.change_serial++;
        if ( g_cmttmz.save_warning[0] ) {
            cmttmz_save_msg ( "Tape saved: %s (%s)", target, g_cmttmz.save_warning );
            fprintf ( stderr, "%s: warning: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), g_cmttmz.save_warning );
        } else {
            cmttmz_save_msg ( "Tape saved: %s", target );
        }
        printf ( "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), g_cmttmz.save_message );
    } else {
        g_cmttmz.save_warning[0] = '\0';
        fprintf ( stderr, "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), g_cmttmz.save_message );
    }
    g_free ( target );
    return rc;
}


int cmttmz_save_fits ( const char *path ) {
    if ( !g_cmttmz.file || !path ) return 0;
    const char *ext = cmtext_get_filename_extension ( path );
    if ( !ext ) return 0;
    if ( !g_ascii_strcasecmp ( ext, "tmz" ) || !g_ascii_strcasecmp ( ext, "tzx" ) ) return 1;
    if ( !g_ascii_strcasecmp ( ext, "mzt" ) ) {
        /* stejná podmínka jako cmttmz_build_mzt() */
        if ( g_cmttmz.file->block_count == 0 ) return 0;
        for ( uint32_t i = 0; i < g_cmttmz.file->block_count; i++ ) {
            st_TMZ_MZT_BLOCK_INFO info;
            if ( 1 != tmz_mzt_block_info ( &g_cmttmz.file->blocks[i], &info ) ) return 0;
            tmz_mzt_block_info_free ( &info );
        }
        return 1;
    }
    if ( !g_ascii_strcasecmp ( ext, "mzf" ) || !g_ascii_strcasecmp ( ext, "m12" ) ) {
        /* stejná podmínka jako cmttmz_save() */
        st_MZF *mzf = ( g_cmttmz.file->block_count == 1 ) ? cmttmz_block_get_mzf ( 0 ) : NULL;
        if ( !mzf ) return 0;
        mzf_free ( mzf );
        return 1;
    }
    return 0;
}


int cmttmz_export_block ( int block_id, const char *path ) {
    g_cmttmz.save_message[0] = '\0';
    g_cmttmz.save_warning[0] = '\0';
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) {
        cmttmz_save_msg ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    if ( block_id < 0 || ( uint32_t ) block_id >= g_cmttmz.file->block_count ) {
        cmttmz_save_msg ( "Block %d does not exist", block_id );
        return EXIT_FAILURE;
    }
    if ( !path || !path[0] ) {
        cmttmz_save_msg ( "Missing file name" );
        return EXIT_FAILURE;
    }
    const char *ext = cmtext_get_filename_extension ( path );
    int rc = EXIT_FAILURE;
    if ( ext && ( !g_ascii_strcasecmp ( ext, "mzf" ) || !g_ascii_strcasecmp ( ext, "m12" ) ) ) {
        st_MZF *mzf = cmttmz_block_get_mzf ( block_id );
        if ( !mzf ) {
            cmttmz_save_msg ( "Block %d does not contain an MZF file", block_id );
        } else {
            rc = cmttmz_write_mzf ( mzf, path );
            mzf_free ( mzf );
        }
    } else if ( ext && !g_ascii_strcasecmp ( ext, "wav" ) ) {
        /* blok požadovanou rychlostí (jako by se teď otevřel), bez zásahu do souboru a playeru */
        const st_TZX_BLOCK *src = &g_cmttmz.file->blocks[block_id];
        st_TZX_BLOCK *built = NULL;
        if ( cmttmz_block_speed_rebuildable ( block_id ) ) {
            en_CMTSPEED speed;
            st_MZTAPE_PULSES_LENGTH pulses;
            cmttmz_block_wanted_speed ( block_id, &speed, &pulses );
            if ( !cmttmz_block_built_with ( block_id, speed, &pulses ) ) {
                built = cmttmz_block_build ( block_id, speed, &pulses );
                if ( built ) src = built;
            }
        }
        st_TMZ_PLAYER_CONFIG cfg;
        tmz_player_config_init ( &cfg );
        cfg.sample_rate = CMTTMZ_EXPORT_WAV_RATE;
        cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
        cfg.v1_convert = 1;
        cfg.mz_cpu_clock = cmttmz_cpu_clock_before ( ( uint32_t ) block_id );
        en_TMZ_PLAYER_ERROR perr = TMZ_PLAYER_OK;
        st_CMT_STREAM *stream = tmz_player_play_block ( src, &cfg, &perr );
        if ( !stream ) {
            cmttmz_save_msg ( "Block %d gives no signal: %s", block_id, tmz_player_error_string ( perr ) );
        } else {
            rc = cmt_stream_save_wav ( stream, CMTTMZ_EXPORT_WAV_RATE, ( char* ) path );
            if ( rc != EXIT_SUCCESS ) cmttmz_save_msg ( "Can't write WAV file %s", path );
            cmt_stream_destroy ( stream );
        }
        if ( built ) tmz_block_free ( built );
    } else {
        cmttmz_save_msg ( "Unsupported file type for exporting a block: %s", path );
    }
    if ( rc == EXIT_SUCCESS ) {
        cmttmz_save_msg ( "Block %d exported: %s", block_id, path );
        printf ( "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), g_cmttmz.save_message );
    } else {
        fprintf ( stderr, "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), g_cmttmz.save_message );
    }
    return rc;
}


/* ------------------------------------------------------------------------- */
/*  Import souboru jako bloků, export celé pásky do WAV                      */
/* ------------------------------------------------------------------------- */


/* Doxygen v cmt_tmz.h */
void cmttmz_clock_contexts ( const st_TZX_FILE *f, uint32_t *out ) {
    uint32_t clock = 0;
    for ( uint32_t i = 0; i < f->block_count; i++ ) {
        out[i] = clock ? clock : MZCMT_FM_DEFAULT_TIMEBASE;
        const st_TZX_BLOCK *b = &f->blocks[i];
        if ( b->id == TMZ_BLOCK_ID_MZ_MACHINE_INFO && b->data && b->length >= 4 + 1 + 4 ) clock = cmttmz_rd32 ( b->data + 4 + 1 );
    }
}


/* Doxygen v cmt_tmz.h */
void cmttmz_warn_new_blocks_clock ( uint32_t first, uint32_t count ) {
    st_TZX_FILE *f = g_cmttmz.file;
    if ( !f || !count || first >= f->block_count ) return;
    uint32_t end = ( count > f->block_count - first ) ? f->block_count : first + count;
    uint32_t *clock = ( uint32_t* ) malloc ( ( size_t ) f->block_count * sizeof ( uint32_t ) );
    /* bez paměti se varování vynechá (úprava už proběhla, jako u importu) */
    if ( !clock ) return;
    cmttmz_clock_contexts ( f, clock );
    int differs = 0;
    for ( uint32_t p = first; p < end && !differs; p++ ) {
        /* takt používá jen blok 0x46 (tmz_player.c); nový blok počítá s výchozím taktem
         * jako blok importovaný ze souboru bez bloku 0x43 */
        if ( f->blocks[p].id == TMZ_BLOCK_ID_MZ_FILE ) differs = ( clock[p] != MZCMT_FM_DEFAULT_TIMEBASE );
    }
    free ( clock );
    if ( !differs || strstr ( g_cmttmz.edit_warning, CMTTMZ_CLOCK_WARNING ) ) return;
    size_t len = strlen ( g_cmttmz.edit_warning );
    snprintf ( g_cmttmz.edit_warning + len, sizeof ( g_cmttmz.edit_warning ) - len, "%s%s", len ? "; " : "",
               CMTTMZ_CLOCK_WARNING );
}


/**
 * @brief Nastaví text chyby importu "Can't import <cesta>: <důvod>"; u dlouhé cesty zkrátí cestu, ne důvod.
 *
 * Zkrácená cesta začíná "..." a koncem cesty (jméno souboru); začátek řezu
 * se posune na začátek znaku UTF-8, aby text nezačínal půlkou vícebajtového
 * znaku.
 *
 * @param path Cesta souboru (UTF-8). @param why Důvod (anglicky).
 * @pre Vlákno emulátoru nebo pod device_state_lock().
 * @post Text v g_cmttmz.edit_error (cmttmz_get_edit_error()), důvod v g_cmttmz.prepare_error;
 *       vedlejší účinek: řádek na stderr s celou cestou.
 */
static void cmttmz_import_error ( const char *path, const char *why ) {
    char msg[CMTTMZ_EDIT_ERROR_SIZE];
    const char *head = "Can't import ";
    size_t fixed = strlen ( head ) + 2 + strlen ( why );
    size_t room = ( fixed + 4 < sizeof ( msg ) ) ? sizeof ( msg ) - 1 - fixed : 0;
    size_t plen = strlen ( path );
    if ( plen <= room ) {
        snprintf ( msg, sizeof ( msg ), "%s%s: %s", head, path, why );
    } else if ( room > 3 ) {
        /* konec cesty (jméno souboru) je užitečnější než začátek; řez ne uprostřed
         * znaku UTF-8 (pokračovací bajty 10xxxxxx přeskočit, text se tím jen zkrátí) */
        const char *tail = path + plen - ( room - 3 );
        while ( *tail && ( ( ( unsigned char ) *tail & 0xC0u ) == 0x80u ) ) tail++;
        snprintf ( msg, sizeof ( msg ), "%s...%s: %s", head, tail, why );
    } else {
        snprintf ( msg, sizeof ( msg ), "Can't import: %s", why );
    }
    cmttmz_set_edit_error ( msg );
    snprintf ( g_cmttmz.prepare_error, sizeof ( g_cmttmz.prepare_error ), "%s", why );
    fprintf ( stderr, "%s: Can't import %s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), path, why );
}


/**
 * @brief Připojí větu k varování importu (g_cmttmz.import_warning, oddělovač "; ").
 * @param text Věta (anglicky); co se nevejde, se ořízne.
 * @pre Vlákno emulátoru nebo pod device_state_lock().
 */
static void cmttmz_import_warn ( const char *text ) {
    size_t len = strlen ( g_cmttmz.import_warning );
    snprintf ( g_cmttmz.import_warning + len, sizeof ( g_cmttmz.import_warning ) - len, "%s%s", len ? "; " : "", text );
}


int cmttmz_import_file_at ( const char *path, int at, uint32_t *imported ) {
    if ( imported ) *imported = 0;
    g_cmttmz.edit_error[0] = '\0';
    g_cmttmz.import_warning[0] = '\0';
    g_cmttmz.prepare_error[0] = '\0';
    if ( !path || !path[0] ) {
        cmttmz_set_edit_error ( "Missing file name" );
        return EXIT_FAILURE;
    }
    int no_tape = ( !g_cmttmz.file || !g_cmt_tmz_extension->container );
    uint32_t old = no_tape ? 0 : g_cmttmz.file->block_count;
    if ( no_tape ) at = 0;
    if ( at < 0 || ( uint32_t ) at > old ) {
        char msg[CMTTMZ_EDIT_ERROR_SIZE];
        snprintf ( msg, sizeof ( msg ), "Insert position %d is out of range (0 to %u)", at, old );
        cmttmz_set_edit_error ( msg );
        return EXIT_FAILURE;
    }

    /* 1. převod souboru stejně jako otevření (výchozí rychlost a pulzy g_cmt,
     * normální polarita WAV); páska se zatím nemění, bez pásky ani nevzniká */
    st_CMTTMZ_IMPORT_OPTS opts;
    cmttmz_open_opts ( &opts );
    st_CMTTMZ_IMPORT_REPORT rep;
    st_CMTTMZ_BLOCK_SPEED *speeds = NULL;
    st_TZX_FILE *file = cmttmz_import_file_speeds ( path, &opts, &rep, &speeds );
    const char *why = NULL;
    if ( !file ) why = rep.error[0] ? rep.error : "Can't read the file";
    else if ( file->block_count == 0 ) why = "The file contains no blocks";
    else if ( !speeds ) why = "Not enough memory";
    uint32_t count = file ? file->block_count : 0;
    uint32_t *imp_clock = NULL, *old_clock = NULL;
    if ( !why ) {
        imp_clock = ( uint32_t* ) malloc ( ( size_t ) count * sizeof ( uint32_t ) );
        old_clock = ( uint32_t* ) malloc ( ( old ? ( size_t ) old : 1 ) * sizeof ( uint32_t ) );
        if ( !imp_clock || !old_clock ) why = "Not enough memory";
    }
    if ( why ) {
        cmttmz_import_error ( path, why );
        tzx_free ( file );
        free ( speeds );
        free ( imp_clock );
        free ( old_clock );
        return EXIT_FAILURE;
    }
    /* takt CPU (blok 0x43) pro bloky před importem: v importovaném souboru a na pásce */
    cmttmz_clock_contexts ( file, imp_clock );
    if ( !no_tape ) cmttmz_clock_contexts ( g_cmttmz.file, old_clock );

    /* 2. bez pásky nová prázdná páska (jako RECORD do prázdného decku), až teď */
    if ( no_tape && EXIT_SUCCESS != cmttmz_new_tape ( ) ) {
        cmttmz_import_error ( path, "Not enough memory" );
        tzx_free ( file );
        free ( speeds );
        free ( imp_clock );
        free ( old_clock );
        return EXIT_FAILURE;
    }

    /* 3. vložení společnou údržbou úprav */
    int rc = cmttmz_insert_internal ( at, file->blocks, speeds, count, 0, 1, rep.source );
    /* při úspěchu převzala data bloků páska (v poli NULL), tzx_free() uvolní jen obal */
    tzx_free ( file );
    free ( speeds );
    if ( rc != EXIT_SUCCESS ) {
        /* nová páska zase pryč: deck bez pásky jako před importem */
        if ( no_tape ) cmttmz_eject ( );
        free ( imp_clock );
        free ( old_clock );
        return EXIT_FAILURE;
    }

    /* 4. varování: importu, nepřehratelné bloky (bez generování signálu), takt CPU */
    snprintf ( g_cmttmz.import_warning, sizeof ( g_cmttmz.import_warning ), "%s", rep.warning );
    int unplayable = 0;
    for ( uint32_t i = ( uint32_t ) at; i < ( uint32_t ) at + count; i++ ) {
        if ( g_cmttmz.info[i].unplayable ) unplayable++;
    }
    if ( unplayable ) {
        char t[64];
        snprintf ( t, sizeof ( t ), "%d imported block(s) cannot be played", unplayable );
        cmttmz_import_warn ( t );
    }
    uint32_t n = g_cmttmz.file->block_count;
    uint32_t *new_clock = ( uint32_t* ) malloc ( ( size_t ) n * sizeof ( uint32_t ) );
    if ( new_clock ) {
        cmttmz_clock_contexts ( g_cmttmz.file, new_clock );
        int differs = 0;
        for ( uint32_t p = 0; p < n && !differs; p++ ) {
            /* takt používá jen blok 0x46 (tmz_player.c, MZ File): signál ostatních
             * bloků na taktu nezávisí */
            if ( g_cmttmz.file->blocks[p].id != TMZ_BLOCK_ID_MZ_FILE ) continue;
            uint32_t was;
            if ( p < ( uint32_t ) at ) was = old_clock[p];
            else if ( p < ( uint32_t ) at + count ) was = imp_clock[p - ( uint32_t ) at];
            else was = old_clock[p - count];
            differs = ( was != new_clock[p] );
        }
        free ( new_clock );
        /* import povolený, ale uživatel se to dozví */
        if ( differs ) cmttmz_import_warn ( CMTTMZ_CLOCK_WARNING );
    }
    free ( imp_clock );
    free ( old_clock );
    if ( g_cmttmz.import_warning[0] ) {
        fprintf ( stderr, "%s: warning: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), g_cmttmz.import_warning );
    }
    if ( !cmt_quiet_active ( ) ) {
        printf ( "%s: imported %u block(s) from %s at position %d\n", cmtext_get_description ( g_cmt_tmz_extension ),
                 count, path, at );
    }
    if ( imported ) *imported = count;
    return EXIT_SUCCESS;
}


const char* cmttmz_get_import_warning ( void ) {
    return g_cmttmz.import_warning;
}


/** @brief Mez délky exportu celé pásky do WAV v sekundách (stejná jako mez bloku). */
#define CMTTMZ_EXPORT_TAPE_MAX_SECONDS CMT_VSTREAM_MAX_BLOCK_SECONDS

/** @brief Nejvýš kolik nepřehratelných bloků varování exportu vyjmenuje (zbytek počtem). */
#define CMTTMZ_EXPORT_WARN_MAX_LISTED 8

#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: mez délky exportu celé pásky v sekundách (0 = CMTTMZ_EXPORT_TAPE_MAX_SECONDS). */
static uint32_t s_test_export_limit_s = 0;


void cmttmz_test_set_export_limit ( uint32_t seconds ) {
    s_test_export_limit_s = seconds;
}
#endif


/**
 * @brief Připojí stream jednoho bloku ke streamu celé pásky (běhy úrovní, s mezí délky).
 *
 * @param[in,out] out Stream celé pásky (vstream 44 100 Hz); *out NULL =
 *        vznikne s počáteční úrovní prvního běhu.
 * @param block Stream bloku (vstream, čte se od začátku).
 * @param[in,out] total Počet vzorků streamu celé pásky.
 * @param max Mez vzorků.
 * @return 1 = připojeno; 0 = mez překročena; -1 = selhání alokace.
 */
static int cmttmz_export_append ( st_CMT_STREAM **out, st_CMT_STREAM *block, uint64_t *total, uint64_t max ) {
    st_CMT_VSTREAM *vs = block->str.vstream;
    cmt_vstream_read_reset ( vs );
    uint64_t samples = 0;
    int value = 0;
    while ( EXIT_SUCCESS == cmt_vstream_read_pulse ( vs, &samples, &value ) ) {
        if ( samples == 0 ) continue;
        if ( *total + samples > max ) return 0;
        if ( !*out ) {
            *out = cmt_stream_new ( CMT_STREAM_TYPE_VSTREAM );
            if ( !*out ) return -1;
            ( *out )->str.vstream = cmt_vstream_new ( CMTTMZ_EXPORT_WAV_RATE, CMT_VSTREAM_BYTELENGTH8, value, CMT_STREAM_POLARITY_NORMAL );
            if ( !( *out )->str.vstream ) return -1;
        }
        if ( EXIT_SUCCESS != cmt_vstream_add_value_long ( ( *out )->str.vstream, value, samples ) ) return -1;
        *total += samples;
    }
    return 1;
}


/**
 * @brief Zapíše stream jako WAV atomicky (dočasný soubor ve stejném adresáři a g_rename()).
 *
 * Existující cíl chráněný proti zápisu se odmítne předem (cmttmz_target_writable()).
 *
 * @param stream Stream celé pásky. @param path Cíl (UTF-8).
 * @return EXIT_SUCCESS; EXIT_FAILURE (text do save_message, cíl beze změny).
 */
static int cmttmz_write_wav_atomic ( st_CMT_STREAM *stream, const char *path ) {
    if ( !cmttmz_target_writable ( path ) ) {
        cmttmz_save_msg ( "Can't write WAV file %s", path );
        return EXIT_FAILURE;
    }
    gchar *tmp = g_strdup_printf ( "%s.XXXXXX", path );
    int fd = g_mkstemp_full ( tmp, O_RDWR, 0666 );
    if ( fd < 0 ) {
        cmttmz_save_msg ( "Can't write file %s: %s", path, g_strerror ( errno ) );
        g_free ( tmp );
        return EXIT_FAILURE;
    }
    close ( fd );
    int ok = ( EXIT_SUCCESS == cmt_stream_save_wav ( stream, CMTTMZ_EXPORT_WAV_RATE, tmp ) );
#ifdef MZTEST_HEADLESS
    if ( ok && s_test_fail_before_replace ) {
        cmttmz_save_msg ( "Can't write file %s: test failure before replace", path );
        g_remove ( tmp );
        g_free ( tmp );
        return EXIT_FAILURE;
    }
#endif
    if ( !ok || g_rename ( tmp, path ) != 0 ) {
        cmttmz_save_msg ( "Can't write WAV file %s", path );
        g_remove ( tmp );
        g_free ( tmp );
        return EXIT_FAILURE;
    }
    g_free ( tmp );
    return EXIT_SUCCESS;
}


/**
 * @brief Připojí k varování exportu seznam přeskočených bloků jednoho druhu.
 *
 * "<text>: 3, 5" (nejvýš CMTTMZ_EXPORT_WARN_MAX_LISTED čísel, zbytek
 * "and N more"); bez bloku daného druhu nic.
 *
 * @param skipped Druh přeskočení po blocích (0 = nepřeskočen). @param count Počet bloků.
 * @param kind Druh, který se vypíše. @param text Úvod věty (anglicky).
 */
static void cmttmz_export_warn_skipped ( const uint8_t *skipped, uint32_t count, uint8_t kind, const char *text ) {
    char list[CMTTMZ_SAVE_MESSAGE_SIZE];
    size_t len = 0;
    int listed = 0, more = 0;
    list[0] = '\0';
    for ( uint32_t i = 0; i < count; i++ ) {
        if ( skipped[i] != kind ) continue;
        if ( listed >= CMTTMZ_EXPORT_WARN_MAX_LISTED ) {
            more++;
            continue;
        }
        int n = snprintf ( list + len, sizeof ( list ) - len, "%s%u", listed ? ", " : "", ( unsigned ) i );
        if ( n > 0 && ( size_t ) n < sizeof ( list ) - len ) len += ( size_t ) n;
        listed++;
    }
    if ( !listed ) return;
    if ( more ) cmttmz_save_warn ( "%s: %s and %d more", text, list, more );
    else cmttmz_save_warn ( "%s: %s", text, list );
}


/**
 * @brief Tělo cmttmz_export_tape_wav(): zprávu a varování skládá v save_message / save_warning (pomocníci uložení).
 * @param path Cíl.
 * @return Jako cmttmz_export_tape_wav().
 * @pre Jako cmttmz_export_tape_wav(); volá ji jen obal, který zprávu uložení uschová a vrátí.
 */
static int cmttmz_export_tape_wav_body ( const char *path ) {
    g_cmttmz.save_message[0] = '\0';
    g_cmttmz.save_warning[0] = '\0';
    if ( !g_cmttmz.file || !g_cmt_tmz_extension->container ) {
        cmttmz_save_msg ( "No tape inserted" );
        return EXIT_FAILURE;
    }
    if ( !path || !path[0] ) {
        cmttmz_save_msg ( "Missing file name" );
        return EXIT_FAILURE;
    }
    const char *ext = cmtext_get_filename_extension ( path );
    if ( !ext || g_ascii_strcasecmp ( ext, "wav" ) ) {
        cmttmz_save_msg ( "Unsupported file type for exporting the tape: %s", path );
        return EXIT_FAILURE;
    }
    uint32_t count = g_cmttmz.file->block_count;
    if ( count == 0 ) {
        cmttmz_save_msg ( "The tape is empty" );
        return EXIT_FAILURE;
    }

    /* kopie pole bloků: bloky s měnitelnou rychlostí rychlostí, kterou mají
     * hrát (jako export bloku); soubor pásky ani player se nemění */
    st_TZX_FILE tmp = *g_cmttmz.file;
    tmp.blocks = ( st_TZX_BLOCK* ) malloc ( ( size_t ) count * sizeof ( st_TZX_BLOCK ) );
    st_TZX_BLOCK **built = ( st_TZX_BLOCK** ) calloc ( count, sizeof ( st_TZX_BLOCK* ) );
    if ( !tmp.blocks || !built ) {
        free ( tmp.blocks );
        free ( built );
        cmttmz_save_msg ( "Not enough memory" );
        return EXIT_FAILURE;
    }
    tmp.block_capacity = count;
    memcpy ( tmp.blocks, g_cmttmz.file->blocks, ( size_t ) count * sizeof ( st_TZX_BLOCK ) );
    for ( uint32_t i = 0; i < count; i++ ) {
        if ( !cmttmz_block_speed_rebuildable ( ( int ) i ) ) continue;
        en_CMTSPEED speed;
        st_MZTAPE_PULSES_LENGTH pulses;
        cmttmz_block_wanted_speed ( ( int ) i, &speed, &pulses );
        if ( cmttmz_block_built_with ( ( int ) i, speed, &pulses ) ) continue;
        built[i] = cmttmz_block_build ( ( int ) i, speed, &pulses );
        if ( !built[i] ) {
            /* blok by se exportoval jinou rychlostí, než hraje: chyba, ne tichá náhrada */
            for ( uint32_t k = 0; k < i; k++ ) {
                if ( built[k] ) tmz_block_free ( built[k] );
            }
            free ( built );
            free ( tmp.blocks );
            cmttmz_save_msg ( "Block %u cannot be rebuilt at its speed, the tape was not exported", ( unsigned ) i );
            fprintf ( stderr, "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), g_cmttmz.save_message );
            return EXIT_FAILURE;
        }
        tmp.blocks[i] = *built[i];
    }

    /* průchod playerem od začátku pásky: smyčky, skoky, volání, zastavení pásky */
    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init ( &cfg );
    cfg.sample_rate = CMTTMZ_EXPORT_WAV_RATE;
    cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
    cfg.v1_convert = 1;
    st_TMZ_PLAYER_STATE st;
    tmz_player_state_init ( &st, &tmp, &cfg );
    st.current_block = 0;
    uint32_t limit_s = CMTTMZ_EXPORT_TAPE_MAX_SECONDS;
#ifdef MZTEST_HEADLESS
    if ( s_test_export_limit_s ) limit_s = s_test_export_limit_s;
#endif
    const uint64_t max = ( uint64_t ) limit_s * CMTTMZ_EXPORT_WAV_RATE;
    st_CMT_STREAM *out = NULL;
    uint64_t total = 0;
    int failed = 0; /* 1 = mez délky, 2 = alokace */
    /* přeskočené bloky, každý jednou (i když ho smyčka přehraje vícekrát):
     * 1 = player ho nepřehraje, 2 = signál by při 44 100 Hz nešel přečíst */
    uint8_t *skipped = ( uint8_t* ) calloc ( count, 1 );
    if ( !skipped ) failed = 2;
    /* rozpočet iterací playeru se obnoví jen po bloku se signálem: smyčka bez
     * signálu (řídicí bloky, nepřehratelné bloky, zastavení pásky) export
     * ukončí, smyčku se signálem ukončí mez délky */
    uint32_t budget = TMZ_PLAYER_MAX_ITERATIONS;
    while ( !failed ) {
        en_TMZ_PLAYER_ERROR err = TMZ_PLAYER_OK;
        st_CMT_STREAM *s = tmz_player_play_next_budget ( &st, &budget, &err );
        if ( s ) {
            int r = cmttmz_export_append ( &out, s, &total, max );
            cmt_stream_destroy ( s );
            budget = TMZ_PLAYER_MAX_ITERATIONS;
            if ( r == 0 ) failed = 1;
            else if ( r < 0 ) failed = 2;
            continue;
        }
        if ( err == TMZ_PLAYER_OK ) {
            if ( st.stopped ) continue; /* zastavení pásky: export pokračuje dalším blokem */
            break;                      /* konec pásky */
        }
        if ( st.finished ) {
            /* vyčerpaný rozpočet iterací: řídicí bloky se točí bez bloku se signálem */
            cmttmz_save_warn ( "Control flow does not reach a playable block after block %u, export stopped",
                               ( unsigned ) st.last_played_block );
            break;
        }
        /* blok, který player přehrát neumí: přeskočit s varováním (player už stojí za ním) */
        if ( st.last_played_block < count && !skipped[st.last_played_block] ) {
            skipped[st.last_played_block] = ( err == TMZ_PLAYER_ERROR_UNREADABLE ) ? 2 : 1;
        }
    }
    if ( skipped ) {
        cmttmz_export_warn_skipped ( skipped, count, 1, "Blocks that cannot be played were skipped" );
        cmttmz_export_warn_skipped ( skipped, count, 2, "Blocks that cannot be rendered at 44100 Hz were skipped" );
        free ( skipped );
    }
    for ( uint32_t i = 0; i < count; i++ ) {
        if ( built[i] ) tmz_block_free ( built[i] );
    }
    free ( built );
    free ( tmp.blocks );

    int rc = EXIT_FAILURE;
    if ( failed == 1 ) {
        cmttmz_save_msg ( "The tape is longer than %u hours, not exported", ( unsigned ) ( CMTTMZ_EXPORT_TAPE_MAX_SECONDS / 3600u ) );
    } else if ( failed == 2 ) {
        cmttmz_save_msg ( "Not enough memory" );
    } else if ( !out ) {
        cmttmz_save_msg ( "The tape gives no signal, not exported" );
    } else {
        rc = cmttmz_write_wav_atomic ( out, path );
    }
    if ( out ) cmt_stream_destroy ( out );

    if ( rc == EXIT_SUCCESS ) {
        /* délka v desetinách sekundy celými čísly: zpráva jádra nezávisí na
         * locale procesu (printf "%.1f" by v češtině dal desetinnou čárku) */
        uint64_t tenths = ( total * 10u + CMTTMZ_EXPORT_WAV_RATE / 2u ) / CMTTMZ_EXPORT_WAV_RATE;
        unsigned long long whole = ( unsigned long long ) ( tenths / 10u );
        unsigned frac = ( unsigned ) ( tenths % 10u );
        if ( g_cmttmz.save_warning[0] ) cmttmz_save_msg ( "Tape exported: %s, %llu.%u s (%s)", path, whole, frac, g_cmttmz.save_warning );
        else cmttmz_save_msg ( "Tape exported: %s, %llu.%u s", path, whole, frac );
        printf ( "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), g_cmttmz.save_message );
    } else {
        g_cmttmz.save_warning[0] = '\0';
        fprintf ( stderr, "%s: %s\n", cmtext_get_description ( g_cmt_tmz_extension ), g_cmttmz.save_message );
    }
    return rc;
}


int cmttmz_export_tape_wav ( const char *path ) {
    /* zpráva exportu nesmí přepsat zprávu uložení, kterou okno ještě nečetlo
     * pomocníci uložení se použijí a zpráva uložení se vrátí */
    char msg[CMTTMZ_SAVE_MESSAGE_SIZE], warn[CMTTMZ_SAVE_MESSAGE_SIZE];
    memcpy ( msg, g_cmttmz.save_message, sizeof ( msg ) );
    memcpy ( warn, g_cmttmz.save_warning, sizeof ( warn ) );
    int rc = cmttmz_export_tape_wav_body ( path );
    memcpy ( g_cmttmz.export_message, g_cmttmz.save_message, sizeof ( g_cmttmz.export_message ) );
    memcpy ( g_cmttmz.export_warning, g_cmttmz.save_warning, sizeof ( g_cmttmz.export_warning ) );
    memcpy ( g_cmttmz.save_message, msg, sizeof ( msg ) );
    memcpy ( g_cmttmz.save_warning, warn, sizeof ( warn ) );
    return rc;
}


const char* cmttmz_get_export_message ( void ) {
    return g_cmttmz.export_message;
}


const char* cmttmz_get_export_warning ( void ) {
    return g_cmttmz.export_warning;
}


const char* cmttmz_get_save_message ( void ) {
    return g_cmttmz.save_message;
}


const char* cmttmz_get_save_warning ( void ) {
    return g_cmttmz.save_warning;
}


/* ------------------------------------------------------------------------- */
/*  Veřejné dotazy                                                           */
/* ------------------------------------------------------------------------- */


const st_TZX_FILE* cmttmz_get_file ( void ) {
    return g_cmttmz.file;
}


unsigned cmttmz_get_open_serial ( void ) {
    return g_cmttmz.open_serial;
}


unsigned cmttmz_get_tape_serial ( void ) {
    return g_cmttmz.tape_serial;
}


int cmttmz_block_is_mzf ( int block_id ) {
    if ( !g_cmttmz.file || block_id < 0 || ( uint32_t ) block_id >= g_cmttmz.file->block_count ) return 0;
    uint8_t id = g_cmttmz.file->blocks[block_id].id;
    return ( id == TMZ_BLOCK_ID_MZ_STANDARD_DATA ) || ( id == TMZ_BLOCK_ID_MZ_TURBO_DATA ) || ( id == TMZ_BLOCK_ID_MZ_FILE );
}


st_MZF* cmttmz_block_get_mzf ( int block_id ) {
    if ( !cmttmz_block_is_mzf ( block_id ) ) return NULL;
    return tmz_block_to_mzf ( &g_cmttmz.file->blocks[block_id], NULL );
}


const char* cmttmz_get_open_warning ( void ) {
    return g_cmttmz.warning;
}


const char* cmttmz_tap_code_txt ( int code ) {
    switch ( code ) {
        case 0x00:
            return "Program Header";
        case 0x01:
            return "Num Array Header";
        case 0x02:
            return "Char Array Header";
        case 0x03:
            return "File Header";
        default:
            return "Data";
    }
}



int cmttmz_get_player_state ( st_TMZ_PLAYER_STATE *out ) {
    if ( !out || !g_cmttmz.file || !g_cmt_tmz_extension->container ) return EXIT_FAILURE;
    *out = g_cmttmz.player;
    return EXIT_SUCCESS;
}


/**
 * @brief Přečte 16bitové LE číslo z dat bloku (počet opakování 0x24, počet volání 0x26).
 * @param b Blok. @return Hodnota; 0, je-li blok kratší než 2 B.
 */
static uint16_t cmttmz_block_u16 ( const st_TZX_BLOCK *b ) {
    if ( !b->data || b->length < 2 ) return 0;
    return ( uint16_t ) ( b->data[0] | ( b->data[1] << 8 ) );
}


/**
 * @brief Je takt CPU platný stav playeru pro vloženou pásku?
 *
 * config.mz_cpu_clock je 0 (výchozí 3 546 895 Hz), nebo takt z některého
 * bloku 0x43 souboru: player ho nastavuje jen v tmz_player_play_next()
 * z bloku 0x43 a přímé otevření z cmttmz_cpu_clock_before(). Který blok
 * 0x43 platí, závisí na cestě páskou (skoky, smyčky, volání), proto se
 * neověřuje konkrétní blok, jen původ hodnoty.
 *
 * @param clock Takt ze stavu. @return true = 0 nebo takt některého bloku 0x43.
 */
static bool cmttmz_cpu_clock_in_file ( uint32_t clock ) {
    if ( clock == 0 ) return true;
    for ( uint32_t i = 0; i < g_cmttmz.file->block_count; i++ ) {
        const st_TZX_BLOCK *b = &g_cmttmz.file->blocks[i];
        if ( b->id == TMZ_BLOCK_ID_MZ_MACHINE_INFO && b->data && b->length >= 4 + 1 + 4
             && cmttmz_rd32 ( b->data + 4 + 1 ) == clock ) return true;
    }
    return false;
}


int cmttmz_set_player_state ( const st_TMZ_PLAYER_STATE *in ) {
    if ( !in || !g_cmttmz.file || !g_cmt_tmz_extension->container ) return EXIT_FAILURE;
    const st_TZX_FILE *f = g_cmttmz.file;
    uint32_t count = f->block_count;
    if ( count == 0 ) return EXIT_FAILURE;

    /* pozice: current_block <= block_count bez ohledu na finished. Invariant
     * knihovny "current_block < block_count dokud !finished" přehrávání
     * nedodržuje: po posledním bloku se signálem je current_block ==
     * block_count a finished se nastaví až při dalším tmz_player_play_next();
     * skok mimo pásku (handle_jump) nastaví finished na bloku skoku
     * (current_block < block_count) a cmttmz_advance() ho jako konec pásky
     * ponechá. */
    if ( in->current_block > count ) return EXIT_FAILURE;

    /* last_played_block: po každém úspěšném otevření bloku (přímém
     * i postupném, cmttmz_play_from()) je to otevřený blok; neúspěch stav
     * vrací. Výjimka je konec pásky (cmttmz_advance() -> CMTTMZ_ADVANCE_END
     * stav nevrací): player je finished, otevřený zůstává poslední blok se
     * signálem a last_played_block je poslední blok, který player po jeho
     * přehrání zpracoval jako datový (zastavení pásky, blok s příznakem
     * skip, blok bez signálu) - i před otevřeným blokem, vede-li tam skok. */
    const st_CMTEXT_BLOCK *open = g_cmt_tmz_extension->block;
    if ( !open || open->block_id < 0 ) return EXIT_FAILURE;
    if ( in->last_played_block >= count ) return EXIT_FAILURE;
    if ( !in->finished && in->last_played_block != ( uint32_t ) open->block_id ) return EXIT_FAILURE;

    /* smyčky: start_block je blok hned za blokem Loop Start (handle_loop_start,
     * tmz_player.c), remaining 1 až počet opakování toho bloku (Loop End
     * odečítá, při 0 smyčku zruší - 0 ve stavu nevznikne a podtekla by) */
    if ( in->loop_depth < 0 || in->loop_depth > TMZ_PLAYER_MAX_LOOP_DEPTH ) return EXIT_FAILURE;
    for ( int i = 0; i < in->loop_depth; i++ ) {
        uint32_t start = in->loop_stack[i].start_block;
        if ( start == 0 || start > count ) return EXIT_FAILURE;
        const st_TZX_BLOCK *ls = &f->blocks[start - 1];
        if ( ls->id != TZX_BLOCK_ID_LOOP_START ) return EXIT_FAILURE;
        uint32_t rem = in->loop_stack[i].remaining;
        if ( rem < 1 || rem > cmttmz_block_u16 ( ls ) ) return EXIT_FAILURE;
    }

    /* volání: call_block je blok Call Sequence, total_offsets = jeho počet
     * offsetů (handle_call_sequence: count > 0 a data na všechny offsety),
     * current_offset_idx menší */
    if ( in->call_depth < 0 || in->call_depth > TMZ_PLAYER_MAX_CALL_DEPTH ) return EXIT_FAILURE;
    for ( int i = 0; i < in->call_depth; i++ ) {
        const st_TMZ_PLAYER_CALL_ENTRY *c = &in->call_stack[i];
        if ( c->call_block >= count ) return EXIT_FAILURE;
        const st_TZX_BLOCK *cb = &f->blocks[c->call_block];
        if ( cb->id != TZX_BLOCK_ID_CALL_SEQUENCE ) return EXIT_FAILURE;
        uint16_t n = cmttmz_block_u16 ( cb );
        if ( n == 0 || cb->length < 2 + ( uint32_t ) n * 2 ) return EXIT_FAILURE;
        if ( c->total_offsets != n || c->current_offset_idx >= c->total_offsets ) return EXIT_FAILURE;
    }

    /* takt CPU: 0 nebo z některého bloku 0x43 */
    if ( !cmttmz_cpu_clock_in_file ( in->config.mz_cpu_clock ) ) return EXIT_FAILURE;

    /* úroveň signálu: 0, 1 nebo TZX_LEVEL_AFTER_PAUSE */
    if ( in->signal_level != 0 && in->signal_level != 1 && in->signal_level != TZX_LEVEL_AFTER_PAUSE ) return EXIT_FAILURE;

    /* příznaky: invariant playeru stopped => !finished */
    bool stopped = in->stopped ? true : false;
    bool finished = in->finished ? true : false;
    if ( stopped && finished ) return EXIT_FAILURE;

    /* file a config (sample_rate, typ streamu, převod v1) jsou vždy z rozšíření */
    st_TMZ_PLAYER_STATE st = g_cmttmz.player;
    if ( st.file != g_cmttmz.file ) cmttmz_player_init_at ( &st, 0 );
    st.file = g_cmttmz.file;
    st.config.mz_cpu_clock = in->config.mz_cpu_clock;
    st.current_block = in->current_block;
    st.last_played_block = in->last_played_block;
    memset ( st.loop_stack, 0, sizeof ( st.loop_stack ) );
    memcpy ( st.loop_stack, in->loop_stack, ( size_t ) in->loop_depth * sizeof ( in->loop_stack[0] ) );
    st.loop_depth = in->loop_depth;
    memset ( st.call_stack, 0, sizeof ( st.call_stack ) );
    memcpy ( st.call_stack, in->call_stack, ( size_t ) in->call_depth * sizeof ( in->call_stack[0] ) );
    st.call_depth = in->call_depth;
    st.stopped = stopped;
    st.signal_level = in->signal_level;
    st.finished = finished;
    g_cmttmz.player = st;
    return EXIT_SUCCESS;
}
