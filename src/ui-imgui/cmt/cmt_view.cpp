/**
 * @file   cmt_view.cpp
 * @brief  Plnění kopie stavu Virtual CMT pro okno (cmt_view_fill()) a testovací stavy (cmt_view_make_fixture()).
 *
 * Jediné místo UI, které čte jádro kazety pro nové okno: g_cmt (transport,
 * režim, výchozí rychlost), kontejner a index pásky, údaje bloků pro UI
 * (cmttmz_get_block_ui()), pozici (cmt_tape_get_ui_position()), motor
 * (cmt_motor_get_state()) a průběh záznamu (cmt_record_get_progress()).
 * Vše pod device_state_lock(), který drží volající.
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

#include "main.h"
#include "libs/imgui/imgui.h"
#include "ui-imgui/message/message_window.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <glib.h>
#include <string>
#include <vector>

#include "i18n.h"

#include "emulator/hw-generic/cmt/cmt.h"
#include "emulator/hw-generic/cmt/cmthack.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_block.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "emulator/hw-generic/cmt/cmt_request.h"
#include "emulator/replay.h"
#include "emulator/replay_media.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/mztape/mztape.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/tzx/tzx.h"

#include "cmt_view.h"

#include <map>


/* ------------------------------------------------------------------------- */
/*  Pseudo-překlad pro testy (texty o 40 % delší)                            */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
/** @brief Testy: prodlužovat přeložené texty okna a kopie o 40 % (cmt_view_test_set_stretch()). */
static bool s_test_stretch = false;

void cmt_view_test_set_stretch ( bool on )
{
    s_test_stretch = on;
}

bool cmt_view_test_stretch ( void )
{
    return s_test_stretch;
}

const char *cmt_view_test_stretch_text ( const char *key, const char *translated )
{
    if ( !s_test_stretch ) return translated;
    static std::map<std::string, std::string> cache;
    std::map<std::string, std::string>::iterator it = cache.find ( key );
    if ( it != cache.end ( ) ) return it->second.c_str ( );
    std::string s ( translated );
    std::string pool;
    size_t chars = 0;
    for ( size_t i = 0; i < s.size ( ); i++ ) {
        unsigned char c = ( unsigned char ) s[i];
        if ( ( c & 0xC0 ) != 0x80 ) chars++;
        if ( g_ascii_isalnum ( c ) ) pool += ( char ) c;
    }
    if ( pool.empty ( ) ) pool = "x";
    size_t add = ( chars * 4 + 9 ) / 10;
    for ( size_t i = 0; i < add; i++ ) s += pool[i % pool.size ( )];
    return cache.emplace ( key, s ).first->second.c_str ( );
}

const char *cmt_view_test_tr ( const char *msgid )
{
    return cmt_view_test_stretch_text ( msgid, gettext ( msgid ) );
}

const char *cmt_view_test_trc ( const char *ctx_msgid, const char *msgid )
{
    return cmt_view_test_stretch_text ( ctx_msgid, pgettext_impl ( ctx_msgid, msgid ) );
}

const char *cmt_view_test_trl ( const char *msgid )
{
    static std::map<std::string, std::string> cache;
    std::string key = std::string ( s_test_stretch ? "1" : "0" ) + msgid;
    std::map<std::string, std::string>::iterator it = cache.find ( key );
    if ( it != cache.end ( ) ) return it->second.c_str ( );
    std::string s = std::string ( cmt_view_test_tr ( msgid ) ) + "##" + msgid;
    return cache.emplace ( key, s ).first->second.c_str ( );
}

/* texty kopie (typy, stavy, druhy bloků) se v testech prodlužují jako texty okna */
#undef _
#define _( STRING ) cmt_view_test_tr ( STRING )
#endif


/** @brief Hodnota čítačů kopie, která se nikdy nerovná čítači jádra při prvním plnění (vynucené sestavení). */
#define CMT_VIEW_SERIAL_NONE UINT_MAX


std::string cmt_view_mzf_type_text ( int ftype )
{
    const char *word = NULL;
    switch ( ftype ) {
        case 0x01: word = _( "machine code" ); break;
        case 0x02: word = _( "BASIC program" ); break;
        case 0x03: word = _( "data" ); break;
        case 0x04: word = _( "BASIC data" ); break;
        case 0x22: word = _( "CP/M file" ); break;
        default: break;
    }
    char buf[64];
    if ( word ) snprintf ( buf, sizeof ( buf ), "%02X %s", ftype & 0xFF, word );
    else snprintf ( buf, sizeof ( buf ), "%02X", ftype & 0xFF );
    return std::string ( buf );
}


/**
 * @brief Text rychlosti (poměr nebo vlastní pulzy).
 * @param speed en_CMTSPEED.
 * @return "1:1", "2:1 (cp/m)" ... nebo přeložené "Custom pulses"; "" pro CMTSPEED_NONE.
 */
static std::string cmt_view_speed_text ( int speed )
{
    en_CMTSPEED s = ( en_CMTSPEED ) speed;
    if ( cmtspeed_is_custom ( s ) ) return std::string ( _( "Custom pulses" ) );
    if ( s == CMTSPEED_NONE ) return std::string ( );
    return std::string ( cmtspeed_get_ratiotxt ( s ) );
}


/**
 * @brief Text stavu bloku pro sloupec Status.
 * @param st Stav.
 * @return Přeložený text.
 */
static std::string cmt_view_status_text ( en_CMT_VIEW_STATUS st )
{
    switch ( st ) {
        case CMT_VIEW_STATUS_OK: return _( "OK" );
        case CMT_VIEW_STATUS_NOT_DECODED: return _( "Not decoded" );
        case CMT_VIEW_STATUS_UNSAVED: return _( "Unsaved" );
        case CMT_VIEW_STATUS_UNPLAYABLE: return _( "Cannot play" );
        case CMT_VIEW_STATUS_CONTROL: return _( "Control block" );
        case CMT_VIEW_STATUS_CHECKSUM_ERROR: return _( "Checksum error" );
        default: return std::string ( );
    }
}


void cmt_view_reset ( st_CMT_VIEW *v )
{
    v->has_tape = false;
    v->tape_name.clear ( );
    v->tape_path.clear ( );
    v->modified = false;
    v->open_serial = CMT_VIEW_SERIAL_NONE;
    v->change_serial = CMT_VIEW_SERIAL_NONE;
    v->tape_serial = CMT_VIEW_SERIAL_NONE;
    v->state = CMT_VIEW_STATE_STOP;
    v->paused = false;
    v->user_paused = false;
    v->motor_on = false;
    v->motor_known = false;
    v->block_count = 0;
    v->current_block = -1;
    v->tape_elapsed_s = -1.0;
    v->tape_total_s = -1.0;
    v->block_pos_s = -1.0;
    v->block_progress = 0.0;
    v->rec_time_s = 0.0;
    v->rec_pulses = 0;
    v->mode = CMT_VIEW_MODE_TAPE;
    v->direct_block_loadable = false;
    /* testovací stavy počítají s ROM, která přímé načtení podporuje (výchozí ROM všech architektur) */
    v->direct_supported = true;
    v->echo = false;
    v->default_speed = CMTSPEED_1_1;
    v->default_speed_text = cmt_view_speed_text ( CMTSPEED_1_1 );
    /* pulzy UniCMT 1x jako fixtures (SPEC K10) */
    v->default_pulses_us[0] = 470.0;
    v->default_pulses_us[1] = 494.0;
    v->default_pulses_us[2] = 240.0;
    v->default_pulses_us[3] = 278.0;
    v->replay_recording = false;
    v->replay_playing = false;
    v->message.clear ( );
    v->message_is_error = false;
    v->message_is_warning = false;
    v->record_serial = CMT_VIEW_SERIAL_NONE;
    v->post_serial = CMT_VIEW_SERIAL_NONE;
    v->blocks.clear ( );
    v->lengths_pending = false;
    v->lengths_request_serial = CMT_VIEW_SERIAL_NONE;
    /* první zásah smí hned */
    v->lengths_frames = CMT_VIEW_LENGTHS_RETRY_FRAMES;
}


bool cmt_view_read_tape_info ( st_CMTTMZ_TAPE_INFO *out )
{
    return EXIT_SUCCESS == cmttmz_tape_info_get ( out );
}


