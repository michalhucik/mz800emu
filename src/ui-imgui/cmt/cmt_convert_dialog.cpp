/**
 * @file   cmt_convert_dialog.cpp
 * @brief  Modál "Convert to profile - block N" okna Virtual CMT.
 *
 * Rozvržení: ztlumený řádek s aktuálním profilem bloku, seznam profilů
 * (tabulka ID | popis; "(current)" ztlumeně pod ID aktuálního profilu, popis
 * přes celou zbylou šířku a zalomí se jen dlouhý; vybraný řádek výplní
 * výběru tabulky okna row-selected, ne barvou primárního tlačítka; popisy
 * knihovny včetně značky " [unverified]" a s µs, cmt_view_profile_description();
 * seznam se posouvá, když se nevejde),
 * řádek Speed (výběr rychlosti jen u profilu, který ji přijímá, jinak text
 * "Set by the profile"), ztlumená poznámka, že po převodu na jiný profil než
 * MZ800EMU-CMT Set speed rychlost bloku nezmění (znění odliší výběr "teď"
 * a Set speed "potom"), řádek stavu (změna pásky) a tlačítka Convert /
 * Cancel vpravo (společný tvar, imgui_cmt_button()). Celý dialog nejvýš
 * 90 % pracovní plochy.
 *
 * Výběr profilu: klik nebo šipky (výběr jde s fokusem navigace); výchozí je
 * aktuální profil bloku, a není-li v nabídce, nic (Convert zakázané do volby).
 * Rychlost: "Profile default" (CMTSPEED_NONE) nebo poměr ze seznamu
 * rychlostí MZ bez vlastních pulzů; zda ji profil přijme celou, ověří až
 * jádro (zkušební stavba; chyba jde do řádku zpráv).
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
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

#include "i18n.h"

#include "emulator/hw-generic/cmt/cmt_request.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/tmz/tmz_profile.h"
#include "device_request.h"
#include "cmt_view.h"
#include "cmt_tokens.h"
#include "cmt_window.h"
#include "ImGuiCmt.hpp"
#include "cmt_convert_dialog.h"
#include "ui-imgui/imgui_modal.h"


/* ------------------------------------------------------------------------- */
/*  Pseudo-překlad pro testy (texty o 40 % delší, jako cmt_window.cpp)       */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
/* _(), _L() a C_() s testovacím prodloužením textů (společné cmt_view.cpp) */
#undef _
#define _(STRING) cmt_view_test_tr ( STRING )
#define _L(STRING) cmt_view_test_trl ( STRING )
#undef C_
#define C_(CTX, STR) cmt_view_test_trc ( CTX "\004" STR, STR )
#endif


/* ------------------------------------------------------------------------- */
/*  Konstanty a stav                                                         */
/* ------------------------------------------------------------------------- */

/** @brief Stabilní ID modálu (titulek se mění s číslem bloku a jazykem, ID ne). */
#define CCV_POPUP_ID "###cmt_convert"

/**
 * @brief Šířka dialogu v násobcích velikosti písma (omezí ji 90 % plochy).
 *
 * 48 em: při 100 % (28 px) se popisy profilů z knihovny vejdou většinou na
 * jeden řádek (aspoň 8 profilů v jednom řádku).
 */
#define CCV_W_EM 48.0f

/** @brief Nejméně řádků seznamu profilů vidět (jinak se posouvá celý seznam ve zbytku plochy). */
#define CCV_LIST_MIN_ROWS 3

/** @brief Nejvýš tolik převodů čeká na výsledek (starší se zapomenou; okno pošle nejvýš pár). */
#define CCV_PENDING_MAX 32u

/** @brief Položka výběru rychlosti. */
typedef struct st_CCV_SPEED {
    en_CMTSPEED speed; /**< rychlost (CMTSPEED_NONE = výchozí profilu) */
    std::string text;  /**< text položky ("Profile default", "2:1 (2400 Bd)") */
} st_CCV_SPEED;

/** @brief Čekající převod (zpráva po výsledku zásahu). */
typedef struct st_CCV_PENDING {
    uint32_t seq;        /**< pořadové číslo zásahu */
    int block_id;        /**< převáděný blok */
    std::string profile; /**< cílový profil */
} st_CCV_PENDING;

/**
 * @brief Stav dialogu (UI vlákno).
 * @invariant sel == -1 nebo 0 <= sel < profiles.size(); -1 = nic vybraného (aktuální profil bloku není v nabídce, Convert zakázané).
 */
