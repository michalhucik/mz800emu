/**
 * @file   cmt_tapeinfo_dialog.cpp
 * @brief  Modál "Tape information - <jméno>" okna Virtual CMT: formulář Archive Info a Target machine.
 *
 * Rozvržení (návrh okna, SPEC, koncept
 * cmt-tapeinfo-v1 bez oddílu Check): posouvané tělo s formulářem (sloupec
 * popisků vlevo, šířka z nejdelšího popisku; deset polí a Target machine;
 * krátká pole ve dvojicích na jednom řádku - Year + Language, Type + Price,
 * Protection + Origin - takže je formulář o tři řádky kratší), pod ním
 * mezera 2u a patička, která je vždy vidět:
 * ztlumené věty o formátu souboru a o dalším bloku informací, chyba zápisu,
 * výzva po změně pásky, vlevo Reload (jen po změně pásky) a vpravo OK /
 * Cancel (společný tvar tlačítek, imgui_cmt_button()). Celý dialog nejvýš
 * 90 % pracovní plochy; když se formulář nevejde, posouvá se jen tělo a jeho
 * spodní hrana leží mezi řádky formuláře. Každé pole přijme nejvýš
 * CMTTMZ_TAPE_INFO_MAX_CHARS znaků (cti_limit_cb()).
 *
 * Zápis: OK porovná formulář s hodnotami přečtenými při otevření; beze
 * změny dialog zavře bez zásahu, se změnou zařadí
 * cmt_request_tape_info_set() s čítačem výměn pásky z doby čtení a čeká na
 * výsledek (OK zakázané). Výsledek obslouží
 * cmt_tapeinfo_dialog_request_result(). Po změně pásky (i odmítnutém zápisu)
 * Reload (cti_reload()) přečte informace znovu: pole, která uživatel změnil,
 * zůstanou, ostatní převezmou nové hodnoty, OK pak zapisuje s novým čítačem;
 * titulek převezme jméno pásky, která je v decku teď. Je-li v decku páska
 * s jiným jménem nebo cestou než při čtení, výzva to řekne ("A different
 * tape is in the deck now. ...").
 * Po úspěšném zápisu se v okně kazety vybere blok vybraný při otevření na
 * svém novém čísle (bloky informací na začátku pásky čísla posunou).
 *
 * Target machine: "Not specified" znamená bez bloku 0x43 (-1); páska
 * s blokem 0x43, jehož stroj formulář nezná (0 = generický, jiná hodnota),
 * ukazuje "Not specified" a dokud uživatel nevybere jiný stroj, zůstane
 * hodnota beze změny (nic se nezapíše ani neodstraní).
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

#include "emulator/hw-generic/cmt/cmt_tmz_meta.h"
#include "emulator/hw-generic/cmt/cmt_request.h"
#include "libs/tmz/tmz.h"
#include "device_request.h"
#include "cmt_view.h"
#include "cmt_tokens.h"
#include "cmt_window.h"
#include "cmt_tapeinfo_dialog.h"
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

/** @brief Stabilní ID modálu (titulek se mění se jménem pásky a jazykem, ID ne). */
#define CTI_POPUP_ID "###cmt_tapeinfo"

/** @brief Index pole Comment (víceřádkové) v pořadí st_CMTTMZ_TAPE_INFO. */
#define CTI_FIELD_COMMENT 9

/** @brief Počet řádků textu pole Comment. */
#define CTI_COMMENT_LINES 3

/** @brief Šířka polí formuláře v násobcích velikosti písma (bez sloupce popisků; omezí ji 90 % plochy). */
#define CTI_FIELD_W_EM 22.0f

/** @brief Počet položek výběru Target machine (Not specified + MZ-700, MZ-800, MZ-1500, MZ-80B). */
#define CTI_MACHINE_ITEMS 5

/**
 * @brief Stav dialogu (UI vlákno).
 *
 * @invariant seq patří jen otevřenému dialogu: výsledek zápisu se k dialogu váže, jen když je dialog otevřený
 *            a seq se shoduje; výsledek zápisu zavřeného dialogu jde do řádku zpráv.
 * @invariant form.tape_serial == orig.tape_serial == tape_serial, je-li open.
 * @invariant tape_name a tape_path popisují pásku, ze které je orig (otevření nebo poslední Reload).
 */