bool cmt_view_read_tape_check ( std::vector<st_CMTTMZ_CHECK_ITEM> *items, uint32_t *blocks_checked )
{
    items->clear ( );
    if ( blocks_checked ) *blocks_checked = 0;
    st_CMTTMZ_CHECK_ITEM *arr = NULL;
    uint32_t count = 0, checked = 0;
    if ( EXIT_SUCCESS != cmttmz_tape_check ( &arr, &count, &checked ) ) return false;
    if ( arr ) items->assign ( arr, arr + count );
    g_free ( arr );
    if ( blocks_checked ) *blocks_checked = checked;
    return true;
}


std::string cmt_view_profile_description ( const char *desc )
{
    /* nejistotu, kterou knihovna uvádí (" [unverified]"), uživatel vidí stejně jako přes MCP */
    std::string s = desc ? desc : "";
    /* "470/494 us" -> "470/494 µs": "us" za číslicí (i přes mezeru), samostatné slovo */
    std::string out;
    for ( size_t i = 0; i < s.size ( ); i++ ) {
        const bool word_end = ( i + 2 >= s.size ( ) ) || !g_ascii_isalnum ( s[i + 2] );
        const bool after_num = ( i >= 1 && g_ascii_isdigit ( s[i - 1] ) ) || ( i >= 2 && s[i - 1] == ' ' && g_ascii_isdigit ( s[i - 2] ) );
        if ( s[i] == 'u' && i + 1 < s.size ( ) && s[i + 1] == 's' && word_end && after_num ) {
            out += "\xC2\xB5s";
            i++;
            continue;
        }
        out += s[i];
    }
    return out;
}


void cmt_view_read_profiles ( std::vector<st_CMT_VIEW_PROFILE> *out )
{
    out->clear ( );
    uint32_t n = cmttmz_profile_count ( );
    for ( uint32_t i = 0; i < n; i++ ) {
        const char *id = NULL, *desc = NULL;
        int speed = 0;
        if ( EXIT_SUCCESS != cmttmz_profile_get ( i, &id, &desc, &speed ) ) continue;
        st_CMT_VIEW_PROFILE p;
        p.id = id ? id : "";
        p.description = cmt_view_profile_description ( desc );
        p.accepts_speed = ( speed != 0 );
        out->push_back ( p );
    }
}


bool cmt_view_request_lengths ( st_CMT_VIEW *v )
{
    /* každé volání = jeden snímek okna */
    if ( v->lengths_frames < UINT_MAX ) v->lengths_frames++;
    if ( !v->has_tape || !v->lengths_pending ) return false;
    /* jen zastavený deck: krok by zdržel přehrávání (i v pauze) a záznam */
    if ( v->state != CMT_VIEW_STATE_STOP ) return false;
    if ( v->lengths_frames < CMT_VIEW_LENGTHS_INTERVAL_FRAMES ) return false;
    /* předchozí zásah nic nezměnil (odmítnutý při replay, zahozený): opakovat až po delší době */
    if ( v->lengths_request_serial == v->change_serial && v->lengths_frames < CMT_VIEW_LENGTHS_RETRY_FRAMES ) return false;
    if ( replay_is_active ( ) ) return false;
    if ( !cmt_request_lengths ( ) ) return false;
    v->lengths_request_serial = v->change_serial;
    v->lengths_frames = 0;
    return true;
}


/**
 * @brief Nastaví poslední zprávu řádku zpráv s úrovní.
 * @param v Kopie. @param text Zpráva (NULL = ""). @param level Úroveň (barva řádku).
 */
static void cmt_view_set_message_level ( st_CMT_VIEW *v, const char *text, en_CMT_VIEW_MSG_LEVEL level )
{
    v->message = text ? text : "";
    v->message_is_error = ( level == CMT_VIEW_MSG_ERROR );
    v->message_is_warning = ( level == CMT_VIEW_MSG_WARNING );
}


void cmt_view_set_message ( st_CMT_VIEW *v, const char *text, bool is_error )
{
    cmt_view_set_message_level ( v, text, is_error ? CMT_VIEW_MSG_ERROR : CMT_VIEW_MSG_INFO );
}


/** @brief Poslední zpráva cmt_view_post() pro řádek zpráv (UI vlákno). */
static std::string s_post_text;
/** @brief Úroveň poslední zprávy cmt_view_post(). */
static en_CMT_VIEW_MSG_LEVEL s_post_level = CMT_VIEW_MSG_INFO;
/** @brief Čítač zpráv cmt_view_post() pro řádek (0 = žádná; CMT_VIEW_SERIAL_NONE se přeskočí). */
static unsigned s_post_serial = 0;
/** @brief Snímek ImGui, ve kterém okno naposledy nakreslilo obsah (cmt_view_window_mark_shown()). */
static int s_shown_frame = -1000;
/** @brief Čítač výsledků nahrávání, který už je ohlášený (cmt_view_poll_record()); NONE = ještě nečteno. */
static unsigned s_record_seen = CMT_VIEW_SERIAL_NONE;
#ifdef MZTEST_HEADLESS
/** @brief Testy: vynucená viditelnost okna (-1 = podle kreslení). */
static int s_test_force_shown = -1;

void cmt_view_test_force_window_shown ( int mode )
{
    s_test_force_shown = mode;
}
#endif


void cmt_view_window_mark_shown ( void )
{
    s_shown_frame = ImGui::GetFrameCount ( );
}


void cmt_view_window_mark_hidden ( void )
{
    s_shown_frame = -1000;
}


bool cmt_view_window_shown ( void )
{
#ifdef MZTEST_HEADLESS
    if ( s_test_force_shown >= 0 ) return s_test_force_shown != 0;
#endif
    if ( !ImGui::GetCurrentContext ( ) ) return false;
    return ( ImGui::GetFrameCount ( ) - s_shown_frame ) <= 1;
}


void cmt_view_post ( const char *text, en_CMT_VIEW_MSG_LEVEL level )
{
    if ( !cmt_view_window_shown ( ) ) {
        /* okno není vidět: varování a chyba modálně, informace se zahodí */
        if ( level != CMT_VIEW_MSG_INFO )
            imgui_message_create ( ( level == CMT_VIEW_MSG_ERROR ) ? MESSAGE_TYPE_ERROR : MESSAGE_TYPE_WARNING, text ? text : "" );
        return;
    }
    s_post_text = text ? text : "";
    s_post_level = level;
    s_post_serial++;
    if ( s_post_serial == CMT_VIEW_SERIAL_NONE ) s_post_serial = 1;
}


void cmt_view_post_message ( const char *text, bool is_error )
{
    cmt_view_post ( text, is_error ? CMT_VIEW_MSG_ERROR : CMT_VIEW_MSG_INFO );
}


std::string cmt_view_opened_message ( const char *name, int blocks )
{
    char *text = g_strdup_printf ( _( "Opened %s, blocks: %d" ), ( name && name[0] ) ? name : _( "(new tape)" ), blocks );
    std::string s ( text ? text : "" );
    g_free ( text );
    return s;
}


std::string cmt_view_opened_raw_message ( const char *name, int blocks )
{
    char *text = g_strdup_printf ( _( "Opened %s, blocks: %d. Use Analyze to find files." ), ( name && name[0] ) ? name : _( "(new tape)" ),
                                   blocks );
    std::string s ( text ? text : "" );
    g_free ( text );
    return s;
}


/**
 * @brief Desítkové číslo jádra na pozici @p j: [-]číslice (nejvýš 9), za nimi konec textu nebo znak, který není ASCII písmeno ani číslice.
 * @param in Text. @param j Pozice prvního znaku čísla. @param[out] end Pozice za číslem. @param[out] value Hodnota.
 * @return true = na pozici je číslo v přesném tvaru jádra.
 */
static bool cmt_view_core_number ( const std::string &in, size_t j, size_t *end, long long *value )
{
    const bool neg = ( j < in.size ( ) && in[j] == '-' );
    if ( neg ) j++;
    const size_t d0 = j;
    while ( j < in.size ( ) && in[j] >= '0' && in[j] <= '9' ) j++;
    const size_t nd = j - d0;
    if ( nd == 0 || nd > 9 ) return false;
    if ( j < in.size ( ) && g_ascii_isalnum ( in[j] ) ) return false;
    long long n = 0;
    for ( size_t k = d0; k < j; k++ ) n = n * 10LL + ( long long ) ( in[k] - '0' );
    *value = neg ? -n : n;
    *end = j;
    return true;
}


