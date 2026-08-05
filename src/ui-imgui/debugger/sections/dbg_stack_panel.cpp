/*
 * dbg_stack_panel.cpp - implementace Stack monitor panelu.
 *
 * V0: skeleton + hex dump + Set BP from SP-256.
 * V1: stack regions + low-water mark + dropdown v header + side panel +
 *     marker watermark + Add region modal.
 *
 * Refresh: g_dbg_ui.refresh.should_refresh - panel ctve dbgapi
 * (CMD_STACK_DUMP + CMD_STACK_REGIONS_LIST) jen kdyz je tick. Mezi tiky
 * pouziva cache (buf, sp_now, regions[]).
 *
 * Self-rate-limit: pokud CMDRQ fronta plna (emu vlakno blokovano,
 * napr. CMT/FileBrowser), skipuje request a zachova cache.
 *
 * Threading: pouze UI vlakno. Pristup ke g_stack je single-threaded.
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

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <float.h>

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "dbg_stack_panel.h"
#include "ui-imgui/debugger/debugger_state.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/debugger/stack_history_window/stack_history_window.h"
#include "ui-imgui/debugger/stack_regions_window/stack_regions_window.h"
#include "ui-imgui/bootstrap/myimgui.h"

extern "C" {
#include "emulator/debugger/dbgapi_cmdrq.h"
#include "emulator/debugger/dbgapi_ui.h"
#include "emulator/debugger/breakpoints.h"
#include "emulator/debugger/bookmarks/bookmarks.h"
#include "emulator/emulator.h"
#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
}

/* g_dbgapi_cmdrq_queue je definovany v emulator/debugger/dbgapi.c. */
extern "C" st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


/* ========================================================================= */
/*  Panel state                                                              */
/* ========================================================================= */

namespace {

/**
 * @brief Default pocet radku nad aktualnim SP (asymmetric split z V0).
 *
 * Stack roste dolu, SP ukazuje na nejnovejsi pushnuty slot. "Nad SP" jsou
 * starsi/hlubsi polozky. V0 hodnota = 32 (40 total = 32 nad + 8 pod).
 * V2 toggle "Lock SP center" prepinal na symetric 20/20 (deprecated).
 * V10: hodnota je default pro g_stack.sp_lines_above, ale runtime ji
 * meni vertikalni slider vlevo od hex tabulky (range 0..K_LINES_TOTAL-1).
 */
constexpr int K_LINES_ABOVE_SP_DEFAULT = 32;

/**
 * @brief Default pocet radku pod aktualnim SP (asymmetric split z V0).
 *
 * V0 hodnota = 8 (= jen vrchni cast budouciho stack growth viditelne).
 */
constexpr int K_LINES_BELOW_SP_DEFAULT = 8;

/**
 * @brief Celkovy pocet zobrazenych radku v tabulce hex dumpu.
 *
 * Konstanta, nezavisi na pozici SP v tabulce (= V10 slider meni jen
 * sp_lines_above v ramci tohoto rozsahu). Vyska tabulky a buffer
 * velikost pevne dany.
 */
constexpr int K_LINES_TOTAL = K_LINES_ABOVE_SP_DEFAULT
                                + K_LINES_BELOW_SP_DEFAULT;

/**
 * @brief Pocet radku nad SP pri "Center SP" reset (V10).
 *
 * Pouziva se v tlacitku "Center SP" v sticky header (= one-click reset
 * sp_lines_above na stred tabulky) a pri INI migraci old klice
 * lock_sp_center=1 -> sp_lines_above = K_LINES_TOTAL/2.
 */
constexpr int K_LINES_ABOVE_SP_CENTERED = K_LINES_TOTAL / 2;

/**
 * @brief Velikost cache bufferu pro hex dump dat (B).
 *
 * Pri word-oriented zobrazeni: K_LINES_TOTAL * 2 = 80 B. Pri lichem SP
 * (byte-oriented fallback) staci K_LINES_TOTAL = 40 B, ale cache vzdy
 * drzi worst-case 80 B (= jednoduchsi management, prepocet base_addr).
 */
constexpr int K_BUF_SIZE = K_LINES_TOTAL * 2;

/**
 * @brief "Zadny region" sentinel pro selected_region_idx.
 */
constexpr int K_REGION_NONE = -1;

/**
 * @brief Maximalni delka jmena regionu v UI bufferu.
 *
 * Drzime stejnou velikost jako dbgapi snapshot (32 B vc. '\0') aby
 * UI buffer mohl byt primo poslat do dbgapi ADD struct bez prekladu.
 */
constexpr int STACK_REGION_NAME_MAX_UI = 32;

/**
 * @brief Persistentni stav Stack panelu (drzeny mezi framy).
 *
 * Cache poslednich precteni z dbgapi + pomocna data pro render.
 *
 * @invariant buf_valid == false na startu, true az po prvni uspesne
 *            CMD_STACK_DUMP odpovedi.
 * @invariant base_addr je adresa odpovidajici buf[0] (= nejvyssi
 *            zobrazena adresa, vrchol viditelneho okna).
 * @invariant sp_now je platne jen kdyz buf_valid == true.
 * @invariant regions_valid == true po prvni uspesne CMD_STACK_REGIONS_LIST.
 * @invariant selected_region_idx == K_REGION_NONE pokud nejake region neni
 *            vybran v UI dropdownu, jinak v intervalu <0, regions.count).
 */
struct StackPanelState {
    /* Cache hex dump bajtu. buf[0] odpovida adrese base_addr (= nejvyssi
     * zobrazena, stack roste dolu, takze base = SP + 2*K_LINES_ABOVE_SP). */
    uint8_t  buf[K_BUF_SIZE];

    /* Adresa odpovidajici buf[0] - vrchol viditelneho okna. */
    uint16_t base_addr;

    /* Aktualni hodnota SP precten v okamziku posledniho refreshe. */
    uint16_t sp_now;

    /* 1 = SP byl lichy v okamziku refreshe (UI prepne na byte-oriented). */
    uint8_t  sp_odd;

    /* 1 = cache je platna (= byl alespon jeden uspesny refresh). */
    bool     buf_valid;

    /* Cache regionu z CMD_STACK_REGIONS_LIST. */
    st_DBGAPI_STACK_REGIONS_LIST_PARAM regions;
    bool     regions_valid;

    /* Index vybraneho regionu v dropdownu (-1 = none). */
    int      selected_region_idx;

    /* Open flag pro "Add region" modal. Setuje se na true pri kliku
     * na tlacitko v sticky header, ImGui dialog si jej drzi az do
     * zavreni (OK / Cancel). */
    bool     add_modal_open;

    /* Input bufery pro Add region modal. Persistovane mezi framy
     * (ImGui InputText). */
    char     add_name[STACK_REGION_NAME_MAX_UI];
    char     add_base_hex[8];
    char     add_limit_hex[8];

    /* Validacni chyba pri Add (zobrazi se ve modalu). NULL = bez chyby. */
    const char *add_error;

    /* Flag pro yellow hint v Add modal: true = pri otevreni modalu byl SP
     * roven 0xFFFF (= Z80 reset state, uzivatel jeste nenastavil SP). V tom
     * pripade reset_add_form pouzil default base $10F0 (NEWSP MZ-700/MZ-800
     * monitor stack) namisto current SP. Hint zustava zobrazen po cely cas
     * otevreni modalu (= nereaguje na zmenu SP behem editace), shazuje se
     * pri pristim otevreni. */
    bool     add_reset_hint;

    /* V2: Lock SP center toggle (V10 deprecated, zachovano pro INI
     * zpetnou kompatibilitu).
     * false (default) = asymetric split 32/8 (V0 chovani).
     * true = symetric 20/20 (= SP zhruba uprostred tabulky).
     *
     * V10: Lock SP center toggle v sticky header je nahrazen tlacitkem
     * "Center SP" (= one-click reset sp_lines_above na K_LINES_TOTAL/2).
     * Pole zustava jen pro propagate INI: pokud old INI obsahuje
     * lock_sp_center=1, mapujeme ho v cfg_propagate_lock_sp_center na
     * sp_lines_above = K_LINES_ABOVE_SP_CENTERED. Save: hodnotu jiz
     * nezapisujeme (= novy klic sp_lines_above je primary). */
    bool lock_sp_center;

    /* V10: runtime pozice SP marker radku v hex dump tabulce.
     * Range <0, K_LINES_TOTAL-1>, default K_LINES_ABOVE_SP_DEFAULT (= 32).
     * Hodnota predstavuje pocet radku zobrazenych nad SP (= radek SP je
     * v tabulce na indexu sp_lines_above). Slider vlevo od tabulky umoznuje
     * uzivateli dragem nastavit libovolnou pozici. K_LINES_BELOW se
     * automaticky odvodi jako K_LINES_TOTAL - sp_lines_above.
     *
     * Persistence v [STACK_PANEL] INI pres novy klic "sp_lines_above"
     * (CFGENTYPE_UNSIGNED, default 32, range 0..K_LINES_TOTAL-1). */
    int sp_lines_above;

    /* V2: SP history recording toggle (= shadow stavu emu flagu).
     * Default false - recording vypnut = zero hot-path overhead. UI ho
     * preklepa pres dbgapi STACK_HISTORY_ENABLE, emu nastavi
     * g_stack_history_active. */
    bool history_enabled;

    /* V2: cache snapshotu SP history pro sparkline render. Alokovan
     * staticky pres velikost DBGAPI_STACK_HISTORY_MAX (4096 * 8 B = 32 KB)
     * - jednorazova pamet pro UI. */
    st_DBGAPI_STACK_HISTORY_SAMPLE history_samples[ DBGAPI_STACK_HISTORY_MAX ];
    uint32_t history_count;       /**< Pocet validnich vzorku v history_samples */
    bool     history_valid;        /**< true = cache naplnena alespon jednou */

    /* V2: stack creep detekce.
     * creep_slope = posledni vypocteny slope (SP/cycle, zaporna = klesa).
     * creep_warning = true pokud slope < K_CREEP_SLOPE_THRESHOLD a
     *                 history je aktivni. */
    float creep_slope;
    bool  creep_warning;

    /* V2: float buffer pro ImGui::PlotLines (potrebuje float pole).
     * Naplnuje se pri refreshi z history_samples (= sp casti). */
    float sparkline_buf[ DBGAPI_STACK_HISTORY_MAX ];

    /* V3: per-radek decode info z disasm-back heuristiky. Index 0 odpovida
     * radku s adresou base_addr (nejvyssi zobrazena adresa), index roste
     * smerem k nizsim adresam (DESC). Naplnuje handler CMD_STACK_DUMP. */
    st_DBGAPI_STACK_DECODE_INFO decode_buf[ K_LINES_TOTAL ];
    bool  decode_valid;

    /* V2.1: hlavni SP history sparkline - klikatelny vybrany sample.
     * selected_history_idx >= 0 a < history_count = sample zvyrazneny
     * (zluty crosshair + info text pod sparkline). -1 = zadny vybran.
     * Vyber se vytvori LMB klikem v plot area, zrusi LMB klikem mimo
     * data (Y mimo polyline rangu) nebo opetovnym kliknutim na stejny
     * sample. */
    int  selected_history_idx;

    /* V2.1: toggle pro vykresleni push/pop/other event markeru nad
     * sparkline. Default true (= zobrazit). Pri vysokem countu (>= 2*W)
     * se push/pop markery automaticky filtruji jako "noise" a zobrazuji
     * se jen "other" delta markery (LD SP,X / INT vector dispatch). */
    bool show_events;

    /* V7: Edit region modal stav. Pri otevreni se predvyplni z existujici
     * region cache (edit_idx, edit_name, edit_base_hex, edit_limit_hex).
     * Modal pouziva stable popup ID "###stack_edit_modal". */
    bool     edit_modal_open;       /**< true = modal otevren (mirror BeginPopupModal) */
    int      edit_idx;              /**< Index editovaneho regionu, -1 = invalid */
    char     edit_name[STACK_REGION_NAME_MAX_UI];
    char     edit_base_hex[8];
    char     edit_limit_hex[8];
    const char *edit_error;         /**< NULL = bez chyby, jinak label ChervenymTextom */

    /* V3.2: RMB popup pro Decode sloupec.
     * Pri pravem kliku na decoded radek se uchova target adresa CALL/RST
     * (= cil Focus DA), popup se otevre v render funkci po dokresleni
     * vsech radku tabulky. Single popup state staci - ImGui umoznuje jen
     * jeden popup aktivni najednou.
     *
     * V3.2 stable popup ID je "stack_decode_popup" (= sdilene mezi radky -
     * fokus je vzdy jen jeden, prepiseme target_addr pri otevreni). */
    uint16_t decode_rmb_target;
    bool     decode_rmb_open;

    /* V8.5: shadow stav "SP history" CollapsingHeader (= rozbaleno/zbaleno).
     * ImGui CollapsingHeader je imperativni - vraci current open state pri
     * volani render. Sledujeme posledni hodnotu z predchoziho framu, aby
     * dbg_stack_panel_render() mohl v aktualnim framu rozhodnout o velikosti
     * sparkline section (= 1-frame lag pri prvnim collapse/expand, v UX
     * neviditelne). Default true = header se rozbali pri prvnim render
     * (souhlas s ImGuiTreeNodeFlags_DefaultOpen). */
    bool     history_header_open;

    /* V8.5: detekce transition history_enabled false->true pro auto-expand
     * collapsing header. Pri zapnuti SP history toggle (= checkbox v sticky
     * header) chceme, aby se "SP history" CollapsingHeader automaticky
     * rozbalil, i kdyz ho uzivatel drive rucne zbalil. */
    bool     history_prev_enabled;

    /* V10.1: snapshot scroll stavu hex tabulky z minuleho framu (= predani
     * informace z BeginTable scope do slideru, ktery se renderuje pred
     * tabulkou pres SameLine). Slider tyto hodnoty pouziva pro sync sipky
     * jezdce s vizualne viditelnou casti hex tabulky:
     *
     *   hex_scroll_y_last     - GetScrollY uvnitr BeginTable(ScrollY)
     *   hex_scroll_max_last   - GetScrollMaxY (= max scroll offset)
     *   hex_viewport_h_last   - vyska viditelne casti table content area
     *   hex_row_h_last        - vyska jednoho radku tabulky v px (data row)
     *   hex_state_valid       - true az po prvnim render framu (jinak slider
     *                           pouzije fallback = celou tabulku jako track)
     *
     * 1-frame lag je v UX neviditelny (= refresh interval 100 ms >> 1 frame
     * pri 60 FPS = 16 ms).
     *
     * V10.1: pending_scroll_to_sp - request, ze pri pristim render tabulky
     * ma byt SP radek scrollnut do viditelne casti (= drag slideru zmenil
     * sp_lines_above, ale SP je mimo viewport => scroll). */
    float    hex_scroll_y_last;
    float    hex_scroll_max_last;
    float    hex_viewport_h_last;
    float    hex_row_h_last;
    bool     hex_state_valid;
    bool     pending_scroll_to_sp;

    /* V11: Word sloupec - double-click edit 16-bit hex hodnoty.
     *
     *   word_edit_row          - index radku (= i v render_table loop) ktery
     *                            je prave editovan, -1 = nic needitujeme.
     *   word_edit_addr         - adresa pameti pro zapis (= addr pri otevreni
     *                            edit modu, ulozena zvlast kvuli race vuci
     *                            refreshi base_addr).
     *   word_edit_orig         - puvodni hodnota pred editaci (= zobrazena
     *                            pri otevreni edit boxu).
     *   word_edit_just_opened  - true v prvnim framu po double-click; pouziva
     *                            se pro SetKeyboardFocusHere + naplnit edit_buf.
     *   word_edit_buf          - retezec InputText (uint16 hex, max 4 znaky
     *                            + NUL = velikost bufferu 5).
     *
     * Edit se commit pri Enter (= EnterReturnsTrue flag), cancel pri Escape
     * (= ImGui InputText Escape vrati neaktivni focus, IsItemDeactivated
     * bez IsItemDeactivatedAfterEdit = cancel). Pri kliku mimo InputText
     * (= IsItemDeactivated bez Enter) bereme jako cancel (= bez zapisu).
     *
     * Pri double-click se emu pauzne pres dbg_ui_pause() pokud bezi (= aby
     * editace nebyla na pohyblivem cili). Po commit MEM_WRITE pres
     * dbg_ui_mem_write(addr, &lsb_msb, 2) - LE byte order. */
    int      word_edit_row;
    uint16_t word_edit_addr;
    uint16_t word_edit_orig;
    bool     word_edit_just_opened;
    char     word_edit_buf[5];  /* V11.2: max 4 hex znaky + NUL */

    /* V10.2: snapshot presneho screen-Y bodu, kde v hex tabulce zacina
     * prvni data row (= bod hned pod header rowem, uvnitr scroll area).
     * Slider tuto hodnotu pouzije misto aproximace pres
     * GetTextLineHeightWithSpacing - aproximace v V10.1 zpusobovala off-by-1
     * (header row v ImGui table ma vysku GetFrameHeight + paddings, nikoliv
     * GetTextLineHeightWithSpacing).
     *
     * hex_child_top_y_last = screen-Y bod, kde zacina horni hrana
     * BeginChild kontaineru "###stack_top_section" (= shoda s GetCursorScreenPos
     * v okamziku, kdy se vola stack_panel_render_sp_slider). Slider odecte
     * hex_child_top_y_last od hex_data_top_y_last a ziska tak presny
     * track_top_offset pro aktualni layout.
     *
     * 1-frame lag: pri prvni render snapshot jeste neni validni a slider
     * pouzije fallback GetTextLineHeightWithSpacing. Po prvnim refresh
     * snapshot uz odpovida realne geometrii tabulky a sipka sedi presne. */
    float    hex_data_top_y_last;
    float    hex_child_top_y_last;