typedef struct st_CTI {
    bool open;                 /**< dialog je otevřený (i čekající na výsledek zápisu) */
    bool open_pending;         /**< modál se má otevřít (OpenPopup v kontextu okna Virtual CMT) */
    int drawn_frame;           /**< snímek posledního kreslení (-1 = žádný) */
    ImGuiID popup_id;          /**< ID popupu (0 = ještě neotevřený); zavření mimo modál */
    unsigned tape_serial;      /**< čítač výměn pásky v době čtení formuláře */
    std::string tape_name;     /**< jméno pásky pro titulek ("" = nová páska) z doby čtení formuláře */
    std::string tape_path;     /**< cesta souboru pásky z doby čtení formuláře ("" bez souboru): jiná páska v decku = jiná výzva */
    st_CMTTMZ_TAPE_INFO orig;  /**< hodnoty přečtené při otevření (srovnání "změnilo se něco") */
    st_CMTTMZ_TAPE_INFO form;  /**< hodnoty formuláře (úpravy uživatele) */
    int machine_sel;           /**< vybraná položka Target machine (0 = Not specified, 1..4 = MZ-700 .. MZ-80B) */
    uint32_t seq;              /**< zásah zápisu otevřeného dialogu, na jehož výsledek se čeká (0 = žádný) */
    std::string status;        /**< řádek stavu (chyba zápisu); "" = nic */
    bool status_error;         /**< řádek stavu je chyba (barva error), jinak varování */
    bool refused_changed;      /**< zápis odmítnut, protože se páska změnila: OK zakázané do Reload nebo zavření */
    bool combo_was_open;       /**< výběr Target machine byl otevřený v minulém snímku (Escape, který ho zavřel, dialog nezavře) */
    bool skipped;              /**< okno kazety v některém snímku obsah nekreslilo (cmt_tapeinfo_dialog_skip_frame()); příští kreslení ověří popup */
    int sel_block;             /**< vybraný řádek okna kazety při otevření (-1 = žádný); po zápisu se vybere na novém čísle */
    int open_block_count;      /**< počet bloků pásky při čtení formuláře (posun čísel po vložení / odstranění bloků informací) */
    int recenter_frames;       /**< ještě tolik snímků umístit modál doprostřed okna */
    ImVec2 last_size;          /**< velikost modálu z minulého snímku */
    float body_content_h;      /**< výška obsahu posouvaného těla z minulého snímku (0 = neznámá) */
    float footer_h;            /**< výška patičky z minulého snímku (0 = neznámá) */
    std::vector<float> body_marks; /**< horní okraje řádků formuláře (místní y v těle) z posledního snímku: hrana posouvané oblasti jen mezi řádky */
} st_CTI;

/** @brief Stav dialogu (UI vlákno; std::string a struktury se inicializují konstruktorem). */
static st_CTI s_cti;
/** @brief Výchozí hodnoty polí bez konstruktoru už nastavené. */
static bool s_cti_init = false;

#ifdef MZTEST_HEADLESS
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_cti_items;      /**< prvky rozpracovaného snímku */
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_cti_items_done; /**< prvky posledního nakresleného snímku */
static ImVec2 s_cti_wmin, s_cti_wmax, s_cti_imin, s_cti_imax; /**< obdélník modálu a vnitřku z posledního snímku */
static bool s_cti_drawn = false;                               /**< modál se v posledním snímku kreslil */
static std::string s_cti_title;                                /**< titulek z posledního snímku */
static bool s_cti_in_body = false;                             /**< záznam prvků posouvaného těla */
#endif

/** @brief Nastaví pole bez konstruktoru (jednou). */
static void cti_init_once ( void )
{
    if ( s_cti_init ) return;
    s_cti_init = true;
    s_cti.drawn_frame = -1;
}

/**
 * @brief Zaznamená prvek dialogu pro testy (v aplikaci nic).
 * @param name Jméno. @param text Text. @param min Levý horní roh. @param max Pravý dolní roh.
 * @param disabled Zakázaný. @param reason Důvod zákazu (NULL = žádný).
 * @pre Uvnitř modálu (ořez = GetCurrentWindow()->ClipRect).
 */
static void cti_record ( const char *name, const std::string &text, ImVec2 min, ImVec2 max, bool disabled, const char *reason )
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
    it.scroll_y = s_cti_in_body;
    it.popup = true;
    it.reason = reason ? reason : "";
    s_cti_items.push_back ( it );
#else
    ( void ) name;
    ( void ) text;
    ( void ) min;
    ( void ) max;
    ( void ) disabled;
    ( void ) reason;
#endif
}

/** @brief Zaznamená poslední položku ImGui (testy). @param name Jméno. @param text Text. @param disabled Zakázaná. @param reason Důvod. */
static void cti_record_item ( const char *name, const std::string &text, bool disabled, const char *reason )
{
    cti_record ( name, text, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), disabled, reason );
}

/**
 * @brief Jméno prvku s indexem pro testy ("ti_field_2"); v aplikaci jen základ.
 * @param buf Pole znaků (sizeof musí dát jeho velikost). @param base Základ. @param i Index.
 * @return Jméno prvku.
 */
#ifdef MZTEST_HEADLESS
#define CTI_NAME( buf, base, i ) ( snprintf ( buf, sizeof ( buf ), "%s_%d", base, ( int ) ( i ) ), buf )
#else
#define CTI_NAME( buf, base, i ) ( ( void ) ( i ), base )
#endif


/* ------------------------------------------------------------------------- */
/*  Texty a pomocné funkce                                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Popisek pole formuláře (přeložený).
 * @param field Index 0 .. CMTTMZ_TAPE_INFO_FIELDS - 1. @return Text.
 */
static const char *cti_label ( int field )
{
    switch ( field ) {
        case 0: return _ ( "Title" );
        case 1: return _ ( "Publisher" );
        case 2: return _ ( "Author" );
        case 3: return _ ( "Year" );
        /* kontext: jazyk programu (ne jazyk rozhraní) a typ programu (ne sloupec Type tabulky bloků) */
        case 4: return C_ ( "Tape information", "Language" );
        case 5: return C_ ( "Tape information", "Type" );
        case 6: return _ ( "Price" );
        case 7: return _ ( "Protection" );
        case 8: return _ ( "Origin" );
        default: return _ ( "Comment" );
    }
}

/**
 * @brief Text položky Target machine.
 * @param sel Položka 0 .. CTI_MACHINE_ITEMS - 1. @return Text (jména strojů se nepřekládají).
 */
