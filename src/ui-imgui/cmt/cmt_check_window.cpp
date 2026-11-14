/**
 * @file   cmt_check_window.cpp
 * @brief  Nemodální okno "Tape check": souhrn a nálezy kontroly vložené pásky.
 *
 * Rozvržení (návrh okna, SPEC, část Kontrola pásky):
 * nadpis = souhrn ("Blocks checked: %d. Warnings: %d.", při mezi počtu nálezů
 * jádra "Blocks checked: %d. Warnings: more than %d.", bez nálezů "No
 * problems found. Blocks checked: %d.", bez pásky "No tape inserted."), při
 * zastaralých nálezech věta "The tape changed. Run the check again." (barva
 * warn), tabulka nálezů (Závažnost | Block N NAME | text, u řídicího bloku
 * jen Block N; mezi sloupci 2 x 3u; text se zalamuje; tabulka se posouvá)
 * a tlačítko Check again vpravo dole (společný tvar, imgui_cmt_button()).
 * Okno má velikost z obsahu (nejvýš 90 % pracovní plochy) a při otevření se
 * objeví vedle okna Virtual CMT tak, aby nezakrylo tabulku bloků
 * (imgui_cmt_window_place_beside(), případně se zmenšeným seznamem; bez okna
 * kazety uprostřed pracovní plochy; vyčnívá-li později z plochy, vrátí se).
 * Klik na nález okno kazety dopředu nevynáší (okno kontroly zůstane vidět).
 * Výšky zalomených řádků se počítají jen při změně nálezů, šířky nebo písma.
 *
 * Nálezy a jména bloků se čtou jen při otevření (i opakovaném z menu)
 * a tlačítkem Check again, pod zámkem stavu zařízení, který drží volající
 * (main_window.cpp); zastaralé jsou po výměně nebo úpravě pásky (čítač
 * výměn pásky) i po připojení záznamu (počet bloků).
 * Čísla bloků v textech jádra (od 0) okno ukazuje od 1 jako tabulka okna
 * kazety ("Jump to block 7 ..." -> "Jump to block 8 ...").
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
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <string>
#include <vector>
#include <map>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

#include "i18n.h"

#include "emulator/hw-generic/cmt/cmt_tmz_meta.h"
#include "cmt_view.h"
#include "cmt_tokens.h"
#include "cmt_window.h"
#include "cmt_check_window.h"


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
/*  Stav                                                                     */
/* ------------------------------------------------------------------------- */

/** @brief Stabilní ID okna (titulek se překládá, ID ne). */
#define CCK_WINDOW_ID "###cmt_tape_check"

/** @brief Šířka okna v násobcích velikosti písma (omezí ji 90 % plochy). */
#define CCK_W_EM 32.0f

/** @brief Nejvýš tolik řádků nálezů je vidět bez posouvání (při dost velké ploše). */
#define CCK_ROWS_VISIBLE 10

/** @brief Jeden nález pro řádek okna (kopie, texty připravené při čtení). */
typedef struct st_CCK_ROW {
    int block_id;      /**< blok nálezu (od 0), -1 = nález bez bloku (mez počtu nálezů) */
    bool warning;      /**< závažnost varování (jinak informace) */
    std::string block; /**< "Block N NAME" (číslo od 1); "" bez bloku */
    std::string text;  /**< text jádra s čísly bloků od 1 (anglicky, nepřekládá se) */
} st_CCK_ROW;

/**
 * @brief Stav okna (UI vlákno).
 * @invariant read_serial, read_block_count a read_has_tape popisují pásku, ze které jsou rows.
 */