    /* V13: Word fade highlight tracking. Per-řádek si pamatujeme adresu
     * a Word hodnotu z minulého refreshe + čas poslední změny. Pokud se
     * v dalším refreshi adresa stejného řádku nezměnila (= stejný viewport,
     * žádný auto-scroll / SP move) a hodnota se změnila, zaznamenáme nový
     * change_time. Render kód pak na základě uplynulé doby od change_time
     * vykreslí zlatý fade přes CellBg v sloupci Word.
     *
     *   prev_words[i]      - hodnota Word na řádku i z minulého refreshe
     *   prev_addrs[i]      - adresa řádku i z minulého refreshe (= base_addr
     *                        - i*step v okamžiku refreshe)
     *   change_time[i]     - ImGui::GetTime() okamžiku poslední detekované
     *                        změny hodnoty (sekundy). 0.0 = nikdy.
     *   prev_valid         - false před prvním refreshem (= nic k porovnání),
     *                        i po refreshi false pokud poslední snapshot
     *                        byl byte-mode (= žádné Word hodnoty k sledování).
     *
     * Fade délka K_WORD_FADE_SEC v sekundách (= 1.5 s, shoda se zadáním V13).
     * Fade probíhá jen v word-mode; v byte-mode (lichý SP) je fade tracking
     * vypnut a prev_valid se resetuje na false (= příští word-mode refresh
     * začne čistě, žádný "stale fade" z dávno editovaného řádku). */
    uint16_t prev_words[K_LINES_TOTAL];
    uint16_t prev_addrs[K_LINES_TOTAL];
    double   change_time[K_LINES_TOTAL];
    bool     prev_valid;
};

static StackPanelState g_stack = {
    {0},                /* buf */
    0,                  /* base_addr */
    0,                  /* sp_now */
    0,                  /* sp_odd */
    false,              /* buf_valid */
    {0, 0, {}},         /* regions (count=0, sp_now=0, regions[]={}) */
    false,              /* regions_valid */
    K_REGION_NONE,      /* selected_region_idx */
    false,              /* add_modal_open */
    {0},                /* add_name */
    {0},                /* add_base_hex */
    {0},                /* add_limit_hex */
    NULL,               /* add_error */
    false,              /* add_reset_hint */
    false,              /* lock_sp_center (V2 deprecated, V10 default off) */
    K_LINES_ABOVE_SP_DEFAULT,  /* sp_lines_above (V10 default 32) */
    false,              /* history_enabled (V2 default off) */
    {},                 /* history_samples */
    0,                  /* history_count */
    false,              /* history_valid */
    0.0f,               /* creep_slope */
    false,              /* creep_warning */
    {},                 /* sparkline_buf */
    {},                 /* decode_buf (V3) */
    false,              /* decode_valid (V3) */
    -1,                 /* selected_history_idx (V2.1) */
    true,               /* show_events (V2.1 default on) */
    false,              /* edit_modal_open (V7) */
    -1,                 /* edit_idx (V7) */
    {0},                /* edit_name */
    {0},                /* edit_base_hex */
    {0},                /* edit_limit_hex */
    NULL,               /* edit_error */
    0,                  /* decode_rmb_target (V3.2) */
    false,              /* decode_rmb_open (V3.2) */
    true,               /* history_header_open (V8.5 default true = shoda s DefaultOpen) */
    false,              /* history_prev_enabled (V8.5) */
    0.0f,               /* hex_scroll_y_last (V10.1) */
    0.0f,               /* hex_scroll_max_last (V10.1) */
    0.0f,               /* hex_viewport_h_last (V10.1) */
    0.0f,               /* hex_row_h_last (V10.1) */
    false,              /* hex_state_valid (V10.1) */
    false,              /* pending_scroll_to_sp (V10.1) */
    -1,                 /* word_edit_row (V11) */
    0,                  /* word_edit_addr (V11) */
    0,                  /* word_edit_orig (V11) */
    false,              /* word_edit_just_opened (V11) */
    {0},                /* word_edit_buf (V11) */
    0.0f,               /* hex_data_top_y_last (V10.2) */
    0.0f,               /* hex_child_top_y_last (V10.2) */
    {},                 /* prev_words (V13) */
    {},                 /* prev_addrs (V13) */
    {},                 /* change_time (V13) */
    false               /* prev_valid (V13) */
};


/**
 * @brief Empirický práh stack creep detekce.
 *
 * Slope < threshold (= zápornější) -> creep warning. Hodnota -1.0e-4
 * znamená "SP klesá v průměru o 0.0001 jednotek na 1 T-state". Pro
 * Z80 3.5 MHz to odpovídá pádu SP o cca 350 B/s = pomalý leak.
 *
 * Tuning: -1.0e-3 = strictnější (= zachytí jen rychlý leak), -1.0e-5 =
 * citlivější (= i dlouhodobé pomalé klesání). Default mid-ground.
 * V2 zatim nebudeme exposovat UI (= konstanta v kódu).
 */
constexpr float K_CREEP_SLOPE_THRESHOLD = -1.0e-4f;

/**
 * @brief Velikost slope-window pri creep detekci.
 *
 * Lineární regrese se počítá přes posledních N vzorků. 256 je shoda s
 * návrhem (sekce 4.6). Méně = reaktivnější ale šumovější, víc =
 * stabilnější ale pomalejší detekce.
 */
constexpr uint32_t K_CREEP_SLOPE_WINDOW = 256u;


/**
 * @brief V13: doba zlatého fade efektu po změně Word hodnoty (sekundy).
 *
 * Po detekci memory write na řádku se cell Word zvýrazní zlatým bg
 * (alpha klesá lineárně 1.0 -> 0.0). 1.5 s je shoda se zadáním V13 a
 * subjektivně dobře viditelné při typické refresh frekvenci 10 Hz
 * (= 15 refresh ticků v rámci fade okna).
 */
constexpr double K_WORD_FADE_SEC = 1.5;


/**
 * @brief Vrati aktualni pocet radku nad SP (V10 runtime hodnota).
 *
 * Vraci g_stack.sp_lines_above clipnute na <0, K_LINES_TOTAL-1>. Hodnotu
 * meni vertikalni slider vlevo od hex tabulky (V10). Tlacitko "Center SP"
 * ji jednorazove nastavi na K_LINES_TOTAL/2 (= V2 "lock_sp_center on"
 * chovani jako quick reset).
 *
 * Clip na <0, K_LINES_TOTAL-1> chrani pred poskozeniym INI (= drag
 * v slideru je nativne clipovan na rozsah <0, K_LINES_TOTAL-1>, ale
 * persistence z INI muze prinest neplatnou hodnotu).
 */
static inline int stack_panel_lines_above(void)
{
    int v = g_stack.sp_lines_above;
    if (v < 0) v = 0;
    if (v > K_LINES_TOTAL - 1) v = K_LINES_TOTAL - 1;
    return v;
}

} /* anon namespace */


/* ========================================================================= */
/*  Refresh dat z emulatoru                                                  */
/* ========================================================================= */

/**
 * @brief Provede single dbgapi STACK_DUMP fetch a aktualizuje cache.
 *
 * Self-rate-limit: pokud je CMDRQ fronta plna, refresh skipne (= cache
 * zustane). Timeout 50 ms (= bezpecna rezerva nad safepoint 20 ms pri
 * 50 Hz emulaci). Pri timeoutu zachova predchozi cache.
 *
 * Strategie volby base_addr:
 *   SP-anchored mode (lines_above = K_LINES_ABOVE_SP). Handler v emu
 *   sam spocita base = sp_now + lines_above*2 z aktualniho SP a buf
 *   naplni DESC od base dolu. Vraci pouzitou base v p.addr. UI tim
 *   ma zaruceno ze okno je konzistentni s SP v temz ticku - zadny
 *   1-tick lag.
 *
 * Side effects: aktualizuje g_stack.buf, sp_now, sp_odd, base_addr,
 *               buf_valid.
 */
static void stack_panel_refresh_dump(void)
{
    /* Self-rate-limit: fronta plna = skip. */
    if (dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue)) {
        return;
    };

    st_DBGAPI_STACK_DUMP_PARAM p;
    p.addr        = 0; /* SP-anchored mode -> handler prepise */
    p.len         = (uint16_t)K_BUF_SIZE;
    /* Lines_above je dynamicke - V10 hodnota z g_stack.sp_lines_above
     * (= pozice SP radku, default 32, range 0..K_LINES_TOTAL-1, slider
     * vlevo od hex tabulky). Handler v emu spocita base = sp +
     * lines_above*step, kde step je 1 (lichy SP) nebo 2. Render uziva
     * stejnou hodnotu pro mapovani buf[i] na addr. */
    p.lines_above = (uint16_t)stack_panel_lines_above();
    p.buf         = g_stack.buf;
    p.sp_now      = 0;
    p.sp_odd      = 0;
    /* V3: decode pole - handler vyplni disasm-back heuristikou pro kazdy
     * radek tabulky. Jeden round-trip, zadne extra CMDRQ. */
    p.decode_buf   = g_stack.decode_buf;
    p.decode_count = (uint16_t)K_LINES_TOTAL;

    /* Timeout 50 ms - shoda s cpu_panel pristupem (V2.1 Fix C). */
    if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                    DBGAPI_CMD_STACK_DUMP,
                                    &p, NULL, 50))
    {
        return;
    };

    g_stack.base_addr    = p.addr;
    g_stack.sp_now       = p.sp_now;
    g_stack.sp_odd       = p.sp_odd;
    g_stack.buf_valid    = true;
    /* Decode platí jen pro sudý SP (word-mode). Handler v opačném
     * případě vynuluje decode_buf, ale flag tu setujeme jen pokud má
     * decode smysl - render používá flag pro skip. */
    g_stack.decode_valid = (p.sp_odd == 0);

    /* V13: tracking Word změn pro fade highlight. Probíhá jen v word-mode
     * (= sudý SP); v byte-mode se invaliduje prev_valid (= příští word-mode
     * refresh začne s čistým stavem, žádný stale fade z dřívějška).
     *
     * Highlight bere jen reálné memory writes:
     *  (a) prev_valid - máme co srovnávat
     *  (b) prev_addrs[i] == cur_addr - viewport se nezměnil (= base_addr
     *      ani lines_above se neposunuly), takže řádek odpovídá stejné
     *      paměti. Při auto-scroll / SP move se addr[i] změní = NEzapíše
     *      change_time = žádný fade z scrollu.
     *  (c) prev_words[i] != cur_word - hodnota se reálně změnila. */
    if (p.sp_odd == 0) {
        double now = ImGui::GetTime();
        for (int i = 0; i < K_LINES_TOTAL; i++) {
            uint16_t addr_i = (uint16_t)(p.addr - (uint32_t)i * 2);
            int off = i * 2;
            uint8_t lsb = g_stack.buf[off];
            uint8_t msb = (off > 0) ? g_stack.buf[off - 1] : 0;
            uint16_t word_i = (uint16_t)(((uint16_t)msb << 8) | lsb);

            if (g_stack.prev_valid
                    && g_stack.prev_addrs[i] == addr_i
                    && g_stack.prev_words[i] != word_i) {
                g_stack.change_time[i] = now;
            };

            g_stack.prev_addrs[i] = addr_i;
            g_stack.prev_words[i] = word_i;
        };
        g_stack.prev_valid = true;
    } else {
        /* Byte-mode: fade tracking nedává smysl (= jednotlivé byty,
         * Word sloupec zobrazuje "--"). Invalidace = příští word-mode
         * refresh restartuje sledování. */
        g_stack.prev_valid = false;
    };
}


/**
 * @brief Refresh seznamu stack regionu z emulatoru.
 *
 * Vola dbgapi CMD_STACK_REGIONS_LIST a naplni g_stack.regions snapshot.
 * Self-rate-limit + 50 ms timeout shoda s stack_panel_refresh_dump.
 *
 * Side effects: aktualizuje g_stack.regions + regions_valid. Pokud
 *               selected_region_idx ukazoval na region ktery byl mezitim
 *               odebran (count se zmensil), reset na K_REGION_NONE.
 */
static void stack_panel_refresh_regions(void)
{
    if (dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue)) {
        return;
    };

    /* Lokalni param - emu ho naplni. Pak prekopirujeme do cache. */
    st_DBGAPI_STACK_REGIONS_LIST_PARAM local;
    memset(&local, 0, sizeof(local));

    if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                    DBGAPI_CMD_STACK_REGIONS_LIST,
                                    &local, NULL, 50))
    {
        return;
    };

    g_stack.regions = local;
    g_stack.regions_valid = true;

    /* Pokud byl vybrany region mezitim odebran (count se snizil pod
     * selected idx), reset na "none". */
    if (g_stack.selected_region_idx >= g_stack.regions.count) {
        g_stack.selected_region_idx = K_REGION_NONE;
    };
}


/* ========================================================================= */
/*  Pomocne funkce - selected region info                                    */
/* ========================================================================= */

/**
 * @brief Vrati pointer na vybrany region info nebo NULL pokud zadny vybran.
 *
 * @return Pointer na st_DBGAPI_STACK_REGION_INFO v cache, NULL pri
 *         selected_region_idx == K_REGION_NONE / mimo rozsah.
 */
static const st_DBGAPI_STACK_REGION_INFO *stack_panel_get_selected(void)
{
    if (!g_stack.regions_valid) return NULL;
    int idx = g_stack.selected_region_idx;
    if (idx < 0 || idx >= g_stack.regions.count) return NULL;
    return &g_stack.regions.regions[idx];
}


/**
 * @brief V12: najde index regionu v UI cache obsahujícího danou adresu.
 *
 * Lineární průchod přes @c g_stack.regions snapshot (= UI thread-local
 * data, žádný race vůči EMU vláknu). Match podmínka @c limit <= addr <=
 * @c base, vrací index prvního shody. Pokud cache není ještě platná
 * (= před prvním refreshem), vrátí -1.
 *
 * Slouží render smyčce hex tabulky pro per-řádek region background color
 * (V12). Odpovídá globálnímu @ref stack_regions_find_by_addr, ale pracuje
 * nad UI snapshotem, ne přes přímý přístup ke globálnímu polu (= shoda
 * s pattern of g_stack.regions.regions[] cache).
 *
 * @param addr  Adresa řádku v hex tabulce.
 * @return Index regionu (0..regions.count-1), nebo -1 pokud mimo všechny.
 */
static int stack_panel_region_idx_for_addr(uint16_t addr)
{
    if (!g_stack.regions_valid) return -1;
    int n = g_stack.regions.count;
    for (int i = 0; i < n; i++) {
        const st_DBGAPI_STACK_REGION_INFO *r = &g_stack.regions.regions[i];
        if (addr >= r->limit && addr <= r->base) {
            return i;
        };
    };
    return -1;
}


/* ========================================================================= */
/*  Set BP from SP - 256                                                     */
/* ========================================================================= */

/**
 * @brief Vytvori novy SP_THRESHOLD BPT s thresholdem = current_sp - 256.
 *
 * V0 quick action - bez konceptu region. Tlacitko v sticky header
 * jednoklikem zalozi BP, ktery se aktivuje pri poklesu SP pod current_sp
 * - 256 (= 256 B headroom od aktualniho SP smerem dolu). Prakticke
 * pouziti: rychly safety net "pokud stack klesne o vic nez 256 B nez
 * je ted, zastav".
 *
 * Race-safe vuci EMU vlaknu: V1.7+ pres dbg_ui_bp_create_with_init
 * (= CMD_BP_CREATE_WITH_INIT pres dbgapi frontu, EMU vlakno atomicky
 * vytvori BP + nastavi type + threshold v jednom dispatch). 16-bit
 * wrap pri SP < 256 je zachovan jako-je (uzivatel hodnotu uvidi a muze
 * upravit v BP edit).
 *
 * Side effects: registruje novy BPT, BP overlay tak ihned ukaze
 *               podsvicenou hranici.
 */
static void stack_panel_set_bp_sp_minus_256(void)
{
    if (!g_stack.buf_valid) {
        return;
    };

    uint16_t threshold = (uint16_t)(g_stack.sp_now - 256);

    /* Atomicky create + init: addr = threshold (= placeholder pro legacy
     * bptmap dispatch, SP_THRESHOLD enforcement uziva sp_threshold field),
     * type = SP_THRESHOLD, sp_threshold = threshold. BP zustane v SINGLE
     * mode (default) = "fire kdyz SP < threshold". */
    st_DBGAPI_BP_UPDATE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.update_mask = DBGAPI_BP_UM_ADDR
                  | DBGAPI_BP_UM_TYPE
                  | DBGAPI_BP_UM_SP_THRESHOLD;
    p.addr = threshold;
    p.type = (uint8_t)BPT_TYPE_SP_THRESHOLD;
    p.sp_threshold = threshold;
    (void)dbg_ui_bp_create_with_init(&p, NULL);
}


/* ========================================================================= */
/*  Reset watermark pro vybrany region                                       */
/* ========================================================================= */

/**
 * @brief Posle dbgapi CMD_STACK_REGIONS_RESET_WATERMARK pro vybrany region.
 *
 * No-op pokud nejake region neni vybran. Race-safe vuci EMU vlaknu
 * (= sync request pres frontu, EMU handler bezi v safepointu).
 */
static void stack_panel_reset_watermark_selected(void)
{
    if (g_stack.selected_region_idx < 0) return;
    if (g_stack.selected_region_idx >= g_stack.regions.count) return;

    if (dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue)) return;

    st_DBGAPI_STACK_REGIONS_REMOVE_PARAM p;
    p.index = g_stack.selected_region_idx;

    dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                               DBGAPI_CMD_STACK_REGIONS_RESET_WATERMARK,
                               &p, NULL, 50);
    /* Force refresh regionu pristim frame (= novy watermark v UI). */
    g_stack.regions_valid = false;
}


/**
 * @brief Posle dbgapi CMD_STACK_REGIONS_REMOVE pro zadany index.
 *
 * Race-safe vuci EMU vlaknu. Po uspesnem odebrani force refresh regionu.
 *
 * @param idx  Index regionu k odebrani (0..count-1).
 */
static void stack_panel_remove_region(int idx)
{
    if (idx < 0 || idx >= g_stack.regions.count) return;
    if (dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue)) return;

    st_DBGAPI_STACK_REGIONS_REMOVE_PARAM p;
    p.index = idx;
    dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                               DBGAPI_CMD_STACK_REGIONS_REMOVE,
                               &p, NULL, 50);
    /* Pokud byl prave odstraneny vybran, reset selected. Pripadny
     * pretek (selected ukazoval na idx-1 nebo dal vpravo) se vyresi
     * pri pristim refreshi v stack_panel_refresh_regions. */
    if (g_stack.selected_region_idx == idx) {
        g_stack.selected_region_idx = K_REGION_NONE;
    };
    g_stack.regions_valid = false;
}


/**
 * @brief Resetuje stav Add modal na default (current SP, prazdne jmeno).
 *
 * Vola se pri otevreni modalu pres tlacitko "Add region from current SP".
 * Default name = "region_N" kde N = next free index, base = current_sp,
 * limit = base - 256.
 *
 * Specialni pripady pro default base:
 *  - cache SP neni jeste platna (buf_valid==false) NEBO sp_now==0xFFFF
 *    (= Z80 reset state, uzivatel jeste neudelal LD SP,X): pouzijeme
 *    NEWSP default $10F0 (= ROM monitor stack pozice MZ-700/MZ-800),
 *    aby se predeslo vzniku neuziteneho regionu $FEFF-$FFFF, do ktereho
 *    ROM monitor nikdy nezapisuje. Soucasne nastavime add_reset_hint=true,
 *    aby modal zobrazil zluty hint vysvetlujici, proc base neni current SP.
 *  - jinak: base = aktualni SP.
 */