static const char *cti_machine_text ( int sel )
{
    switch ( sel ) {
        case 1: return "MZ-700";
        case 2: return "MZ-800";
        case 3: return "MZ-1500";
        case 4: return "MZ-80B";
        default: return _ ( "Not specified" );
    }
}

/**
 * @brief Položka Target machine pro hodnotu stroje z jádra.
 * @param machine -1 / en_TMZ_MACHINE. @return 1..4 pro MZ-700 .. MZ-80B, jinak 0 (Not specified).
 */
static int cti_machine_sel ( int machine )
{
    if ( machine >= TMZ_MACHINE_MZ700 && machine <= TMZ_MACHINE_MZ80B ) return machine - TMZ_MACHINE_MZ700 + 1;
    return 0;
}

/**
 * @brief Hodnota stroje pro zápis z vybrané položky.
 *
 * Not specified = bez bloku 0x43 (-1); měla-li páska stroj, který formulář
 * nezná (0 = generický, jiný bajt), zůstane jeho hodnota (nic se nezapíše
 * ani neodstraní, dokud uživatel nevybere jiný stroj).
 *
 * @param sel Položka. @return Hodnota machine pro st_CMTTMZ_TAPE_INFO.
 */
static int cti_machine_value ( int sel )
{
    if ( sel >= 1 && sel <= 4 ) return TMZ_MACHINE_MZ700 + sel - 1;
    return ( cti_machine_sel ( s_cti.orig.machine ) == 0 ) ? s_cti.orig.machine : -1;
}

/** @brief Změnil uživatel ve formuláři něco proti hodnotám z otevření? @return true = ano. */
static bool cti_changed ( void )
{
    for ( uint32_t f = 0; f < CMTTMZ_TAPE_INFO_FIELDS; f++ ) {
        const char *a = cmttmz_tape_info_text_const ( &s_cti.orig, f );
        const char *b = cmttmz_tape_info_text_const ( &s_cti.form, f );
        if ( a && b && strcmp ( a, b ) != 0 ) return true;
    }
    return cti_machine_value ( s_cti.machine_sel ) != s_cti.orig.machine;
}

/**
 * @brief Text výzvy po změně pásky (jedno místo msgid; řádek stavu i důvod zákazu OK).
 *
 * Bez pásky "No tape inserted." (Reload není). Je-li v decku páska s jiným
 * jménem nebo cestou souboru než ta, ze které je formulář (výměna pásky,
 * např. přes MCP), výzva to řekne, aby uživatel věděl, že OK po Reload
 * zapíše do jiné pásky; jinak jen změna téže pásky.
 *
 * @param v Kopie stavu okna (nesmí být NULL).
 * @return Přeložený text.
 */
static const char *cti_text_tape_changed ( const st_CMT_VIEW *v )
{
    if ( !v->has_tape ) return _ ( "No tape inserted." );
    if ( v->tape_name != s_cti.tape_name || v->tape_path != s_cti.tape_path )
        return _ ( "A different tape is in the deck now. Press Reload to read it; your changes are kept." );
    return _ ( "The tape changed. Press Reload to read it again; your changes are kept." );
}

/**
 * @brief Zavře dialog (stav, popup); čekající zásah zápisu zůstane a jeho výsledek jde do řádku zpráv.
 * @post s_cti.open == false.
 */
static void cti_close ( void )
{
    cti_init_once ( );
    imgui_cmt_close_popup ( &s_cti.popup_id );
    s_cti.open = false;
    s_cti.open_pending = false;
    s_cti.status.clear ( );
    s_cti.refused_changed = false;
}


/**
 * @brief Zapamatuje si výběr řádku okna kazety a počet bloků pásky (obnova výběru po zápisu).
 * @param v Kopie stavu okna.
 */
static void cti_remember_selection ( const st_CMT_VIEW *v )
{
    unsigned serial = 0;
    const int sel = imgui_cmt_window_selected_block ( &serial );
    s_cti.sel_block = ( sel >= 0 && serial == v->tape_serial ) ? sel : -1;
    s_cti.open_block_count = v->block_count;
}

/**
 * @brief Reload: znovu přečte informace o pásce a ponechá pole, která uživatel změnil.
 *
 * Pole, kde se formulář liší od hodnot z minulého čtení, zůstanou s textem
 * uživatele; ostatní (a Target machine, nezměnil-li ho uživatel) převezmou
 * nové hodnoty. Nové čtení dá i nový čítač výměn pásky, OK pak zapisuje nad
 * aktuální páskou; titulek dialogu převezme jméno této pásky.
 *
 * @param v Kopie stavu okna (nesmí být NULL).
 * @return true = přečteno; false bez pásky (formulář beze změny).
 * @pre UI vlákno drží device_state_lock() (kreslení okna kazety).
 * @post Při true refused_changed == false, řádek stavu prázdný, tape_serial = čítač nového čtení,
 *       tape_name a tape_path podle @p v.
 */