typedef struct st_CCV {
    bool open;                               /**< dialog otevřený */
    bool open_pending;                       /**< modál se má otevřít */
    int drawn_frame;                         /**< snímek posledního kreslení (-1 = žádný) */
    ImGuiID popup_id;                        /**< ID popupu (0 = ještě neotevřený) */
    bool skipped;                            /**< okno kazety v některém snímku obsah nekreslilo (cmt_convert_dialog_skip_frame()); příští kreslení ověří popup */
    int block_id;                            /**< převáděný blok */
    unsigned tape_serial;                    /**< čítač výměn pásky při otevření */
    std::string current;                     /**< aktuální profil bloku */
    std::vector<st_CMT_VIEW_PROFILE> profiles; /**< nabídka profilů */
    int sel;                                 /**< vybraný profil (index), -1 = žádný */
    en_CMTSPEED speed;                       /**< vybraná rychlost (CMTSPEED_NONE = výchozí profilu) */
    int recenter_frames;                     /**< ještě tolik snímků umístit modál doprostřed okna */
    ImVec2 last_size;                        /**< velikost modálu z minulého snímku */
    float list_content_h;                    /**< výška obsahu seznamu z minulého snímku (0 = neznámá) */
    float footer_h;                          /**< výška části pod seznamem z minulého snímku (0 = neznámá) */
    bool scroll_to_sel;                      /**< příští kreslení posune seznam na vybraný profil */
    bool combo_was_open;                     /**< výběr rychlosti byl otevřený v minulém snímku (Escape, který ho zavřel, dialog nezavře) */
} st_CCV;

static st_CCV s_ccv;                              /**< stav dialogu */
static bool s_ccv_init = false;                   /**< pole bez konstruktoru nastavená */
static std::vector<st_CCV_PENDING> s_ccv_pending; /**< převody čekající na výsledek */

#ifdef MZTEST_HEADLESS
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_ccv_items;      /**< prvky rozpracovaného snímku */
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_ccv_items_done; /**< prvky posledního nakresleného snímku */
static ImVec2 s_ccv_wmin, s_ccv_wmax, s_ccv_imin, s_ccv_imax; /**< obdélník modálu a vnitřku */
static bool s_ccv_drawn = false;                               /**< modál se v posledním snímku kreslil */
static std::string s_ccv_title;                                /**< titulek z posledního snímku */
static bool s_ccv_in_list = false;                             /**< záznam prvků posouvaného seznamu */
#endif

/** @brief Nastaví pole bez konstruktoru (jednou). */
static void ccv_init_once ( void )
{
    if ( s_ccv_init ) return;
    s_ccv_init = true;
    s_ccv.drawn_frame = -1;
    s_ccv.block_id = -1;
    s_ccv.sel = -1;
}

/**
 * @brief Zaznamená prvek dialogu pro testy (v aplikaci nic).
 * @param name Jméno. @param text Text. @param min Levý horní roh. @param max Pravý dolní roh.
 * @param disabled Zakázaný. @param reason Důvod zákazu (NULL = žádný).
 */
static void ccv_record ( const char *name, const std::string &text, ImVec2 min, ImVec2 max, bool disabled, const char *reason )
{
#ifdef MZTEST_HEADLESS
    st_CMT_WINDOW_TEST_ITEM it;
    it.name = name;
    it.text = text;
    it.disabled = disabled;
    it.min = min;
    it.max = max;
    ImGuiWindow *w = ImGui::GetCurrentWindow ( );
    it.clip_min = w->ClipRect.Min;
    it.clip_max = w->ClipRect.Max;
    it.container = false;
    it.deck = false;
    it.right = false;
    it.scroll_y = s_ccv_in_list;
    it.popup = true;
    it.reason = reason ? reason : "";
    s_ccv_items.push_back ( it );
#else
    ( void ) name;
    ( void ) text;
    ( void ) min;
    ( void ) max;
    ( void ) disabled;
    ( void ) reason;
#endif
}

/**
 * @brief Označí poslední zaznamenaný prvek jako obal (řádek tabulky obsahuje buňky; z kontroly překryvu se vynechá; jen testy).
 */
static void ccv_record_mark_container ( void )
{
#ifdef MZTEST_HEADLESS
    if ( !s_ccv_items.empty ( ) ) s_ccv_items.back ( ).container = true;
#endif
}

/** @brief Zaznamená poslední položku ImGui (testy). @param name Jméno. @param text Text. @param disabled Zakázaná. @param reason Důvod. */
static void ccv_record_item ( const char *name, const std::string &text, bool disabled, const char *reason )
{
    ccv_record ( name, text, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), disabled, reason );
}

/**
 * @brief Jméno prvku s indexem pro testy ("cv_row_2"); v aplikaci jen základ.
 * @param buf Pole znaků (sizeof musí dát jeho velikost). @param base Základ. @param i Index.
 * @return Jméno prvku.
 */