static void stack_panel_reset_add_form(void)
{
    int next_idx = g_stack.regions_valid ? g_stack.regions.count : 0;
    /* Default name "region_N" - vejde se vzdy do 32 B (region_N kde
     * N je 0..7 = max 8 znaku + '\0'). */
    snprintf(g_stack.add_name, sizeof(g_stack.add_name),
              "region_%d", next_idx);

    uint16_t base;
    if (!g_stack.buf_valid || g_stack.sp_now == 0xFFFF) {
        /* Z80 reset state nebo neaktivni SP read - pouzij NEWSP default
         * ($10F0 = ROM monitor stack pozice MZ-700/MZ-800). */
        base = 0x10F0;
        g_stack.add_reset_hint = true;
    } else {
        base = g_stack.sp_now;
        g_stack.add_reset_hint = false;
    };
    uint16_t limit = (uint16_t)(base - 256);

    snprintf(g_stack.add_base_hex, sizeof(g_stack.add_base_hex),
              "%04X", base);
    snprintf(g_stack.add_limit_hex, sizeof(g_stack.add_limit_hex),
              "%04X", limit);

    g_stack.add_error = NULL;
}


/**
 * @brief Parsne hexadecimalni string na uint16_t.
 *
 * @param s    Vstupni string ("ABCD", "abcd", "0xABCD", ...).
 * @param out  Vystup uint16_t.
 * @return true pri uspechu, false pri prazdnem stringu / invalid znaku.
 */
static bool stack_panel_parse_hex16(const char *s, uint16_t *out)
{
    if (!s || !*s) return false;
    /* Optional "0x" prefix preskocime. */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return false;

    uint32_t val = 0;
    int digits = 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return false;
        val = (val << 4) | (uint32_t)d;
        digits++;
        if (digits > 4) return false;
    };
    *out = (uint16_t)val;
    return true;
}


/**
 * @brief Posle dbgapi CMD_STACK_REGIONS_ADD podle hodnot v Add modalu.
 *
 * Predcasna validace v UI (= rychla zpetna vazba):
 *  - name nesmi byt prazdny, max 31 znaku
 *  - base / limit musi byt validni hex (4 znaky)
 *  - base > limit (= netrivialni region)
 *
 * Skutecna autoritativni validace (regex jmena, full check, name conflict)
 * probiha v stack_regions_add v emu vlaknu. Pokud handler vrati index < 0,
 * zobrazime generickou chybu "Add failed".
 *
 * @return true pokud byl add submitnut a uspesny (= modal se zavre),
 *         false pri validacni chybe nebo failnutim ADD (= g_stack.add_error
 *         obsahuje zpravu, modal zustane otevreny).
 */
static bool stack_panel_submit_add(void)
{
    g_stack.add_error = NULL;

    /* Lokalni validace. */
    size_t name_len = strlen(g_stack.add_name);
    if (name_len == 0) {
        g_stack.add_error = _("Name is required");
        return false;
    };
    if (name_len >= STACK_REGION_NAME_MAX_UI) {
        g_stack.add_error = _("Name is too long");
        return false;
    };

    uint16_t base = 0;
    uint16_t limit = 0;
    if (!stack_panel_parse_hex16(g_stack.add_base_hex, &base)) {
        g_stack.add_error = _("Invalid base (hex)");
        return false;
    };
    if (!stack_panel_parse_hex16(g_stack.add_limit_hex, &limit)) {
        g_stack.add_error = _("Invalid limit (hex)");
        return false;
    };
    if (base <= limit) {
        g_stack.add_error = _("Base must be greater than limit");
        return false;
    };

    if (dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue)) {
        g_stack.add_error = _("Queue full, try later");
        return false;
    };

    st_DBGAPI_STACK_REGIONS_ADD_PARAM p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, g_stack.add_name, sizeof(p.name) - 1);
    p.base           = base;
    p.limit          = limit;
    p.result_index   = -1;

    if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                    DBGAPI_CMD_STACK_REGIONS_ADD,
                                    &p, NULL, 50))
    {
        g_stack.add_error = _("Add failed (queue / timeout)");
        return false;
    };

    if (p.result_index < 0) {
        g_stack.add_error = _("Add failed (full / invalid name / duplicate)");
        return false;
    };

    /* Force refresh regionu + auto-select prave pridany. */
    g_stack.regions_valid = false;
    g_stack.selected_region_idx = p.result_index;
    return true;
}


/* ========================================================================= */
/*  V7 - Edit region modal: open + submit                                    */
/* ========================================================================= */

/**
 * @brief Predvyplni stav Edit modalu hodnotami z existujici cache.
 *
 * Volat pred ImGui::OpenPopup("###stack_edit_modal") z kliku na [E] tlacitko
 * v region card. Pre-fill bere data z g_stack.regions.regions[idx] (= LIST
 * snapshot z posledniho refreshe), nejde tedy primo do globalniho pole emu.
 *
 * @param idx  Index regionu v cache (0..regions.count-1).
 */
static void stack_panel_open_edit_modal(int idx)
{
    if (!g_stack.regions_valid) return;
    if (idx < 0 || idx >= g_stack.regions.count) return;

    const st_DBGAPI_STACK_REGION_INFO *r = &g_stack.regions.regions[idx];

    g_stack.edit_idx = idx;
    /* Bezpecna kopie jmena - zachovavame '\0' termination. */
    memset(g_stack.edit_name, 0, sizeof(g_stack.edit_name));
    strncpy(g_stack.edit_name, r->name, sizeof(g_stack.edit_name) - 1);

    snprintf(g_stack.edit_base_hex, sizeof(g_stack.edit_base_hex),
              "%04X", r->base);
    snprintf(g_stack.edit_limit_hex, sizeof(g_stack.edit_limit_hex),
              "%04X", r->limit);

    g_stack.edit_error = NULL;
    g_stack.edit_modal_open = true;
}


/**
 * @brief Posle dbgapi CMD_STACK_REGIONS_EDIT podle hodnot v Edit modalu.
 *
 * Predcasna validace v UI (= rychla zpetna vazba):
 *  - name nesmi byt prazdny, max 31 znaku
 *  - base / limit musi byt validni hex (max 4 znaky)
 *  - base > limit (= netrivialni region)
 *
 * Autoritativni validace (regex jmena, name duplicate, overlap detect)
 * probiha v stack_regions_edit v emu vlaknu. Pokud handler vrati success=0,
 * UI zobrazi genericky error (lokalne nevime, kterou podminku to porusilo).
 *
 * @return true pokud byl edit submitnut a uspesny (= modal se zavre),
 *         false pri validacni chybe nebo failnutim EDIT (= g_stack.edit_error
 *         obsahuje zpravu, modal zustane otevreny).
 */
static bool stack_panel_submit_edit(void)
{
    g_stack.edit_error = NULL;

    /* Validace indexu (modal mohl ztratit cilovy region pri refresh - rare). */
    if (g_stack.edit_idx < 0) {
        g_stack.edit_error = _("Invalid region index");
        return false;
    };

    /* Lokalni validace. */
    size_t name_len = strlen(g_stack.edit_name);
    if (name_len == 0) {
        g_stack.edit_error = _("Name is required");
        return false;
    };
    if (name_len >= STACK_REGION_NAME_MAX_UI) {
        g_stack.edit_error = _("Name is too long");
        return false;
    };

    uint16_t base = 0;
    uint16_t limit = 0;
    if (!stack_panel_parse_hex16(g_stack.edit_base_hex, &base)) {
        g_stack.edit_error = _("Invalid base (hex)");
        return false;
    };
    if (!stack_panel_parse_hex16(g_stack.edit_limit_hex, &limit)) {
        g_stack.edit_error = _("Invalid limit (hex)");
        return false;
    };
    if (base <= limit) {
        g_stack.edit_error = _("Base must be greater than limit");
        return false;
    };

    if (dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue)) {
        g_stack.edit_error = _("Queue full, try later");
        return false;
    };

    st_DBGAPI_STACK_REGIONS_EDIT_PARAM p;
    memset(&p, 0, sizeof(p));
    p.idx     = (uint8_t)g_stack.edit_idx;
    strncpy(p.name, g_stack.edit_name, sizeof(p.name) - 1);
    p.base    = base;
    p.limit   = limit;
    p.success = false;

    if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                    DBGAPI_CMD_STACK_REGIONS_EDIT,
                                    &p, NULL, 50))
    {
        g_stack.edit_error = _("Edit failed (queue / timeout)");
        return false;
    };

    if (!p.success) {
        /* Handler odmitnul - duvodu nevime presne (= invalid name regex,
         * duplicate name, overlap, ...). Zobrazime genericky error +
         * naznak ze overlap je pravdepodobny duvod. */
        g_stack.edit_error = _("Edit failed (invalid name / overlap / duplicate)");
        return false;
    };

    /* Force refresh regionu pristim frame (= nove hodnoty + reset stats). */
    g_stack.regions_valid = false;
    return true;
}


/* ========================================================================= */
/*  V2: SP history - enable + refresh                                        */
/* ========================================================================= */

/**
 * @brief Posle dbgapi CMD_STACK_HISTORY_ENABLE pres frontu.
 *
 * Race-safe vuci EMU vlaknu - flag g_stack_history_active se aktualizuje
 * v safepointu mezi instrukcemi. Self-rate-limit: pokud fronta plna,
 * UI checkbox se vrati do puvodniho stavu (= shadow zmenu rollbackneme).
 *
 * @param enable  true = zapnout recording, false = vypnout + reset bufferu.
 */
extern "C" void dbg_stack_panel_set_history_enabled(bool enable);

static void stack_panel_set_history_enabled(bool enable)
{
    if (dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue)) {
        /* Rollback UI shadow - prikaz se neposlal. */
        g_stack.history_enabled = !enable;
        return;
    };

    st_DBGAPI_STACK_HISTORY_ENABLE_PARAM p;
    p.enable = enable ? 1u : 0u;

    if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                    DBGAPI_CMD_STACK_HISTORY_ENABLE,
                                    &p, NULL, 50))
    {
        /* Timeout / queue refused - rollback. */
        g_stack.history_enabled = !enable;
        return;
    };

    /* Pri vypnuti vyclear UI cache (= ihned schova sparkline). */
    if (!enable) {
        g_stack.history_count = 0;
        g_stack.history_valid = true;
        g_stack.creep_warning = false;
        g_stack.creep_slope   = 0.0f;
        /* V2.1: invalidate vybrany sample - data uz neexistuji. */
        g_stack.selected_history_idx = -1;
    };
}


/**
 * @brief V9.1: verejny wrapper pro `stack_panel_set_history_enabled`.
 *
 * Externi okna (= Stack History okno) potrebuji menit history recording
 * stejnou cestou jako hlavni Stack Monitor checkbox. Wrapper deleguje
 * do interni helperu - zachovava rollback chovani pri full CMDRQ fronte
 * a vyclearovani cache pri vypnuti.
 *
 * @param enable  true = zapnout recording, false = vypnout + reset bufferu.
 *
 * @pre Volat z UI vlakna. g_stack.history_enabled shadow musi byt jiz
 *      nastaven na cilovou hodnotu pred volanim (= konvence pouzita
 *      v puvodnim ImGui::Checkbox + helper call patternu).
 */
extern "C" void dbg_stack_panel_set_history_enabled(bool enable)
{
    /* Set shadow na cilovou hodnotu (= konvence helperu). Pri full
     * fronte / timeoutu se shadow rollbackne uvnitr
     * stack_panel_set_history_enabled. */
    g_stack.history_enabled = enable;
    stack_panel_set_history_enabled(enable);
}


/**
 * @brief Refresh UI cache SP history snapshotu z emu.
 *
 * Vola dbgapi CMD_STACK_HISTORY_GET, kopiruje samples + count + slope.
 * Self-rate-limit + 50 ms timeout shoda s ostatnimi STACK_* refresh
 * funkcemi. Pri timeoutu zachova predchozi cache.
 *
 * Vyhodnoceni creep warningu: slope < K_CREEP_SLOPE_THRESHOLD AND
 * history obsahuje alespon slope window vzorku (jinak slope nestabilni).
 *
 * Side effects: aktualizuje g_stack.history_samples, history_count,
 *               history_valid, creep_slope, creep_warning, sparkline_buf.
 */
static void stack_panel_refresh_history(void)
{
    if (!g_stack.history_enabled) return;
    if (dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue)) return;

    st_DBGAPI_STACK_HISTORY_GET_PARAM p;
    p.max_count    = DBGAPI_STACK_HISTORY_MAX;
    p.slope_window = K_CREEP_SLOPE_WINDOW;
    p.count        = 0;
    p.active       = 0;
    p.slope        = 0.0f;
    p.samples      = g_stack.history_samples;

    if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                    DBGAPI_CMD_STACK_HISTORY_GET,
                                    &p, NULL, 50))
    {
        return;
    };

    g_stack.history_count = p.count;
    g_stack.history_valid = true;
    g_stack.creep_slope   = p.slope;

    /* Creep warning - jen pokud mame plne slope window, jinak je vypocet
     * nestabilni (= z 5 vzorku se da z nahodnych push/pop dovodit
     * cokoliv). */
    g_stack.creep_warning = ( p.count >= K_CREEP_SLOPE_WINDOW
                              && p.slope < K_CREEP_SLOPE_THRESHOLD );

    /* Naplnit float sparkline_buf z SP casti samples (= ImGui::PlotLines
     * potrebuje float pole). */
    for (uint32_t i = 0; i < p.count; i++) {
        g_stack.sparkline_buf[i] = (float)g_stack.history_samples[i].sp;
    };

    /* V2.1: ring buffer pretekl a stary vybrany sample uz neexistuje
     * (= history_count je sice stejny pri saturaci, ale lineární index
     * 0 ukazuje na novejsi data nez minule). Invalidace pri kazdem
     * refreshi - selected_history_idx je "live" jen do dalsiho ticku.
     * Uzivatel by si stejne vybral data ktera se hned posunou (sample
     * pri continual recordingu, ne pri pause). Pri stopnutem emu (=
     * refresh neprobiha) selection vydrzi. */
    if (g_stack.selected_history_idx >= (int)p.count) {
        g_stack.selected_history_idx = -1;
    };
}


/**
 * @brief Vrati pointer + count do sparkline_buf filtrovany na vzorky
 *        kde SP padl do <limit..base> daneho regionu.
 *
 * Pomocna funkce pro mini-sparkline per region - vytvori temporary
 * filtered float pole. Vraci pres out_buf/out_count.
 *
 * @param r          Region (NULL = no filter).
 * @param tmp_buf    Caller-poskytnuty docasny buffer (alokovany na zasobniku).
 * @param tmp_max    Velikost tmp_buf.
 * @return Pocet validnich hodnot v tmp_buf.
 */
static uint32_t stack_panel_filter_sparkline_for_region(
    const st_DBGAPI_STACK_REGION_INFO *r,
    float *tmp_buf, uint32_t tmp_max)
{
    if (!g_stack.history_valid || g_stack.history_count == 0) return 0;
    if (!r) return 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < g_stack.history_count && n < tmp_max; i++) {
        uint16_t sp = g_stack.history_samples[i].sp;
        if (sp >= r->limit && sp <= r->base) {
            tmp_buf[n++] = (float)sp;
        };
    };
    return n;
}


/* ========================================================================= */
/*  V3.1: Splitter helpers (vertikalni + horizontalni)                        */
/* ========================================================================= */

/* ========================================================================= */
/*  Render - sticky header                                                   */
/* ========================================================================= */

/**
 * @brief Vykresli sticky header: SP / Depth / Region dropdown / Reset W /
 *        Set BP from SP-256 / Add region.
 *
 * Layout (dva radky):
 *   Radek 1: SP: XXXXh   Depth: NN B   Region: [combo] [Reset W]
 *   Radek 2: [Set BP from SP-256]  [+ Add region from current SP]
 *
 * Depth = base - sp_now pokud SP padne do regionu (= "vybran region a
 * SP v nem"), jinak "--" jako v V0.
 */
static void stack_panel_render_header(void)
{
    const st_DBGAPI_STACK_REGION_INFO *sel = stack_panel_get_selected();

    /* Radek 1 - SP + Depth + Region dropdown + Reset W. */
    char sp_str[16];
    if (g_stack.buf_valid) {
        snprintf(sp_str, sizeof(sp_str), "%04Xh", g_stack.sp_now);
    } else {
        snprintf(sp_str, sizeof(sp_str), "----h");
    };

    char depth_str[16];
    if (sel && g_stack.buf_valid && g_stack.sp_now <= sel->base
        && g_stack.sp_now >= sel->limit)
    {
        int depth = (int)sel->base - (int)g_stack.sp_now;
        snprintf(depth_str, sizeof(depth_str), "%d B", depth);
    } else {
        snprintf(depth_str, sizeof(depth_str), "--");
    };

    ImGui::Text("SP: %s   Depth: %s   ", sp_str, depth_str);
    ImGui::SameLine();

    /* V11: "Region:" je tlacitko - klik otevre + focusne Stack Regions
     * samostatne okno (= V8 window). Predtim byl plain label - V11 cross-link
     * pro snazsi navigaci. SmallButton zachova kompaktni look sticky headeru. */
    if (ImGui::SmallButton(_L("Region:###stack_region_open"))) {
        if (g_gui) {
            g_gui->showStackRegionsWindow = true;
            stack_regions_window_request_focus();
        };
    };
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Open Stack Regions window"));
    };
    ImGui::SameLine();

    /* Region dropdown - combo seznam "(none)" + jmena regionu. */
    const char *preview = sel ? sel->name : _("(none)");
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("###stack_region_combo", preview)) {
        /* "(none)" entry. */
        bool none_sel = (g_stack.selected_region_idx == K_REGION_NONE);
        if (ImGui::Selectable(_L("(none)###stack_region_none"), none_sel)) {
            g_stack.selected_region_idx = K_REGION_NONE;
        };

        if (g_stack.regions_valid) {
            for (int i = 0; i < g_stack.regions.count; i++) {
                const st_DBGAPI_STACK_REGION_INFO *r = &g_stack.regions.regions[i];
                bool is_sel = (g_stack.selected_region_idx == i);
                /* Stable per-item ID: jmeno + index. */
                char label[64];
                snprintf(label, sizeof(label), "%s###stack_region_item_%d",
                          r->name, i);
                if (ImGui::Selectable(label, is_sel)) {
                    g_stack.selected_region_idx = i;
                };
            };
        };
        ImGui::EndCombo();
    };

    /* V11: Reset W presunut z radku 1 na radek 3 (= za Center SP,
     * right-aligned). Radek 1 nyni obsahuje jen SP/Depth/Region. */

    /* Radek 2 - Set BP. V8: "+ Add region" tlacitko presunuto do Stack
     * Regions samostatneho okna (= jediny entry point pro Add). */
    ImGui::BeginDisabled(!g_stack.buf_valid);
    if (ImGui::Button(_L("Set BP from SP-256###stack_set_bp"))) {
        stack_panel_set_bp_sp_minus_256();
    };
    ImGui::EndDisabled();

    /* Radek 3 (V2) - SP history toggle + Center SP tlacitko (V10) +
     * (volitelne) creep warning.
     *
     * SP history checkbox kontroluje shadow flag g_stack.history_enabled
     * (cache lokalniho UI stavu) a pri zmene posila dbgapi
     * STACK_HISTORY_ENABLE - emu vlakno teprve podle nej nastavi
     * g_stack_history_active. UI neceka na potvrzeni, jen self-rate-limit
     * pres frontu (= shoda s ostatnimi STACK_REGIONS_* commands).
     *
     * V10: Lock SP center toggle nahrazen tlacitkem "Center SP" - klik
     * jednorazove nastavi sp_lines_above na K_LINES_TOTAL/2 (= stred
     * tabulky). Slider vlevo od hex tabulky umoznuje libovolnou pozici. */
    bool hist_changed = ImGui::Checkbox(
        _L("SP history###stack_hist_toggle"), &g_stack.history_enabled);
    if (hist_changed) {
        stack_panel_set_history_enabled(g_stack.history_enabled);
    };
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
            _("Record SP changes for sparkline + stack creep detection"));
    };

    ImGui::SameLine();
    if (ImGui::Button(_L("Center SP###stack_center_sp"))) {
        g_stack.sp_lines_above = K_LINES_ABOVE_SP_CENTERED;
    };
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
            _("Reset SP marker to the middle row of the hex dump table"));
    };

    /* V11: Reset W tlacitko presunuto z radku 1 (= V10 vedle Region
     * dropdownu) na radek 3 za Center SP, right-aligned (= shoda s V9.1
     * [Stack History] btn patternem pres GetContentRegionAvail).
     *
     * Tlacitko je disabled kdyz neni vybran region (= shoda s V10 behavior). */
    {
        ImGui::SameLine();
        float btn_w = ImGui::CalcTextSize(_("Reset W")).x
                    + ImGui::GetStyle().FramePadding.x * 2.0f;
        float avail_w = ImGui::GetContentRegionAvail().x;
        if (avail_w > btn_w) {
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() + avail_w - btn_w);
        };
        ImGui::BeginDisabled(sel == NULL);
        if (ImGui::Button(_L("Reset W###stack_reset_w"))) {
            stack_panel_reset_watermark_selected();
        };
        ImGui::EndDisabled();
        if (sel != NULL && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                _("Reset watermark and counters for the selected region"));
        };
    };

    /* Stack creep warning - pokud history aktivni a slope < threshold,
     * zobraz alert. Threshold je empiricky, lze tunit (= viz konstanta
     * K_CREEP_SLOPE_THRESHOLD nize v render_history). */
    if (g_stack.history_enabled && g_stack.creep_warning) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s",
            _("[stack creep]"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s slope=%.4f",
                _("SP trending downward (linear regression):"),
                (double)g_stack.creep_slope);
        };
    };
}