static bool cti_reload ( const st_CMT_VIEW *v )
{
    st_CMTTMZ_TAPE_INFO now;
    if ( !v->has_tape || !cmt_view_read_tape_info ( &now ) ) return false;
    const bool machine_edited = ( cti_machine_value ( s_cti.machine_sel ) != s_cti.orig.machine );
    st_CMTTMZ_TAPE_INFO form = now;
    for ( uint32_t f = 0; f < CMTTMZ_TAPE_INFO_FIELDS; f++ ) {
        const char *o = cmttmz_tape_info_text_const ( &s_cti.orig, f );
        const char *u = cmttmz_tape_info_text_const ( &s_cti.form, f );
        if ( o && u && strcmp ( o, u ) != 0 ) snprintf ( cmttmz_tape_info_text ( &form, f ), CMTTMZ_TAPE_INFO_TEXT_SIZE, "%s", u );
    }
    const int machine_sel = s_cti.machine_sel;
    s_cti.orig = now;
    s_cti.form = form;
    s_cti.machine_sel = machine_edited ? machine_sel : cti_machine_sel ( now.machine );
    s_cti.tape_serial = now.tape_serial;
    /* titulek a výzva podle pásky, ze které je teď formulář */
    s_cti.tape_name = v->tape_name;
    s_cti.tape_path = v->tape_path;
    s_cti.refused_changed = false;
    s_cti.status.clear ( );
    cti_remember_selection ( v );
    return true;
}

/* ------------------------------------------------------------------------- */
/*  Veřejné funkce                                                           */
/* ------------------------------------------------------------------------- */

void cmt_tapeinfo_dialog_open ( const st_CMT_VIEW *v )
{
    cti_close ( );
    st_CMTTMZ_TAPE_INFO info;
    if ( !v || !v->has_tape || !cmt_view_read_tape_info ( &info ) ) return;
    s_cti.orig = info;
    s_cti.form = info;
    s_cti.tape_serial = info.tape_serial;
    s_cti.tape_name = v->tape_name;
    s_cti.tape_path = v->tape_path;
    s_cti.machine_sel = cti_machine_sel ( info.machine );
    s_cti.seq = 0;
    s_cti.combo_was_open = false;
    s_cti.skipped = false;
    cti_remember_selection ( v );
    s_cti.open = true;
    s_cti.open_pending = true;
    s_cti.recenter_frames = 3;
    s_cti.last_size = ImVec2 ( 0.0f, 0.0f );
    s_cti.body_content_h = 0.0f;
    s_cti.footer_h = 0.0f;
}

#ifdef MZTEST_HEADLESS
bool cmt_tapeinfo_dialog_is_open ( void )
{
    cti_init_once ( );
    return s_cti.open;
}
#endif

void cmt_tapeinfo_dialog_abandon ( void )
{
    if ( s_cti.open ) cti_close ( );
}

bool cmt_tapeinfo_dialog_skip_frame ( void )
{
    cti_init_once ( );
    if ( !s_cti.open ) return false;
    /* okno kazety obsah nekreslí: úpravy formuláře, čekající zápis i popup zůstanou */
    s_cti.drawn_frame = ImGui::GetFrameCount ( );
    s_cti.skipped = true;
    return true;
}

void cmt_tapeinfo_dialog_frame ( void )
{
    cti_init_once ( );
    const int frame = ImGui::GetCurrentContext ( ) ? ImGui::GetFrameCount ( ) : 0;
    /* okno Virtual CMT dialog v tomto snímku nenakreslilo (zavřené okno; okno bez obsahu ohlásí
     * cmt_tapeinfo_dialog_skip_frame()): jako Cancel */
    if ( s_cti.open && s_cti.drawn_frame != frame ) cti_close ( );
}