typedef struct st_CCK {
    bool open;                   /**< okno otevřené */
    bool read_pending;           /**< příští kreslení přečte nálezy */
    bool focus_request;          /**< příští kreslení přenese okno dopředu */
    bool read_has_tape;          /**< při čtení byla vložená páska */
    unsigned read_serial;        /**< čítač výměn pásky při čtení */
    int read_block_count;        /**< počet bloků pásky při čtení (připojení záznamu čítač výměn nezvedá) */
    uint32_t blocks_checked;     /**< počet zkontrolovaných bloků */
    int warnings;                /**< počet nálezů se závažností varování (vypsaných) */
    bool capped;                 /**< jádro nálezy omezilo (CMTTMZ_CHECK_MAX_ITEMS; souhrnný nález bez bloku): skutečný počet varování je vyšší */
    std::vector<st_CCK_ROW> rows; /**< nálezy */
    int reads;                   /**< počet čtení (diagnostika a testy) */
    float list_content_h;        /**< výška obsahu tabulky z minulého snímku (0 = neznámá) */
    int place_frames;            /**< ještě tolik snímků umístit okno vedle okna kazety (otevření, okno mimo pracovní plochu) */
    float place_max_h;           /**< největší výška okna podle polohy (pás okna kazety nad nebo pod tabulkou bloků), FLT_MAX = bez omezení */
    ImVec2 last_size;            /**< velikost okna z minulého snímku (0 = neznámá) */
    std::vector<float> heights;  /**< výška každého řádku tabulky (zalomené texty), mezipaměť */
    bool heights_valid;          /**< heights platí pro řádky, šířky sloupců a písmo z klíče */
    float heights_key_bw;        /**< šířka sloupce bloku, pro kterou heights platí */
    float heights_key_tw;        /**< šířka sloupce textu, pro kterou heights platí */
    float heights_key_font;      /**< velikost písma, pro kterou heights platí */
    float blk_w;                 /**< nejširší text sloupce bloku (mezipaměť) */
    float blk_w_font;            /**< velikost písma, pro kterou blk_w platí (0 = neplatné) */
    int blk_w_reads;             /**< reads, pro které blk_w platí */
} st_CCK;

static st_CCK s_cck; /**< stav okna */

#ifdef MZTEST_HEADLESS
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_cck_items;      /**< prvky rozpracovaného snímku */
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_cck_items_done; /**< prvky posledního nakresleného snímku */
static ImVec2 s_cck_wmin, s_cck_wmax, s_cck_imin, s_cck_imax; /**< obdélník okna a vnitřku */
static bool s_cck_drawn = false;                               /**< okno se v posledním snímku kreslilo */
static bool s_cck_in_list = false;                             /**< záznam prvků posouvané tabulky */
#endif

/**
 * @brief Zaznamená prvek okna pro testy (v aplikaci nic).
 * @param name Jméno. @param text Text. @param min Levý horní roh. @param max Pravý dolní roh. @param disabled Zakázaný.
 */
static void cck_record ( const char *name, const std::string &text, ImVec2 min, ImVec2 max, bool disabled )
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
    it.scroll_y = s_cck_in_list;
    it.popup = false;
    it.reason = "";
    s_cck_items.push_back ( it );
#else
    ( void ) name;
    ( void ) text;
    ( void ) min;
    ( void ) max;
    ( void ) disabled;
#endif
}

/**
 * @brief Označí poslední zaznamenaný prvek jako obal (řádek tabulky obsahuje buňky; z kontroly překryvu se vynechá; jen testy).
 */
static void cck_record_mark_container ( void )
{
#ifdef MZTEST_HEADLESS
    if ( !s_cck_items.empty ( ) ) s_cck_items.back ( ).container = true;
#endif
}

/**
 * @brief Jméno prvku s indexem pro testy ("ck_row_2"); v aplikaci jen základ.
 * @param buf Pole znaků (sizeof musí dát jeho velikost). @param base Základ. @param i Index.
 * @return Jméno prvku.
 */
#ifdef MZTEST_HEADLESS
#define CCK_NAME( buf, base, i ) ( snprintf ( buf, sizeof ( buf ), "%s_%d", base, ( int ) ( i ) ), buf )
#else
#define CCK_NAME( buf, base, i ) ( ( void ) ( i ), base )
#endif


/* ------------------------------------------------------------------------- */
/*  Čtení nálezů                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Přečte nálezy vložené pásky a jména bloků s nálezem (jednou; ne každý snímek).
 *
 * Pod zámkem stavu zařízení běží jen průchod jádra (cmttmz_tape_check(),
 * lineární se seznamem bloků, nejvýš CMTTMZ_CHECK_MAX_ITEMS nálezů) a stavba
 * údajů bloků s nálezem (nejvýš tolik bloků, kolik je nálezů), ne celé
 * pásky. Změřeno v test_j5_check_bench (MZ_CMT_BENCH=1).
 *
 * @pre UI vlákno drží device_state_lock().
 * @post s_cck.rows, blocks_checked, warnings, capped, read_serial a read_block_count popisují aktuální pásku.
 */