/* ========================================================================= */
/*  Render - RMB popup pro Decode sloupec (V3.2)                             */
/* ========================================================================= */

/**
 * @brief Vykresli RMB popup pro klikatelnou Decode bunku.
 *
 * V3.3: rozsireno o "Save as bookmark" a "Create BP" - analog popup
 *       polozek z CPU panelu V1 ("Add to Watch" / "Add breakpoint at <reg>")
 *       a V0 stack-monitor "Set BP from SP-256".
 *
 * Polozky:
 *   - "Focus in Disassembly (main)" + "#2..#5" - 5x slot dle
 *     @c dbg_disasm_show_in_slot. Slot 0 je main DA, sloty 1..4 jsou
 *     extra okna Disassembly #2..#5.
 *   - Separator
 *   - "Copy target hex" - zkopiruje napr. "4080" do clipboardu (bez
 *     `h` suffixu = shoda s V3.1 styl tabulky).
 *
 * Cilova adresa je @c g_stack.decode_rmb_target, nastavena v
 * @ref stack_panel_render_table pri pravem kliku na Decode bunku. Popup
 * se otevre pres OpenPopup volane ze stejneho misa pres g_stack.decode_rmb_open.
 *
 * Stable ID popupu "###stack_decode_popup" pres tri hashe (= label se
 * nemeni, ale stable ID kvuli konzistenci s ostatnimi popupy).
 *
 * Side effects: pri kliknute polozce volame dbg_disasm_show_in_slot
 * resp. ImGui::SetClipboardText. Bez emu CMDRQ - nevyzaduje pauzu.
 */
static void stack_panel_render_decode_rmb_popup(void)
{
    /* OpenPopup pri otevrenem flagu - flag se nastavuje pri pravem kliku
     * v render_table. Volame OpenPopup tady (ne primo v table cell),
     * protoze ImGui popup state musi byt v predvidatelnem render scope. */
    if (g_stack.decode_rmb_open) {
        ImGui::OpenPopup("###stack_decode_popup");
        g_stack.decode_rmb_open = false;
    };

    if (!ImGui::BeginPopup("###stack_decode_popup")) {
        return;
    };

    uint16_t target = g_stack.decode_rmb_target;

    /* Focus in Disassembly #1..#5. Slot 0 = main DA okno (otevre hlavni
     * debug okno pokud zavrene), sloty 1..4 = extra Disassembly #2..#5. */
    for (int slot = 0; slot < 5; slot++) {
        char label[64];
        if (slot == 0) {
            snprintf(label, sizeof(label),
                      "%s", _("Focus in Disassembly (main)"));
        } else {
            snprintf(label, sizeof(label),
                      "%s #%d", _("Focus in Disassembly"), slot + 1);
        };
        if (ImGui::MenuItem(label)) {
            dbg_disasm_show_in_slot(slot, target);
        };
    };

    ImGui::Separator();

    /* Copy target hex - bez `h` suffixu (= shoda se stylem V3.1 tabulky). */
    {
        char label[64];
        snprintf(label, sizeof(label),
                  "%s ($%04X)", _("Copy target hex"), target);
        if (ImGui::MenuItem(label)) {
            char clip[8];
            snprintf(clip, sizeof(clip), "%04X", target);
            ImGui::SetClipboardText(clip);
        };
    };

    ImGui::Separator();

    /* V3.3: Save as bookmark - quick-add bez dialogu. user_input je hex
     * literal ve formatu "#XXXX" (= shoda s patternem z dbg_disassembled.cpp
     * "Add to bookmarks"). Comment je prazdny - user ho doplni v Bookmarks
     * okne. Po pridani Bookmarks okno otevreme (= visual feedback). */
    {
        char label[80];
        snprintf(label, sizeof(label),
                  "%s ($%04X)###stack_decode_save_bm",
                  _("Save as bookmark"), target);
        if (ImGui::MenuItem(label)) {
            char input_buf[16];
            snprintf(input_buf, sizeof(input_buf), "#%04X", target);
            bookmarks_add(input_buf, "");
            g_gui->showBookmarksWindow = true;
        };
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                _("Adds a bookmark pointing at the target address.\n"
                  "Comment is empty - edit it in the Bookmarks window."));
        };
    };

    /* V3.3: Create BP - vytvori execution breakpoint na cilove adrese.
     * Pouziva dbg_ui_bp_add helper (= race-safe vs EMU vlakno pres dbgapi
     * CMD_BP_ADD). Pokud na adrese uz BP existuje, dbg_ui_bp_add vrati
     * false (= no-op z pohledu UI). */
    {
        char label[80];
        snprintf(label, sizeof(label),
                  "%s ($%04X)###stack_decode_create_bp",
                  _("Create BP"), target);
        if (ImGui::MenuItem(label)) {
            int new_id = -1;
            if (dbg_ui_bp_add(target, &new_id)) {
                dbg_refresh_request();
            };
        };
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                _("Creates an execution breakpoint at the target address.\n"
                  "No-op if an existing BP occupies it."));
        };
    };

    ImGui::EndPopup();
}


/* ========================================================================= */
/*  Render - vertikalni SP slider (V10)                                      */
/* ========================================================================= */

/**
 * @brief Vykresli vertikalni slider pro pozici SP radku v hex tabulce.
 *
 * V10.1 - scroll-aware sync (Mozic A) + vetsi sipka:
 *
 *  - Track slideru odpovida VIDITELNE casti hex tabulky (= viewport, ne
 *    celym K_LINES_TOTAL radkum). Pozice sipky se pocita podle viditelneho
 *    rozsahu radku z g_stack.hex_scroll_y_last / hex_viewport_h_last
 *    snapshotu z minuleho framu.
 *  - Pokud SP radek je mimo viewport (nad nebo pod), sipka se umisti na
 *    horni resp. dolni hranu tracku jako signal "scrollni nahoru/dolu".
 *  - Drag slideru posune sp_lines_above v ramci viditeleho rozsahu
 *    radku a nastavi pending_scroll_to_sp tak, aby pristi render tabulky
 *    posunul SP radek do stredu viewportu (= sipka se vrati do tracku).
 *  - Sipka jezdce je vyrazne vetsi (= 20 px vyska, 22 px sirka slideru)
 *    pro lepsi viditelnost.
 *
 * Fallback: pokud hex_state_valid je false (= prvni frame pred prvnim
 * render tabulky), slider pouzije puvodni V10 chovani (track = celych
 * K_LINES_TOTAL radku v celku).
 *
 * @param slider_w Sirka slideru v px (= sirka prostoru pro sipku).
 * @param slider_h Vyska slideru v px (= shoda s vyskou hex tabulky).
 *
 * @note Funkce volana z hlavniho render layoutu pred hex tabulkou
 *       (BeginChild + SameLine). InvisibleButton spravuje hover/active
 *       sam, render dela DrawList z window background.
 *
 * @note Stable ImGui ID "###stack_sp_slider" - tri hashe pro stabilitu
 *       proti zmenam label/decoration.
 */
static void stack_panel_render_sp_slider(float slider_w, float slider_h)
{
    if (slider_w <= 0.0f || slider_h <= 0.0f) return;

    ImVec2 cursor    = ImGui::GetCursorScreenPos();

    /* V10.2: track_top_offset se pocita z presneho snapshotu hex tabulky -
     * (hex_data_top_y_last - cursor.y) = vyska header rowu + ImGui table
     * internal padding v aktualnim layoutu. Pri prvnim framu (snapshot
     * neni jeste validni) fallback na puvodni aproximaci pres
     * GetTextLineHeightWithSpacing (= V10.1 chovani, drobny off-by-1 pri
     * prvnim framu - po prvnim refreshi se sjednoti).
     *
     * Y align: slider track zacina presne pod headerem hex tabulky a kazdy
     * radek tracku odpovida row_step (= track_size.y / visible_count).
     * Sipka jezdce pak sedi v stredu radku obsahujiciho marker '>'. */
    float track_top_offset;
    if (g_stack.hex_state_valid && g_stack.hex_data_top_y_last > cursor.y) {
        track_top_offset = g_stack.hex_data_top_y_last - cursor.y;
    } else {
        const float row_h_imgui = ImGui::GetTextLineHeightWithSpacing();
        track_top_offset = row_h_imgui;
    };
    /* Bottom margin podobne jako padding pridany v render_table (+8). */
    const float track_bottom_margin = 8.0f;

    float track_h = slider_h - track_top_offset - track_bottom_margin;
    if (track_h < 10.0f) {
        /* Pri extremne nizkem okne shrink na minimum aby draw math
         * nemela nulovou/negativni vysku. */
        track_h = 10.0f;
    };

    ImVec2 track_min = ImVec2(cursor.x, cursor.y + track_top_offset);
    ImVec2 track_size = ImVec2(slider_w, track_h);

    /* Spocti viditelny rozsah radku v hex tabulce z minuleho framu.
     * Pokud snapshot neni jeste validni (= prvni frame), fallback na
     * fixed range 0..K_LINES_TOTAL (= puvodni V10 chovani). */
    int    visible_first = 0;
    int    visible_last  = K_LINES_TOTAL - 1;
    int    visible_count = K_LINES_TOTAL;
    float  data_row_h    = ImGui::GetTextLineHeightWithSpacing();
    bool   scroll_valid  = g_stack.hex_state_valid
                            && g_stack.hex_row_h_last > 0.0f
                            && g_stack.hex_viewport_h_last > 0.0f;

    if (scroll_valid) {
        data_row_h = g_stack.hex_row_h_last;
        /* Viditelny scroll viewport zacina pod headerem; ImGui ScrollY
         * je relativni k zacatku content (= prvni data row). */
        float scroll_y = g_stack.hex_scroll_y_last;
        /* Aproximace viewport vysky pro data rows (= window size minus
         * header row + frame padding); ImGui frozen header je vne ScrollY,
         * takze viewport pro data = window - header_h. */
        float vp_h = g_stack.hex_viewport_h_last - data_row_h;
        if (vp_h < data_row_h) vp_h = data_row_h;
        visible_first = (int)(scroll_y / data_row_h);
        int last_full = visible_first
                        + (int)((vp_h) / data_row_h) - 1;
        if (last_full < visible_first) last_full = visible_first;
        if (visible_first < 0) visible_first = 0;
        if (last_full > K_LINES_TOTAL - 1) last_full = K_LINES_TOTAL - 1;
        visible_last  = last_full;
        visible_count = visible_last - visible_first + 1;
        if (visible_count < 1) visible_count = 1;
    };

    /* InvisibleButton zarezervuje misto v layoutu na celou vysku slider_h
     * (= aby SameLine s tabulkou mel stejnou referencni vysku). Hit area
     * je v cele rade slider_w x slider_h - drag math pak prepocte mouse Y
     * vuci track_min/track_h. */
    ImGui::InvisibleButton("###stack_sp_slider", ImVec2(slider_w, slider_h),
                            ImGuiButtonFlags_MouseButtonLeft);

    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    /* Drag handling - prepocet mouse Y na row index v ramci viditeleho
     * rozsahu. Krome zmeny sp_lines_above zadame pending_scroll_to_sp
     * (= tabulka v aktualnim framu scrollne na novy SP). Tim slider
     * funguje jako "vyber SP pozice + scroll", ne jako "vyber + necham
     * mimo viewport". */
    if (active) {
        float mouse_y = ImGui::GetIO().MousePos.y;
        float rel = (mouse_y - track_min.y) / track_size.y;
        if (rel < 0.0f) rel = 0.0f;
        if (rel > 1.0f) rel = 1.0f;
        int new_idx;
        if (scroll_valid) {
            /* Mapuj rel na viditeleny rozsah. Pri scroll-mode drag voli
             * SP v tom co user vidi. */
            new_idx = visible_first
                      + (int)(rel * (float)(visible_count - 1) + 0.5f);
        } else {
            new_idx = (int)(rel * (float)(K_LINES_TOTAL - 1) + 0.5f);
        };
        if (new_idx < 0) new_idx = 0;
        if (new_idx > K_LINES_TOTAL - 1) new_idx = K_LINES_TOTAL - 1;
        if (new_idx != g_stack.sp_lines_above) {
            g_stack.sp_lines_above   = new_idx;
            /* Po drag taky zarid, aby SP radek byl viditelny (= scroll
             * na SP). Pokud uz je viditelny, SetScrollY centruje v pristi
             * frame, coz je OK UX (= drag = "podivam se na to tady"). */
            g_stack.pending_scroll_to_sp = true;
        };
    };

    /* Render pres DrawList - track ramecek (subtle) + sipka. */
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Track background subtle: tenky ramecek (= vizualni naznak ze sloupec
     * ma vyznam). Plne tonujeme jen pri hover/active. */
    ImU32 track_col = (hovered || active)
                       ? IM_COL32(120, 120, 120, 180)
                       : IM_COL32(80, 80, 80, 100);
    dl->AddRect(track_min,
                 ImVec2(track_min.x + slider_w,
                         track_min.y + track_size.y),
                 track_col, 2.0f);

    /* Y pozice sipky:
     *   - Pri scroll_valid + SP v ramci viditeleho rozsahu: pozice
     *     odpovida viditelnemu offset SP (= visible_offset / visible_count).
     *   - Pri SP < visible_first: sipka na top edge tracku (= "scrollni
     *     nahoru, SP je vyse").
     *   - Pri SP > visible_last: sipka na bottom edge tracku.
     *   - Fallback bez scroll_valid: puvodni V10 (= idx / K_LINES_TOTAL). */
    int idx = stack_panel_lines_above();
    float arrow_y;
    bool sp_off_top    = false;
    bool sp_off_bottom = false;

    if (scroll_valid) {
        if (idx < visible_first) {
            arrow_y = track_min.y + 1.0f;  /* maly inset od horni hrany */
            sp_off_top = true;
        } else if (idx > visible_last) {
            arrow_y = track_min.y + track_size.y - 1.0f;
            sp_off_bottom = true;
        } else {
            /* V10.2: row_step = skutecna vyska radku v hex tabulce
             * (= data_row_h z snapshotu). Drive (V10.1) byl pouzity
             * track_size.y / visible_count, coz NEodpovida realne pixelove
             * vysce radku - vznikalo systematicke pozicni offset, sipka byla
             * o 1 radek niz vuci SP markeru '>'. Snapshot data_row_h je
             * synced 1:1 s vyskou data row v BeginTable. */
            int visible_offset = idx - visible_first;
            float row_step = data_row_h;
            arrow_y = track_min.y
                       + ((float)visible_offset + 0.5f) * row_step;
        };
    } else {
        float row_step = track_size.y / (float)K_LINES_TOTAL;
        arrow_y = track_min.y + ((float)idx + 0.5f) * row_step;
    };

    /* V10.1: vetsi sipka. Vyska polovicni = 10 px (=> total 20 px).
     * Slider sirka je 22 px (volana funkce predava slider_w), takze
     * sipka ma width-to-height aspect ~ 1:1 (= klasicky bold pointer). */
    constexpr float ARROW_HALF_HEIGHT = 10.0f;
    float arrow_half_h = ARROW_HALF_HEIGHT;
    /* Pri "off-edge" rezimu posun sipku do interioru tracku aby nebyla
     * orezana - tip ma byt vne hrany, base uvnitr. */
    if (sp_off_top) {
        arrow_y = track_min.y + arrow_half_h + 1.0f;
    };
    if (sp_off_bottom) {
        arrow_y = track_min.y + track_size.y - arrow_half_h - 1.0f;
    };

    float tip_x  = track_min.x + slider_w - 2.0f;
    float base_x = track_min.x + 1.0f;

    ImVec2 a = ImVec2(base_x, arrow_y - arrow_half_h);
    ImVec2 b = ImVec2(tip_x,  arrow_y);
    ImVec2 c = ImVec2(base_x, arrow_y + arrow_half_h);

    /* Barva šipky:
     *   - default zluta, jasnejsi pri hover/active
     *   - pri "off-edge" rezimu cervena (= "SP mimo viewport, scrollni") */
    ImU32 arrow_col;
    if (sp_off_top || sp_off_bottom) {
        arrow_col = (hovered || active)
                     ? IM_COL32(255, 90, 60, 255)
                     : IM_COL32(220, 70, 50, 255);
    } else {
        arrow_col = (hovered || active)
                     ? IM_COL32(255, 235, 60, 255)
                     : IM_COL32(220, 190, 40, 255);
    };
    dl->AddTriangleFilled(a, b, c, arrow_col);

    /* Pri hover zobrazime tooltip s aktualni hodnotou + info o viewport
     * state - pomocnik pro UX (= uzivatel vidi presne ktery radek a zda
     * je SP viditelne). */
    if (hovered) {
        if (sp_off_top) {
            ImGui::SetTooltip("%s %d / %d\n%s",
                               _("SP row:"), idx, K_LINES_TOTAL - 1,
                               _("(SP is above viewport)"));
        } else if (sp_off_bottom) {
            ImGui::SetTooltip("%s %d / %d\n%s",
                               _("SP row:"), idx, K_LINES_TOTAL - 1,
                               _("(SP is below viewport)"));
        } else {
            ImGui::SetTooltip("%s %d / %d",
                               _("SP row:"), idx, K_LINES_TOTAL - 1);
        };
    };
}