bool cmt_tapeinfo_dialog_request_result ( const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *edit )
{
    cti_init_once ( );
    if ( 0 != strcmp ( res->name, CMT_REQUEST_TAPE_INFO_SET_NAME ) ) return false;
    const bool ours = s_cti.open && s_cti.seq != 0 && res->seq == s_cti.seq;
    if ( ours ) s_cti.seq = 0;
    std::string err;
    bool changed_tape = false;
    if ( res->status != DEVICE_REQ_DONE ) {
        err = _ ( "The tape cannot be edited while an input replay is recorded or played." );
    } else if ( res->rc == CMT_REQUEST_RC_TAPE_CHANGED ) {
        changed_tape = true;
    } else if ( res->rc != 0 ) {
        /* text jádra anglicky (nepřekládá se) */
        err = ( edit && edit->message[0] ) ? cmt_view_core_text ( edit->message ) : std::string ( _ ( "The tape could not be edited." ) );
    }
    if ( ours ) {
        /* neúspěch: dialog zůstane otevřený s úpravami uživatele */
        if ( changed_tape ) {
            /* výzvu (Reload) ukáže kreslení podle refused_changed */
            s_cti.refused_changed = true;
            s_cti.status.clear ( );
            return true;
        }
        if ( !err.empty ( ) ) {
            s_cti.status = err;
            s_cti.status_error = true;
            return true;
        }
        cti_close ( );
        /* výběr řádku okna kazety: informační bloky na začátku pásky posunuly čísla bloků o rozdíl počtu bloků
         * (zápis zvedl čítač výměn pásky a okno by výběr zrušilo) */
        if ( edit && edit->count > 0 && s_cti.sel_block >= 0 ) {
            const int nb = s_cti.sel_block + ( int ) edit->block_count - s_cti.open_block_count;
            if ( nb >= 0 && nb < ( int ) edit->block_count ) cmt_view_select_after_edit ( edit->tape_serial, nb );
        }
    } else {
        /* dialog se mezitím zavřel: výsledek do řádku zpráv */
        if ( changed_tape ) {
            cmt_view_post ( _ ( "The tape changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
            return true;
        }
        if ( !err.empty ( ) ) {
            cmt_view_post ( err.c_str ( ), CMT_VIEW_MSG_ERROR );
            return true;
        }
    }
    /* úspěch: varování jádra (znaky nahrazené '?', takt) místo zprávy o změně */
    if ( edit && edit->message[0] ) cmt_view_post ( cmt_view_core_text ( edit->message ).c_str ( ), CMT_VIEW_MSG_WARNING );
    else if ( !edit || edit->count > 0 ) cmt_view_post ( _ ( "Tape information updated." ), CMT_VIEW_MSG_INFO );
    return true;
}


/* ------------------------------------------------------------------------- */
/*  Kreslení                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Zalomí text po slovech na šířku.
 * @param text Text. @param max_w Šířka (px). @return Řádky (aspoň jeden).
 */
static std::vector<std::string> cti_wrap ( const std::string &text, float max_w )
{
    std::vector<std::string> lines;
    std::string line, word;
    for ( size_t i = 0; i <= text.size ( ); i++ ) {
        if ( i == text.size ( ) || text[i] == ' ' ) {
            if ( word.empty ( ) ) continue;
            std::string cand = line.empty ( ) ? word : line + " " + word;
            if ( !line.empty ( ) && ImGui::CalcTextSize ( cand.c_str ( ) ).x > max_w ) {
                lines.push_back ( line );
                line = word;
            } else {
                line = cand;
            }
            word.clear ( );
        } else {
            word += text[i];
        }
    }
    if ( !line.empty ( ) || lines.empty ( ) ) lines.push_back ( line );
    return lines;
}

/**
 * @brief Omezí text pole na CMTTMZ_TAPE_INFO_MAX_CHARS znaků.
 *
 * Blok 0x32 unese v položce nejvýš 255 bajtů ISO 8859-1; každý znak
 * formuláře (i '\n' = 0Dh a znak mimo ISO 8859-1 = '?') je po převodu
 * jeden bajt, takže mez platí pro počet znaků UTF-8. Delší vložený nebo
 * napsaný text se na konci zkrátí hned v poli (jádro by ho odmítlo až po OK).
 *
 * @param data Data zpětného volání ImGui (ImGuiInputTextFlags_CallbackEdit).
 * @return 0.
 */
static int cti_limit_cb ( ImGuiInputTextCallbackData *data )
{
    if ( data->EventFlag != ImGuiInputTextFlags_CallbackEdit ) return 0;
    int chars = 0;
    for ( int i = 0; i < data->BufTextLen; i++ ) {
        /* začátek znaku UTF-8 = bajt, který není pokračovací (10xxxxxx) */
        if ( ( ( unsigned char ) data->Buf[i] & 0xC0u ) == 0x80u ) continue;
        if ( ++chars > CMTTMZ_TAPE_INFO_MAX_CHARS ) {
            data->DeleteChars ( i, data->BufTextLen - i );
            break;
        }
    }
    return 0;
}

/**
 * @brief Zalomený text na šířku (jeden záznam pro testy s celým textem).
 * @param name Jméno prvku. @param text Text. @param color Barva. @param w Šířka.
 */
static void cti_wrapped ( const char *name, const std::string &text, ImU32 color, float w )
{
    std::vector<std::string> lines = cti_wrap ( text, w );
    ImVec2 p0 = ImGui::GetCursorScreenPos ( );
    float wmax = 0.0f;
    ImGui::PushStyleColor ( ImGuiCol_Text, color );
    for ( const std::string &l : lines ) {
        ImGui::TextUnformatted ( l.c_str ( ), l.c_str ( ) + l.size ( ) );
        wmax = ImMax ( wmax, ImGui::CalcTextSize ( l.c_str ( ) ).x );
    }
    ImGui::PopStyleColor ( );
    cti_record ( name, text, p0, ImVec2 ( p0.x + wmax, ImGui::GetItemRectMax ( ).y ), false, NULL );
}

void cmt_tapeinfo_dialog_draw ( const st_CMT_VIEW *v, const char *edit_reason )
{
    cti_init_once ( );
    if ( !s_cti.open ) return;
    s_cti.drawn_frame = ImGui::GetFrameCount ( );
#ifdef MZTEST_HEADLESS
    s_cti_items.clear ( );
    s_cti_drawn = false;
#endif
    /* po snímcích bez obsahu okna: popup, který mezitím zavřelo ImGui, otevřít znovu (stav dialogu zůstal) */
    if ( s_cti.skipped ) {
        s_cti.skipped = false;
        if ( !s_cti.open_pending && s_cti.popup_id && !ImGui::IsPopupOpen ( s_cti.popup_id, ImGuiPopupFlags_None ) ) s_cti.open_pending = true;
    }
    if ( s_cti.open_pending ) {
        if ( !imgui_modal_open_when_free ( CTI_POPUP_ID ) ) return;
        s_cti.open_pending = false;
        s_cti.popup_id = ImGui::GetID ( CTI_POPUP_ID );
    }
    /* titulek bez pevného bufferu: dlouhé jméno pásky se neusekne uprostřed znaku UTF-8 */
    gchar *title = g_strdup_printf ( _ ( "Tape information - %s" ), s_cti.tape_name.empty ( ) ? _ ( "(new tape)" ) : s_cti.tape_name.c_str ( ) );
    std::string full_title = std::string ( title ? title : "" ) + CTI_POPUP_ID;
    g_free ( title );
#ifdef MZTEST_HEADLESS
    s_cti_title = full_title;
#endif

    const ImGuiStyle &st = ImGui::GetStyle ( );
    const float F = ImGui::GetFrameHeight ( );
    const float Lh = ImGui::GetTextLineHeight ( );
    const ImVec2 work = imgui_cmt_window_work_size ( );
    const float max_w = floorf ( 0.9f * work.x ) - 2.0f * st.WindowPadding.x - 2.0f * st.WindowBorderSize;
    const float max_h = floorf ( 0.9f * work.y );
    /* sloupec popisků podle nejdelšího popisku */
    float label_w = ImGui::CalcTextSize ( _ ( "Target machine" ) ).x;
    for ( int f = 0; f < ( int ) CMTTMZ_TAPE_INFO_FIELDS; f++ ) label_w = ImMax ( label_w, ImGui::CalcTextSize ( cti_label ( f ) ).x );
    label_w = ceilf ( label_w + 2.0f * st.ItemSpacing.x );
    const float comment_h = ceilf ( ( float ) CTI_COMMENT_LINES * Lh + 2.0f * st.FramePadding.y );
    /* výška formuláře: Title, Publisher, Author, tři dvojice krátkých polí (Year + Language, Type + Price,
     * Protection + Origin), Comment, Target machine */
    const float form_h = 7.0f * F + comment_h + 8.0f * st.ItemSpacing.y;
    const float deco = F + 2.0f * st.WindowPadding.y + 2.0f * st.WindowBorderSize;
    const float footer_h = ( s_cti.footer_h > 0.0f ) ? s_cti.footer_h : F + st.ItemSpacing.y;
    const float body_max = floorf ( ImMax ( 2.0f * F, max_h - deco - footer_h - st.ItemSpacing.y - 2.0f ) );
    const float body_content = ( s_cti.body_content_h > 0.0f ) ? s_cti.body_content_h : form_h;
    const bool body_scroll = body_content > body_max;
    float body_h = floorf ( ImMin ( body_content, body_max ) );
    /* nevejde-li se formulář, končí posouvaná oblast mezi řádky (začátek prvního řádku, který se celý nevejde;
     * řádky z minulého snímku), nikdy uprostřed pole nebo popisku */
    if ( body_scroll ) {
        float best = -1.0f;
        for ( float mk : s_cti.body_marks )
            if ( mk <= body_max && mk > best ) best = mk;
        if ( best >= 2.0f * F ) body_h = floorf ( best );
    }
    float W = label_w + floorf ( CTI_FIELD_W_EM * ImGui::GetFontSize ( ) ) + ( body_scroll ? st.ScrollbarSize : 0.0f );
    W = floorf ( ImMin ( W, max_w ) );

    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 0.0f, 0.0f ), ImVec2 ( floorf ( 0.9f * work.x ), max_h ) );
    imgui_cmt_window_modal_center ( s_cti.last_size, s_cti.recenter_frames > 0 ? ImGuiCond_Always : ImGuiCond_Appearing );
    if ( s_cti.recenter_frames > 0 ) s_cti.recenter_frames--;
    if ( !ImGui::BeginPopupModal ( full_title.c_str ( ), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings ) ) {
        /* modál zavřel někdo jiný: jako Cancel */
        s_cti.popup_id = 0;
        cti_close ( );
        return;
    }
    const bool appearing = ImGui::IsWindowAppearing ( );
    /* aktivní pole na začátku snímku: Escape patří jemu (vrátí text a pole opustí) */
    const bool editing = ( ImGui::GetActiveID ( ) != 0 );
    ImGuiWindow *win = ImGui::GetCurrentWindow ( );
    ImGui::Dummy ( ImVec2 ( W, 0.0f ) );

    const bool tape_changed = !v->has_tape || v->tape_serial != s_cti.tape_serial || s_cti.refused_changed;
    bool do_ok = false, do_cancel = false, do_reload = false;
    bool combo_open = false;

    /* --- posouvané tělo: formulář --- */
    ImGui::BeginChild ( "##cti_body", ImVec2 ( W, body_h ), ImGuiChildFlags_NavFlattened, 0 );
#ifdef MZTEST_HEADLESS
    s_cti_in_body = true;
#endif
    {
        const float field_w = ImMax ( 4.0f * F, ImGui::GetContentRegionAvail ( ).x - label_w );
        /* dvojice krátkých polí: druhý popisek ve sloupci podle nejdelšího z nich, obě pole stejně široká */
        float label2_w = 0.0f;
        for ( int f = 4; f <= 8; f += 2 ) label2_w = ImMax ( label2_w, ImGui::CalcTextSize ( cti_label ( f ) ).x );
        label2_w = ceilf ( label2_w + 2.0f * st.ItemSpacing.x );
        const float half_w = floorf ( ( field_w - label2_w - st.ItemSpacing.x ) * 0.5f );
        char nb[32];
        s_cti.body_marks.clear ( );
        for ( int f = 0; f < ( int ) CMTTMZ_TAPE_INFO_FIELDS; f++ ) {
            char *buf = cmttmz_tape_info_text ( &s_cti.form, ( uint32_t ) f );
            /* Year (3), Type (5), Protection (7) začínají řádek s druhým polem vpravo; Language, Price, Origin jsou vpravo */
            const bool left = ( f == 3 || f == 5 || f == 7 );
            const bool right = ( f == 4 || f == 6 || f == 8 );
            float x0, y0;
            if ( right ) {
                ImGui::SameLine ( 0.0f, st.ItemSpacing.x );
                x0 = ImGui::GetCursorPosX ( );
                y0 = ImGui::GetCursorPosY ( );
            } else {
                x0 = ImGui::GetCursorPosX ( );
                y0 = ImGui::GetCursorPosY ( );
                s_cti.body_marks.push_back ( y0 );
            }
            const float lw = right ? label2_w : label_w;
            /* popisek svisle na úrovni textu pole (vpravo za SameLine() přidá ImGui posun účaří řádku pole sám) */
            ImGui::SetCursorPosY ( y0 + ( right ? 0.0f : st.FramePadding.y ) );
            if ( right ) ImGui::SetCursorPosX ( x0 );
            ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT_DIM );
            ImGui::TextUnformatted ( cti_label ( f ) );
            ImGui::PopStyleColor ( );
            cti_record_item ( CTI_NAME ( nb, "ti_label", f ), cti_label ( f ), false, NULL );
            ImGui::SetCursorPos ( ImVec2 ( x0 + lw, y0 ) );
            ImGui::PushID ( f );
            if ( f == CTI_FIELD_COMMENT ) {
                /* Comment: víceřádkový, Enter = nový řádek */
                ImGui::InputTextMultiline ( "##ti_field", buf, CMTTMZ_TAPE_INFO_TEXT_SIZE, ImVec2 ( field_w, comment_h ),
                                            ImGuiInputTextFlags_CallbackEdit, cti_limit_cb );
            } else {
                ImGui::SetNextItemWidth ( left ? half_w : ( right ? field_w - half_w - st.ItemSpacing.x - label2_w : field_w ) );
                /* Enter v jednořádkovém poli = OK */
                if ( ImGui::InputText ( "##ti_field", buf, CMTTMZ_TAPE_INFO_TEXT_SIZE,
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit, cti_limit_cb ) )
                    do_ok = true;
            }
            ImGui::PopID ( );
            cti_record_item ( CTI_NAME ( nb, "ti_field", f ), buf, false, NULL );
        }
        /* Target machine */
        const float x0 = ImGui::GetCursorPosX ( );
        const float y0 = ImGui::GetCursorPosY ( );
        s_cti.body_marks.push_back ( y0 );
        ImGui::SetCursorPosY ( y0 + st.FramePadding.y );
        ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT_DIM );
        ImGui::TextUnformatted ( _ ( "Target machine" ) );
        ImGui::PopStyleColor ( );
        cti_record_item ( "ti_machine_label", _ ( "Target machine" ), false, NULL );
        ImGui::SetCursorPos ( ImVec2 ( x0 + label_w, y0 ) );
        ImGui::SetNextItemWidth ( field_w );
        if ( ImGui::BeginCombo ( "##ti_machine", cti_machine_text ( s_cti.machine_sel ) ) ) {
            combo_open = true;
            for ( int k = 0; k < CTI_MACHINE_ITEMS; k++ ) {
                ImGui::PushID ( k );
                /* stabilní ID položky (text se mění s jazykem) */
                const std::string item = std::string ( cti_machine_text ( k ) ) + "###ti_machine_item";
                if ( ImGui::Selectable ( item.c_str ( ), k == s_cti.machine_sel ) ) s_cti.machine_sel = k;
                if ( k == s_cti.machine_sel ) ImGui::SetItemDefaultFocus ( );
                ImGui::PopID ( );
            }
            ImGui::EndCombo ( );
        }
        cti_record_item ( "ti_machine", cti_machine_text ( s_cti.machine_sel ), false, NULL );
    }
    {
        ImGuiWindow *bw = ImGui::GetCurrentWindow ( );
        s_cti.body_content_h = ceilf ( bw->DC.CursorMaxPos.y - bw->DC.CursorStartPos.y );
    }