#ifdef MZTEST_HEADLESS
#define CCV_NAME( buf, base, i ) ( snprintf ( buf, sizeof ( buf ), "%s_%d", base, ( int ) ( i ) ), buf )
#else
#define CCV_NAME( buf, base, i ) ( ( void ) ( i ), base )
#endif


/* ------------------------------------------------------------------------- */
/*  Pomocné funkce                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief Položky výběru rychlosti: výchozí profilu a poměry MZ (bez vlastních pulzů, převod je nepřijme).
 * @return Seznam (texty přeložené v době volání).
 */
static std::vector<st_CCV_SPEED> ccv_speeds ( void )
{
    std::vector<st_CCV_SPEED> out;
    st_CCV_SPEED d;
    d.speed = CMTSPEED_NONE;
    d.text = _ ( "Profile default" );
    out.push_back ( d );
    for ( const ImGuiCmtSpeedList_t &s : ImGuiCmt::getMzSpeedList ( ) ) {
        if ( !cmtspeed_is_valid ( s.cmtspeed ) ) continue;
        st_CCV_SPEED e;
        e.speed = s.cmtspeed;
        e.text = s.cmtspeed_txt;
        out.push_back ( e );
    }
    return out;
}

/** @brief Text vybrané rychlosti. @param speed Rychlost. @return Text položky (výchozí profilu, není-li v seznamu). */
static std::string ccv_speed_text ( en_CMTSPEED speed )
{
    std::vector<st_CCV_SPEED> l = ccv_speeds ( );
    for ( const st_CCV_SPEED &s : l )
        if ( s.speed == speed ) return s.text;
    return l.front ( ).text;
}

/** @brief Přijímá vybraný profil rychlost? @return true = ano. */
static bool ccv_sel_accepts_speed ( void )
{
    return s_ccv.sel >= 0 && s_ccv.sel < ( int ) s_ccv.profiles.size ( ) && s_ccv.profiles[( size_t ) s_ccv.sel].accepts_speed;
}

/** @brief Zavře dialog (stav a popup). @post s_ccv.open == false. */
static void ccv_close ( void )
{
    ccv_init_once ( );
    imgui_cmt_close_popup ( &s_ccv.popup_id );
    s_ccv.open = false;
    s_ccv.open_pending = false;
}


/* ------------------------------------------------------------------------- */
/*  Veřejné funkce                                                           */
/* ------------------------------------------------------------------------- */

void cmt_convert_dialog_open ( const st_CMT_VIEW *v, int block_id )
{
    ccv_close ( );
    if ( !v || block_id < 0 || block_id >= v->block_count ) return;
    const st_CMT_VIEW_BLOCK *b = &v->blocks[( size_t ) block_id];
    s_ccv.block_id = block_id;
    s_ccv.tape_serial = v->tape_serial;
    s_ccv.current = b->profile_id;
    cmt_view_read_profiles ( &s_ccv.profiles );
    s_ccv.sel = -1;
    for ( size_t i = 0; i < s_ccv.profiles.size ( ); i++ )
        if ( s_ccv.profiles[i].id == s_ccv.current ) s_ccv.sel = ( int ) i;
    /* aktuální profil mimo nabídku (neznámý profil ze souboru, profil, který nabídka nenese): nic předvybraného,
     * Convert zakázané, dokud uživatel profil nevybere - převod je nevratný */
    s_ccv.speed = CMTSPEED_NONE;
    s_ccv.open = true;
    s_ccv.open_pending = true;
    s_ccv.skipped = false;
    s_ccv.recenter_frames = 3;
    s_ccv.last_size = ImVec2 ( 0.0f, 0.0f );
    s_ccv.list_content_h = 0.0f;
    s_ccv.footer_h = 0.0f;
    s_ccv.scroll_to_sel = true;
    s_ccv.combo_was_open = false;
}

#ifdef MZTEST_HEADLESS
bool cmt_convert_dialog_is_open ( void )
{
    ccv_init_once ( );
    return s_ccv.open;
}
#endif

void cmt_convert_dialog_abandon ( void )
{
    if ( s_ccv.open ) ccv_close ( );
}

bool cmt_convert_dialog_skip_frame ( void )
{
    ccv_init_once ( );
    if ( !s_ccv.open ) return false;
    /* okno kazety obsah nekreslí: výběr profilu a rychlosti i popup zůstanou */
    s_ccv.drawn_frame = ImGui::GetFrameCount ( );
    s_ccv.skipped = true;
    return true;
}

void cmt_convert_dialog_frame ( void )
{
    ccv_init_once ( );
    const int frame = ImGui::GetCurrentContext ( ) ? ImGui::GetFrameCount ( ) : 0;
    /* okno Virtual CMT dialog v tomto snímku nenakreslilo (zavřené okno; okno bez obsahu ohlásí
     * cmt_convert_dialog_skip_frame()): jako Cancel */
    if ( s_ccv.open && s_ccv.drawn_frame != frame ) ccv_close ( );
}