std::string cmt_view_core_text ( const char *core )
{
    std::string in = core ? core : "";
    std::string out;
    size_t i = 0;
    /* Seznamy čísel bloků: věta, která začíná "Blocks " (na začátku textu, za "; " - jádro spojuje varování
     * středníkem - nebo hned za "(" - varování v závorce za cestou), má za prvním ": " seznam čísel oddělených ", "
     * ("Blocks that cannot be played were skipped: 3, 5 and 2 more", "Blocks the player cannot play (skipped):
     * 3 (ID 0x18), 7 (ID 0x20)"). Věta (i seznam) končí středníkem nebo závorkou, která uzavře úroveň závorek,
     * na které věta začala. */
    bool blocks_sentence = ( in.compare ( 0, 7, "Blocks " ) == 0 );
    int depth = 0, sentence_depth = 0;
    bool in_list = false;
    while ( i < in.size ( ) ) {
        if ( in[i] == ';' ) {
            in_list = false;
            out += in[i++];
            if ( i < in.size ( ) && in[i] == ' ' ) out += in[i++];
            blocks_sentence = ( in.compare ( i, 7, "Blocks " ) == 0 );
            sentence_depth = depth;
            continue;
        }
        if ( in[i] == '(' ) {
            depth++;
            out += in[i++];
            /* nová věta v závorce jen mimo rozpracovanou větu "Blocks ..." ("(skipped)" ji neukončí) */
            if ( !blocks_sentence && in.compare ( i, 7, "Blocks " ) == 0 ) {
                blocks_sentence = true;
                sentence_depth = depth;
            }
            continue;
        }
        if ( in[i] == ')' ) {
            depth--;
            if ( blocks_sentence && depth < sentence_depth ) blocks_sentence = in_list = false;
            out += in[i++];
            continue;
        }
        if ( blocks_sentence && !in_list && in.compare ( i, 2, ": " ) == 0 ) {
            in_list = true;
            out.append ( in, i, 2 );
            i += 2;
            /* první položka seznamu hned za ": " */
            size_t e;
            long long n;
            if ( cmt_view_core_number ( in, i, &e, &n ) ) {
                out += std::to_string ( n + 1LL );
                i = e;
            }
            continue;
        }
        if ( in_list && in.compare ( i, 2, ", " ) == 0 ) {
            out.append ( in, i, 2 );
            i += 2;
            size_t e;
            long long n;
            if ( cmt_view_core_number ( in, i, &e, &n ) ) {
                out += std::to_string ( n + 1LL );
                i = e;
            }
            continue;
        }
        size_t kw = 0;
        if ( in.compare ( i, 6, "Block " ) == 0 || in.compare ( i, 6, "block " ) == 0 ) kw = 6;
        else if ( in.compare ( i, 4, "Row " ) == 0 || in.compare ( i, 4, "row " ) == 0 ) kw = 4;
        const bool word_start = ( i == 0 ) || in[i - 1] == ' ' || in[i - 1] == '(';
        if ( kw && word_start ) {
            size_t e;
            long long n;
            if ( cmt_view_core_number ( in, i + kw, &e, &n ) ) {
                out.append ( in, i, kw );
                out += std::to_string ( n + 1LL );
                i = e;
                continue;
            }
        }
        out += in[i++];
    }
    return out;
}


/** @brief Nejkratší konec cesty, který cmt_view_core_text_path() hledá, když jádro cestu zkrátilo ("...konec cesty"). */
#define CMT_VIEW_PATH_TAIL_MIN 8u

std::string cmt_view_core_text_path ( const char *core, const char *path )
{
    std::string in = core ? core : "";
    if ( !path || !path[0] ) return cmt_view_core_text ( in.c_str ( ) );
    std::string p ( path );
    /* jádro dlouhou cestu zkrátí na "...<konec cesty>" (cmttmz_import_error()): celá cesta v textu není, chrání se
     * nejdelší konec cesty, který v textu je (aspoň CMT_VIEW_PATH_TAIL_MIN bajtů) */
    if ( in.find ( p ) == std::string::npos ) {
        std::string tail;
        for ( size_t k = p.size ( ) - 1; k >= CMT_VIEW_PATH_TAIL_MIN && k < p.size ( ); k-- ) {
            std::string t = p.substr ( p.size ( ) - k );
            if ( in.find ( t ) != std::string::npos ) {
                tail = t;
                break;
            }
        }
        if ( tail.empty ( ) ) return cmt_view_core_text ( in.c_str ( ) );
        p = tail;
    }
    std::string out;
    size_t from = 0;
    for ( ;; ) {
        size_t at = in.find ( p, from );
        if ( at == std::string::npos ) {
            out += cmt_view_core_text ( in.substr ( from ).c_str ( ) );
            break;
        }
        /* text před cestou převést, cestu ponechat beze změny */
        out += cmt_view_core_text ( in.substr ( from, at - from ).c_str ( ) );
        out += p;
        from = at + p.size ( );
    }
    return out;
}


std::string cmt_view_imported_message ( const char *name, int blocks )
{
    char *text = g_strdup_printf ( _( "Imported %s, blocks: %d" ), name ? name : "", blocks );
    std::string s ( text ? text : "" );
    g_free ( text );
    return s;
}


/** @brief Výběr k obnově po úpravě pásky je zapamatovaný (cmt_view_select_after_edit()). */
static bool s_sel_after_edit = false;
/** @brief Čítač výměn pásky po úpravě, ke kterému zapamatovaný výběr patří. */
static unsigned s_sel_after_edit_serial = 0;
/** @brief Blok k výběru po úpravě. */
static int s_sel_after_edit_block = -1;


void cmt_view_select_after_edit ( unsigned tape_serial, int block_id )
{
    s_sel_after_edit = ( block_id >= 0 );
    s_sel_after_edit_serial = tape_serial;
    s_sel_after_edit_block = block_id;
}


bool cmt_view_take_select_after_edit ( unsigned tape_serial, int *block_id )
{
    if ( !s_sel_after_edit ) return false;
    s_sel_after_edit = false;
    if ( s_sel_after_edit_serial != tape_serial ) return false;
    *block_id = s_sel_after_edit_block;
    return true;
}


const char *cmt_view_loading_mode_message ( int mode )
{
    switch ( mode ) {
        case CMT_LOADING_MODE_TAPE: return _( "Loading mode: Tape" );
        case CMT_LOADING_MODE_BOOST: return _( "Loading mode: Boost" );
        case CMT_LOADING_MODE_DIRECT: return _( "Loading mode: Direct" );
        default: return NULL;
    }
}


const char *cmt_view_direct_unsupported_reason ( void )
{
    return _( "The selected ROM does not support direct loading." );
}


void cmt_view_poll_record ( void )
{
    unsigned rec_serial = 0;
    cmttmz_get_record_result ( NULL, NULL, &rec_serial );
    if ( s_record_seen == CMT_VIEW_SERIAL_NONE ) {
        /* první čtení: starší výsledek se neohlásí */
        s_record_seen = rec_serial;
        return;
    }
    if ( s_record_seen == rec_serial ) return;
    s_record_seen = rec_serial;
    cmt_view_post ( cmttmz_get_record_message ( ),
                    cmttmz_get_record_discarded ( ) ? CMT_VIEW_MSG_WARNING : CMT_VIEW_MSG_INFO );
}


/** @brief Čítač výsledků přímého uložení, který už je ohlášený (cmt_view_poll_direct_save()); platí jen při s_direct_save_init. */
static unsigned s_direct_save_seen = 0;
/** @brief s_direct_save_seen je načtený (první čtení převezme jen čítač). */
static bool s_direct_save_init = false;