#ifdef MZTEST_HEADLESS
    s_cti_in_body = false;
#endif
    ImGui::EndChild ( );

    /* --- patička: věty, řádek stavu, tlačítka (vždy vidět); nad ní mezera aspoň 2u --- */
    const float y_footer = ImGui::GetCursorScreenPos ( ).y;
    ImGui::Dummy ( ImVec2 ( 0.0f, ImGui::GetFontSize ( ) / 7.0f ) );
    /* formát souboru pásky metadata neunese (MZF, MZT): SPEC */
    if ( !s_cti.orig.storable )
        cti_wrapped ( "ti_format_note", _ ( "This tape file format cannot store this information. Save the tape as TMZ to keep it." ), CMT_TOK_TEXT_DIM,
                      W );
    /* další blok Archive Info dál na pásce (formulář ho nevidí ani nemění) */
    if ( s_cti.orig.other_archive_blocks > 0 )
        cti_wrapped ( "ti_other_note", _ ( "This tape has another information block further on; it is left unchanged." ), CMT_TOK_TEXT_DIM, W );
    /* chyba zápisu (i částečně provedeného, který zvedl čítač pásky) a výzva po změně pásky - obojí, chyba první */
    if ( !s_cti.status.empty ( ) ) cti_wrapped ( "ti_error", s_cti.status, s_cti.status_error ? CMT_TOK_ERROR : CMT_TOK_WARN, W );
    if ( tape_changed ) cti_wrapped ( "ti_status", cti_text_tape_changed ( v ), CMT_TOK_WARN, W );

    const char *r_ok = NULL;
    if ( tape_changed ) r_ok = cti_text_tape_changed ( v );
    else if ( edit_reason ) r_ok = edit_reason;
    else if ( s_cti.seq ) r_ok = _ ( "Wait until the tape information is saved." );
    {
        const char *l_ok = _L ( "OK" );
        const char *l_cancel = _L ( "Cancel" );
        /* šířky podle společného pravidla tlačítek okna kazety */
        const float bw_ok = imgui_cmt_button_w ( l_ok );
        const float bw_cancel = imgui_cmt_button_w ( l_cancel );
        const float x0 = ImGui::GetCursorScreenPos ( ).x;
        const float y = ImGui::GetCursorScreenPos ( ).y;
        /* Reload vlevo: jen po změně pásky s vloženou páskou a bez čekajícího zápisu */
        if ( tape_changed && v->has_tape && !s_cti.seq ) {
            const char *l_reload = _L ( "Reload" );
            ImGui::SetCursorScreenPos ( ImVec2 ( x0, y ) );
            if ( imgui_cmt_button ( l_reload, 0.0f, NULL, false ) ) do_reload = true;
            cti_record_item ( "ti_reload", std::string ( l_reload, ImGui::FindRenderedTextEnd ( l_reload ) ), false, NULL );
        }
        ImGui::SetCursorScreenPos ( ImVec2 ( x0 + W - bw_ok - bw_cancel - st.ItemSpacing.x, y ) );
        if ( imgui_cmt_button ( l_ok, bw_ok, r_ok, true ) ) do_ok = true;
        cti_record_item ( "ti_ok", std::string ( l_ok, ImGui::FindRenderedTextEnd ( l_ok ) ), r_ok != NULL, r_ok );
        ImGui::SameLine ( );
        if ( imgui_cmt_button ( l_cancel, bw_cancel, NULL, false ) ) do_cancel = true;
        cti_record_item ( "ti_cancel", std::string ( l_cancel, ImGui::FindRenderedTextEnd ( l_cancel ) ), false, NULL );
    }
    s_cti.footer_h = ImGui::GetCursorScreenPos ( ).y - y_footer;
    /* Escape bez aktivního pole = Cancel (bez dotazu i s úpravami); ne ze snímku otevření a ne Escape,
     * který zavřel výběr Target machine (ImGui ho zavře už v NewFrame(), BeginCombo() pak vrátí false) */
    const bool combo_recent = combo_open || s_cti.combo_was_open;
    s_cti.combo_was_open = combo_open;
    if ( !appearing && !editing && !combo_recent && ImGui::IsKeyPressed ( ImGuiKey_Escape, false ) ) do_cancel = true;

    s_cti.last_size = win->Size;
    if ( imgui_cmt_window_current_outside_work ( ) ) s_cti.recenter_frames = ImMax ( s_cti.recenter_frames, 2 );