bool cmt_convert_dialog_request_result ( const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *edit )
{
    ccv_init_once ( );
    if ( 0 != strcmp ( res->name, CMT_REQUEST_CONVERT_BLOCK_NAME ) ) return false;
    st_CCV_PENDING p;
    p.seq = 0;
    p.block_id = edit ? edit->select_block : -1;
    for ( size_t i = 0; i < s_ccv_pending.size ( ); i++ ) {
        if ( s_ccv_pending[i].seq != res->seq ) continue;
        p = s_ccv_pending[i];
        s_ccv_pending.erase ( s_ccv_pending.begin ( ) + ( long ) i );
        break;
    }
    if ( res->status != DEVICE_REQ_DONE ) {
        cmt_view_post ( _ ( "The tape cannot be edited while an input replay is recorded or played." ), CMT_VIEW_MSG_ERROR );
        return true;
    }
    if ( res->rc == CMT_REQUEST_RC_TAPE_CHANGED ) {
        cmt_view_post ( _ ( "The tape changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
        return true;
    }
    if ( res->rc != 0 ) {
        /* text jádra anglicky (nepřekládá se) */
        cmt_view_post ( ( edit && edit->message[0] ) ? cmt_view_core_text ( edit->message ).c_str ( ) : _ ( "The tape could not be edited." ),
                        CMT_VIEW_MSG_ERROR );
        return true;
    }
    if ( edit && edit->message[0] ) {
        /* varování jádra (nepřenesená nastavení profilu) místo zprávy o převodu */
        cmt_view_post ( cmt_view_core_text ( edit->message ).c_str ( ), CMT_VIEW_MSG_WARNING );
    } else if ( edit && edit->count == 0 ) {
        /* převod na tentýž profil beze změny (pásku nezměnil) */
        cmt_view_post ( _ ( "Nothing to change." ), CMT_VIEW_MSG_INFO );
    } else {
        char buf[160];
        snprintf ( buf, sizeof ( buf ), _ ( "Block %d converted to %s." ), p.block_id + 1, p.profile.c_str ( ) );
        cmt_view_post ( buf, CMT_VIEW_MSG_INFO );
    }
    if ( edit ) cmt_view_select_after_edit ( edit->tape_serial, edit->select_block );
    return true;
}


/* ------------------------------------------------------------------------- */
/*  Kreslení                                                                 */
/* ------------------------------------------------------------------------- */

void cmt_convert_dialog_draw ( const st_CMT_VIEW *v, const char *edit_reason )
{
    ccv_init_once ( );
    if ( !s_ccv.open ) return;
    s_ccv.drawn_frame = ImGui::GetFrameCount ( );
#ifdef MZTEST_HEADLESS
    s_ccv_items.clear ( );
    s_ccv_drawn = false;
#endif
    /* po snímcích bez obsahu okna: popup, který mezitím zavřelo ImGui, otevřít znovu (stav dialogu zůstal) */
    if ( s_ccv.skipped ) {
        s_ccv.skipped = false;
        if ( !s_ccv.open_pending && s_ccv.popup_id && !ImGui::IsPopupOpen ( s_ccv.popup_id, ImGuiPopupFlags_None ) ) s_ccv.open_pending = true;
    }
    if ( s_ccv.open_pending ) {
        if ( !imgui_modal_open_when_free ( CCV_POPUP_ID ) ) return;
        s_ccv.open_pending = false;
        s_ccv.popup_id = ImGui::GetID ( CCV_POPUP_ID );
    }
    char title[128];
    snprintf ( title, sizeof ( title ), _ ( "Convert to profile - block %d" ), s_ccv.block_id + 1 );
    std::string full_title = std::string ( title ) + CCV_POPUP_ID;
#ifdef MZTEST_HEADLESS
    s_ccv_title = full_title;
#endif

    const ImGuiStyle &st = ImGui::GetStyle ( );
    const float F = ImGui::GetFrameHeight ( );
    const float Lh = ImGui::GetTextLineHeight ( );
    const ImVec2 work = imgui_cmt_window_work_size ( );
    const float max_w = floorf ( 0.9f * work.x ) - 2.0f * st.WindowPadding.x - 2.0f * st.WindowBorderSize;
    const float max_h = floorf ( 0.9f * work.y );
    const float W = floorf ( ImMin ( CCV_W_EM * ImGui::GetFontSize ( ), max_w ) );
    const float deco = F + 2.0f * st.WindowPadding.y + 2.0f * st.WindowBorderSize;
    const float top_h = Lh + st.ItemSpacing.y;
    const float footer_h = ( s_ccv.footer_h > 0.0f ) ? s_ccv.footer_h : 2.0f * F + 2.0f * st.ItemSpacing.y;
    const float row_h = Lh + 2.0f * st.CellPadding.y;
    const float list_max = ImMax ( ( float ) CCV_LIST_MIN_ROWS * row_h, max_h - deco - top_h - footer_h - 2.0f * st.ItemSpacing.y - 2.0f );
    const float list_content = ( s_ccv.list_content_h > 0.0f ) ? s_ccv.list_content_h : ( float ) s_ccv.profiles.size ( ) * row_h * 2.0f;
    const float list_h = floorf ( ImMin ( list_content, list_max ) );

    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 0.0f, 0.0f ), ImVec2 ( floorf ( 0.9f * work.x ), max_h ) );
    imgui_cmt_window_modal_center ( s_ccv.last_size, s_ccv.recenter_frames > 0 ? ImGuiCond_Always : ImGuiCond_Appearing );
    if ( s_ccv.recenter_frames > 0 ) s_ccv.recenter_frames--;
    if ( !ImGui::BeginPopupModal ( full_title.c_str ( ), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings ) ) {
        s_ccv.popup_id = 0;
        ccv_close ( );
        return;
    }
    const bool appearing = ImGui::IsWindowAppearing ( );
    ImGuiWindow *win = ImGui::GetCurrentWindow ( );
    ImGui::Dummy ( ImVec2 ( W, 0.0f ) );
    const bool tape_changed = !v->has_tape || v->tape_serial != s_ccv.tape_serial;
    bool do_ok = false, do_cancel = false;

    /* --- ztlumený řádek: aktuální profil bloku --- */
    {
        char buf[200];
        snprintf ( buf, sizeof ( buf ), _ ( "Current profile: %s" ), s_ccv.current.c_str ( ) );
        ImVec2 p0 = ImGui::GetCursorScreenPos ( );
        ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT_DIM );
        ImGui::TextUnformatted ( buf );
        ImGui::PopStyleColor ( );
        ccv_record ( "cv_summary", buf, p0, ImGui::GetItemRectMax ( ), false, NULL );
    }

    /* --- seznam profilů --- */
    ImGui::BeginChild ( "##ccv_list", ImVec2 ( W, list_h ), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_Borders, 0 );