static void cck_read ( void )
{
    s_cck.reads++;
    s_cck.rows.clear ( );
    s_cck.blocks_checked = 0;
    s_cck.warnings = 0;
    s_cck.capped = false;
    s_cck.heights_valid = false;
    unsigned serial = 0;
    int count = 0;
    s_cck.read_has_tape = cmt_view_read_tape_serial ( &serial, &count );
    s_cck.read_serial = serial;
    s_cck.read_block_count = count;
    if ( !s_cck.read_has_tape ) return;
    std::vector<st_CMTTMZ_CHECK_ITEM> items;
    uint32_t checked = 0;
    if ( !cmt_view_read_tape_check ( &items, &checked ) ) {
        s_cck.read_has_tape = false;
        return;
    }
    s_cck.blocks_checked = checked;
    /* jména jen bloků s nálezem (víc nálezů téhož bloku = jedno čtení) */
    std::map<int, std::string> names;
    for ( const st_CMTTMZ_CHECK_ITEM &it : items ) {
        st_CCK_ROW r;
        r.block_id = it.block_id;
        r.warning = ( it.severity == CMTTMZ_CHECK_WARNING );
        if ( r.warning ) s_cck.warnings++;
        /* nález bez bloku = souhrn jádra při mezi počtu nálezů (další se jen započítaly) */
        if ( it.block_id < 0 ) s_cck.capped = true;
        if ( it.block_id >= 0 ) {
            std::map<int, std::string>::iterator nit = names.find ( it.block_id );
            if ( nit == names.end ( ) ) {
                std::string n;
                st_CMT_VIEW_BLOCK b;
                if ( cmt_view_read_block ( it.block_id, &b ) ) {
                    /* řídicí blok jen číslem: druh bloku říká text nálezu ("Group start without a group end"),
                     * popis ze Status by se v řádku opakoval; ostatní jménem jako tabulka */
                    if ( b.kind != CMT_VIEW_KIND_CONTROL ) {
                        n = b.name;
                        if ( n.empty ( ) ) n = _ ( "(no name)" );
                    }
                }
                nit = names.emplace ( it.block_id, n ).first;
            }
            char buf[64];
            snprintf ( buf, sizeof ( buf ), _ ( "Block %d" ), it.block_id + 1 );
            r.block = buf;
            if ( !nit->second.empty ( ) ) {
                r.block += " ";
                r.block += nit->second;
            }
        }
        /* texty nálezů nenesou cestu souboru (jen čísla bloků, typy, ID profilu) */
        r.text = cmt_view_core_text ( it.text );
        s_cck.rows.push_back ( r );
    }
}


/* ------------------------------------------------------------------------- */
/*  Veřejné funkce                                                           */
/* ------------------------------------------------------------------------- */

void cmt_check_window_open ( void )
{
    /* zavřené okno se otevře vedle okna kazety (velikost z obsahu dá až první snímek, proto víc snímků) */
    if ( !s_cck.open ) {
        s_cck.place_frames = 3;
        s_cck.place_max_h = FLT_MAX;
    }
    s_cck.open = true;
    s_cck.read_pending = true;
    s_cck.focus_request = true;
}

#ifdef MZTEST_HEADLESS
bool cmt_check_window_is_open ( void )
{
    return s_cck.open;
}
#endif