void cmt_view_poll_direct_save ( void )
{
    st_CMT_DIRECT_SAVE_RESULT r;
    cmt_direct_save_get_result ( &r );
    if ( !s_direct_save_init ) {
        /* první čtení: starší výsledek se neohlásí */
        s_direct_save_init = true;
        s_direct_save_seen = r.serial;
        return;
    }
    if ( s_direct_save_seen == r.serial ) return;
    s_direct_save_seen = r.serial;
    if ( !r.ok ) {
        cmt_view_post ( _( "The ROM SAVE could not be stored on the tape." ), CMT_VIEW_MSG_ERROR );
        return;
    }
    /* čísla bloků v okně od 1 */
    char text[256];
    snprintf ( text, sizeof ( text ), _( "ROM SAVE stored \"%s\" (%u bytes) as tape block %d." ),
               r.name, ( unsigned ) r.size, r.block_id + 1 );
    cmt_view_post ( text, CMT_VIEW_MSG_INFO );
}


/** @brief Čítač zahozených operací okna, který už je ohlášený (cmt_view_poll_dropped()); platí jen při s_dropped_init. */
static unsigned s_dropped_seen = 0;
/** @brief s_dropped_seen je načtený (první čtení převezme jen čítač). */
static bool s_dropped_init = false;