#ifdef MZTEST_HEADLESS
    s_ccv_in_list = true;
#endif
    {
        /* "(current)" pod ID aktuálního profilu, popis přes celou zbylou šířku */
        const char *mark = _ ( "(current)" );
        float id_w = ImGui::CalcTextSize ( mark ).x;
        for ( const st_CMT_VIEW_PROFILE &p : s_ccv.profiles ) id_w = ImMax ( id_w, ImGui::CalcTextSize ( p.id.c_str ( ) ).x );
        /* vybraný řádek výplní výběru tabulky okna (row-selected #28456B, jiná než primární tlačítko) */
        ImGui::PushStyleColor ( ImGuiCol_Header, CMT_TOK_ROW_SELECTED );
        ImGui::PushStyleColor ( ImGuiCol_HeaderHovered, CMT_TOK_CONTROL_HOVER );
        ImGui::PushStyleColor ( ImGuiCol_HeaderActive, CMT_TOK_CONTROL_ACTIVE );
        if ( ImGui::BeginTable ( "##ccv_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX ) ) {
            ImGui::TableSetupColumn ( "id", ImGuiTableColumnFlags_WidthFixed, ceilf ( id_w ) );
            ImGui::TableSetupColumn ( "desc", ImGuiTableColumnFlags_WidthStretch );
            char nb[32];
            for ( size_t i = 0; i < s_ccv.profiles.size ( ); i++ ) {
                const st_CMT_VIEW_PROFILE &p = s_ccv.profiles[i];
                const bool cur = ( p.id == s_ccv.current );
                ImGui::TableNextRow ( );
                /* výška řádku podle zalomeného popisu, u aktuálního profilu aspoň dva řádky (ID a "(current)") */
                ImGui::TableSetColumnIndex ( 1 );
                const float desc_w = ImGui::GetContentRegionAvail ( ).x;
                float h = ImGui::CalcTextSize ( p.description.c_str ( ), NULL, false, desc_w ).y;
                if ( cur ) h = ImMax ( h, 2.0f * Lh );
                ImGui::TableSetColumnIndex ( 0 );
                ImGui::PushID ( ( int ) i );
                const bool selected = ( ( int ) i == s_ccv.sel );
                if ( ImGui::Selectable ( "##ccv_row", selected,
                                         ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_SelectOnNav,
                                         ImVec2 ( 0.0f, h ) ) ) {
                    s_ccv.sel = ( int ) i;
                    /* rychlost se u profilu bez rychlosti neposílá; výběr zůstane pro případný návrat */
                }
                if ( selected && s_ccv.scroll_to_sel ) {
                    ImGui::SetScrollHereY ( 0.5f );
                    s_ccv.scroll_to_sel = false;
                }
                if ( selected && appearing ) ImGui::SetItemDefaultFocus ( );
#ifdef MZTEST_HEADLESS
                if ( selected ) {
                    /* výplň vybraného řádku pro testy */
                    char hex[16];
                    snprintf ( hex, sizeof ( hex ), "%08X", ( unsigned ) ImGui::GetColorU32 ( ImGuiCol_Header ) );
                    ccv_record ( "cv_sel_fill", hex, ImVec2 ( 0.0f, 0.0f ), ImVec2 ( 0.0f, 0.0f ), false, NULL );
                }
#endif
                ccv_record_item ( CCV_NAME ( nb, "cv_row", i ), selected ? "1" : "0", false, NULL );
                ccv_record_mark_container ( );
                ImGui::PopID ( );
                ImGui::SameLine ( 0.0f, 0.0f );
                ImVec2 ip = ImGui::GetItemRectMin ( );
                ImGui::SetCursorScreenPos ( ip );
                ImGui::TextUnformatted ( p.id.c_str ( ) );
                ccv_record_item ( CCV_NAME ( nb, "cv_id", i ), p.id, false, NULL );
                if ( cur ) {
                    /* hned pod ID (řádek Selectable má výšku celé buňky, kurzor by šel až pod ni) */
                    ImGui::SetCursorScreenPos ( ImVec2 ( ip.x, ip.y + Lh ) );
                    ImGui::PushStyleColor ( ImGuiCol_Text, selected ? CMT_TOK_TEXT_DIM_SELECTED : CMT_TOK_TEXT_DIM );
                    ImGui::TextUnformatted ( mark );
                    ImGui::PopStyleColor ( );
                    ccv_record_item ( CCV_NAME ( nb, "cv_current", i ), mark, false, NULL );
                }
                ImGui::TableSetColumnIndex ( 1 );
                /* popis z registru knihovny anglicky, nepřekládá se */
                ImGui::PushTextWrapPos ( ImGui::GetCursorPosX ( ) + desc_w );
                /* ztlumený popis; na vybraném (modrém) řádku světlejší ztlumení jako tabulka okna (kontrast) */
                ImGui::PushStyleColor ( ImGuiCol_Text, selected ? CMT_TOK_TEXT_DIM_SELECTED : CMT_TOK_TEXT_DIM );
                ImGui::TextUnformatted ( p.description.c_str ( ) );
                ImGui::PopStyleColor ( );
                ImGui::PopTextWrapPos ( );
                ccv_record_item ( CCV_NAME ( nb, "cv_desc", i ), p.description, false, NULL );
            }
            ImGui::EndTable ( );
        }
        ImGui::PopStyleColor ( 3 );
    }
    {
        ImGuiWindow *lw = ImGui::GetCurrentWindow ( );
        s_ccv.list_content_h = ceilf ( lw->DC.CursorMaxPos.y - lw->DC.CursorStartPos.y + 2.0f * st.WindowPadding.y );
    }
#ifdef MZTEST_HEADLESS
    s_ccv_in_list = false;
#endif
    ImGui::EndChild ( );

    /* --- rychlost, stav, tlačítka --- */
    const float y_footer = ImGui::GetCursorScreenPos ( ).y;
    bool combo_open = false;
    {
        /* řádek Speed: výběr jen u profilu, který rychlost přijímá; jinak text "Set by the profile" na místě výběru
         * (výška řádku stejná, dialog při změně výběru neposkočí) */
        const bool accepts = ccv_sel_accepts_speed ( );
        const char *l_speed = _ ( "Speed" );
        ImGui::AlignTextToFramePadding ( );
        ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT_DIM );
        ImGui::TextUnformatted ( l_speed );
        ImGui::PopStyleColor ( );
        ccv_record_item ( "cv_speed_label", l_speed, false, NULL );
        ImGui::SameLine ( );
        if ( accepts ) {
            ImGui::SetNextItemWidth ( ImMin ( floorf ( 12.0f * ImGui::GetFontSize ( ) ), ImGui::GetContentRegionAvail ( ).x ) );
            const std::string preview = ccv_speed_text ( s_ccv.speed );
            if ( ImGui::BeginCombo ( "##cv_speed", preview.c_str ( ) ) ) {
                combo_open = true;
                std::vector<st_CCV_SPEED> l = ccv_speeds ( );
                for ( size_t k = 0; k < l.size ( ); k++ ) {
                    ImGui::PushID ( ( int ) k );
                    /* stabilní ID položky (text se mění s jazykem) */
                    const std::string item = l[k].text + "###cv_speed_item";
                    if ( ImGui::Selectable ( item.c_str ( ), l[k].speed == s_ccv.speed ) ) s_ccv.speed = l[k].speed;
                    if ( l[k].speed == s_ccv.speed ) ImGui::SetItemDefaultFocus ( );
                    ImGui::PopID ( );
                }
                ImGui::EndCombo ( );
            }
            ccv_record_item ( "cv_speed", preview, false, NULL );
        } else {
            const char *fixed = _ ( "Set by the profile" );
            ImGui::TextUnformatted ( fixed );
            ccv_record_item ( "cv_speed_fixed", fixed, false, NULL );
        }
        /* rychlost bloku jiného profilu než MZ800EMU-CMT okno měnit neumí (Set speed zakázané); znění odliší
         * "teď" (výběr rychlosti zde) a "potom" (Set speed po převodu) */
        if ( s_ccv.sel >= 0 && s_ccv.sel < ( int ) s_ccv.profiles.size ( )
             && s_ccv.profiles[( size_t ) s_ccv.sel].id != TMZ_PROFILE_ID_MZ800EMU_CMT ) {
            const char *note = accepts ? _ ( "Choose the speed here. After the conversion, Set speed cannot change this block." )
                                       : _ ( "After the conversion, Set speed cannot change this block." );
            ImVec2 p0 = ImGui::GetCursorScreenPos ( );
            ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT_DIM );
            ImGui::PushTextWrapPos ( ImGui::GetCursorPosX ( ) + W );
            ImGui::TextUnformatted ( note );
            ImGui::PopTextWrapPos ( );
            ImGui::PopStyleColor ( );
            ccv_record ( "cv_note", note, p0, ImVec2 ( ImMin ( ImGui::GetItemRectMax ( ).x, p0.x + W ), ImGui::GetItemRectMax ( ).y ), false, NULL );
        }
    }
    const en_CMTSPEED send_speed = ccv_sel_accepts_speed ( ) ? s_ccv.speed : CMTSPEED_NONE;
    const bool same = ( s_ccv.sel >= 0 && s_ccv.sel < ( int ) s_ccv.profiles.size ( ) && s_ccv.profiles[( size_t ) s_ccv.sel].id == s_ccv.current
                        && send_speed == CMTSPEED_NONE );
    const char *r_ok = NULL;
    if ( tape_changed ) r_ok = _ ( "The tape changed. Close this dialog and open it again." );
    else if ( edit_reason ) r_ok = edit_reason;
    else if ( s_ccv.sel < 0 ) r_ok = _ ( "Select a profile." );
    else if ( same ) r_ok = _ ( "Nothing to change." );
    if ( tape_changed ) {
        std::string t = _ ( "The tape changed. Close this dialog and open it again." );
        ImVec2 p0 = ImGui::GetCursorScreenPos ( );
        ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_WARN );
        ImGui::PushTextWrapPos ( ImGui::GetCursorPosX ( ) + W );
        ImGui::TextUnformatted ( t.c_str ( ) );
        ImGui::PopTextWrapPos ( );
        ImGui::PopStyleColor ( );
        ccv_record ( "cv_status", t, p0, ImVec2 ( ImMin ( ImGui::GetItemRectMax ( ).x, p0.x + W ), ImGui::GetItemRectMax ( ).y ), false, NULL );
    }
    {
        const char *l_ok = _L ( "Convert" );
        const char *l_cancel = _L ( "Cancel" );
        /* šířky podle společného pravidla tlačítek okna kazety */
        const float bw_ok = imgui_cmt_button_w ( l_ok );
        const float bw_cancel = imgui_cmt_button_w ( l_cancel );
        const float x0 = ImGui::GetCursorScreenPos ( ).x;
        const float y = ImGui::GetCursorScreenPos ( ).y;
        ImGui::SetCursorScreenPos ( ImVec2 ( x0 + W - bw_ok - bw_cancel - st.ItemSpacing.x, y ) );
        if ( imgui_cmt_button ( l_ok, bw_ok, r_ok, true ) ) do_ok = true;
        ccv_record_item ( "cv_ok", std::string ( l_ok, ImGui::FindRenderedTextEnd ( l_ok ) ), r_ok != NULL, r_ok );
        ImGui::SameLine ( );
        if ( imgui_cmt_button ( l_cancel, bw_cancel, NULL, false ) ) do_cancel = true;
        /* nic vybraného: počáteční fokus na Cancel - jinak by ho dostal první řádek seznamu a výběr s fokusem
         * (SelectOnNav) by profil předvybral */
        if ( s_ccv.sel < 0 && appearing ) ImGui::SetItemDefaultFocus ( );
        ccv_record_item ( "cv_cancel", std::string ( l_cancel, ImGui::FindRenderedTextEnd ( l_cancel ) ), false, NULL );
    }
    s_ccv.footer_h = ImGui::GetCursorScreenPos ( ).y - y_footer;
    /* klávesy: Enter = Convert, Escape = Cancel - ne při otevřeném výběru rychlosti a ani v prvním snímku po jeho
     * zavření: Escape, který výběr zavřel (ImGui ho zavře už v NewFrame(), BeginCombo() pak vrátí false), patří
     * výběru, ne dialogu; ne ze snímku otevření */
    const bool combo_recent = combo_open || s_ccv.combo_was_open;
    s_ccv.combo_was_open = combo_open;
    if ( !appearing && !combo_recent ) {
        if ( !r_ok && ( ImGui::IsKeyPressed ( ImGuiKey_Enter, false ) || ImGui::IsKeyPressed ( ImGuiKey_KeypadEnter, false ) ) ) do_ok = true;
        if ( ImGui::IsKeyPressed ( ImGuiKey_Escape, false ) ) do_cancel = true;
    }

    s_ccv.last_size = win->Size;
    if ( imgui_cmt_window_current_outside_work ( ) ) s_ccv.recenter_frames = ImMax ( s_ccv.recenter_frames, 2 );