void imgui_cmt_check_window ( void )
{
    if ( !s_cck.open ) return;
#ifdef MZTEST_HEADLESS
    s_cck_items.clear ( );
    s_cck_drawn = false;
#endif
    if ( s_cck.read_pending ) {
        s_cck.read_pending = false;
        cck_read ( );
    }
    unsigned serial = 0;
    int count = 0;
    const bool has = cmt_view_read_tape_serial ( &serial, &count );
    /* zastaralé: jiná páska, úprava pásky (čítač výměn), vysunutí nebo vložení, bloky připojené
     * záznamem (čítač výměn nezvedá, mění jen počet bloků) */
    const bool outdated = ( has != s_cck.read_has_tape ) || ( has && ( serial != s_cck.read_serial || count != s_cck.read_block_count ) );

    const ImGuiStyle &st = ImGui::GetStyle ( );
    const float F = ImGui::GetFrameHeight ( );
    const float Lh = ImGui::GetTextLineHeight ( );
    const ImVec2 work = imgui_cmt_window_work_size ( );
    const float max_w = floorf ( 0.9f * work.x ) - 2.0f * st.WindowPadding.x - 2.0f * st.WindowBorderSize;
    const float max_h = floorf ( 0.9f * work.y );
    const float W = floorf ( ImMin ( CCK_W_EM * ImGui::GetFontSize ( ), max_w ) );
    const float row_h = Lh + 2.0f * st.CellPadding.y;
    /* titulek, nadpis (1,2x), věta zastaralosti, tlačítko, mezery */
    const float other_h = F + 2.0f * st.WindowPadding.y + 1.2f * Lh + Lh + F + 4.0f * st.ItemSpacing.y + 2.0f;
    /* při otevření vedle okna kazety tak, aby nezakrylo tabulku bloků (vpravo, pod, nad, jinak v pásu okna kazety nad
     * nebo pod tabulkou se zmenšeným seznamem); okno, které vyčnívá z pracovní plochy, se vrátí */
    if ( s_cck.place_frames > 0 ) {
        s_cck.place_max_h = imgui_cmt_window_place_beside ( s_cck.last_size, ImGuiCond_Always, other_h + 2.0f * row_h );
        s_cck.place_frames--;
    }
    const float win_max_h = ImMin ( max_h, s_cck.place_max_h );
    const float list_max = ImMax ( row_h, ImMin ( ( float ) CCK_ROWS_VISIBLE * row_h * 1.5f, win_max_h - other_h ) );
    const float list_content = ( s_cck.list_content_h > 0.0f ) ? s_cck.list_content_h : ( float ) s_cck.rows.size ( ) * row_h;
    const float list_h = floorf ( ImMin ( list_content, list_max ) );

    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 0.0f, 0.0f ), ImVec2 ( floorf ( 0.9f * work.x ), win_max_h ) );
    if ( s_cck.focus_request ) ImGui::SetNextWindowFocus ( );
    s_cck.focus_request = false;
    ImGui::PushStyleColor ( ImGuiCol_WindowBg, CMT_TOK_BG );
    ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT );
    ImGui::PushStyleColor ( ImGuiCol_TextDisabled, CMT_TOK_TEXT_DIM );
    ImGui::PushStyleColor ( ImGuiCol_Button, CMT_TOK_CONTROL );
    ImGui::PushStyleColor ( ImGuiCol_ButtonHovered, CMT_TOK_CONTROL_HOVER );
    ImGui::PushStyleColor ( ImGuiCol_ButtonActive, CMT_TOK_CONTROL_ACTIVE );
    ImGui::PushStyleColor ( ImGuiCol_Border, CMT_TOK_CONTROL_BORDER );
    ImGui::PushStyleColor ( ImGuiCol_ChildBg, CMT_TOK_PANEL );
    ImGui::PushStyleColor ( ImGuiCol_Header, CMT_TOK_ROW_SELECTED );
    ImGui::PushStyleColor ( ImGuiCol_HeaderHovered, CMT_TOK_CONTROL_HOVER );
    ImGui::PushStyleColor ( ImGuiCol_HeaderActive, CMT_TOK_CONTROL_ACTIVE );
    ImGui::PushStyleColor ( ImGuiCol_TableRowBgAlt, CMT_TOK_ROW_ALT );
    const int n_colors = 12;
    std::string title = std::string ( _ ( "Tape check" ) ) + CCK_WINDOW_ID;
    const bool visible = ImGui::Begin ( title.c_str ( ), &s_cck.open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar );
    if ( !visible ) {
        ImGui::End ( );
        ImGui::PopStyleColor ( n_colors );
        return;
    }
    ImGuiWindow *win = ImGui::GetCurrentWindow ( );
    ImGui::Dummy ( ImVec2 ( W, 0.0f ) );

    /* --- nadpis: souhrn --- */
    std::string summary;
    if ( !s_cck.read_has_tape ) {
        summary = _ ( "No tape inserted." );
    } else if ( s_cck.rows.empty ( ) ) {
        /* počet zkontrolovaných bloků i bez nálezu (prázdná páska vs. 50 bloků) */
        char buf[128];
        snprintf ( buf, sizeof ( buf ), _ ( "No problems found. Blocks checked: %d." ), ( int ) s_cck.blocks_checked );
        summary = buf;
    } else {
        char buf[128];
        /* při mezi počtu nálezů jádro další jen započítá: počet varování je dolní mez */
        if ( s_cck.capped )
            snprintf ( buf, sizeof ( buf ), _ ( "Blocks checked: %d. Warnings: more than %d." ), ( int ) s_cck.blocks_checked, s_cck.warnings );
        else
            snprintf ( buf, sizeof ( buf ), _ ( "Blocks checked: %d. Warnings: %d." ), ( int ) s_cck.blocks_checked, s_cck.warnings );
        summary = buf;
    }
    ImGui::PushFont ( NULL, ImGui::GetCurrentContext ( )->FontSizeBase * 1.2f );
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos ( );
        ImGui::PushTextWrapPos ( ImGui::GetCursorPosX ( ) + W );
        ImGui::TextUnformatted ( summary.c_str ( ) );
        ImGui::PopTextWrapPos ( );
        ImVec2 p1 = ImGui::GetItemRectMax ( );
        cck_record ( "ck_summary", summary, p0, ImVec2 ( ImMin ( p1.x, p0.x + W ), p1.y ), false );
    }
    ImGui::PopFont ( );
    if ( outdated ) {
        const char *t = _ ( "The tape changed. Run the check again." );
        ImVec2 p0 = ImGui::GetCursorScreenPos ( );
        ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_WARN );
        ImGui::PushTextWrapPos ( ImGui::GetCursorPosX ( ) + W );
        ImGui::TextUnformatted ( t );
        ImGui::PopTextWrapPos ( );
        ImGui::PopStyleColor ( );
        ImVec2 p1 = ImGui::GetItemRectMax ( );
        cck_record ( "ck_outdated", t, p0, ImVec2 ( ImMin ( p1.x, p0.x + W ), p1.y ), false );
    }

    /* --- tabulka nálezů --- */
    int clicked_block = -1;
    if ( !s_cck.rows.empty ( ) ) {
        ImGui::BeginChild ( "##cck_list", ImVec2 ( W, list_h ), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_Borders, 0 );
#ifdef MZTEST_HEADLESS
        s_cck_in_list = true;
#endif
        /* závažnost nálezu (kontext: "Warning" je jinde titulek okna zpráv) */
        const char *w_warn = C_ ( "Tape check severity", "Warning" );
        const char *w_info = C_ ( "Tape check severity", "Info" );
        float sev_w = ImMax ( ImGui::CalcTextSize ( w_warn ).x, ImGui::CalcTextSize ( w_info ).x );
        /* nejširší "Block N JMÉNO" jen při změně nálezů nebo písma (mezipaměť jako výšky řádků) */
        if ( s_cck.blk_w_font != ImGui::GetFontSize ( ) || s_cck.blk_w_reads != s_cck.reads ) {
            s_cck.blk_w = 0.0f;
            for ( const st_CCK_ROW &r : s_cck.rows ) s_cck.blk_w = ImMax ( s_cck.blk_w, ImGui::CalcTextSize ( r.block.c_str ( ) ).x );
            s_cck.blk_w_font = ImGui::GetFontSize ( );
            s_cck.blk_w_reads = s_cck.reads;
        }
        float blk_w = s_cck.blk_w;
        /* sloupec bloku nejvýš třetina šířky (dlouhé jméno se zalomí) */
        blk_w = ImMin ( blk_w, floorf ( W / 3.0f ) );
        const ImU32 dim = CMT_TOK_TEXT_DIM;
        /* mezera mezi sloupci 2 x 3u (při 28 px 24 px): sloupce bez hlavičky drží jen mezera */
        ImGui::PushStyleVar ( ImGuiStyleVar_CellPadding, ImVec2 ( 3.0f * ImGui::GetFontSize ( ) / 7.0f, st.CellPadding.y ) );
        const bool table_open = ImGui::BeginTable ( "##cck_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX );
        ImGui::PopStyleVar ( );
        if ( table_open ) {
            ImGui::TableSetupColumn ( "sev", ImGuiTableColumnFlags_WidthFixed, ceilf ( sev_w ) );
            ImGui::TableSetupColumn ( "block", ImGuiTableColumnFlags_WidthFixed, ceilf ( blk_w ) );
            ImGui::TableSetupColumn ( "text", ImGuiTableColumnFlags_WidthStretch );
            char nb[32];
            for ( size_t i = 0; i < s_cck.rows.size ( ); i++ ) {
                const st_CCK_ROW &r = s_cck.rows[i];
                ImGui::TableNextRow ( );
                ImGui::TableSetColumnIndex ( 1 );
                const float bw = ImGui::GetContentRegionAvail ( ).x;
                ImGui::TableSetColumnIndex ( 2 );
                const float tw = ImGui::GetContentRegionAvail ( ).x;
                /* výšky zalomených řádků jen při změně nálezů, šířek sloupců nebo písma (ne každý snímek, až 501 řádků) */
                if ( i == 0 && ( !s_cck.heights_valid || s_cck.heights_key_bw != bw || s_cck.heights_key_tw != tw
                                 || s_cck.heights_key_font != ImGui::GetFontSize ( ) || s_cck.heights.size ( ) != s_cck.rows.size ( ) ) ) {
                    s_cck.heights.resize ( s_cck.rows.size ( ) );
                    for ( size_t k = 0; k < s_cck.rows.size ( ); k++ )
                        s_cck.heights[k] = ImMax ( ImGui::CalcTextSize ( s_cck.rows[k].block.c_str ( ), NULL, false, bw ).y,
                                                   ImGui::CalcTextSize ( s_cck.rows[k].text.c_str ( ), NULL, false, tw ).y );
                    s_cck.heights_valid = true;
                    s_cck.heights_key_bw = bw;
                    s_cck.heights_key_tw = tw;
                    s_cck.heights_key_font = ImGui::GetFontSize ( );
                }
                const float h = s_cck.heights[i];
                ImGui::TableSetColumnIndex ( 0 );
                ImGui::PushID ( ( int ) i );
                /* zastaralé nálezy a nález bez bloku: řádek nic nevybere */
                const bool selectable = !outdated && r.block_id >= 0;
                if ( ImGui::Selectable ( "##ck_row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2 ( 0.0f, h ) )
                     && selectable )
                    clicked_block = r.block_id;
                cck_record ( CCK_NAME ( nb, "ck_row", i ), "", ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), !selectable );
                cck_record_mark_container ( );
                ImGui::PopID ( );
                ImGui::SameLine ( 0.0f, 0.0f );
                ImGui::SetCursorScreenPos ( ImGui::GetItemRectMin ( ) );
                /* závažnost slovem (ne jen barvou) */
                const ImU32 sev_col = outdated ? dim : ( r.warning ? CMT_TOK_WARN : dim );
                ImGui::PushStyleColor ( ImGuiCol_Text, sev_col );
                ImGui::TextUnformatted ( r.warning ? w_warn : w_info );
                ImGui::PopStyleColor ( );
                cck_record ( CCK_NAME ( nb, "ck_sev", i ), r.warning ? w_warn : w_info, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), false );
                const ImU32 txt_col = outdated ? dim : CMT_TOK_TEXT;
                ImGui::TableSetColumnIndex ( 1 );
                if ( !r.block.empty ( ) ) {
                    ImGui::PushStyleColor ( ImGuiCol_Text, txt_col );
                    ImGui::PushTextWrapPos ( ImGui::GetCursorPosX ( ) + bw );
                    ImGui::TextUnformatted ( r.block.c_str ( ) );
                    ImGui::PopTextWrapPos ( );
                    ImGui::PopStyleColor ( );
                    cck_record ( CCK_NAME ( nb, "ck_block", i ), r.block, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), false );
                }
                ImGui::TableSetColumnIndex ( 2 );
                ImGui::PushStyleColor ( ImGuiCol_Text, txt_col );
                ImGui::PushTextWrapPos ( ImGui::GetCursorPosX ( ) + tw );
                ImGui::TextUnformatted ( r.text.c_str ( ) );
                ImGui::PopTextWrapPos ( );
                ImGui::PopStyleColor ( );
                cck_record ( CCK_NAME ( nb, "ck_text", i ), r.text, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), false );
            }
            ImGui::EndTable ( );
        }
        {
            ImGuiWindow *lw = ImGui::GetCurrentWindow ( );
            s_cck.list_content_h = ceilf ( lw->DC.CursorMaxPos.y - lw->DC.CursorStartPos.y + 2.0f * st.WindowPadding.y );
        }