/* ========================================================================= */
/*  Render - hex dump tabulka                                                */
/* ========================================================================= */

/**
 * @brief Vykresli hex dump tabulku 40 radku kolem SP.
 *
 * Sloupce: Addr, Byte, Word, Decode, Marker.
 * Marker: `>` na radku odpovidajicim sp_now, `=` na radku odpovidajicim
 * watermark vybraneho regionu. Pokud SP == watermark, ma `>` prednost.
 *
 * Default zobrazeni: word-oriented (= 2 B per radek). Pri lichem SP
 * fallback na byte-oriented (= 1 B per radek), Word sloupec prazdny.
 *
 * @param table_h Vyska tabulky v pixelech. Pokud <= 0.0f, pouzije se
 *                idealni vyska (= vsech K_LINES_TOTAL+1 radku se vejde).
 *                Pokud je vyska mensi, tabulka aktivuje vlastni ScrollY
 *                a uzivatel scrolluje uvnitr. V8.4: hlavni okno predava
 *                dynamicky vypocet pres avail_y - sparkline_h, aby se
 *                sparkline section nikdy neorezala.
 */
static void stack_panel_render_table(float table_h)
{
    if (!g_stack.buf_valid) {
        ImGui::TextDisabled("%s", _("(no data)"));
        return;
    };

    /* Word-oriented default, byte-oriented fallback pro liche SP. */
    bool word_mode = (g_stack.sp_odd == 0);

    /* Watermark adresa vybraneho regionu (pokud existuje). */
    const st_DBGAPI_STACK_REGION_INFO *sel = stack_panel_get_selected();
    bool have_watermark = (sel != NULL);
    uint16_t watermark = sel ? sel->watermark : 0;

    ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerV
                            | ImGuiTableFlags_RowBg
                            | ImGuiTableFlags_ScrollY;

    float row_h = ImGui::GetTextLineHeightWithSpacing();
    /* V8.4: pokud caller nepredal vysku (<=0), pouzijeme "idealni"
     * vysku pro vsech K_LINES_TOTAL radku + header. Caller hlavniho
     * okna nyni predava dynamickou vysku, ale Stack Regions nebo
     * jine pripadne re-use site mohou nadale volat s 0.0f. */
    if (table_h <= 0.0f) {
        table_h = row_h * (K_LINES_TOTAL + 1) + 8.0f;
    };

    if (!ImGui::BeginTable("###stack_table", 5, tflags,
                            ImVec2(0.0f, table_h)))
    {
        return;
    };

    /* Sirky sloupcu spoctene dynamicky z aktualniho fontu pres CalcTextSize
     * + ImGui FramePadding (= padding pres cell border). Self-adapting na
     * font-size zmeny.
     *
     * Vzorky pro CalcTextSize:
     *   Addr   = "FFFF"            (4 hex znaky, po V3.1 bez `h`)
     *   Byte   = "FF FF"           (LSB MSB se spacem)
     *   Word   = "FFFF"            (4 hex znaky)
     *   Decode = WidthStretch      (zbytek - decoded "[ret CALL cc XXXX]"
     *                               se vejde do typicke sirky)
     *   M      = ">"               (jeden znak - SP marker, nejdelsi tag)
     */
    const float cell_pad = ImGui::GetStyle().CellPadding.x * 2.0f;
    const float w_addr   = ImGui::CalcTextSize("FFFF").x  + cell_pad;
    const float w_byte   = ImGui::CalcTextSize("FF FF").x + cell_pad;
    const float w_word   = ImGui::CalcTextSize("FFFF").x  + cell_pad;
    const float w_mark   = ImGui::CalcTextSize(">").x     + cell_pad * 2.0f;
    ImGui::TableSetupColumn(_L("Addr###stack_col_addr"),
                            ImGuiTableColumnFlags_WidthFixed, w_addr);
    ImGui::TableSetupColumn(_L("Byte###stack_col_byte"),
                            ImGuiTableColumnFlags_WidthFixed, w_byte);
    ImGui::TableSetupColumn(_L("Word###stack_col_word"),
                            ImGuiTableColumnFlags_WidthFixed, w_word);
    ImGui::TableSetupColumn(_L("Decode###stack_col_decode"),
                            ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(_L("M###stack_col_marker"),
                            ImGuiTableColumnFlags_WidthFixed, w_mark);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    /* V10.2: snapshot presneho screen-Y bodu, kde zacina prvni data row
     * (= sracela na header row + ImGui table internal padding). Sliderem
     * pouzity v dalsim framu pro presny track_top_offset (= shodne se
     * skutecnou pozici radku v tabulce). */
    g_stack.hex_data_top_y_last = ImGui::GetCursorScreenPos().y;

    /* V10.1: pokud slider drag pozadal o auto-scroll na SP radek, provedeme
     * to pred enumeraci radku. Cilovy scroll Y = (sp_lines_above) * row_h
     * tak, aby SP radek byl pribline ve stredu viewportu (= row_h * idx -
     * (viewport_h - row_h) / 2). Clamping ImGui udela sam. */
    if (g_stack.pending_scroll_to_sp && g_stack.hex_state_valid) {
        int idx = stack_panel_lines_above();
        float row_h_local = g_stack.hex_row_h_last;
        float vp_h        = g_stack.hex_viewport_h_last;
        float target_y    = ((float)idx + 0.5f) * row_h_local
                            - vp_h * 0.5f;
        if (target_y < 0.0f) target_y = 0.0f;
        ImGui::SetScrollY(target_y);
        g_stack.pending_scroll_to_sp = false;
    };

    for (int i = 0; i < K_LINES_TOTAL; i++) {
        int step = word_mode ? 2 : 1;
        uint16_t addr = (uint16_t)(g_stack.base_addr - (uint32_t)i * step);

        int off = i * step;
        if (off + (step - 1) >= K_BUF_SIZE) {
            break;
        };

        /* DESC buf semantika z dbgapi.c handleru (V1.2):
         *   buf[k] = mem[base - k], kde base = p->addr = g_stack.base_addr.
         * Pro radek `i` v DESC zobrazeni je `addr = base - i*step`,
         * tj. `addr = base - off` (off = i*step), takze:
         *   b0 = mem[addr]     = buf[off]       (LSB v LE-word na adrese addr)
         *   b1 = mem[addr + 1] = buf[off - 1]   (MSB v LE-word na adrese addr)
         * Pro off == 0 (= prvni radek, addr == base) neexistuje MSB v bufu
         * (mem[base+1] handler nenacetl) - fallback na 0. Pri default
         * K_LINES_ABOVE_SP = 32 je off == 0 daleko nad SP, takze Word sloupec
         * na tom hornim radku ma kosmeticky vyznam. */
        uint8_t b0 = g_stack.buf[off];
        uint8_t b1 = (word_mode && off > 0) ? g_stack.buf[off - 1] : 0;

        ImGui::TableNextRow();

        /* V12: per-řádek background color podle příslušnosti k regionu.
         * Aktivní region (= vybraný v dropdownu "Region:") má sytější
         * modrou, ostatní regiony tlumenější. Mimo všechny regiony bez
         * highlight (= default RowBg). RowBg0 je primary row layer pod
         * CellBg vrstvou marker sloupce - SP marker `>` / watermark `=`
         * tedy zůstává čitelný. */
        int region_idx = stack_panel_region_idx_for_addr(addr);
        if (region_idx >= 0) {
            ImU32 bg;
            if (region_idx == g_stack.selected_region_idx) {
                bg = IM_COL32(60, 100, 160, 80);   /* aktivní region */
            } else {
                bg = IM_COL32(60, 100, 160, 40);   /* jiný region */
            };
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg);
        };

        /* Addr - hex bez `h` suffixu (V3.1: v tabulkach jen hex, suffix
         * je vizualni rusivka). */
        ImGui::TableNextColumn();
        ImGui::Text("%04X", addr);

        /* Byte - v word_mode dva bajty od nizsi adresy k vyssi (LSB MSB). */
        ImGui::TableNextColumn();
        if (word_mode) {
            ImGui::Text("%02X %02X", b0, b1);
        } else {
            ImGui::Text("%02X", b0);
        };

        /* Word - jen v word_mode. LE word na adrese addr = (MSB << 8) | LSB.
         * V3.1: bez `h` suffixu.
         *
         * V11: double-click na buňku otevre 16-bit hex InputText edit
         * (Enter = commit MEM_WRITE, Esc/klik mimo = cancel). Pri double-click
         * se emu pauzne pres dbg_ui_pause() pokud bezi - edit jinak by sel
         * proti pohyblivemu cili (= stack se neustale meni vlaknem emu).
         *
         * V byte-mode (lichy SP) zustava placeholder "--" NEklikatelny -
         * 16-bit word edit nema definovany byte order pri offset = 1B. */
        ImGui::TableNextColumn();
        if (word_mode) {
            uint16_t w = (uint16_t)(((uint16_t)b1 << 8) | b0);

            /* V13: zlatý fade Word cell po memory write. Trvá K_WORD_FADE_SEC
             * od change_time, alpha klesá lineárně 200 -> 0. CellBg vrstva
             * je nad RowBg0 (= V12 region tint), takže fade překryje region
             * background pouze v Word sloupci po dobu efektu. Word sloupec
             * = index 2 (Addr=0, Byte=1, Word=2). */
            if (g_stack.prev_valid && g_stack.change_time[i] > 0.0) {
                double age = ImGui::GetTime() - g_stack.change_time[i];
                if (age >= 0.0 && age < K_WORD_FADE_SEC) {
                    float t = 1.0f - (float)(age / K_WORD_FADE_SEC);
                    int alpha = (int)(t * 200.0f);
                    if (alpha < 0) alpha = 0;
                    if (alpha > 200) alpha = 200;
                    ImU32 fade = IM_COL32(255, 200, 60, alpha);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, fade, 2);
                };
            };

            if (g_stack.word_edit_row == i) {
                /* Edit mode: InputText s focus, EnterReturnsTrue +
                 * CharsHexadecimal pro restrikci na hex znaky. */
                if (g_stack.word_edit_just_opened) {
                    snprintf(g_stack.word_edit_buf,
                              sizeof(g_stack.word_edit_buf),
                              "%04X", g_stack.word_edit_orig);
                    ImGui::SetKeyboardFocusHere();
                    g_stack.word_edit_just_opened = false;
                };

                ImGui::SetNextItemWidth(-1.0f);
                ImGuiInputTextFlags eflags =
                      ImGuiInputTextFlags_EnterReturnsTrue
                    | ImGuiInputTextFlags_CharsHexadecimal
                    | ImGuiInputTextFlags_AutoSelectAll;
                bool commit = ImGui::InputText("###stack_word_edit",
                                                g_stack.word_edit_buf,
                                                sizeof(g_stack.word_edit_buf),
                                                eflags);

                /* Escape detect: ImGui pri Escape pri editaci vyrobi
                 * Deactivated bez DeactivatedAfterEdit (= cancel cesta).
                 * Klik mimo InputText spadne do stejne vetve - take cancel. */
                bool deactivated = ImGui::IsItemDeactivated();
                bool deactivated_edit = ImGui::IsItemDeactivatedAfterEdit();

                if (commit) {
                    /* Parse hex retezce, MEM_WRITE 2 byte LE. */
                    unsigned new_w = 0;
                    if (sscanf(g_stack.word_edit_buf, "%x", &new_w) == 1) {
                        uint8_t bytes[2];
                        bytes[0] = (uint8_t)(new_w & 0xFF);          /* LSB */
                        bytes[1] = (uint8_t)((new_w >> 8) & 0xFF);   /* MSB */
                        dbg_ui_mem_write(g_stack.word_edit_addr, bytes, 2);
                    };
                    g_stack.word_edit_row = -1;
                } else if (deactivated && !deactivated_edit) {
                    /* Cancel (Esc nebo klik mimo bez Enter). */
                    g_stack.word_edit_row = -1;
                };
            } else {
                /* Display mode: Selectable s AllowDoubleClick. Tooltip jen
                 * pri hover, neorezat behavior ostatnich sloupcu (Decode
                 * IsItemClicked pro RMB ma sve vlastni Selectable nizko).
                 *
                 * V11.1: stable ID suffix "###stack_word_<i>" - bez nej ma
                 * vice radku se shodnou Word hodnotou (typicky 0000 na
                 * prazdnem stacku) identicke ImGui ID = "9 visible items
                 * with conflicting ID" DEBUG dialog. Per-row index i je
                 * jedinecny v ramci tabulky. */
                char wlabel[32];
                snprintf(wlabel, sizeof(wlabel), "%04X###stack_word_%d", w, i);
                ImGui::Selectable(wlabel, false,
                                   ImGuiSelectableFlags_AllowDoubleClick);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", _("Double-click to edit"));
                };
                if (ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    /* Pauza emu pri zahajeni editace, jinak by se hodnota
                     * mohla pod uzivateli zmenit (stack vlakno emu meni). */
                    if (!EMULATOR_TEST_PAUSED) {
                        dbg_ui_pause();
                    };
                    g_stack.word_edit_row         = i;
                    g_stack.word_edit_addr        = addr;
                    g_stack.word_edit_orig        = w;
                    g_stack.word_edit_just_opened = true;
                };
            };
        } else {
            ImGui::TextDisabled("--");
        };

        /* Decode (V3) - disasm-back heuristika. Pro slot kde word kandidat
         * vypada jako navratova adresa od CALL/RST instrukce zobrazi
         * "[ret CALL XXXXh]" / "[ret RST XXh]". Jinak prazdne.
         *
         * Platí jen ve word-mode (= sudy SP). Pro lichy SP (byte-mode)
         * word kandidat neni definovany - decode_valid je false a sloupec
         * zobrazi placeholder "--".
         *
         * Index do decode_buf je stejny jako index radku `i` v tabulce,
         * decode_buf[i] odpovida adrese base_addr - i*2 = `addr`. */
        ImGui::TableNextColumn();
        if (word_mode && g_stack.decode_valid && i < K_LINES_TOTAL) {
            const st_DBGAPI_STACK_DECODE_INFO *d = &g_stack.decode_buf[i];

            if (d->type != DBGAPI_STACK_DECODE_NONE) {
                /* V3.2: klikatelny decode - LMB = Focus DA primary, RMB =
                 * popup menu s vyberem slotu + Copy.
                 *
                 * Selectable s textem labelu + stable ID "###stack_decode_<i>"
                 * zaruci stabilni ImGui ID i kdyz se hodnota target meni
                 * mezi refresh ticky (memory feedback_imgui_window_id_three_hashes).
                 *
                 * SpanAllColumns flag NE - klikatelna je jen Decode bunka,
                 * ostatni sloupce (Addr/Byte/Word/Marker) zustavaji ne-klikatelne. */
                char label[64];
                switch (d->type) {
                    case DBGAPI_STACK_DECODE_CALL:
                        snprintf(label, sizeof(label),
                                  "[ret CALL %04X]###stack_decode_%d",
                                  d->target, i);
                        break;
                    case DBGAPI_STACK_DECODE_CALL_CC:
                        snprintf(label, sizeof(label),
                                  "[ret CALL cc %04X]###stack_decode_%d",
                                  d->target, i);
                        break;
                    case DBGAPI_STACK_DECODE_RST:
                        snprintf(label, sizeof(label),
                                  "[ret RST %02X]###stack_decode_%d",
                                  d->target, i);
                        break;
                    default:
                        /* Unreachable - NONE odchyceno v else vetvi nad. */
                        snprintf(label, sizeof(label),
                                  "--###stack_decode_%d", i);
                        break;
                };

                /* LMB klik = focus primary DA na target adresu. NEpauzuje
                 * emulator (= navigacni akce shodne s Bookmarks/CPU panel `>`). */
                if (ImGui::Selectable(label, false, 0)) {
                    dbg_disasm_show_in_slot(0, d->target);
                };

                /* RMB klik = otevre popup s Focus DA #1-5 + Copy. Cilovou
                 * adresu si ulozime do g_stack.decode_rmb_target a flag
                 * decode_rmb_open zajisti OpenPopup ve specializovanem
                 * helperu (= mimo aktualni table scope kvuli ImGui poppup
                 * state managementu). */
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    g_stack.decode_rmb_target = d->target;
                    g_stack.decode_rmb_open = true;
                };

                /* Tooltip on hover - opcode, target + hint "click to focus".
                 * V3.2: pridana radka s click hint. V3.1 mela jen heuristic
                 * info. Tooltip se aktivuje az po prodleve (= ImGui default
                 * delay), takze nerusi pri rychlem scrollovani. */
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        _("Possible return address (heuristic).\n"
                          "Opcode at W-%d: %02X\n"
                          "Call target: %04X\n"
                          "\n"
                          "Click: focus primary Disassembly.\n"
                          "Right-click: choose slot or copy."),
                        (d->type == DBGAPI_STACK_DECODE_RST) ? 1 : 3,
                        d->opcode, d->target);
                };
            } else {
                /* DECODE_NONE = sloupec NEklikatelny (= jen vizualni
                 * placeholder, target neexistuje). */
                ImGui::TextDisabled("--");
            };
        } else {
            /* Cely buf invalid nebo lichy SP (byte-mode) = NEklikatelne. */
            ImGui::TextDisabled("--");
        };

        /* Marker - SP marker `>` ma prednost pred watermark markerem `=`.
         * V byte-mode taky exact match (= addr == sp_now / watermark). */
        ImGui::TableNextColumn();
        if (addr == g_stack.sp_now) {
            ImGui::TextUnformatted(">");
        } else if (have_watermark && addr == watermark) {
            ImGui::TextUnformatted("=");
        };
    };

    /* V10.1: snapshot scroll stavu hex tabulky pro pristi frame slideru.
     * GetScrollY/MaxY/WindowSize jsou ve scope tabulky validni (ScrollY
     * flag vytvori scroll region). Slider tyto hodnoty pouzije pro sync
     * pozice sipky s viditelnou casti tabulky (1-frame lag, UX-invisible). */
    g_stack.hex_scroll_y_last    = ImGui::GetScrollY();
    g_stack.hex_scroll_max_last  = ImGui::GetScrollMaxY();
    g_stack.hex_viewport_h_last  = ImGui::GetWindowSize().y;
    g_stack.hex_row_h_last       = row_h;
    g_stack.hex_state_valid      = true;

    ImGui::EndTable();

    /* V3.2: render RMB popup pro Decode bunky. Volame az po EndTable -
     * popup musi byt mimo table scope kvuli ImGui state managementu
     * (BeginPopup uvnitr BeginTable cell zpusobuje ID konflikty). */
    stack_panel_render_decode_rmb_popup();
}