#ifdef MZTEST_HEADLESS
    s_ccv_wmin = win->Pos;
    s_ccv_wmax = ImVec2 ( win->Pos.x + win->Size.x, win->Pos.y + win->Size.y );
    s_ccv_imin = win->InnerRect.Min;
    s_ccv_imax = win->InnerRect.Max;
    s_ccv_drawn = true;
#endif

    if ( do_ok && !r_ok && !do_cancel ) {
        const std::string &id = s_ccv.profiles[( size_t ) s_ccv.sel].id;
        uint32_t seq = cmt_request_convert_block_on_tape ( s_ccv.tape_serial, s_ccv.block_id, id.c_str ( ), send_speed );
        if ( seq ) {
            st_CCV_PENDING p;
            p.seq = seq;
            p.block_id = s_ccv.block_id;
            p.profile = id;
            /* ztracené výsledky (device_request_clear()) nesmí seznam zvětšovat donekonečna: nejvýš CCV_PENDING_MAX */
            if ( s_ccv_pending.size ( ) >= CCV_PENDING_MAX ) s_ccv_pending.erase ( s_ccv_pending.begin ( ) );
            s_ccv_pending.push_back ( p );
        } else {
            cmt_view_post ( _ ( "The tape could not be edited." ), CMT_VIEW_MSG_ERROR );
        }
        do_cancel = true;
    }
    if ( do_cancel ) {
        ImGui::CloseCurrentPopup ( );
        s_ccv.popup_id = 0;
        ccv_close ( );
    }
    ImGui::EndPopup ( );