#ifdef MZTEST_HEADLESS
    s_cti_wmin = win->Pos;
    s_cti_wmax = ImVec2 ( win->Pos.x + win->Size.x, win->Pos.y + win->Size.y );
    s_cti_imin = win->InnerRect.Min;
    s_cti_imax = win->InnerRect.Max;
    s_cti_drawn = true;
#endif

    if ( do_reload && !do_cancel ) ( void ) cti_reload ( v );
    if ( do_ok && !r_ok && !do_cancel && !do_reload ) {
        if ( !cti_changed ( ) ) {
            /* beze změny: jako Cancel, nic se nezapisuje (pásku neoznačí jako změněnou) */
            do_cancel = true;
        } else {
            st_CMTTMZ_TAPE_INFO in = s_cti.form;
            in.machine = cti_machine_value ( s_cti.machine_sel );
            in.tape_serial = s_cti.tape_serial;
            s_cti.seq = cmt_request_tape_info_set ( s_cti.tape_serial, &in );
            s_cti.status.clear ( );
            if ( !s_cti.seq ) {
                s_cti.status = _ ( "The tape could not be edited." );
                s_cti.status_error = true;
            }
        }
    }
    if ( do_cancel ) {
        ImGui::CloseCurrentPopup ( );
        s_cti.popup_id = 0;
        cti_close ( );
    }
    ImGui::EndPopup ( );