/* ========================================================================= */
/*  Render - Hlavni SP history sparkline (V2)                                */
/* ========================================================================= */

/**
 * @brief Vykresli hlavni sparkline panel cele SP history pod hex tabulkou.
 *
 * V2.1: vlastni implementace pres ImDrawList misto ImGui::PlotLines.
 *
 * Vlastnosti V2.1:
 *  - per-sample hover tooltip (= SP + cycles + index)
 *  - vertikalni crosshair na pozici hover sample
 *  - vertikalni markery push/pop/other events nad sparkline (toggle "Show events")
 *  - klikatelny sample (LMB = vyber + zluty crosshair + info text)
 *
 * Layout:
 *
 *    [marker strip 8 px]       <- nad polyline, vertikalni cary udalosti
 *    [polyline plot 60 px]     <- vlastni sparkline + hover/select crosshair
 *    Slope: ...    Selected: ... [Show events]
 *
 * Pri history_count <= 2 * rect_w (= husta data, ale jeste rozliseno na px):
 *  - vsechny markery: cervena (PUSH delta=-2), zelena (POP delta=+2),
 *    zluta (other delta).
 * Pri history_count > 2 * rect_w (= zhusteno):
 *  - jen "other" markery (LD SP,X / INT vector dispatch) jsou viditelne;
 *    push/pop by zaplnily strip do souvisle barvy a unesly informacni
 *    hodnotu = filtrujeme je out.
 *
 * Hover detection: hit area = InvisibleButton pres rect polyline. Vypocet
 * nejblizsiho indexu = floor((mouse_x - rect_min.x) / px_per_sample).
 *
 * Klik LMB v hit area: vybere sample (toggle - druhy klik na stejny =
 * deselect). Vyber se invaliduje pri novem refreshi pokud index > count.
 *
 * Side effects: pisi do g_stack.selected_history_idx pri kliku.
 *               Data ctena z g_stack.history_samples + history_count.
 */
static void stack_panel_render_history_sparkline(void)
{
    if (!g_stack.history_enabled) return;

    /* V8.5 bod 1: auto-expand pri transition history_enabled false->true.
     * Detekce probiha v dbg_stack_panel_render() (= zde uz vidime stav
     * po transitionu); pred volanim CollapsingHeader nastavuje
     * SetNextItemOpen(true, Always) pres g_stack.history_prev_enabled.
     * Vlastni transition handling je v dbg_stack_panel_render() vyse v
     * cele se navic snazi nastavit g_stack.history_header_open = true,
     * aby uz tento frame mela sparkline section spravnou rezervovanou
     * vysku (nebyla shrink na header_h s opozdenim 1 frame). */

    const bool open = ImGui::CollapsingHeader(
        _L("SP history###stack_history_header"),
        ImGuiTreeNodeFlags_DefaultOpen);

    /* V8.5 bod 2: shadow stav pro dalsi frame - dbg_stack_panel_render()
     * podle nej rozhoduje, zda rezervovat plnou vysku sparkline section
     * nebo jen header_h (= hex tabulka pak vyplni zbyly prostor). */
    g_stack.history_header_open = open;

    if (!open) {
        return;
    };

    if (!g_stack.history_valid || g_stack.history_count == 0) {
        ImGui::TextDisabled("%s", _("(no samples yet, recording...)"));
        return;
    };

    /* V9: Vlastni plot + info delegovany na shared helpery, aby je
     * Stack History okno (= V9 samostatny window) mohlo pouzit beze
     * zmeny chovani. Suffix "main" = unikatni ImGui ID pro Clear tlacitko
     * Selected section, aby nedoslo ke kolizi s identickym tlacitkem v
     * Stack History okne (suffix "win"). */
    float avail_w = ImGui::GetContentRegionAvail().x;
    if (avail_w < 80.0f) avail_w = 80.0f;

    /* Hlavni Stack Monitor: pevna vyska sparkline section (60 plot + 15
     * marker + 2 gap = 77 px). V9.1: marker strip zvetsen z 8 -> 15 px
     * kvuli viditelnosti event markeru. Stack History samostatne okno
     * renderuje vysku dynamicky podle velikosti okna - viz
     * stack_history_window_render. */
    const float plot_total_h = 15.0f + 2.0f + 60.0f;
    dbg_stack_panel_render_history_plot(avail_w, plot_total_h);
    /* V9.1: hlavni okno predava show_open_history_btn = true (= info
     * radek zobrazi vpravo [Stack History] tlacitko, ktere otevre +
     * focusne samostatne Stack History okno). Stack History samotne
     * okno tlacitko nepotrebuje (= V9.1 top action row jiz dostavi
     * [Stack Monitor] btn). */
    dbg_stack_panel_render_history_info("main", true);
}


/* ========================================================================= */
/*  V9 - Public render helpery pro SP history sparkline                      */
/* ========================================================================= */

/**
 * @brief V9: Sdileny render helper pro vlastni plot SP history sparkline.
 *
 * Implementace prevzata z V2.1 stack_panel_render_history_sparkline (= jen
 * cast vykreslujici plot area, bez CollapsingHeader, bez info textu, bez
 * Selected/Show events). Klik a hover pisi do globalniho g_stack stavu,
 * takze klik v jednom okne (hlavni Stack Monitor) se projevi v druhem
 * (Stack History samostatne okno).
 *
 * Layout v ramci predane plochy:
 *   [marker strip 8 px]                  <- vertikalni cary udalosti
 *   [gap 2 px]
 *   [polyline plot zbyla vyska]           <- vlastni sparkline + crosshair
 *
 * Marker strip + gap je fixni (= 10 px), polyline plot je `height - 10`.
 * Pri height < 30 px je polyline plot orezan na minimum 20 px (= total >= 30).
 *
 * Klik LMB = toggle vyberu sample (= second click na stejny = deselect).
 */
extern "C" void dbg_stack_panel_render_history_plot(float width, float height)
{
    if (!g_stack.history_enabled) return;
    if (!g_stack.history_valid || g_stack.history_count == 0) {
        ImGui::TextDisabled("%s", _("(no samples yet, recording...)"));
        return;
    };

    /* Defensivni clamp parametru. */
    if (width < 80.0f) width = 80.0f;
    if (height < 30.0f) height = 30.0f;

    const uint32_t count = g_stack.history_count;

    /* Marker strip + gap (V9.1: zvyseno z 8 -> 15 px pro viditelnost
     * markeru na prvni pohled, bez nutnosti zoomovat). Polyline plot
     * dostane zbyly prostor. */
    const float marker_h = 15.0f;
    const float gap_h    = 2.0f;
    const float plot_h   = height - marker_h - gap_h;

    ImVec2 area_min = ImGui::GetCursorScreenPos();
    ImVec2 area_size(width, height);

    /* Stable ID pro InvisibleButton - dva renderery (hlavni + history
     * window) ho volaji ze stejneho framu, ale v jinych ImGui::Begin
     * scope, takze stejne ID je OK (ImGui ID stack je per-window). */
    ImGui::InvisibleButton("###stack_history_hit", area_size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Plot rect (= rectangle pro polyline, mimo marker strip). */
    const ImVec2 plot_min(area_min.x, area_min.y + marker_h + gap_h);
    const ImVec2 plot_max(area_min.x + width, plot_min.y + plot_h);

    /* Marker strip rect (= nad plotem). */
    const ImVec2 mark_min(area_min.x, area_min.y);
    const ImVec2 mark_max(area_min.x + width, area_min.y + marker_h);

    /* Pozadi plot rect (= subtle frame jako u PlotLines). */
    const ImU32 col_bg     = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 col_border = ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddRectFilled(plot_min, plot_max, col_bg);
    dl->AddRect(plot_min, plot_max, col_border);

    /* Spocti Y-rozsah dat pro normalizaci. */
    float sp_min = (float)g_stack.history_samples[0].sp;
    float sp_max = sp_min;
    for (uint32_t i = 1; i < count; i++) {
        float v = (float)g_stack.history_samples[i].sp;
        if (v < sp_min) sp_min = v;
        if (v > sp_max) sp_max = v;
    };
    float sp_range = sp_max - sp_min;
    if (sp_range < 1.0f) sp_range = 1.0f;    /* ochrana proti div/0 */

    /* Mapper sample index -> screen X (linearni). */
    const float plot_w_inner = plot_max.x - plot_min.x;
    auto idx_to_x = [&](uint32_t i) -> float {
        if (count <= 1) return plot_min.x;
        return plot_min.x
             + ((float)i / (float)(count - 1)) * plot_w_inner;
    };
    /* Mapper SP value -> screen Y. Vyssi SP = nahoře (Y mensi). */
    auto sp_to_y = [&](float sp) -> float {
        float norm = (sp - sp_min) / sp_range;     /* 0..1 */
        return plot_max.y - norm * plot_h;
    };

    /* === Vertikalni markery udalosti (= nad polyline plot) ===
     *
     * V9.1: zvyrazneni markeru - full alpha (255), sytejsi barvy (PUSH
     * sytsi cervena, POP sytsi zelena, other sytsi oranzova/zluta) +
     * tlustsi linka (2.0 px misto 1.0) pro viditelnost na prvni pohled.
     */
    if (g_stack.show_events && count >= 2) {
        const bool dense = ((float)count > 2.0f * plot_w_inner);
        const ImU32 col_push  = IM_COL32(255,  60,  60, 255);  /* delta -2 */
        const ImU32 col_pop   = IM_COL32( 60, 255,  60, 255);  /* delta +2 */
        const ImU32 col_other = IM_COL32(255, 200,   0, 255);  /* LD SP / INT */

        for (uint32_t i = 1; i < count; i++) {
            int delta = (int)g_stack.history_samples[i].sp
                      - (int)g_stack.history_samples[i - 1].sp;
            ImU32 col;
            if (delta == -2) {
                if (dense) continue;
                col = col_push;
            } else if (delta == 2) {
                if (dense) continue;
                col = col_pop;
            } else {
                col = col_other;
            };

            float x = idx_to_x(i);
            dl->AddLine(ImVec2(x, mark_min.y),
                         ImVec2(x, mark_max.y),
                         col, 2.0f);
        };
    };

    /* === Vlastni polyline pres data === */
    if (count >= 2) {
        static ImVec2 s_pts[DBGAPI_STACK_HISTORY_MAX];
        uint32_t n_pts = count;
        if (n_pts > DBGAPI_STACK_HISTORY_MAX) n_pts = DBGAPI_STACK_HISTORY_MAX;
        for (uint32_t i = 0; i < n_pts; i++) {
            s_pts[i].x = idx_to_x(i);
            s_pts[i].y = sp_to_y((float)g_stack.history_samples[i].sp);
        };
        const ImU32 col_line = ImGui::GetColorU32(ImGuiCol_PlotLines);
        dl->AddPolyline(s_pts, (int)n_pts, col_line, ImDrawFlags_None, 1.0f);
    } else {
        float x = idx_to_x(0);
        float y = sp_to_y((float)g_stack.history_samples[0].sp);
        dl->AddCircleFilled(ImVec2(x, y), 2.0f,
                             ImGui::GetColorU32(ImGuiCol_PlotLines));
    };

    /* === Hover detekce + tooltip + crosshair === */
    int hover_idx = -1;
    if (hovered) {
        float mouse_x = ImGui::GetIO().MousePos.x;
        float dx      = mouse_x - plot_min.x;
        if (dx < 0) dx = 0;
        if (count == 1) {
            hover_idx = 0;
        } else {
            float step = plot_w_inner / (float)(count - 1);
            if (step <= 0.0f) {
                hover_idx = 0;
            } else {
                int i = (int)(dx / step + 0.5f);
                if (i < 0) i = 0;
                if (i >= (int)count) i = (int)count - 1;
                hover_idx = i;
            };
        };
    };

    if (hover_idx >= 0) {
        float x = idx_to_x((uint32_t)hover_idx);
        const ImU32 col_hover = IM_COL32(200, 200, 200, 180);
        dl->AddLine(ImVec2(x, plot_min.y), ImVec2(x, plot_max.y),
                     col_hover, 1.0f);
        st_DBGAPI_STACK_HISTORY_SAMPLE s =
            g_stack.history_samples[hover_idx];
        ImGui::BeginTooltip();
        ImGui::Text("idx=%d  SP=%04Xh  cycle=%u",
                     hover_idx, s.sp, (unsigned)s.cycles);
        if (hover_idx > 0) {
            int delta = (int)s.sp
                      - (int)g_stack.history_samples[hover_idx - 1].sp;
            const char *kind;
            if (delta == -2)       kind = "PUSH";
            else if (delta == 2)   kind = "POP";
            else                   kind = "LD SP / other";
            ImGui::Text("delta=%+d (%s)", delta, kind);
        };
        ImGui::EndTooltip();
    };

    /* Klik LMB = vyber / deselect sample. */
    if (clicked && hover_idx >= 0) {
        if (g_stack.selected_history_idx == hover_idx) {
            g_stack.selected_history_idx = -1;
        } else {
            g_stack.selected_history_idx = hover_idx;
        };
    };

    /* === Selected sample - persistentni zluty crosshair === */
    if (g_stack.selected_history_idx >= 0
        && g_stack.selected_history_idx < (int)count)
    {
        float x = idx_to_x((uint32_t)g_stack.selected_history_idx);
        const ImU32 col_sel = IM_COL32(255, 220, 80, 220);
        dl->AddLine(ImVec2(x, mark_min.y), ImVec2(x, plot_max.y),
                     col_sel, 1.5f);
    };
}


/**
 * @brief V9: Sdileny render helper pro info radek pod sparkline plotem.
 *
 * Vykresli:
 *   - Samples + first/last SP/cycle text
 *   - Slope text + (creep!) priznak
 *   - Selected text + Clear tlacitko (pokud je vyber aktivni)
 *   - Show events checkbox
 *
 * Clear tlacitko ma stable ID slozeny z prefixu + clear_id_suffix - dva
 * renderery (hlavni Stack Monitor i V9 Stack History okno) ho mohou
 * vykreslit v jednom framu bez ID kolize.
 *
 * Pri vypnute history nebo prazdne cache vykresli odpovidajici hint.
 */
extern "C" void dbg_stack_panel_render_history_info(const char *clear_id_suffix,
                                                     bool show_open_history_btn)
{
    if (!clear_id_suffix) clear_id_suffix = "x";

    if (!g_stack.history_enabled) {
        ImGui::TextDisabled("%s",
            _("(SP history is disabled, enable in Stack Monitor)"));
        return;
    };
    if (!g_stack.history_valid || g_stack.history_count == 0) {
        ImGui::TextDisabled("%s", _("(no samples yet, recording...)"));
        return;
    };

    const uint32_t count = g_stack.history_count;
    st_DBGAPI_STACK_HISTORY_SAMPLE first = g_stack.history_samples[0];
    st_DBGAPI_STACK_HISTORY_SAMPLE last  =
        g_stack.history_samples[count - 1];

    ImGui::Text("Samples: %u   first SP=%04Xh @ %u   last SP=%04Xh @ %u",
                 (unsigned)count,
                 first.sp, (unsigned)first.cycles,
                 last.sp,  (unsigned)last.cycles);
    ImGui::Text("Slope: %.6f SP/cycle %s",
                 (double)g_stack.creep_slope,
                 g_stack.creep_warning ? "(creep!)" : "");

    if (g_stack.selected_history_idx >= 0
        && g_stack.selected_history_idx < (int)count)
    {
        st_DBGAPI_STACK_HISTORY_SAMPLE s =
            g_stack.history_samples[g_stack.selected_history_idx];
        ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.32f, 1.0f),
                            "Selected: idx=%d  SP=%04Xh  cycle=%u",
                            g_stack.selected_history_idx,
                            s.sp, (unsigned)s.cycles);
        ImGui::SameLine();

        /* ID kompozice: "<preklad>##Clear###stack_history_clear_sel_<suffix>"
         * - ImGui parsuje text takto:
         *   - "###..." (3 hashe) = re-define stable ID = stack_history_clear_sel_<suffix>
         *   - "##..." (2 hashe) = label-segment hidden delimiter
         *   - pred "##" = viditelny text
         * Druhe okno musi mit jiny suffix, jinak by sdilelo stejne ID jako
         * prvni a obe by reagovala stejne. _L() makro nelze pouzit pro
         * runtime retezec - gettext klic by neexistoval v .po; misto toho
         * preklad slozime rucne. Souboru je registrovan jen literal "Clear"
         * (= existujici klic z V2.1). */
        char btn_id[96];
        snprintf(btn_id, sizeof(btn_id),
                  "%s##Clear###stack_history_clear_sel_%s",
                  _("Clear"), clear_id_suffix);
        if (ImGui::SmallButton(btn_id)) {
            g_stack.selected_history_idx = -1;
        };
    };

    /* Show events - stejna kompozice jako Clear (= unikatni ID per
     * caller suffix). Klic "Show events" je registrovan jako N_() pres
     * existujici V2.1 pouziti _L() v puvodnim kodu. */
    char chk_id[96];
    snprintf(chk_id, sizeof(chk_id),
              "%s##Show events###stack_history_show_events_%s",
              _("Show events"), clear_id_suffix);
    ImGui::Checkbox(chk_id, &g_stack.show_events);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s",
            _("Vertical markers above sparkline: red=PUSH, "
              "green=POP, yellow=LD SP / INT dispatch. "
              "When data is dense (count > 2*width), only "
              "non-push/pop markers are shown."));
    };

    /* V9.1: [Stack History] tlacitko (vpravo zarovnano). Otevre/focusne
     * samostatne Stack History okno (= plot resize s velikosti okna,
     * idealni pro detail SP prubehu v case). Renderuje se jen v hlavnim
     * Stack Monitor okne - v samotnem Stack History okne by bylo
     * redundantni (= jsme tam jiz). */
    if (show_open_history_btn && g_gui) {
        ImGui::SameLine();
        char btn_id[96];
        snprintf(btn_id, sizeof(btn_id),
                  "%s##Stack History###stack_history_open_%s",
                  _("Stack History"), clear_id_suffix);
        /* Sirka tlacitka = text + frame padding * 2 (= shoda
         * s ImGui::Button interni geometrii). */
        float btn_w = ImGui::CalcTextSize(_("Stack History")).x
                    + ImGui::GetStyle().FramePadding.x * 2.0f;
        float avail_w = ImGui::GetContentRegionAvail().x;
        if (avail_w > btn_w) {
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() + avail_w - btn_w);
        };
        if (ImGui::Button(btn_id)) {
            g_gui->showStackHistoryWindow = true;
            /* V9.2: request_focus misto ImGui::SetWindowFocus(name).
             * Pattern: pending flag + SetNextWindowFocus PRED Begin v
             * dalsim framu cizovho okna - viz s_focus_pending v
             * stack_history_window.cpp pro detail. Puvodni
             * SetWindowFocus(name) volane AFTER Begin cilovho okna v
             * tomtez framu neaplikovalo platform raise (= title flash,
             * OS z-order beze zmeny). */
            stack_history_window_request_focus();
        };
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                _("Open and focus the Stack History window (plot "
                  "resizes with window size)"));
        };
    };
}