#ifdef MZTEST_HEADLESS
    s_ccv_items_done = s_ccv_items;
#endif
}


/* ------------------------------------------------------------------------- */
/*  Testovací rozhraní                                                       */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
const std::vector<st_CMT_WINDOW_TEST_ITEM> &cmt_convert_dialog_test_items ( void )
{
    return s_ccv_items_done;
}

bool cmt_convert_dialog_test_rect ( ImVec2 *wmin, ImVec2 *wmax, ImVec2 *imin, ImVec2 *imax )
{
    *wmin = s_ccv_wmin;
    *wmax = s_ccv_wmax;
    *imin = s_ccv_imin;
    *imax = s_ccv_imax;
    return s_ccv_drawn && s_ccv.open;
}

const char *cmt_convert_dialog_test_title ( void )
{
    return s_ccv_title.c_str ( );
}

bool cmt_convert_dialog_test_select ( const char *id )
{
    for ( size_t i = 0; i < s_ccv.profiles.size ( ); i++ ) {
        if ( s_ccv.profiles[i].id != id ) continue;
        s_ccv.sel = ( int ) i;
        /* jako výběr uživatele: vybraný řádek v seznamu vidět */
        s_ccv.scroll_to_sel = true;
        return true;
    }
    return false;
}

void cmt_convert_dialog_test_set_speed ( en_CMTSPEED speed )
{
    s_ccv.speed = speed;
}

void cmt_convert_dialog_test_reset ( void )
{
    ccv_close ( );
    s_ccv_pending.clear ( );
    s_ccv_items.clear ( );
    s_ccv_items_done.clear ( );
    s_ccv_drawn = false;
}
#endif