#ifdef MZTEST_HEADLESS
        s_cck_in_list = false;
#endif
        ImGui::EndChild ( );
    }

    /* --- Check again vpravo dole --- */
    {
        const char *l_again = _L ( "Check again" );
        /* tvar a hrana jako ostatní tlačítka okna kazety */
        const float bw = imgui_cmt_button_w ( l_again );
        const float x0 = ImGui::GetCursorScreenPos ( ).x;
        ImGui::SetCursorScreenPos ( ImVec2 ( x0 + W - bw, ImGui::GetCursorScreenPos ( ).y ) );
        if ( imgui_cmt_button ( l_again, bw, NULL, false ) ) s_cck.read_pending = true;
        cck_record ( "ck_again", std::string ( l_again, ImGui::FindRenderedTextEnd ( l_again ) ), ImGui::GetItemRectMin ( ),
                     ImGui::GetItemRectMax ( ), false );
    }
    s_cck.last_size = win->Size;
    /* okno vyčnívá z pracovní plochy viewportu (větší písmo, delší nálezy): znovu vedle okna kazety */
    if ( imgui_cmt_window_current_outside_work ( ) ) s_cck.place_frames = ImMax ( s_cck.place_frames, 2 );
#ifdef MZTEST_HEADLESS
    s_cck_wmin = win->Pos;
    s_cck_wmax = ImVec2 ( win->Pos.x + win->Size.x, win->Pos.y + win->Size.y );
    s_cck_imin = win->InnerRect.Min;
    s_cck_imax = win->InnerRect.Max;
    s_cck_drawn = true;