/**
 * @brief V9: Vrati shadow stav SP history recordingu.
 */
extern "C" bool dbg_stack_panel_history_enabled(void)
{
    return g_stack.history_enabled;
}


/* ========================================================================= */
/*  Render - Regions side panel (V3.1 - V7, smazano ve V8)                   */
/* ========================================================================= */

/* V8: per-region card layout `stack_panel_render_regions_panel` byl smazan
 * - obsah presunut do samostatneho Stack Regions okna s 1-row-per-region
 * table layoutem (= viz stack_regions_window/stack_regions_window.cpp).
 *
 * Mrtva implementace nize ponechana zakomentovana pro pripad pripadneho
 * revertu, ale nemela by byt nikym volana (= zadny call site v V8). */
#if 0
static void stack_panel_render_regions_panel(void)
{
    if (!g_stack.regions_valid || g_stack.regions.count == 0) {
        ImGui::TextDisabled("%s", _("(no regions, click [+] to add)"));
        return;
    };

    float ch = ImGui::CalcTextSize("F").x;
    const float cell_pad = ImGui::GetStyle().CellPadding.x * 2.0f;

    /* Sirky pro spodni "Base | Limit | SP% | Min | Act" radek. */
    const float w_base  = ImGui::CalcTextSize("FFFF").x + cell_pad;
    const float w_limit = ImGui::CalcTextSize("FFFF").x + cell_pad;
    const float w_pct   = ImGui::CalcTextSize("100%").x + cell_pad;
    const float w_min   = ImGui::CalcTextSize("FFFF").x + cell_pad;
    /* Act sloupec: tri SmallButton "E", "R" a "X" - posirovat dost na
     * pohodlne kliky (V7: pridan Edit). */
    const float w_act   = ImGui::CalcTextSize("EE RR XX").x + cell_pad * 2.0f;

    /* Sirka Trend sloupce v horni casti (= mini sparkline). */
    const float w_trend = ch * 10.0f;

    /* Aktualni SP z LIST snapshotu (= bezpecnejsi nez sp_now ze STACK_DUMP
     * snapshot, oba behu refresh ale LIST ma starsi tick - pro per-row
     * SP_pct ho pouzijeme abychom byli konzistentni s "current_sp_in_region"
     * markerem). */
    uint16_t sp = g_stack.regions.sp_now;

    /* Pripravime "request removal" pres lokalni var (= behem iterace
     * nemodifikujeme dbgapi cache uvnitr for loopu, abychom predesli
     * invalidaci pointeru pres compact). */
    int request_remove = -1;
    int request_reset  = -1;
    int request_edit   = -1;  /* V7: index regionu, ktery uzivatel chce editovat. */

    for (int i = 0; i < g_stack.regions.count; i++) {
        const st_DBGAPI_STACK_REGION_INFO *r = &g_stack.regions.regions[i];
        bool is_sel = (g_stack.selected_region_idx == i);

        ImGui::PushID(i);

        /* === Horni cast: Name label / value + Trend label / sparkline.
         * 2 sloupce: levy stretch (Name + value), pravy fixed (Trend +
         * sparkline). */
        ImGuiTableFlags tflags_top = ImGuiTableFlags_NoBordersInBody
                                    | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("###stack_reg_top", 2, tflags_top)) {
            ImGui::TableSetupColumn("name",
                                     ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("trend",
                                     ImGuiTableColumnFlags_WidthFixed,
                                     w_trend);

            /* Radek 1: labels "Name" a "Trend". */
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", _("Name"));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", _("Trend"));

            /* Radek 2: jmeno (Selectable na celou sirku) + sparkline. */
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[64];
            snprintf(label, sizeof(label), "%s###stack_reg_row_%d",
                      r->name, i);
            if (ImGui::Selectable(label, is_sel, 0)) {
                g_stack.selected_region_idx = i;
            };

            ImGui::TableNextColumn();
            if (g_stack.history_enabled && g_stack.history_valid
                && g_stack.history_count > 0)
            {
                float tmp[256];
                uint32_t filtered = stack_panel_filter_sparkline_for_region(
                    r, tmp, (uint32_t)IM_ARRAYSIZE(tmp));
                if (filtered > 0) {
                    char spark_id[32];
                    snprintf(spark_id, sizeof(spark_id),
                              "###stack_reg_spark_%d", i);
                    ImGui::PlotLines(spark_id, tmp, (int)filtered,
                                      0, NULL,
                                      FLT_MAX, FLT_MAX,
                                      ImVec2(w_trend - cell_pad,
                                              ImGui::GetTextLineHeight()
                                                + 4.0f));
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "%s %u %s",
                            _("SP samples in region:"),
                            (unsigned)filtered,
                            _("(filtered from history)"));
                    };
                } else {
                    ImGui::TextDisabled("--");
                };
            } else {
                ImGui::TextDisabled("--");
            };

            ImGui::EndTable();
        };

        /* === Spodni cast: 5 sloupcu Base / Limit / SP% / Min / Act. */
        ImGuiTableFlags tflags_bot = ImGuiTableFlags_BordersInnerV
                                    | ImGuiTableFlags_NoBordersInBodyUntilResize;
        if (ImGui::BeginTable("###stack_reg_bot", 5, tflags_bot)) {
            ImGui::TableSetupColumn("Base",
                                     ImGuiTableColumnFlags_WidthFixed, w_base);
            ImGui::TableSetupColumn("Limit",
                                     ImGuiTableColumnFlags_WidthFixed, w_limit);
            ImGui::TableSetupColumn("SP%",
                                     ImGuiTableColumnFlags_WidthFixed, w_pct);
            ImGui::TableSetupColumn("Min",
                                     ImGuiTableColumnFlags_WidthFixed, w_min);
            ImGui::TableSetupColumn("Act",
                                     ImGuiTableColumnFlags_WidthFixed, w_act);

            /* Radek labelu (TextDisabled = subtle subheader). */
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", _("Base"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", _("Limit"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", _("SP%"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", _("Min"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", _("Act"));

            /* Radek hodnot. */
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%04X", r->base);
            ImGui::TableNextColumn(); ImGui::Text("%04X", r->limit);

            ImGui::TableNextColumn();
            if (sp >= r->limit && sp <= r->base && r->base > r->limit) {
                int range = (int)r->base - (int)r->limit;
                int used  = (int)r->base - (int)sp;
                int pct   = (range > 0) ? (used * 100 / range) : 0;
                ImGui::Text("%d%%", pct);
            } else {
                ImGui::TextDisabled("--");
            };

            ImGui::TableNextColumn(); ImGui::Text("%04X", r->watermark);

            ImGui::TableNextColumn();
            /* V7: Edit tlacitko - otevre Edit modal s pre-fill hodnotami. */
            if (ImGui::SmallButton(_L("E###stack_reg_btn_edit"))) {
                request_edit = i;
            };
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", _("Edit region name/base/limit"));
            };
            ImGui::SameLine();
            if (ImGui::SmallButton(_L("R###stack_reg_btn_reset"))) {
                request_reset = i;
            };
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", _("Reset watermark + counters"));
            };
            ImGui::SameLine();
            if (ImGui::SmallButton(_L("X###stack_reg_btn_del"))) {
                request_remove = i;
            };
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", _("Delete region"));
            };

            ImGui::EndTable();
        };

        ImGui::PopID();

        /* Vizualni oddelovac mezi regiony (= prazdne misto + Separator). */
        if (i + 1 < g_stack.regions.count) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        };
    };

    /* Provedeni odlozenych akci - bezpecne mimo iteraci. */
    if (request_reset >= 0) {
        int saved_sel = g_stack.selected_region_idx;
        g_stack.selected_region_idx = request_reset;
        stack_panel_reset_watermark_selected();
        g_stack.selected_region_idx = saved_sel;
    };
    if (request_remove >= 0) {
        stack_panel_remove_region(request_remove);
    };
    /* V7: otevreni Edit modalu pres OpenPopup + pre-fill state. ImGui
     * OpenPopup musi byt volane mimo BeginTable/EndTable (= jsme za nimi). */
    if (request_edit >= 0) {
        stack_panel_open_edit_modal(request_edit);
        ImGui::OpenPopup("###stack_edit_modal");
    };
}
#endif /* V8: stack_panel_render_regions_panel disabled */


/* ========================================================================= */
/*  Render - Add region modal                                                */
/* ========================================================================= */

/**
 * @brief Vykresli modal dialog "Add region from current SP".
 *
 * Otevirat se musi pres ImGui::OpenPopup("###stack_add_modal") pred prvni
 * iteraci po kliku na tlacitko. Sticky popup zustane otevreny dokud uziv
 * neklikne OK / Cancel / Escape.
 *
 * Formular: Name (text), Base (hex 4), Limit (hex 4), Auto-detect (check).
 * Validace v stack_panel_submit_add (= base > limit, hex parse, name length).
 * Autoritativni validace (regex jmena, name conflict, full) v emu vlaknu.
 */
static void stack_panel_render_add_modal(void)
{
    /* Stable ID: pouze ID prefix s prazdnym labelem - dva prvni znaky ## by
     * sice fungovaly, ale tri ### dovoluji v budoucnu zmenit title bez
     * ztraty ImGui-stored stavu. */
    ImGuiWindowFlags mflags = ImGuiWindowFlags_AlwaysAutoResize;
    bool open = true;
    if (!ImGui::BeginPopupModal(_L("Add region###stack_add_modal"),
                                  &open, mflags))
    {
        /* Modal neni otevreny - okno zustane skryte. */
        g_stack.add_modal_open = false;
        return;
    };

    g_stack.add_modal_open = true;

    /* Hint pred poli. */
    ImGui::TextUnformatted(_("Define new stack region:"));

    /* Yellow hint pri SP=$FFFF v okamziku otevreni modalu (= Z80 reset
     * state). Vysvetluje, proc default base neni current SP ale NEWSP
     * $10F0. Flag se setuje v stack_panel_reset_add_form pri otevreni. */
    if (g_stack.add_reset_hint) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
                            _("Current SP=FFFFh looks like Z80 reset"
                              " state. Recommended base $10F0 (NEWSP)"
                              " preset."));
    };

    ImGui::Separator();

    ImGui::InputText(_L("Name###stack_add_name"),
                      g_stack.add_name, sizeof(g_stack.add_name));

    ImGui::InputText(_L("Base (hex)###stack_add_base"),
                      g_stack.add_base_hex, sizeof(g_stack.add_base_hex),
                      ImGuiInputTextFlags_CharsHexadecimal
                       | ImGuiInputTextFlags_CharsUppercase);

    ImGui::InputText(_L("Limit (hex)###stack_add_limit"),
                      g_stack.add_limit_hex, sizeof(g_stack.add_limit_hex),
                      ImGuiInputTextFlags_CharsHexadecimal
                       | ImGuiInputTextFlags_CharsUppercase);

    /* Validacni chyba (pokud byla pri minulem submitu). */
    if (g_stack.add_error) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                            "%s", g_stack.add_error);
    };

    ImGui::Separator();

    /* OK / Cancel. */
    if (ImGui::Button(_L("OK###stack_add_ok"), ImVec2(80, 0))) {
        if (stack_panel_submit_add()) {
            g_stack.add_modal_open = false;
            ImGui::CloseCurrentPopup();
        };
    };
    ImGui::SameLine();
    if (ImGui::Button(_L("Cancel###stack_add_cancel"), ImVec2(80, 0))) {
        g_stack.add_modal_open = false;
        g_stack.add_error = NULL;
        ImGui::CloseCurrentPopup();
    };

    /* Pokud uzivatel zavrel modal pres (X) tlacitko vlevo nahore,
     * `open` se shodi - synchronizujeme svuj flag. */
    if (!open) {
        g_stack.add_modal_open = false;
        g_stack.add_error = NULL;
    };

    ImGui::EndPopup();
}


/* ========================================================================= */
/*  V7 - Render: Edit region modal                                           */
/* ========================================================================= */

/**
 * @brief Vykresli modal dialog "Edit region".
 *
 * Otevirat se musi pres ImGui::OpenPopup("###stack_edit_modal") po
 * predchozim stack_panel_open_edit_modal(idx) volani (= pre-fill state).
 *
 * Formular: Name (text), Base (hex 4), Limit (hex 4). Pre-fill bere z
 * region cache, validuje lokalne v stack_panel_submit_edit, autoritativni
 * validace + overlap check v emu vlaknu (stack_regions_edit).
 *
 * OK = submit a zavrit pri uspechu. Cancel/Escape/(X) = bez zmeny.
 * Pri OK handler resetuje watermark + push/pop counters editovaneho regionu
 * (= staré stats neplatí pro nový rozsah).
 */
static void stack_panel_render_edit_modal(void)
{
    ImGuiWindowFlags mflags = ImGuiWindowFlags_AlwaysAutoResize;
    bool open = true;
    if (!ImGui::BeginPopupModal(_L("Edit region###stack_edit_modal"),
                                  &open, mflags))
    {
        /* Modal neni otevreny - okno zustane skryte. */
        g_stack.edit_modal_open = false;
        return;
    };

    g_stack.edit_modal_open = true;

    ImGui::TextUnformatted(_("Edit existing stack region:"));

    ImGui::Separator();

    ImGui::InputText(_L("Name###stack_edit_name"),
                      g_stack.edit_name, sizeof(g_stack.edit_name));

    ImGui::InputText(_L("Base (hex)###stack_edit_base"),
                      g_stack.edit_base_hex, sizeof(g_stack.edit_base_hex),
                      ImGuiInputTextFlags_CharsHexadecimal
                       | ImGuiInputTextFlags_CharsUppercase);

    ImGui::InputText(_L("Limit (hex)###stack_edit_limit"),
                      g_stack.edit_limit_hex, sizeof(g_stack.edit_limit_hex),
                      ImGuiInputTextFlags_CharsHexadecimal
                       | ImGuiInputTextFlags_CharsUppercase);

    /* Validacni chyba (pokud byla pri minulem submitu). */
    if (g_stack.edit_error) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                            "%s", g_stack.edit_error);
    };

    ImGui::Separator();

    /* OK / Cancel. */
    if (ImGui::Button(_L("OK###stack_edit_ok"), ImVec2(80, 0))) {
        if (stack_panel_submit_edit()) {
            g_stack.edit_modal_open = false;
            g_stack.edit_idx = -1;
            ImGui::CloseCurrentPopup();
        };
    };
    ImGui::SameLine();
    if (ImGui::Button(_L("Cancel###stack_edit_cancel"), ImVec2(80, 0))) {
        g_stack.edit_modal_open = false;
        g_stack.edit_idx = -1;
        g_stack.edit_error = NULL;
        ImGui::CloseCurrentPopup();
    };

    /* Pokud uzivatel zavrel modal pres (X) tlacitko vlevo nahore,
     * `open` se shodi - synchronizujeme svuj flag. */
    if (!open) {
        g_stack.edit_modal_open = false;
        g_stack.edit_idx = -1;
        g_stack.edit_error = NULL;
    };

    ImGui::EndPopup();
}


/* ========================================================================= */
/*  Public render entry                                                      */
/* ========================================================================= */