void cmt_view_poll_dropped ( void )
{
    unsigned n = cmt_request_get_window_drop_serial ( );
    if ( !s_dropped_init ) {
        /* první čtení: zahozené operace z doby před ním se neohlásí */
        s_dropped_init = true;
        s_dropped_seen = n;
        return;
    }
    if ( n == s_dropped_seen ) return;
    s_dropped_seen = n;
    /* každá změna čítače zjištěná tímto voláním = jedno varování (bez časového
     * slučování; operace zahozené mezi dvěma voláními dají jedno) */
    cmt_view_post ( _ ( "The tape state changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
}


/** @brief Čítač varování návratu pásky po přehrání, který už je ohlášený (cmt_view_poll_replay_warning()); platí jen při s_replay_warn_init. */
static unsigned s_replay_warn_seen = 0;
/** @brief s_replay_warn_seen je načtený (první čtení převezme jen čítač). */
static bool s_replay_warn_init = false;


void cmt_view_poll_replay_warning ( void )
{
    unsigned n = 0;
    const char *text = replay_media_get_cmt_warning ( &n );
    if ( !s_replay_warn_init ) {
        s_replay_warn_init = true;
        s_replay_warn_seen = n;
        return;
    }
    if ( n == s_replay_warn_seen ) return;
    s_replay_warn_seen = n;
    /* text jádra anglicky (jako zprávy nahrávání) */
    if ( text && text[0] ) cmt_view_post ( text, CMT_VIEW_MSG_WARNING );
}


/**
 * @brief Popis řídicího nebo informačního bloku pro sloupec Status.
 *
 * Parametry podle čtení playeru (tmz_player.c: handle_loop_start() čte
 * počet opakování jako LE word, handle_jump() posun jako LE int16, 0x2A
 * a Pause 0 pásku zastaví) a tvorby bloku 0x30 (tzx.c:
 * tzx_block_create_text_description(): délka + text). Jiné bloky jménem
 * typu (tmz_block_id_name()).
 *
 * @param b Blok souboru pásky. @param fallback Jméno typu bloku.
 * @return Přeložený popis ("Loop start x3", "Jump +2", "Stop the tape", ...).
 */
static std::string cmt_view_control_text ( const st_TZX_BLOCK *b, const std::string &fallback )
{
    char buf[96];
    uint32_t len = b->data ? b->length : 0;
    switch ( b->id ) {
        case TZX_BLOCK_ID_LOOP_START:
            if ( len >= 2 ) {
                snprintf ( buf, sizeof ( buf ), _( "Loop start x%u" ), ( unsigned ) ( b->data[0] | ( b->data[1] << 8 ) ) );
                return std::string ( buf );
            }
            return _( "Loop start" );
        case TZX_BLOCK_ID_LOOP_END: return _( "Loop end" );
        case TZX_BLOCK_ID_JUMP:
            if ( len >= 2 ) {
                snprintf ( buf, sizeof ( buf ), _( "Jump %+d" ), ( int ) ( int16_t ) ( b->data[0] | ( b->data[1] << 8 ) ) );
                return std::string ( buf );
            }
            return _( "Jump" );
        case TZX_BLOCK_ID_STOP_48K: return _( "Stop the tape" );
        case TZX_BLOCK_ID_PAUSE: return _( "Stop the tape" );
        case TZX_BLOCK_ID_GROUP_START: return _( "Group start" );
        case TZX_BLOCK_ID_GROUP_END: return _( "Group end" );
        case TZX_BLOCK_ID_TEXT_DESCRIPTION:
            if ( len >= 1 ) {
                /* text bloku je ASCII: jiné bajty jako '?', nejvýš 64 znaků */
                uint32_t n = b->data[0];
                if ( n > len - 1 ) n = len - 1;
                if ( n > 64 ) n = 64;
                std::string t;
                for ( uint32_t k = 0; k < n; k++ ) {
                    unsigned char c = b->data[1 + k];
                    t += ( c >= 0x20 && c < 0x7F ) ? ( char ) c : '?';
                }
                return t;
            }
            return fallback;
        default: return fallback;
    }
}


/**
 * @brief Prázdný blok kopie s výchozími hodnotami (bez MZF, délka neznámá).
 * @param id Index bloku.
 * @return Blok.
 */
static st_CMT_VIEW_BLOCK cmt_view_block_blank ( int id )
{
    st_CMT_VIEW_BLOCK b;
    b.id = id;
    b.kind = CMT_VIEW_KIND_TZX;
    b.type_text = "-";
    b.has_mzf = false;
    b.fsize = b.fstrt = b.fexec = -1;
    b.speed_kind = CMT_VIEW_SPEED_NONE;
    b.cmtspeed = CMTSPEED_NONE;
    b.speed_is_mz = true;
    for ( int k = 0; k < 4; k++ ) b.pulses_us[k] = 0.0;
    b.length_s = -1.0;
    b.status = CMT_VIEW_STATUS_OK;
    b.source = CMT_VIEW_SOURCE_TAPE_FILE;
    b.sample_rate = 0;
    b.pulse_count = 0;
    b.playable = true;
    b.selectable = true;
    b.unsaved = false;
    b.profile_convertible = false;
    return b;
}


/**
 * @brief Sestaví jeden blok kopie z indexu pásky a údajů bloku pro UI.
 *
 * @param c Kontejner vložené pásky (SIMPLE_TAPE).
 * @param i Index bloku.
 * @param src Zdroj importu pásky (cmttmz_get_source()).
 * @return Blok kopie (vlastní své řetězce).
 * @pre UI vlákno pod device_state_lock().
 */
static st_CMT_VIEW_BLOCK cmt_view_block_from_core ( st_CMTEXT_CONTAINER *c, int i, en_CMTTMZ_SOURCE src )
{
    st_CMT_VIEW_BLOCK b = cmt_view_block_blank ( i );
    const st_CMTEXT_TAPE_INDEX *ix = &c->tape->index[i];
    st_CMTTMZ_BLOCK_UI bui;
    cmttmz_get_block_ui ( i, &bui );

    switch ( ix->bltype ) {
        case CMTEXT_BLOCK_TYPE_MZF:
        {
            b.kind = CMT_VIEW_KIND_MZF;
            b.has_mzf = true;
            char utf8[MZF_FNAME_UTF8_BUF_SIZE];
            utf8[0] = '\0';
            if ( bui.mzf_name_len ) sharpmz_str_to_utf8 ( bui.mzf_name, bui.mzf_name_len, utf8, sizeof ( utf8 ), SHARPMZ_CHARSET_EU );
            b.name = utf8;
            b.type_text = cmt_view_mzf_type_text ( ix->item.mzf.ftype );
            b.fsize = ix->item.mzf.fsize;
            b.fstrt = ix->item.mzf.fstrt;
            b.fexec = ix->item.mzf.fexec;
            /* profil bloku 0x46 a převoditelnost: jen při sestavení seznamu, ne každý snímek */
            b.profile_convertible = ( cmttmz_block_profile_convertible ( i ) != 0 );
            char pid[64];
            if ( EXIT_SUCCESS == cmttmz_block_profile ( i, pid, sizeof ( pid ) ) ) b.profile_id = pid;
            break;
        }
        case CMTEXT_BLOCK_TYPE_TAPHEADER:
            b.kind = CMT_VIEW_KIND_TAP_HEADER;
            b.name = ix->item.taphdr.fname ? ix->item.taphdr.fname : "";
            b.type_text = cmttmz_tap_code_txt ( ix->item.taphdr.code );
            break;
        case CMTEXT_BLOCK_TYPE_TAPDATA:
            b.kind = CMT_VIEW_KIND_TAP_DATA;
            b.name = _( "ZX data" );
            b.type_text = cmttmz_tap_code_txt ( -1 );
            break;
        case CMTEXT_BLOCK_TYPE_WAV:
            b.kind = CMT_VIEW_KIND_RAW;
            b.name = _( "WAV recording" );
            break;
        case CMTEXT_BLOCK_TYPE_TZX:
            if ( ix->item.tzx.id == TZX_BLOCK_ID_CSW_RECORDING ) {
                b.kind = CMT_VIEW_KIND_RAW;
                b.name = _( "Raw recording" );
            } else {
                char buf[96];
                snprintf ( buf, sizeof ( buf ), "TZX 0x%02X %s", ix->item.tzx.id, ix->item.tzx.name ? ix->item.tzx.name : "" );
                b.kind = CMT_VIEW_KIND_TZX;
                b.name = buf;
            }
            break;
        case CMTEXT_BLOCK_TYPE_CONTROL:
        default:
            b.kind = CMT_VIEW_KIND_CONTROL;
            b.name = ix->item.tzx.name ? ix->item.tzx.name : "";
            break;
    }

    /* rychlost */
    switch ( ix->blspeed ) {
        case CMTEXT_BLOCK_SPEED_DEFAULT:
            b.speed_kind = CMT_VIEW_SPEED_DEFAULT;
            b.speed_text = cmt_view_speed_text ( g_cmt.mz_cmtspeed );
            break;
        case CMTEXT_BLOCK_SPEED_SET:
        {
            b.speed_kind = CMT_VIEW_SPEED_SET;
            en_CMTSPEED s = cmtext_container_get_block_cmt_speed ( c, i );
            /* TAP: CMTSPEED_NONE znamená 1:1 */
            b.speed_text = cmt_view_speed_text ( ( s == CMTSPEED_NONE ) ? CMTSPEED_1_1 : s );
            b.cmtspeed = ( int ) s;
            break;
        }
        case CMTEXT_BLOCK_SPEED_NONE:
        default:
            b.speed_kind = CMT_VIEW_SPEED_NONE;
            break;
    }
    if ( b.has_mzf && b.speed_kind != CMT_VIEW_SPEED_NONE ) {
        st_MZTAPE_PULSES_LENGTH p;
        if ( 0 == cmt_tape_get_block_pulses ( i, &p ) ) {
            b.pulses_us[0] = p.long_pulse.high * 1e6;
            b.pulses_us[1] = p.long_pulse.low * 1e6;
            b.pulses_us[2] = p.short_pulse.high * 1e6;
            b.pulses_us[3] = p.short_pulse.low * 1e6;
        }
    }

    /* seznam rychlostí: bloky ZX TAP mají vlastní (ImGuiCmt::getZxSpeedList()) */
    b.speed_is_mz = ( b.kind != CMT_VIEW_KIND_TAP_HEADER && b.kind != CMT_VIEW_KIND_TAP_DATA );

    /* délka, stav, původ */
    if ( bui.length_state == CMTTMZ_LENGTH_KNOWN ) b.length_s = bui.length_s;
    else if ( bui.length_state == CMTTMZ_LENGTH_NO_SIGNAL && b.kind == CMT_VIEW_KIND_CONTROL ) b.length_s = 0.0;
    else b.length_s = -1.0;

    /* příznak bloku, ne poloha v pásce: přesunutý neuložený blok zůstane neuložený */
    b.unsaved = ( bui.unsaved != 0 );
    /* neuložený blok ukazuje Unsaved vždy, s předností před všemi ostatními stavy
     * (dřív rozhodnutí D-G3-2 dávalo přednost Not decoded): modál
     * mazání se ptá přesně na bloky, které tabulka ukazuje jako Unsaved */
    if ( b.unsaved ) b.status = CMT_VIEW_STATUS_UNSAVED;
    else if ( b.kind == CMT_VIEW_KIND_CONTROL ) b.status = CMT_VIEW_STATUS_CONTROL;
    else if ( bui.unplayable ) b.status = CMT_VIEW_STATUS_UNPLAYABLE;
    else if ( b.kind == CMT_VIEW_KIND_RAW ) b.status = CMT_VIEW_STATUS_NOT_DECODED;
    else b.status = CMT_VIEW_STATUS_OK;
    b.status_text = cmt_view_status_text ( b.status );
    if ( b.status == CMT_VIEW_STATUS_CONTROL ) {
        const st_TZX_FILE *f = cmttmz_get_file ( );
        if ( f && ( uint32_t ) i < f->block_count ) b.status_text = cmt_view_control_text ( &f->blocks[i], b.name );
    }

    /* importovaný blok podle vlastního původu, ne podle zdroje celé pásky; po uložení
     * je to blok souboru pásky */
    if ( bui.recorded ) b.source = CMT_VIEW_SOURCE_RECORDED;
    else if ( bui.imported && bui.unsaved ) b.source = CMT_VIEW_SOURCE_IMPORTED;
    else if ( b.has_mzf && ( src == CMTTMZ_SOURCE_MZF || src == CMTTMZ_SOURCE_MZT ) ) b.source = CMT_VIEW_SOURCE_IMPORTED_MZF;
    else b.source = CMT_VIEW_SOURCE_TAPE_FILE;

    b.sample_rate = bui.csw_rate;
    b.pulse_count = bui.csw_pulses;
    b.playable = ( b.kind != CMT_VIEW_KIND_CONTROL ) && !bui.unplayable;
    b.selectable = b.playable;
    return b;
}


/**
 * @brief Spočítá pozici od začátku pásky, délku pásky a průběh bloku z bloků kopie.
 *
 * Délka pásky = součet délek bloků (řídicí a nepřehratelné 0); neznámá,
 * je-li neznámá délka některého bloku se signálem. Pozice = součet délek
 * bloků před aktuálním + pozice v aktuálním (pořadí podle indexu; smyčky
 * a skoky pásky se nepočítají).
 *
 * @param v Kopie s vyplněnými blocks a current_block.
 * @param pos_s Pozice v aktuálním bloku v s (< 0 = neznámá).
 */
static void cmt_view_compute_times ( st_CMT_VIEW *v, double pos_s )
{
    double total = 0.0;
    bool total_known = true;
    double before = 0.0;
    bool before_known = true;
    for ( const st_CMT_VIEW_BLOCK &b : v->blocks ) {
        double len = b.length_s;
        if ( !b.playable ) len = ( len > 0.0 ) ? len : 0.0;
        if ( len < 0.0 ) {
            total_known = false;
            if ( b.id < v->current_block ) before_known = false;
            continue;
        }
        total += len;
        if ( b.id < v->current_block ) before += len;
    }
    v->tape_total_s = total_known ? total : -1.0;
    v->lengths_pending = !total_known;
    v->block_progress = 0.0;
    v->block_pos_s = -1.0;
    if ( v->state == CMT_VIEW_STATE_RECORD ) {
        v->tape_elapsed_s = v->tape_total_s;
        return;
    }
    if ( v->current_block >= 0 && pos_s >= 0.0 ) v->block_pos_s = pos_s;
    if ( v->current_block < 0 || pos_s < 0.0 || !before_known ) {
        v->tape_elapsed_s = ( v->current_block < 0 && v->has_tape ) ? 0.0 : -1.0;
        if ( !v->has_tape ) v->tape_elapsed_s = -1.0;
        return;
    }
    v->tape_elapsed_s = before + pos_s;
    double len = v->blocks[( size_t ) v->current_block].length_s;
    if ( len > 0.0 ) {
        double p = pos_s / len;
        v->block_progress = ( p < 0.0 ) ? 0.0 : ( ( p > 1.0 ) ? 1.0 : p );
    }
}


/**
 * @brief Kontejner vložené pásky (jako cmt_view_fill()), NULL bez pásky.
 * @return Kontejner typu SIMPLE_TAPE s páskou, nebo NULL.
 * @pre UI vlákno pod device_state_lock().
 */
static st_CMTEXT_CONTAINER *cmt_view_tape_container ( void )
{
    st_CMTEXT_CONTAINER *c = ( CMT_TEST_FILLED && g_cmt.ext ) ? cmtext_get_container ( g_cmt.ext ) : NULL;
    if ( !c || cmtext_container_get_type ( c ) != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE || !c->tape ) return NULL;
    return c;
}


void cmt_view_fill ( st_CMT_VIEW *v )
{
    /* výsledek nahrávání: ohlásí se jednou přes cmt_view_post() (při
     * skrytém okně modálně nebo vůbec, jinak do řádku níže) */
    unsigned rec_serial = 0;
    cmttmz_get_record_result ( NULL, NULL, &rec_serial );
    if ( v->record_serial == CMT_VIEW_SERIAL_NONE && s_record_seen == CMT_VIEW_SERIAL_NONE ) {
        /* první plnění před prvním cmt_view_poll_record(): starší výsledek se neohlásí */
        s_record_seen = rec_serial;
    }
    v->record_serial = rec_serial;
    cmt_view_poll_record ( );
    /* ROM SAVE uložený přímo na pásku (Direct Save): jedna zpráva */
    cmt_view_poll_direct_save ( );
    /* operace okna zahozené emu vláknem kvůli změně decku: jedno varování */
    cmt_view_poll_dropped ( );
    /* rychlosti bloků nevrácené po přehrání záznamu vstupů (jiný soubor pásky): jedno varování */
    cmt_view_poll_replay_warning ( );
    /* zprávy cmt_view_post(); první plnění převezme jen čítač, stará zpráva
     * z doby před otevřením okna se neukáže */
    if ( v->post_serial == CMT_VIEW_SERIAL_NONE ) {
        v->post_serial = s_post_serial;
    } else if ( v->post_serial != s_post_serial ) {
        v->post_serial = s_post_serial;
        cmt_view_set_message_level ( v, s_post_text.c_str ( ), s_post_level );
    }

    v->mode = CMTHACK_TEST_IS_INSTALLED ? CMT_VIEW_MODE_DIRECT
        : ( CMT_TEST_CPU_BOOST_ENABLED ? CMT_VIEW_MODE_BOOST : CMT_VIEW_MODE_TAPE );
    /* táž podmínka jako odmítnutí CMT_LOAD_DIRECTLY_NO_HACK (cmt_load_directly_apply());
     * ROM mění emu vlákno pod zámkem stavu zařízení, který plnění drží */
    v->direct_supported = ( CMTHACK_TEST_IS_INSTALLED || cmthack_rom_supported ( ) ) ? true : false;
    v->echo = ( g_cmt.echo != 0 );
    v->default_speed = ( int ) g_cmt.mz_cmtspeed;
    v->default_speed_text = cmt_view_speed_text ( g_cmt.mz_cmtspeed );
    v->default_pulses_us[0] = g_cmt.mz_custom_pulses.long_pulse.high * 1e6;
    v->default_pulses_us[1] = g_cmt.mz_custom_pulses.long_pulse.low * 1e6;
    v->default_pulses_us[2] = g_cmt.mz_custom_pulses.short_pulse.high * 1e6;
    v->default_pulses_us[3] = g_cmt.mz_custom_pulses.short_pulse.low * 1e6;
    {
        int rm = g_atomic_int_get ( &g_replay_mode );
        v->replay_recording = ( rm == REPLAY_MODE_RECORDING );
        v->replay_playing = ( rm == REPLAY_MODE_PLAYING );
    }
    cmt_motor_get_state ( &v->motor_known, &v->motor_on );

    st_CMTEXT_CONTAINER *c = cmt_view_tape_container ( );
    if ( !c ) {
        /* bez pásky: nic z dřívější pásky nezůstane */
        v->has_tape = false;
        v->tape_name.clear ( );
        v->tape_path.clear ( );
        v->modified = false;
        v->open_serial = cmttmz_get_open_serial ( );
        v->change_serial = cmttmz_get_change_serial ( );
        v->tape_serial = cmttmz_get_tape_serial ( );
        v->state = CMT_VIEW_STATE_STOP;
        v->paused = false;
        v->user_paused = false;
        v->block_count = 0;
        v->current_block = -1;
        v->tape_elapsed_s = -1.0;
        v->tape_total_s = -1.0;
        v->block_pos_s = -1.0;
        v->block_progress = 0.0;
        v->rec_time_s = 0.0;
        v->rec_pulses = 0;
        v->direct_block_loadable = false;
        v->blocks.clear ( );
        v->lengths_pending = false;
        return;
    }

    v->has_tape = true;
    const char *name = cmtext_container_get_name ( c );
    const char *path = cmtext_container_get_filepath ( c );
    v->tape_name = name ? name : "";
    v->tape_path = path ? path : "";
    v->modified = cmttmz_is_modified ( ) ? true : false;
    v->state = CMT_TEST_RECORD ? CMT_VIEW_STATE_RECORD : ( CMT_TEST_PLAY ? CMT_VIEW_STATE_PLAY : CMT_VIEW_STATE_STOP );
    v->paused = CMT_TEST_PAUSED ? true : false;
    v->user_paused = ( v->paused && g_cmt.user_paused ) ? true : false;
    v->tape_serial = cmttmz_get_tape_serial ( );

    unsigned open_serial = cmttmz_get_open_serial ( );
    unsigned change_serial = cmttmz_get_change_serial ( );
    int count = cmtext_container_get_count_blocks ( c );
    if ( open_serial != v->open_serial || change_serial != v->change_serial || ( int ) v->blocks.size ( ) != count ) {
        v->blocks.clear ( );
        v->blocks.reserve ( ( size_t ) count );
        en_CMTTMZ_SOURCE src = cmttmz_get_source ( );
        for ( int i = 0; i < count; i++ ) v->blocks.push_back ( cmt_view_block_from_core ( c, i, src ) );
        v->open_serial = open_serial;
        v->change_serial = change_serial;
    }
    v->block_count = ( int ) v->blocks.size ( );

    double pos_s = -1.0;
    if ( v->state == CMT_VIEW_STATE_RECORD ) {
        v->current_block = v->block_count;
        cmt_record_get_progress ( &v->rec_time_s, &v->rec_pulses );
    } else {
        v->rec_time_s = 0.0;
        v->rec_pulses = 0;
        int id = -1;
        double p = 0.0;
        if ( EXIT_SUCCESS == cmt_tape_get_ui_position ( &id, &p ) && id < v->block_count ) {
            v->current_block = id;
            pos_s = p;
        } else {
            id = cmttmz_get_open_block_id ( );
            v->current_block = ( id >= 0 && id < v->block_count ) ? id : -1;
        }
    }
    cmt_view_compute_times ( v, pos_s );
    /* příští LOAD s CMT hackem vezme aktuální blok s MZF, nezjistil-li na něm
     * hack konec pásky (cmthack_tape_load_header(); stav platí do pohybu pásky) */
    v->direct_block_loadable = ( v->current_block >= 0 && v->current_block < v->block_count )
        && v->blocks[( size_t ) v->current_block].has_mzf;
    if ( v->direct_block_loadable && CMTHACK_TEST_IS_INSTALLED ) {
        st_CMTHACK_TAPE_STATE hs;
        if ( cmthack_tape_state_get ( &hs ) && hs.end ) v->direct_block_loadable = false;
    }
}


bool cmt_view_read_tape_serial ( unsigned *serial, int *block_count )
{
    if ( serial ) *serial = cmttmz_get_tape_serial ( );
    st_CMTEXT_CONTAINER *c = cmt_view_tape_container ( );
    if ( block_count ) *block_count = c ? cmtext_container_get_count_blocks ( c ) : 0;
    return c != NULL;
}


bool cmt_view_read_block ( int block_id, st_CMT_VIEW_BLOCK *out )
{
    st_CMTEXT_CONTAINER *c = cmt_view_tape_container ( );
    if ( !c || block_id < 0 || block_id >= cmtext_container_get_count_blocks ( c ) ) return false;
    *out = cmt_view_block_from_core ( c, block_id, cmttmz_get_source ( ) );
    return true;
}


bool cmt_view_tape_single_raw ( void )
{
    st_CMTEXT_CONTAINER *c = cmt_view_tape_container ( );
    if ( !c || cmtext_container_get_count_blocks ( c ) != 1 ) return false;
    return cmt_view_block_from_core ( c, 0, cmttmz_get_source ( ) ).kind == CMT_VIEW_KIND_RAW;
}


/* ------------------------------------------------------------------------- */
/*  Testovací stavy (koncepty v2)                                            */
/* ------------------------------------------------------------------------- */


/**
 * @brief Blok MZF pro fixture.
 * @param id Index. @param name Jméno (UTF-8). @param ftype Typ MZF. @param fsize Velikost těla.
 * @param length_s Délka v s. @param st Stav.
 * @return Blok (rychlost DEFAULT 1:1, pulzy UniCMT 1x z konceptu, původ z importu MZF).
 */
static st_CMT_VIEW_BLOCK cmt_view_fixture_mzf ( int id, const char *name, int ftype, int fsize, double length_s,
                                                en_CMT_VIEW_STATUS st )
{
    st_CMT_VIEW_BLOCK b = cmt_view_block_blank ( id );
    b.kind = CMT_VIEW_KIND_MZF;
    b.has_mzf = true;
    b.name = name;
    b.type_text = cmt_view_mzf_type_text ( ftype );
    b.fsize = fsize;
    b.fstrt = 0x1200;
    b.fexec = 0x1200;
    b.speed_kind = CMT_VIEW_SPEED_DEFAULT;
    b.speed_text = cmt_view_speed_text ( CMTSPEED_1_1 );
    /* koncept v2 (SPEC K10): "Long pulse (µs) 470 / 494" a krátký pulz UniCMT 1x 240 / 278 */
    b.pulses_us[0] = 470.0;
    b.pulses_us[1] = 494.0;
    b.pulses_us[2] = 240.0;
    b.pulses_us[3] = 278.0;
    b.length_s = length_s;
    b.status = st;
    b.status_text = cmt_view_status_text ( st );
    b.source = CMT_VIEW_SOURCE_TAPE_FILE;
    return b;
}


/**
 * @brief Pět bloků konceptu v2 (LOADER, FLAPPY, HISCORE, Raw recording, SAVED GAME).
 * @param v Kopie (blocks se přepíše).
 * @param recording true = varianta cmt-full-recording-v2 (HISCORE OK, SAVED GAME OK).
 */
static void cmt_view_fixture_games ( st_CMT_VIEW *v, bool recording )
{
    v->blocks.clear ( );
    v->blocks.push_back ( cmt_view_fixture_mzf ( 0, "LOADER", 0x01, 1024, 21.0, CMT_VIEW_STATUS_OK ) );
    v->blocks.push_back ( cmt_view_fixture_mzf ( 1, "FLAPPY", 0x01, 12288, 112.0, CMT_VIEW_STATUS_OK ) );
    v->blocks.push_back ( cmt_view_fixture_mzf ( 2, "HISCORE", 0x03, 256, 9.0,
                                                 recording ? CMT_VIEW_STATUS_OK : CMT_VIEW_STATUS_CHECKSUM_ERROR ) );
    st_CMT_VIEW_BLOCK raw = cmt_view_block_blank ( 3 );
    raw.kind = CMT_VIEW_KIND_RAW;
    raw.name = _( "Raw recording" );
    raw.type_text = "-";
    raw.speed_kind = CMT_VIEW_SPEED_NONE;
    raw.length_s = 44.0;
    /* nahraný a neuložený (páska Modified): Unsaved má přednost před Not decoded */
    raw.unsaved = !recording;
    raw.status = raw.unsaved ? CMT_VIEW_STATUS_UNSAVED : CMT_VIEW_STATUS_NOT_DECODED;
    raw.status_text = cmt_view_status_text ( raw.status );
    raw.source = CMT_VIEW_SOURCE_RECORDED;
    raw.sample_rate = 8860800;
    raw.pulse_count = 61230;
    v->blocks.push_back ( raw );
    st_CMT_VIEW_BLOCK saved = cmt_view_fixture_mzf ( 4, "SAVED GAME", 0x01, 4096, 38.0,
                                                     recording ? CMT_VIEW_STATUS_OK : CMT_VIEW_STATUS_UNSAVED );
    saved.source = CMT_VIEW_SOURCE_RECORDED;
    saved.unsaved = !recording;
    v->blocks.push_back ( saved );
    v->block_count = ( int ) v->blocks.size ( );
}


/**
 * @brief Páska games.tmz s pěti bloky ve stavu PLAY na bloku 2 (FLAPPY), společný základ fixtures.
 * @param v Kopie.
 */
static void cmt_view_fixture_base_playing ( st_CMT_VIEW *v )
{
    cmt_view_reset ( v );
    v->has_tape = true;
    v->tape_name = "games.tmz";
    v->tape_path = "C:/Users/user/Tapes/games.tmz";
    v->modified = true;
    v->open_serial = 1;
    v->change_serial = 1;
    v->tape_serial = 1;
    cmt_view_fixture_games ( v, false );
    v->state = CMT_VIEW_STATE_PLAY;
    v->paused = false;
    v->motor_known = true;
    v->motor_on = true;
    v->current_block = 1;
    /* koncept: 01:05.8 od začátku pásky = LOADER 21 s + 44.8 s v FLAPPY (40 % ze 112 s) */
    v->block_progress = 0.4;
    v->block_pos_s = 0.4 * 112.0;
    v->tape_elapsed_s = 21.0 + 0.4 * 112.0;
    v->tape_total_s = 21.0 + 112.0 + 9.0 + 44.0 + 38.0;
    v->mode = CMT_VIEW_MODE_TAPE;
    v->direct_block_loadable = true;
    /* skutečný text jádra po nahrání (cmt_tmz_record.c: "Recorded " + "\"%s\", %u B"; jádro se nepřekládá) */
    cmt_view_set_message ( v, "Recorded \"SAVED GAME\", 4096 B", false );
}


void cmt_view_make_fixture ( st_CMT_VIEW *v, en_CMT_VIEW_FIXTURE f )
{
    switch ( f ) {
        case CMT_VIEW_FIXTURE_PLAYING_FULL:
        case CMT_VIEW_FIXTURE_PLAYING_COMPACT:
            cmt_view_fixture_base_playing ( v );
            break;

        case CMT_VIEW_FIXTURE_NO_TAPE:
            cmt_view_reset ( v );
            v->open_serial = 1;
            v->change_serial = 1;
            v->tape_serial = 1;
            break;

        case CMT_VIEW_FIXTURE_EMPTY_TAPE:
            cmt_view_reset ( v );
            v->has_tape = true;
            v->open_serial = 1;
            v->change_serial = 1;
            v->tape_serial = 1;
            v->tape_elapsed_s = 0.0;
            v->tape_total_s = 0.0;
            /* ve STOP jádro motor nezná; koncept ukazuje "Motor off" (okno rozhodne podle motor_known) */
            v->motor_known = false;
            v->motor_on = false;
            break;

        case CMT_VIEW_FIXTURE_RECORDING:
            cmt_view_fixture_base_playing ( v );
            cmt_view_fixture_games ( v, true );
            v->state = CMT_VIEW_STATE_RECORD;
            v->paused = false;
            v->motor_known = true;
            v->motor_on = true;
            v->current_block = v->block_count;
            v->block_progress = 0.0;
            v->block_pos_s = -1.0;
            v->tape_elapsed_s = v->tape_total_s;
            v->rec_time_s = 12.4;
            v->rec_pulses = 1234;
            v->direct_block_loadable = false;
            /* nahrávání samo nic nehlásí: v řádku zůstává zpráva z otevření pásky */
            cmt_view_set_message ( v, cmt_view_opened_message ( "games.tmz", 5 ).c_str ( ), false );
            break;

        case CMT_VIEW_FIXTURE_WAITING_MOTOR:
            cmt_view_fixture_base_playing ( v );
            v->modified = false;
            /* stav konceptu: blok 2 na začátku, PLAY v pauze řízené počítačem */
            v->blocks[4].status = CMT_VIEW_STATUS_OK;
            v->blocks[4].status_text = cmt_view_status_text ( CMT_VIEW_STATUS_OK );
            v->paused = true;
            v->motor_known = true;
            v->motor_on = false;
            v->block_progress = 0.0;
            v->block_pos_s = 0.0;
            v->tape_elapsed_s = 21.0;
            cmt_view_set_message ( v, cmt_view_opened_message ( "games.tmz", 5 ).c_str ( ), false );
            break;

        case CMT_VIEW_FIXTURE_PAUSED:
            cmt_view_fixture_base_playing ( v );
            /* pauza tlačítkem: motor dál běží, páska stojí ve 40 % bloku FLAPPY */
            v->paused = true;
            v->user_paused = true;
            v->motor_known = true;
            v->motor_on = true;
            cmt_view_set_message ( v, cmt_view_opened_message ( "games.tmz", 5 ).c_str ( ), false );
            break;

        case CMT_VIEW_FIXTURE_DIRECT:
            cmt_view_fixture_base_playing ( v );
            v->modified = false;
            v->blocks[4].status = CMT_VIEW_STATUS_OK;
            v->blocks[4].status_text = cmt_view_status_text ( CMT_VIEW_STATUS_OK );
            v->state = CMT_VIEW_STATE_STOP;
            v->paused = false;
            v->motor_known = true;
            v->motor_on = false;
            v->mode = CMT_VIEW_MODE_DIRECT;
            v->block_progress = 0.0;
            v->block_pos_s = 0.0;
            v->tape_elapsed_s = 21.0;
            v->direct_block_loadable = true;
            /* přímé načtení nic nehlásí: v řádku zůstává zpráva přepnutí režimu */
            cmt_view_set_message ( v, cmt_view_loading_mode_message ( CMT_LOADING_MODE_DIRECT ), false );
            break;

        case CMT_VIEW_FIXTURE_LONG_TEXTS:
        {
            cmt_view_fixture_base_playing ( v );
            /* jméno souboru 200 znaků */
            std::string longname;
            while ( longname.size ( ) < 196 ) longname += "long_tape_name_";
            longname.resize ( 196 );
            longname += ".tmz";
            v->tape_name = longname;
            v->tape_path = std::string ( "C:/Users/user/Tapes/" ) + longname;
            /* jméno bloku 16 znaků, prázdné jméno, znaky mimo ASCII (UTF-8) */
            v->blocks[0].name = "ABCDEFGHIJKLMNOP";
            v->blocks[1].name = "";
            v->blocks[2].name = "\xC3\xA4\xC3\xB6\xC3\xBC SCORE \xC3\x9F"; /* "äöü SCORE ß" */
            /* skutečný text jádra (cmt_tmz.c: "Tape saved: %s (%s)" s varováním "Speed of block %u cannot be stored in MZT") */
            cmt_view_set_message ( v, "Tape saved: C:/Users/user/Tapes/a_very_long_directory_name/another_long_directory_name/games_backup_copy_number_two.tmz (Speed of block 0 cannot be stored in MZT)", false );
            break;
        }

        case CMT_VIEW_FIXTURE_MANY_BLOCKS:
        {
            cmt_view_reset ( v );
            v->has_tape = true;
            v->tape_name = "many.tmz";
            v->tape_path = "C:/Users/user/Tapes/many.tmz";
            v->open_serial = 1;
            v->change_serial = 1;
            v->tape_serial = 1;
            v->blocks.reserve ( 300 );
            for ( int i = 0; i < 300; i++ ) {
                char name[32];
                snprintf ( name, sizeof ( name ), "PROGRAM %03d", i + 1 );
                v->blocks.push_back ( cmt_view_fixture_mzf ( i, name, 0x01, 1024 + i, 20.0 + ( i % 7 ), CMT_VIEW_STATUS_OK ) );
            }
            v->block_count = 300;
            v->state = CMT_VIEW_STATE_PLAY;
            v->motor_known = true;
            v->motor_on = true;
            v->current_block = 150;
            double before = 0.0, total = 0.0;
            for ( const st_CMT_VIEW_BLOCK &b : v->blocks ) {
                total += b.length_s;
                if ( b.id < 150 ) before += b.length_s;
            }
            v->block_progress = 0.25;
            v->block_pos_s = 0.25 * v->blocks[150].length_s;
            v->tape_elapsed_s = before + 0.25 * v->blocks[150].length_s;
            v->tape_total_s = total;
            v->direct_block_loadable = true;
            break;
        }

        case CMT_VIEW_FIXTURE_CONTROL_BLOCKS:
        {
            cmt_view_reset ( v );
            v->has_tape = true;
            v->tape_name = "loops.tzx";
            v->tape_path = "C:/Users/user/Tapes/loops.tzx";
            v->open_serial = 1;
            v->change_serial = 1;
            v->tape_serial = 1;
            /* popisy stejnými formáty jako cmt_view_control_text() (literály pro xgettext),
             * stav podle cmt_view_block_from_core() */
            char loop_desc[64], jump_desc[64];
            snprintf ( loop_desc, sizeof ( loop_desc ), _( "Loop start x%u" ), 3u );
            snprintf ( jump_desc, sizeof ( jump_desc ), _( "Jump %+d" ), 2 );
            struct {
                const char *name;
                std::string desc;
            } ctl[3] = {
                { "Loop Start", loop_desc }, { "Loop End", _( "Loop end" ) }, { "Jump to Block", jump_desc },
            };
            v->blocks.push_back ( cmt_view_fixture_mzf ( 0, "LOADER", 0x01, 1024, 21.0, CMT_VIEW_STATUS_OK ) );
            for ( int k = 0; k < 3; k++ ) {
                st_CMT_VIEW_BLOCK c = cmt_view_block_blank ( ( k == 0 ) ? 1 : ( k + 2 ) );
                c.kind = CMT_VIEW_KIND_CONTROL;
                c.name = ctl[k].name;
                c.length_s = 0.0;
                c.status = CMT_VIEW_STATUS_CONTROL;
                c.status_text = ctl[k].desc;
                c.playable = false;
                c.selectable = false;
                if ( k == 0 ) {
                    v->blocks.push_back ( c );
                    v->blocks.push_back ( cmt_view_fixture_mzf ( 2, "LEVEL", 0x01, 2048, 30.0, CMT_VIEW_STATUS_OK ) );
                } else {
                    v->blocks.push_back ( c );
                }
            }
            st_CMT_VIEW_BLOCK bad = cmt_view_block_blank ( 5 );
            bad.kind = CMT_VIEW_KIND_TZX;
            bad.name = "TZX 0x15 Direct Recording";
            bad.status = CMT_VIEW_STATUS_UNPLAYABLE;
            bad.status_text = cmt_view_status_text ( bad.status );
            bad.playable = false;
            bad.selectable = false;
            v->blocks.push_back ( bad );
            st_CMT_VIEW_BLOCK stop = cmt_view_block_blank ( 6 );
            stop.kind = CMT_VIEW_KIND_CONTROL;
            stop.name = "Stop the Tape if 48K";
            stop.length_s = 0.0;
            stop.status = CMT_VIEW_STATUS_CONTROL;
            stop.status_text = _( "Stop the tape" );
            stop.playable = false;
            stop.selectable = false;
            v->blocks.push_back ( stop );
            v->block_count = ( int ) v->blocks.size ( );
            v->state = CMT_VIEW_STATE_STOP;
            v->current_block = 0;
            v->block_pos_s = 0.0;
            v->tape_elapsed_s = 0.0;
            /* délka bloku, který player neumí, je neznámá -> délka pásky neznámá */
            v->tape_total_s = -1.0;
            v->lengths_pending = false;
            v->direct_block_loadable = true;
            cmt_view_set_message ( v, cmt_view_opened_message ( "loops.tzx", v->block_count ).c_str ( ), false );
            break;
        }

        case CMT_VIEW_FIXTURE_COUNT:
        default:
            cmt_view_reset ( v );
            break;
    }
}


int cmt_view_fixture_selection ( en_CMT_VIEW_FIXTURE f )
{
    /* cmt-full-v2: vybraný řádek 4 (Raw recording), pozice pásky na řádku 2 */
    return ( f == CMT_VIEW_FIXTURE_PLAYING_FULL ) ? 3 : -1;
}