#ifdef MZTEST_HEADLESS
    s_cti_items_done = s_cti_items;
#endif
}


/* ------------------------------------------------------------------------- */
/*  Testovací rozhraní                                                       */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
const std::vector<st_CMT_WINDOW_TEST_ITEM> &cmt_tapeinfo_dialog_test_items ( void )
{
    return s_cti_items_done;
}

bool cmt_tapeinfo_dialog_test_rect ( ImVec2 *wmin, ImVec2 *wmax, ImVec2 *imin, ImVec2 *imax )
{
    *wmin = s_cti_wmin;
    *wmax = s_cti_wmax;
    *imin = s_cti_imin;
    *imax = s_cti_imax;
    return s_cti_drawn && s_cti.open;
}

const char *cmt_tapeinfo_dialog_test_title ( void )
{
    return s_cti_title.c_str ( );
}

void cmt_tapeinfo_dialog_test_set_text ( int field, const char *text )
{
    char *buf = cmttmz_tape_info_text ( &s_cti.form, ( uint32_t ) field );
    if ( buf ) snprintf ( buf, CMTTMZ_TAPE_INFO_TEXT_SIZE, "%s", text ? text : "" );
}

void cmt_tapeinfo_dialog_test_set_machine ( int machine )
{
    s_cti.machine_sel = cti_machine_sel ( machine );
}

void cmt_tapeinfo_dialog_test_reset ( void )
{
    cti_close ( );
    s_cti.seq = 0;
    s_cti_items.clear ( );
    s_cti_items_done.clear ( );
    s_cti_drawn = false;
}
#endif