extern "C" void dbg_stack_panel_render(void)
{
    /* V6: jednorazovy sync shadow history_enabled s emu g_stack_history_active.
     * Recording flag mohl byt obnoven z [STACK_HISTORY] sekce INI pri startu
     * (= debugger_init -> stack_history_cfg_init). UI panel musi shadow
     * inicializovat z emu, ne jen z hardcoded "false". STACK_HISTORY_GET
     * vraci p.active, ktery pouzijeme k sync. Po prvnim uspesnem dotazu se
     * jiz shadow opira o checkbox toggle (stack_panel_set_history_enabled). */
    static bool s_history_sync_done = false;
    if (!s_history_sync_done) {
        if (!dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue)) {
            st_DBGAPI_STACK_HISTORY_GET_PARAM p;
            p.max_count    = 0;             /* sync only, nepotrebujeme samples */
            p.slope_window = 0;
            p.count        = 0;
            p.active       = 0;
            p.slope        = 0.0f;
            p.samples      = NULL;
            if (dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                            DBGAPI_CMD_STACK_HISTORY_GET,
                                            &p, NULL, 50))
            {
                g_stack.history_enabled = (p.active != 0);
                s_history_sync_done = true;
            };
        };
    };

    /* Refresh dat pokud je refresh tick. Tri endpointy:
     *  - dump (= hex window kolem SP) - jen v hlavnim Stack Monitor okne
     *  - regions (= snapshot stack_regions + per-region SP%) - sdilene
     *  - history (= V2 ring buffer pro sparkline + slope, jen kdyz
     *               recording aktivni) - sdilene
     *
     * V8: regions + history refresh je extrahovany do `dbg_stack_panel_frame_refresh`
     * aby ho mohlo volat i Stack Regions okno (= sdilena data, ale dump
     * je specificky pro hlavni okno). */
    if (g_dbg_ui.refresh.should_refresh) {
        stack_panel_refresh_dump();
    };
    dbg_stack_panel_frame_refresh();

    /* Sticky header - vzdy v plne sirce okna. */
    stack_panel_render_header();
    ImGui::Separator();

    /* V8 layout (zjednoduseny - bez per-region card panelu, ten je nyni
     * v samostatnem Stack Regions okne):
     *   Top section: hex dump tabulka pres celou sirku okna
     *   Bottom section: SP history sparkline pres celou sirku okna
     *
     * Hlavni Stack Monitor okno se nyni soustredi pouze na hex dump kolem
     * SP a SP history. Regions tabulka a Add/Edit/Reset/Delete akce jsou
     * v samostatnem Stack Regions okne (Alt+Shift+S). Aktivni region pro
     * watermark `=` marker a Depth indicator si stale vybira dropdown
     * Region: v sticky headeru.
     *
     * V8.4 layout fix: vyska sparkline section je vypoctena dynamicky
     * podle obsahu (= polyline + Samples/Slope text + volitelny Selected:
     * radek + Show events checkbox + collapsing header). Tim:
     *   - kdyz SP history toggle OFF, sparkline_h = 0 a tabulka zabere
     *     cele okno (zadny prazdny prostor dole)
     *   - kdyz selection sample aktivni, sparkline roste o jeden radek
     *     a tabulka shrink presne tolik
     *   - "Slope: X.XXXXXX SP/cycle" se uz neorezava
     */
    float avail_y = ImGui::GetContentRegionAvail().y;

    /* Vyska sparkline section - dynamicky podle aktualniho obsahu.
     *
     * Skladba (kdyz history_enabled):
     *   - CollapsingHeader "SP history" .... GetFrameHeightWithSpacing
     *   - marker strip + plot + gap ........ 8 + 2 + 60 = 70 px
     *   - "Samples: ... first ... last ..." TextLineHeightWithSpacing
     *   - "Slope: X.XXXXXX SP/cycle" ....... TextLineHeightWithSpacing
     *   - volitelne "Selected: ..." + Clear  TextLineHeightWithSpacing
     *   - "Show events" checkbox ........... GetFrameHeightWithSpacing
     *   - drobny padding pro rezervu ....... 4 px
     *
     * Kdyz history_enabled == false, sparkline_h = 0 - render_history
     * vyresi az uvnitr (early return) a tabulka zabere cely zbytek.
     *
     * Pozn.: pri prvotnim refresh muze byt history_count == 0 (=
     * sparkline ukaze "(no samples yet)" misto plot+slope), ale
     * collapsing header je porad otevreny a ostatni radky se
     * vykresluji. V tom edge case nase rezervovana vyska bude o
     * nekolik px vetsi nez skutecna - to neni problem (= prazdny
     * prostor < 1 radek), naopak chrani pred uskakovanim layoutu az
     * data dorazi.
     */
    /* V8.5 bod 1: detekce transition history_enabled false->true.
     * Pri zapnuti SP history toggle force-expand "SP history" CollapsingHeader
     * pres SetNextItemOpen + zaroven preset shadow flag, aby uz tento frame
     * dostal sparkline_h plnou vysku (bez 1-frame lag). */
    if (g_stack.history_enabled && !g_stack.history_prev_enabled) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        g_stack.history_header_open = true;
    };
    g_stack.history_prev_enabled = g_stack.history_enabled;

    float sparkline_h = 0.0f;
    if (g_stack.history_enabled) {
        const ImGuiStyle &style = ImGui::GetStyle();
        const float line_h  = ImGui::GetTextLineHeightWithSpacing();
        const float frame_h = ImGui::GetFrameHeightWithSpacing();

        if (!g_stack.history_header_open) {
            /* V8.5 bod 2: zbaleny CollapsingHeader = sparkline section
             * rezervuje pouze vysku samotneho header radku. Hex tabulka
             * vyplni cely zbyly prostor. Shadow stav (history_header_open)
             * je z predchoziho framu - 1-frame lag pri prvnim collapse/
             * expand je v UX neviditelny. */
            sparkline_h = frame_h;
        } else {
            /* Header (collapsing) + plot area (marker strip + gap + plot). */
            const float plot_area_h = 8.0f + 2.0f + 60.0f;
            sparkline_h = frame_h            /* CollapsingHeader */
                        + plot_area_h        /* marker + gap + polyline */
                        + line_h             /* Samples ... line */
                        + line_h             /* Slope ... line */
                        + frame_h            /* Show events checkbox */
                        + style.ItemSpacing.y * 2.0f
                        + 4.0f;              /* maly padding rezerva */

            /* Volitelny Selected: info radek + Clear button (= jen kdyz
             * uzivatel klikl na sample ve V2.1 sparkline). */
            if (g_stack.selected_history_idx >= 0) {
                sparkline_h += line_h;
            };
        };
    };

    /* Vyska top sekce = zbytek po sparkline. Pri nizkem okne zachovavame
     * minimum 80 px pro top, jinak nebude videt ani jeden radek. */
    float top_h = avail_y - sparkline_h;
    if (sparkline_h > 0.0f) {
        top_h -= ImGui::GetStyle().ItemSpacing.y;
    };
    if (top_h < 80.0f) top_h = 80.0f;

    /* === Top section: vertikalni SP slider (V10) + hex dump ===
     * V8.4: BeginChild dostane top_h, tabulka uvnitr ma vlastni ScrollY -
     * pokud se vsech K_LINES_TOTAL radku nevejde, uzivatel scrolluje.
     *
     * V10: vlevo od tabulky vertikalni slider s sipkovym jezdcem, ktery
     * nastavuje sp_lines_above (= pozici SP radku v tabulce). Slider
     * sdili vysku s hex tabulkou (= aligned s vyskou plot area). */
    if (ImGui::BeginChild("###stack_top_section", ImVec2(0.0f, top_h),
                           false, ImGuiWindowFlags_NoScrollbar))
    {
        /* Predame celou dostupnou vysku - tabulka aktivuje vlastni
         * ScrollY pokud potreba. */
        float inner_h = ImGui::GetContentRegionAvail().y;

        /* V10.1: Slider sirka 22 px (V10 mela 14 px). Vetsi sirka =
         * tlustsi sipka jezdce (= aspect ~ 1:1 pri ARROW_HALF_HEIGHT=10). */
        const float slider_w = 22.0f;

        stack_panel_render_sp_slider(slider_w, inner_h);
        ImGui::SameLine(0.0f, 2.0f);
        stack_panel_render_table(inner_h);
    };
    ImGui::EndChild();

    /* === Bottom section: SP history sparkline pres celou sirku okna ===
     * V8.4: BeginChild jen pokud history_enabled - jinak by zustaval
     * 0px child element ktery sice nezabira pixely, ale stale je v
     * ImGui frame state machine zbytecne (= bez impactu na vykon, ale
     * cisteji). */
    if (g_stack.history_enabled) {
        if (ImGui::BeginChild("###stack_bottom_section",
                               ImVec2(0.0f, 0.0f), false))
        {
            stack_panel_render_history_sparkline();
        };
        ImGui::EndChild();
    };

    /* Modaly Add / Edit - delegovano na sdilenou render funkci, kterou
     * vola i Stack Regions samostatne okno. */
    dbg_stack_panel_render_modals();
}


/* ========================================================================= */
/*  V8 - Public API pro Stack Regions samostatne okno                        */
/* ========================================================================= */

extern "C" void dbg_stack_panel_frame_refresh(void)
{
    /* Per-frame guard: pokud dbg_stack_panel_render() uz refresh udelal
     * v tomto framu (= ImGui::GetFrameCount() shodne s ulozenou hodnotou),
     * druhe volani je no-op. Tim se vyhneme dvojite serializaci CMDRQ pri
     * soucasnem zobrazenem hlavnim Stack Monitor i Stack Regions okne. */
    static int s_last_frame = -1;
    int frame = ImGui::GetFrameCount();
    if (frame == s_last_frame) return;
    s_last_frame = frame;

    if (g_dbg_ui.refresh.should_refresh) {
        stack_panel_refresh_regions();
        if (g_stack.history_enabled) {
            stack_panel_refresh_history();
        };
    } else if (!g_stack.regions_valid) {
        /* Force refresh regionu mimo tick, pokud cache jeste neni platna
         * (= prvni frame po otevreni okna nebo po ADD/REMOVE). */
        stack_panel_refresh_regions();
    };
}


extern "C" int dbg_stack_panel_regions_count(void)
{
    if (!g_stack.regions_valid) return 0;
    return g_stack.regions.count;
}


extern "C" const void *dbg_stack_panel_get_region(int idx)
{
    if (!g_stack.regions_valid) return NULL;
    if (idx < 0 || idx >= g_stack.regions.count) return NULL;
    return &g_stack.regions.regions[idx];
}


extern "C" uint16_t dbg_stack_panel_sp_now(void)
{
    return g_stack.regions_valid ? g_stack.regions.sp_now : 0;
}


extern "C" int dbg_stack_panel_get_selected(void)
{
    return g_stack.selected_region_idx;
}


extern "C" void dbg_stack_panel_set_selected(int idx)
{
    if (idx < 0) {
        g_stack.selected_region_idx = K_REGION_NONE;
        return;
    };
    if (g_stack.regions_valid && idx >= g_stack.regions.count) {
        g_stack.selected_region_idx = K_REGION_NONE;
        return;
    };
    g_stack.selected_region_idx = idx;
}


/* V8 deferred request flags pro Add / Edit modal opening.
 *
 * Stack Regions okno volá `dbg_stack_panel_request_add_modal()` /
 * `dbg_stack_panel_request_edit_modal(idx)` z ImGui Button kliků.
 * Tyto funkce nemohou ImGui::OpenPopup() volat přímo, protože modal
 * popup musí byt registrovaný ve stejném ImGui ID stack jako jeho
 * BeginPopupModal volání (= v `dbg_stack_panel_render_modals`).
 *
 * Proto deferred: request funkce jenom setují flag + pre-fill state,
 * `dbg_stack_panel_render_modals` ho přečte a zavolá OpenPopup před
 * BeginPopupModal.
 */
namespace {
bool s_request_open_add  = false;
bool s_request_open_edit = false;
int  s_request_edit_idx  = -1;
}


extern "C" void dbg_stack_panel_request_add_modal(void)
{
    /* Pre-fill state se musí provést hned (= aby uživatel viděl správné
     * default hodnoty při otevření modalu). OpenPopup zustane deferred. */
    stack_panel_reset_add_form();
    g_stack.add_modal_open = true;
    s_request_open_add = true;
}


extern "C" void dbg_stack_panel_request_edit_modal(int idx)
{
    if (!g_stack.regions_valid) return;
    if (idx < 0 || idx >= g_stack.regions.count) return;

    stack_panel_open_edit_modal(idx);
    s_request_open_edit = true;
    s_request_edit_idx  = idx;
}


extern "C" void dbg_stack_panel_request_reset_region(int idx)
{
    if (!g_stack.regions_valid) return;
    if (idx < 0 || idx >= g_stack.regions.count) return;

    /* stack_panel_reset_watermark_selected pouziva selected_region_idx -
     * docasne ho prepneme, zavolame, vratime. */
    int saved = g_stack.selected_region_idx;
    g_stack.selected_region_idx = idx;
    stack_panel_reset_watermark_selected();
    g_stack.selected_region_idx = saved;
}


extern "C" void dbg_stack_panel_request_remove_region(int idx)
{
    stack_panel_remove_region(idx);
}


extern "C" void dbg_stack_panel_render_region_trend(int region_idx,
                                                     float width, float height)
{
    if (region_idx < 0 || !g_stack.regions_valid
        || region_idx >= g_stack.regions.count)
    {
        ImGui::TextDisabled("--");
        return;
    };
    if (!g_stack.history_enabled || !g_stack.history_valid
        || g_stack.history_count == 0)
    {
        ImGui::TextDisabled("--");
        return;
    };

    const st_DBGAPI_STACK_REGION_INFO *r = &g_stack.regions.regions[region_idx];
    float tmp[256];
    uint32_t filtered = stack_panel_filter_sparkline_for_region(
        r, tmp, (uint32_t)IM_ARRAYSIZE(tmp));
    if (filtered == 0) {
        ImGui::TextDisabled("--");
        return;
    };

    char spark_id[40];
    snprintf(spark_id, sizeof(spark_id),
              "###stack_reg_trend_%d", region_idx);
    ImGui::PlotLines(spark_id, tmp, (int)filtered,
                      0, NULL,
                      FLT_MAX, FLT_MAX,
                      ImVec2(width, height));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s %u %s",
            _("SP samples in region:"),
            (unsigned)filtered,
            _("(filtered from history)"));
    };
}


extern "C" void dbg_stack_panel_render_modals(void)
{
    /* Zpracovani deferred OpenPopup pozadavku z external oken (= Stack
     * Regions). OpenPopup musi byt volane ve stejnem ImGui ID scope jako
     * BeginPopupModal nize (= top-level scope aktualniho okna). */
    if (s_request_open_add) {
        ImGui::OpenPopup("###stack_add_modal");
        s_request_open_add = false;
    };
    if (s_request_open_edit) {
        ImGui::OpenPopup("###stack_edit_modal");
        s_request_open_edit = false;
        (void)s_request_edit_idx; /* pre-fill uz probehl v request funkci */
    };

    /* Add region modal - BeginPopupModal je no-op pokud popup neni otevreny.
     * Volat kazdy frame z kazdeho okna, ktere je registrovano jako mozny
     * trigger (= hlavni Stack Monitor + Stack Regions samostatne okno). */
    stack_panel_render_add_modal();

    /* V7: Edit region modal - same. */
    stack_panel_render_edit_modal();
}


/* ========================================================================= */
/*  V6 - persistence UI preferenci do cfgmain.ini sekce [STACK_PANEL]        */
/* ========================================================================= */

namespace {

/**
 * @brief Idempotence flag - cfg modul se registruje jen jednou.
 */
bool s_cfg_panel_registered = false;

/**
 * @brief V10: Propagate cb pro deprecated [STACK_PANEL] lock_sp_center.
 *
 * Zachovano pro INI zpetnou kompatibilitu. Hodnota true mapuje
 * sp_lines_above na K_LINES_TOTAL/2 (= V2 symetric 20/20 chovani).
 * Hodnota false ponecha sp_lines_above na default K_LINES_ABOVE_SP_DEFAULT
 * (= V0 asymetric 32/8).
 *
 * Pozor: pokud INI obsahuje OBOJI lock_sp_center=1 i sp_lines_above=N,
 * vyhrava posledni propagate ve volani cfgmodule_propagate (= dle poradi
 * registrace elementu nize). Element sp_lines_above je registrovan az
 * po lock_sp_center, takze pri kolizi vyhrava sp_lines_above (= novy
 * V10 klic je primarni, lock_sp_center jen pre-fill pro old INI bez
 * sp_lines_above).
 */
void cfg_propagate_lock_sp_center(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    g_stack.lock_sp_center = cfgelement_get_bool_value(elm) ? true : false;
    if (g_stack.lock_sp_center) {
        g_stack.sp_lines_above = K_LINES_ABOVE_SP_CENTERED;
    };
}

/**
 * @brief V10: Save cb pro deprecated [STACK_PANEL] lock_sp_center.
 *
 * Vzdy zapisujeme false - novy V10 klic sp_lines_above je primary.
 * Tim postupne se "old INI" promaze (= pri prvnim save uz nebude
 * obsahovat lock_sp_center=1 a nepřemaže sp_lines_above pri nasledujicim
 * loadu).
 */
void cfg_save_lock_sp_center(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    cfgelement_set_bool_value(elm, 0);
}

/**
 * @brief V10: Propagate cb pro [STACK_PANEL] sp_lines_above.
 *
 * Nacte hodnotu z INI, clip na <0, K_LINES_TOTAL-1>. Default 32
 * (= V0 asymetric chovani).
 */
void cfg_propagate_sp_lines_above(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    unsigned v = cfgelement_get_unsigned_value(elm);
    if (v > (unsigned)(K_LINES_TOTAL - 1)) {
        v = (unsigned)(K_LINES_TOTAL - 1);
    };
    g_stack.sp_lines_above = (int)v;
}

/**
 * @brief V10: Save cb pro [STACK_PANEL] sp_lines_above.
 */
void cfg_save_sp_lines_above(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    int v = g_stack.sp_lines_above;
    if (v < 0) v = 0;
    if (v > K_LINES_TOTAL - 1) v = K_LINES_TOTAL - 1;
    cfgelement_set_unsigned_value(elm, (unsigned)v);
}

/**
 * @brief V8: Propagate cb pro [STACK_PANEL] show_regions_window.
 *
 * Cilove uloziste je g_gui->showStackRegionsWindow (= viditelnost
 * samostatneho Stack Regions okna). Hodnota se aplikuje az pri prvnim
 * propagate (= startup po nacteni INI).
 */
void cfg_propagate_show_regions_window(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    if (g_gui) {
        g_gui->showStackRegionsWindow = cfgelement_get_bool_value(elm)
                                         ? true : false;
    };
}

/**
 * @brief V8: Save cb pro [STACK_PANEL] show_regions_window.
 */
void cfg_save_show_regions_window(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    int val = (g_gui && g_gui->showStackRegionsWindow) ? 1 : 0;
    cfgelement_set_bool_value(elm, val);
}

/**
 * @brief V9: Propagate cb pro [STACK_PANEL] show_history_window.
 *
 * Cilove uloziste je g_gui->showStackHistoryWindow (= viditelnost
 * samostatneho Stack History okna). Hodnota se aplikuje az pri prvnim
 * propagate (= startup po nacteni INI).
 */
void cfg_propagate_show_history_window(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    if (g_gui) {
        g_gui->showStackHistoryWindow = cfgelement_get_bool_value(elm)
                                         ? true : false;
    };
}

/**
 * @brief V9: Save cb pro [STACK_PANEL] show_history_window.
 */
void cfg_save_show_history_window(void *e, void *data)
{
    (void)data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *)e;
    int val = (g_gui && g_gui->showStackHistoryWindow) ? 1 : 0;
    cfgelement_set_bool_value(elm, val);
}

}  /* anonymous namespace */


extern "C" void dbg_stack_panel_cfg_init(void)
{
    if (s_cfg_panel_registered) return;
    s_cfg_panel_registered = true;

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, (char *)"STACK_PANEL");
    if (!cmod) {
        /* Duplicate - bezpecne ignorujeme, panel pojede bez persistence. */
        return;
    };

    CFGELM *elm;
    elm = cfgmodule_register_new_element(cmod, (char *)"lock_sp_center",
                                          CFGENTYPE_BOOL, 0);
    cfgelement_set_propagate_cb(elm, cfg_propagate_lock_sp_center, NULL);
    cfgelement_set_save_cb(elm, cfg_save_lock_sp_center, NULL);

    /* V10: pozice SP markeru v hex dump tabulce (= pocet radku nad SP).
     * Default K_LINES_ABOVE_SP_DEFAULT (= 32, V0 asymetric chovani).
     * Range <0, K_LINES_TOTAL-1>. Registrovan PO lock_sp_center, takze
     * pri current INI obsahujicim oba klice se aplikuje sp_lines_above
     * jako posledni (= V10 klic je primary, lock_sp_center pre-fill jen
     * pro INI bez sp_lines_above). */
    elm = cfgmodule_register_new_element(cmod, (char *)"sp_lines_above",
                                          CFGENTYPE_UNSIGNED,
                                          (unsigned)K_LINES_ABOVE_SP_DEFAULT,
                                          (unsigned)0,
                                          (unsigned)(K_LINES_TOTAL - 1));
    cfgelement_set_propagate_cb(elm, cfg_propagate_sp_lines_above, NULL);
    cfgelement_set_save_cb(elm, cfg_save_sp_lines_above, NULL);

    /* V8: viditelnost Stack Regions samostatneho okna. Default false
     * (= closed pri prvnim spusteni). */
    elm = cfgmodule_register_new_element(cmod, (char *)"show_regions_window",
                                          CFGENTYPE_BOOL, 0);
    cfgelement_set_propagate_cb(elm, cfg_propagate_show_regions_window, NULL);
    cfgelement_set_save_cb(elm, cfg_save_show_regions_window, NULL);

    /* V9: viditelnost Stack History samostatneho okna. Default false. */
    elm = cfgmodule_register_new_element(cmod, (char *)"show_history_window",
                                          CFGENTYPE_BOOL, 0);
    cfgelement_set_propagate_cb(elm, cfg_propagate_show_history_window, NULL);
    cfgelement_set_save_cb(elm, cfg_save_show_history_window, NULL);

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