#endif
    ImGui::End ( );
    ImGui::PopStyleColor ( n_colors );
    /* klik na nález: blok v tabulce okna kazety (čítač pásky ze čtení nálezů), řádek do pohledu; okno kazety se dopředu
     * nevynáší - okno kontroly zůstane vidět */
    if ( clicked_block >= 0 ) imgui_cmt_window_select_block ( clicked_block, s_cck.read_serial );
#ifdef MZTEST_HEADLESS
    s_cck_items_done = s_cck_items;
#endif
}


/* ------------------------------------------------------------------------- */
/*  Testovací rozhraní                                                       */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
const std::vector<st_CMT_WINDOW_TEST_ITEM> &cmt_check_window_test_items ( void )
{
    return s_cck_items_done;
}

bool cmt_check_window_test_rect ( ImVec2 *wmin, ImVec2 *wmax, ImVec2 *imin, ImVec2 *imax )
{
    *wmin = s_cck_wmin;
    *wmax = s_cck_wmax;
    *imin = s_cck_imin;
    *imax = s_cck_imax;
    return s_cck_drawn && s_cck.open;
}

int cmt_check_window_test_reads ( void )
{
    return s_cck.reads;
}

void cmt_check_window_test_reset ( void )
{
    s_cck.open = false;
    s_cck.read_pending = false;
    s_cck.focus_request = false;
    s_cck.rows.clear ( );
    s_cck.reads = 0;
    s_cck.list_content_h = 0.0f;
    s_cck_items.clear ( );
    s_cck_items_done.clear ( );
    s_cck_drawn = false;
    s_cck.place_frames = 0;
    s_cck.place_max_h = FLT_MAX;
    s_cck.last_size = ImVec2 ( 0.0f, 0.0f );
    s_cck.capped = false;
    s_cck.heights_valid = false;
    s_cck.blk_w_font = 0.0f;
}
#endif
