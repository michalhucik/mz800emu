/*
 * File:   event_viewer_window.cpp
 *
 * Events panel - implementace Vlna 1 Commit 7.
 *
 * Log tab UI nad in-memory ringem z @c eventlog.c:
 *   - Toolbar: Mode combo (OFF/WHEN_WINDOW_OPEN/ALWAYS), Capacity
 *     input + Clear button, per-kategorie visibility checkboxy,
 *     Quick filter preset buttony, filter textbox, Total/Filtered
 *     counters.
 *   - Tabulka: Frame | Cycle | Sline | Px | PC | Cat | Sub | Detail.
 *   - Klik na řádek context menu: "Show in disasm" /
 *     "Show port in Overview" / "Copy event to clipboard".
 *
 * Strip tab (Vlna 2 Commit 10 + 11):
 *   - 2D canvas X = pixel column, Y = scanline (per-arch
 *     g_mzhal.video_screen_width x g_mzhal.video_screen_height).
 *   - Per-frame ring iteration (= jen eventy z aktuálního screens_total).
 *   - Bod per event, color = kategorie, size = subtype priority.
 *   - Toolbar: "Fit to window" / "1:1" mode combo, zoom slider
 *     (0.5..4x), Colors collapsible sekce s per-kategorie ColorEdit3.
 *   - Default Mesen-inspired color scheme (= 24 hodnot v
 *     @c s_default_category_colors).
 *   - Hit-test: lineární scan nad per-frame draw cache
 *     @c s_strip_drawn (= O(N), typicky <0.5 ms pro N<2000).
 *   - Hover tooltip: Pxclk/Frame/Sline/Px/PC/Cat/Sub/Detail (reuse
 *     Vlna 1 dekódérů @c evw_format_subtype / @c evw_format_detail).
 *   - Single click = select highlight (bílý outline), double click =
 *     @c evw_click_pause(e, true) (pause + disasm focus), pravý klik
 *     = context popup (Pause+disasm / Pause here / Show in Log tab /
 *     Copy event to clipboard).
 *   - "Show in Log tab" přes cross-tab switch
 *     (@c want_switch_tab + @c ImGuiTabItemFlags_SetSelected) +
 *     scroll na řádek (@c want_scroll_to_idx + @c SetScrollHereY).
 *   - Show previous frame double buffer = Commit 12.
 *
 * Vlna 2 Commit 14: Bookmarky (★) - per-event označení s pevným klíčem
 * @c (screens_total, pxclk_in_screen) drženým přes ring overflow. Log
 * tab dostane nový sloupec ★ s klikatelnou hvězdou (žlutá = bookmarked,
 * tlumená = ne). Strip context menu má "Bookmark this event" /
 * "Remove bookmark" položku. Strip canvas kreslí žlutý kroužek kolem
 * bookmarkovaných bodů. Toolbar (mezi Quick filter dropdown a Filter
 * textbox) má bookmark controls: "★ N" counter | Prev | Next | "Show
 * only ★" toggle | "Clear all" button s confirm popupem. Hotkeys
 * Ctrl+B (next bookmark) / Ctrl+Shift+B (prev bookmark). Storage:
 * file-static @c std::vector<st_EVW_BOOKMARK> (in-session only - cfg
 * persistence je follow-up commit).
 *
 * Mimo scope tohoto commitu (= dle README zadání):
 *   - Cfg persistence bookmarků (= follow-up po Commit 14).
 *   - Saved filter presets dropdown (Commit 15).
 *   - Row coloring per kategorie v Log tabu (Commit 16).
 *   - Rich decoder pro PSG/CTC/PIOZ80/GDG/FDC (Commit 20).
 *
 *
 * Lifecycle: open / close detekce volá @c eventlog_notify_window_open()
 * který v módu @c MODE_WHEN_WINDOW_OPEN spustí / zastaví ring recording.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "main.h"
#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"

#include "debugger/trace/eventlog.h"
#include "debugger/trace/tlog_common.h"  /* tlog_common_get_screens_total() = thread-safe accessor pro g_gdg. */
#include "debugger/trace/marklog.h"  /* marklog_register / marklog_record - Commit 20 auto-mark. */
#include "debugger/eventlog_filter.h"
#include "debugger/eventlog_decoder.h"  /* eventlog_decode_detail() - Vlna 3 Commit 21. */
#include "debugger/debugger.h"
#include "emulator/mzarch/mzhal.h"
#include "emulator/emulator.h" /* EMULATOR_TEST_PAUSED makro - detekce pause stavu pro scanline cursor. */

#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "emulator/cfgmain.h"

#include "event_viewer_window.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <algorithm>  /* std::sort - Group by skupin (Vlna 4 Commit 27). */

/* ===========================================================================
 *  Konstanty
 * =========================================================================== */

/**
 * @brief Maximální délka filter expression bufferu v UI (ImGui InputText).
 *
 * Záměrně shodné s @c EVENTLOG_FILTER_MAX_EXPR_LEN, aby parser dostal
 * jakýkoliv obsah InputTextu bez ořezání.
 */
#define EVW_FILTER_BUF_LEN   ( EVENTLOG_FILTER_MAX_EXPR_LEN )

/**
 * @brief Počet binů per-frame heatmapy v Log tabu (Vlna 4 Commit 28).
 *
 * Heatmapa rozděluje pxclk osu aktuálního snímku (0..@c g_mzhal.video_screen_width *
 * @c g_mzhal.video_screen_height - 1) na @c EVW_HEATMAP_BIN_COUNT stejně širokých
 * binů. 64 binů je kompromis mezi rozlišením (= dostatečně jemný histogram
 * aby šly odlišit early/mid/late frame fáze) a vizuální čitelností (= bary
 * mají rozumnou šířku i v úzkém okně).
 *
 * Pro MZ-800 PAL (= @c g_mzhal.video_screen_width=1136, @c g_mzhal.video_screen_height=312)
 * vychází jeden bin na ~5538 pxclk = ~5 scanline.
 */
#define EVW_HEATMAP_BIN_COUNT 64

/**
 * @brief Pixelová výška heatmap regionu nad Log tabulkou.
 *
 * Hodnota držena jako @c float aby šla přímo do @c ImVec2() bez konverze.
 * 70 px = dostatečně velké pro vizuálně čitelné bary, ale nezabírá příliš
 * prostoru nad tabulkou.
 */
#define EVW_HEATMAP_HEIGHT_PX 70.0f

/* ===========================================================================
 *  Heatmap bin struct (Vlna 4 Commit 28)
 * =========================================================================== */

/**
 * @brief Jeden bin per-frame heatmapy nad Log tabulkou.
 *
 * Lifecycle: file-static pole @c s_heatmap o @c EVW_HEATMAP_BIN_COUNT
 * prvcích. Reset @c memset(0) na začátku každého renderu Log tabu při
 * @c heatmap_enabled, naplnění lineárním scanem visible+matched eventů.
 *
 * @field total    Součet všech eventů v binu (= výška sloupce).
 * @field per_cat  Histogram per kategorie (= stack chart segments).
 *                 Index 0..EVENTLOG_CAT_COUNT-1 viz @ref en_EVENTLOG_CATEGORY.
 */
typedef struct st_EVW_HEATMAP_BIN {
    uint32_t total;
    uint32_t per_cat[EVENTLOG_CAT_COUNT];
} st_EVW_HEATMAP_BIN;

/* ===========================================================================
 *  Group by enum (Vlna 4 Commit 27)
 * =========================================================================== */

/**
 * @brief Režim seskupování řádků v Log tabu.
 *
 * Toolbar dropdown @c "Group by" přepíná mezi chronologickým výpisem
 * (= výchozí) a třemi aggregate pohledy. Per-frame re-grouping běží
 * v @c evw_render_log_tab(), složitost O(N log N) podle počtu visible
 * eventů (po category mask + filter + bookmark-only filtru).
 *
 * Hodnoty se persistují v cfg klíči @c [EVENT_VIEWER_WINDOW] @c group_by
 * jako uint (= shoda s @c CFGENTYPE_UNSIGNED).
 */
enum en_EVW_GROUP_BY {
    EVW_GROUP_NONE     = 0, /**< Chronologický výpis (= výchozí, beze změny vůči Vlně 1). */
    EVW_GROUP_FRAME    = 1, /**< Skupiny per @c screens_total. */
    EVW_GROUP_CATEGORY = 2, /**< Skupiny per @c category. */
    EVW_GROUP_PC       = 3, /**< Skupiny per @c pc hodnota. */
    EVW_GROUP_COUNT          /**< Sentinel (= počet platných hodnot). */
};

/* ===========================================================================
 *  UI state (interní, file-static)
 * =========================================================================== */

/**
 * @brief Persistovaný UI stav Events okna.
 *
 * Pole jsou drženy v file-static instanci @c s_state. Lifecycle:
 *   - Defaulty se nastavují v @c event_viewer_window_register_persistence().
 *   - Cfg propagate přepíše hodnoty z @c [EVENT_VIEWER_WINDOW] sekce.
 *   - @c event_viewer_window_apply_persisted() propaguje @c window_open
 *     do @c g_gui->showEventViewerWindow po vzniku @c g_gui struktury.
 *
 * @field filter_text       Buffer pro ImGui InputText filter řádku.
 *                          NULL-terminated, max @c EVW_FILTER_BUF_LEN.
 * @field filter_handle     Parsovaný filter z @c filter_text (alokovaný
 *                          @c eventlog_filter_parse). Reparseuje se při
 *                          změně @c filter_text. Může být @c NULL při OOM.
 * @field filter_dirty      One-shot flag pro reparse v render loopu.
 * @field cfg_filter_text   Cfg-vlastněný pointer pro CFGENTYPE_TEXT bind
 *                          (cfgmodule vlastní paměť, my jen kopírujeme).
 * @field cfg_window_open   Cfg-vlastněný uint pro klíč "window_open".
 *                          Po propagate ho čte @ref
 *                          event_viewer_window_apply_persisted().
 * @field show_categories   Toggle "Categories" expand sekce v toolbaru.
 * @field cached_total      Počet všech eventů v ringu (cache per frame).
 * @field cached_filtered   Počet eventů po filter eval (cache per frame).
 * @field cached_frame      Frame number kdy byla cache spočítaná (= guard
 *                          proti opakovanému scanu uvnitř téhož framu).
 * @field follow_tail       Toggle "Follow tail" v toolbaru (live debug
 *                          UX). Default ON - tabulka auto-scrolluje na
 *                          konec po každém renderu. Klik na řádek (=
 *                          click-pause) vypne automaticky, opětovné
 *                          zapnutí jen explicit toggle. Persistováno
 *                          v cfg klíči "follow_tail".
 * @field cfg_follow_tail   Cfg-vlastněný uint pro klíč "follow_tail".
 *                          Default 1. Hodnota se kopíruje do
 *                          @c follow_tail při prvním renderu (= stejný
 *                          lifecycle jako @c cfg_filter_text -> @c
 *                          filter_text).
 * @field selected_idx      Index aktuálně označeného řádku v ringu
 *                          (= row highlight, click-pause cíl). @c -1
 *                          znamená "žádný výběr". Po close+open Events
 *                          okna se resetuje na @c -1 (lifecycle stejný
 *                          jako @c s_screen_width_initialized).
 * @field want_switch_tab   Cross-window tab switch request. Hodnoty:
 *                          @c -1 = žádný požadavek, @c 0 = přepni na
 *                          Log tab, @c 1 = přepni na Strip tab. Konzumuje
 *                          ho TabBar render (= předá @c
 *                          ImGuiTabItemFlags_SetSelected do @c
 *                          BeginTabItem) a resetuje zpět na @c -1.
 * @field want_scroll_to_idx Index řádku v ringu, na který má Log tab po
 *                          přepnutí scrollovat. @c -1 = bez scrollu.
 *                          Konzumuje ho @c evw_render_log_tab() jakmile
 *                          ve své iteraci narazí na shodný @c i.
 * @field show_only_bookmarked  Toggle "Show only ★" v toolbaru (Commit 14).
 *                          Pokud @c true, filter pipeline v Log i Strip
 *                          tabu navíc AND-uje @c evw_bookmark_contains()
 *                          (= viditelné jen bookmarked eventy bez ohledu
 *                          na filter expression). In-session only.
 * @field confirm_clear_bookmarks_open  Jednorázový flag - příští render
 *                          otevře "Clear all bookmarks?" confirm popup.
 */
typedef struct st_EVW_STATE {
    char filter_text[EVW_FILTER_BUF_LEN];
    st_EVENTLOG_FILTER *filter_handle;
    bool filter_dirty;

    char *cfg_filter_text;
    unsigned cfg_window_open;

    bool show_categories;

    size_t cached_total;
    size_t cached_filtered;
    int    cached_frame;

    bool     follow_tail;
    unsigned cfg_follow_tail;
    int64_t  selected_idx;

    int     want_switch_tab;
    int64_t want_scroll_to_idx;

    bool    show_only_bookmarked;
    bool    confirm_clear_bookmarks_open;

    /* Toggle barevné kategorie v Log tabulce. Sdílí
     * s_strip_state.category_colors[], alpha snížená na ~40 aby text
     * zůstal čitelný. Cfg klíč log_row_coloring. */
    bool    row_coloring;
    unsigned cfg_row_coloring;

    /* Group by režim Log tabu (Vlna 4 Commit 27). Hodnota z
     * @ref en_EVW_GROUP_BY, default @c EVW_GROUP_NONE (= chronologický
     * výpis). Cfg klíč @c group_by jako uint (CFGENTYPE_UNSIGNED). */
    int      group_by;
    unsigned cfg_group_by;

    /* Heatmapa per-frame nad Log tabulkou (Vlna 4 Commit 28). Default
     * OFF (= region zabírá místo, user zapne explicit). Cfg klíč
     * @c heatmap (CFGENTYPE_BOOL). */
    bool     heatmap_enabled;
    unsigned cfg_heatmap_enabled;
} st_EVW_STATE;

static st_EVW_STATE s_state = {};

/**
 * @brief Pending flag pro cross-window focus request.
 *
 * Nastaven @c event_viewer_window_request_focus(), aplikován v
 * @c event_viewer_window_render() PŘED @c ImGui::Begin. Pattern shodný
 * se @c memmap_window / @c stack_history_window.
 */
static bool s_focus_pending = false;

/**
 * @brief Předchozí stav viditelnosti okna (= edge detekce open / close).
 *
 * Při změně @c *p_open != @c s_prev_open zavoláme
 * @c eventlog_notify_window_open() aby emu vrstva propagovala
 * @c MODE_WHEN_WINDOW_OPEN start / stop recordingu.
 */
static bool s_prev_open = false;

/**
 * @brief One-shot flag pro init šíře filter screenu.
 *
 * Filter parser používá @c eventlog_filter_set_screen_width() pro
 * dekódování @c sline / @c px z @c pxclk_in_screen. Width = celý
 * pxclk per scanline včetně blanking (@c g_mzhal.video_screen_width z
 * @c mz{800,700,1500}_video.h, typicky 1136 pro MZ-800 PAL).
 * Per-arch hodnota je compile-time, takže stačí nastavit 1×.
 */
static bool s_screen_width_initialized = false;

/* ===========================================================================
 *  Strip tab - state + default colors (Vlna 2 Commit 10)
 * =========================================================================== */

/**
 * @brief UI state pro Strip tab (= 2D canvas).
 *
 * Pole jsou držena v file-static instanci @c s_strip_state. Lifecycle:
 *   - Init v @ref evw_init_strip_state_once() při prvním renderu Strip
 *     tabu (lazy init aby cfg propagate měla šanci proběhnout dřív, ale
 *     fakticky se Strip state z cfg zatím nečte - persistence per-kategorie
 *     barev je follow-up).
 *   - Mutace přes toolbar (mode combo, zoom slider, ColorEdit3 callbacky).
 *
 * @field fit_to_window     Pokud @c true, canvas se škáluje na velikost
 *                          dostupného content regionu se zachováním
 *                          aspect ratio. Pokud @c false, 1 logický px =
 *                          1 fyzický px (= 1:1 mode, scrollbars přes
 *                          ChildWindow).
 * @field zoom              Multiplier nad "fit" nebo "1:1" base scale.
 *                          Range 0.5 .. 4.0, default 1.0.
 * @field category_colors   Per-kategorie ImU32 color (24 hodnot,
 *                          @ref en_EVENTLOG_CATEGORY index). Default
 *                          z @c s_default_category_colors. Mutace přes
 *                          Colors collapsible sekci v toolbaru.
 * @field initialized       One-shot flag pro lazy init (= aby se
 *                          @c category_colors neresetoval každý render).
 * @field show_colors       Toggle Colors collapsible sekce v Strip
 *                          toolbaru (default @c false = collapsed).
 * @field show_previous_frame Toggle "Show previous frame" v Strip
 *                          toolbaru (Commit 12). Default @c false.
 *                          Pokud @c true, render iteruje body z obou
 *                          screens_total (current + current-1)
 *                          a previous se kreslí s alpha/2 (= "ghost"
 *                          overlay). Persistováno v cfg klíči
 *                          @c [EVENT_VIEWER_WINDOW] strip_show_prev.
 * @field cfg_show_previous_frame Cfg-vlastněný uint pro klíč
 *                          @c strip_show_prev. Lifecycle stejný jako
 *                          @c cfg_follow_tail - hodnotu kopíruje
 *                          @c evw_init_strip_state_once() do
 *                          @c show_previous_frame.
 * @field show_grid         Toggle "Grid" v Strip toolbaru. Default
 *                          @c false. Při ON render kreslí jemnou mřížku
 *                          v logical Strip souřadnicích: sekundární
 *                          čáry každých 64 px / 32 sline, primární
 *                          (zvýrazněné) každých 256 px / 128 sline.
 *                          Persistováno v cfg klíči @c strip_grid.
 * @field cfg_show_grid     Cfg-vlastněný uint pro klíč @c strip_grid.
 * @field show_legend       Toggle "Legend" v Strip toolbaru. Default
 *                          @c true. Při ON Strip toolbar renderuje
 *                          collapsible sekci "Legend" s barevnými
 *                          čtverečky + názvy aktivních kategorií
 *                          (dle @c g_eventlog_active_mask).
 *                          Persistováno v cfg klíči @c strip_legend.
 * @field cfg_show_legend   Cfg-vlastněný uint pro klíč @c strip_legend.
 */
typedef struct st_EVW_STRIP_STATE {
    bool     fit_to_window;
    float    zoom;
    ImU32    category_colors[EVENTLOG_CAT_COUNT];
    bool     initialized;
    bool     show_colors;
    int64_t  selected_idx;
    int64_t  hovered_idx;
    int64_t  popup_idx;
    bool     show_previous_frame;
    unsigned cfg_show_previous_frame;
    bool     show_grid;
    unsigned cfg_show_grid;
    bool     show_legend;
    unsigned cfg_show_legend;
} st_EVW_STRIP_STATE;

static st_EVW_STRIP_STATE s_strip_state = {};

/**
 * @brief Cache jednoho vykresleného bodu pro hit-test.
 *
 * Naplní se během render loopu Strip tabu (= zápis při každém
 * @c AddCircleFilled). Po render loopu se vector použije pro lineární
 * scan kandidátů pod kurzorem.
 *
 * @field event_idx   Index eventu v ringu (= @c eventlog_get_event idx).
 * @field screen_x    Absolutní X souřadnice středu bodu v ImGui screen
 *                    space (= po násobení scale + @c canvas_origin.x).
 * @field screen_y    Absolutní Y souřadnice středu bodu.
 * @field radius      Poloměr bodu v screen pixelech (= výsledek
 *                    @c evw_subtype_radius pro danou kategorii).
 * @field from_prev_frame  Flag, zda event pochází z předchozího framu
 *                    (Commit 12 "Show previous frame" toggle). Použito
 *                    v tooltipu pro indikátor "(previous frame)" a pro
 *                    debug pouze - hit-test prioritu nemění.
 */
typedef struct st_EVW_STRIP_DRAWN {
    size_t event_idx;
    float  screen_x;
    float  screen_y;
    float  radius;
    bool   from_prev_frame;
} st_EVW_STRIP_DRAWN;

/**
 * @brief Per-frame draw cache pro hit-test ve Strip tabu.
 *
 * Lifecycle: @c clear() na začátku @c evw_render_strip_tab(),
 * @c push_back() při každém vykresleném bodu, lineární scan po render
 * loopu pro určení hover/click cíle. Vector je file-static aby se
 * znovu nealokovala kapacita každý frame (= amortizovaná O(1) reuse).
 *
 * @note Pro N < 10000 (typicky <2000 visible events/frame) je lineární
 *       scan akceptovatelný (<0.5 ms). Grid binning optimalizace je
 *       follow-up pokud profiling ukáže potřebu.
 */
static std::vector<st_EVW_STRIP_DRAWN> s_strip_drawn;

/* ===========================================================================
 *  Bookmarky (Vlna 2 Commit 14)
 * =========================================================================== */

/**
 * @brief Klíč bookmarku - dvojice (frame number, pxclk uvnitř framu).
 *
 * Klíč je pevný napříč ring overflow - dokud event fyzicky existuje v
 * ringu, hit-test podle @c (screens_total, pxclk_in_screen) ho najde.
 * Po overflow (= ring přepsal event) bookmark přežije, jen
 * @c evw_bookmark_find_event_idx() vrátí @c -1 (= žádný match v ringu).
 *
 * Lifecycle: in-session only (Commit 14). Cfg persistence
 * (@c [EVENT_VIEWER_BOOKMARKS] sekce) je follow-up commit - bookmarky
 * se ztrácí po zavření emu.
 *
 * @field screens_total      Frame number eventu (= @c st_EVENTLOG_EVENT
 *                           field @c screens_total). Unikátní per frame.
 * @field pxclk_in_screen    Pxclk pozice uvnitř framu (0..SCREEN_TICKS-1).
 *                           Unikátní per frame, dohromady s
 *                           @c screens_total tvoří globálně unique klíč.
 */
typedef struct st_EVW_BOOKMARK {
    uint32_t screens_total;
    uint32_t pxclk_in_screen;
} st_EVW_BOOKMARK;

/**
 * @brief Storage pro bookmarky - dynamic array.
 *
 * Pattern stejný jako @c s_strip_drawn (= file-static vector, reuse
 * kapacity, žádná external API). Typicky <100 bookmarků per session,
 * lineární scan stačí.
 *
 * Lifetime: po dobu běhu procesu. Mutace pouze přes API funkce
 * @c evw_bookmark_toggle / @c evw_bookmark_clear_all.
 */
static std::vector<st_EVW_BOOKMARK> s_bookmarks;


/**
 * @brief Vrátí počet aktuálně registrovaných bookmarků.
 *
 * @return Počet bookmarků v @c s_bookmarks (= @c size()).
 */
static size_t evw_bookmark_count ( void )
{
    return s_bookmarks.size ( );
}


/**
 * @brief Test zda dvojice (frame, pxclk) je registrovaná jako bookmark.
 *
 * Lineární scan @c s_bookmarks. Komplexita O(N), N = počet bookmarků
 * (typicky <100, akceptovatelné).
 *
 * @param screens         Frame number eventu.
 * @param pxclk           Pxclk pozice uvnitř framu.
 * @return @c true pokud klíč existuje, jinak @c false.
 */
static bool evw_bookmark_contains ( uint32_t screens, uint32_t pxclk )
{
    for ( const auto &b : s_bookmarks ) {
        if ( b.screens_total == screens && b.pxclk_in_screen == pxclk ) {
            return true;
        }
    }
    return false;
}


/**
 * @brief Toggle bookmark pro daný klíč.
 *
 * Pokud klíč @c (screens, pxclk) existuje v @c s_bookmarks, odstraní
 * ho (= remove bookmark). Jinak přidá nový (= add bookmark).
 *
 * Side effect: mutace @c s_bookmarks vectoru.
 *
 * @param screens   Frame number eventu.
 * @param pxclk     Pxclk pozice uvnitř framu.
 */
static void evw_bookmark_toggle ( uint32_t screens, uint32_t pxclk )
{
    for ( auto it = s_bookmarks.begin ( ); it != s_bookmarks.end ( ); ++it ) {
        if ( it->screens_total == screens && it->pxclk_in_screen == pxclk ) {
            s_bookmarks.erase ( it );
            return;
        }
    }
    st_EVW_BOOKMARK nb;
    nb.screens_total   = screens;
    nb.pxclk_in_screen = pxclk;
    s_bookmarks.push_back ( nb );
}


/**
 * @brief Smaže všechny bookmarky.
 *
 * Side effect: vyprázdní @c s_bookmarks (= @c clear()). Kapacita
 * vectoru se nevolá uvolnit - další push_back reuse paměť.
 */
static void evw_bookmark_clear_all ( void )
{
    s_bookmarks.clear ( );
}


/**
 * @brief Najde idx eventu v ringu, který je bookmarkovaný, ve směru
 *        @c direction od @c s_state.selected_idx.
 *
 * Algoritmus:
 *  1. Start = @c s_state.selected_idx + direction (= o jeden krok dál).
 *  2. Pokud @c selected_idx == -1, start = 0 (+1) nebo count-1 (-1).
 *  3. Iteruj v daném směru a hledej event s
 *     @c evw_bookmark_contains(e->screens_total, e->pxclk_in_screen) ==
 *     @c true. Vrátí první match.
 *  4. Pokud žádný match, vrátí @c -1.
 *
 * Komplexita: O(N * B), N = count ringu, B = počet bookmarků
 * (contains scan). Pro typické N<50000 a B<100 je to <5 ms na běžném
 * CPU, akceptovatelné pro interactive akci (button click). Pokud user
 * má 1000+ bookmarků, performance se může projevit - to je dokumentační
 * poznámka, ne optimalizace teď.
 *
 * @param direction   +1 = next (forward), -1 = prev (backward).
 * @return Idx eventu v ringu, nebo @c -1 pokud žádný bookmark match.
 */
static int evw_bookmark_find_event_idx ( int direction )
{
    if ( s_bookmarks.empty ( ) ) return -1;
    if ( direction != +1 && direction != -1 ) return -1;

    const size_t count = eventlog_get_count ( );
    if ( count == 0 ) return -1;

    /* Start = selected + direction. Pokud selected je -1, výchozí
     * pozice = -1 pro +1 (= scan začne od 0), nebo count pro -1
     * (= scan začne od count-1). */
    int64_t start;
    if ( s_state.selected_idx < 0 ) {
        start = ( direction == +1 ) ? -1 : (int64_t) count;
    } else {
        start = s_state.selected_idx;
    }

    if ( direction == +1 ) {
        for ( int64_t i = start + 1; i < (int64_t) count; i++ ) {
            const st_EVENTLOG_EVENT *e = eventlog_get_event ( (size_t) i );
            if ( !e ) continue;
            if ( evw_bookmark_contains ( e->screens_total,
                                          e->pxclk_in_screen ) ) {
                return (int) i;
            }
        }
    } else {
        for ( int64_t i = start - 1; i >= 0; i-- ) {
            const st_EVENTLOG_EVENT *e = eventlog_get_event ( (size_t) i );
            if ( !e ) continue;
            if ( evw_bookmark_contains ( e->screens_total,
                                          e->pxclk_in_screen ) ) {
                return (int) i;
            }
        }
    }
    return -1;
}


/**
 * @brief Default Mesen-inspired color scheme per kategorie.
 *
 * Index = @ref en_EVENTLOG_CATEGORY hodnota (0..23). Hodnoty jsou
 * @c ImU32 v ABGR layoutu (= @c IM_COL32 makro). Mapování dle README
 * Vlny 2 (Mesen-inspired):
 *   - CPU red / orange (interrupt activity)
 *   - IORQ cyan / blue
 *   - MMIO gray / brown
 *   - GDG green family (mode/banking/scroll/colors/video)
 *   - PIO/CTC purple family
 *   - PSG magenta
 *   - FDC amber
 *   - BP_FIRE / USER_MARK bright white (velké body, viditelné na pozadí)
 *
 * Při změně default hodnot zachovat index alignment s
 * @c en_EVENTLOG_CATEGORY (= kompilační chyba pokud se nový bod přidá
 * na střed enumu místo na konec).
 */
static const ImU32 s_default_category_colors[EVENTLOG_CAT_COUNT] = {
    IM_COL32 ( 255,  68,  68, 255 ), /* EVENTLOG_CAT_CPU_INT       = 0  red */
    IM_COL32 ( 255, 136,   0, 255 ), /* EVENTLOG_CAT_CPU_PIN_EDGE  = 1  orange */
    IM_COL32 ( 255,  68,  68, 255 ), /* EVENTLOG_CAT_IRQ_ACK_IM2   = 2  red */
    IM_COL32 (  68, 170, 255, 255 ), /* EVENTLOG_CAT_IORQ_IN       = 3  cyan */
    IM_COL32 (  68, 136, 255, 255 ), /* EVENTLOG_CAT_IORQ_OUT      = 4  blue */
    IM_COL32 ( 136, 136, 136, 255 ), /* EVENTLOG_CAT_MMIO_R        = 5  gray */
    IM_COL32 ( 170, 136, 136, 255 ), /* EVENTLOG_CAT_MMIO_W        = 6  brown-gray */
    IM_COL32 (  68, 255,  68, 255 ), /* EVENTLOG_CAT_GDG_MODE      = 7  green */
    IM_COL32 (  34, 200,  34, 255 ), /* EVENTLOG_CAT_GDG_BANKING   = 8  dark green */
    IM_COL32 ( 136, 255,  68, 255 ), /* EVENTLOG_CAT_GDG_HWSCROLL  = 9  lime */
    IM_COL32 ( 255, 255,  68, 255 ), /* EVENTLOG_CAT_GDG_COLORS    = 10 yellow */
    IM_COL32 (  34, 102,  34, 255 ), /* EVENTLOG_CAT_GDG_VIDEO     = 11 dark green */
    IM_COL32 ( 136, 136, 255, 255 ), /* EVENTLOG_CAT_PIO8255       = 12 light purple */
    IM_COL32 ( 170, 136, 255, 255 ), /* EVENTLOG_CAT_CTC8253       = 13 lavender */
    IM_COL32 ( 136, 100, 255, 255 ), /* EVENTLOG_CAT_PIOZ80        = 14 violet */
    IM_COL32 ( 255,  68, 255, 255 ), /* EVENTLOG_CAT_PSG           = 15 magenta */
    IM_COL32 ( 255, 170,   0, 255 ), /* EVENTLOG_CAT_FDC           = 16 amber */
    IM_COL32 ( 136, 136,  68, 255 ), /* EVENTLOG_CAT_MEMEXT        = 17 olive */
    IM_COL32 ( 255, 255, 255, 255 ), /* EVENTLOG_CAT_BP_FIRE       = 18 white (big) */
    IM_COL32 ( 255, 255, 255, 255 ), /* EVENTLOG_CAT_USER_MARK     = 19 white (huge) */
    IM_COL32 ( 220, 220, 220, 255 ), /* EVENTLOG_CAT_CPU_CTRL      = 20 light gray */
    IM_COL32 ( 255, 200,  68, 255 ), /* EVENTLOG_CAT_GDG_WFRF      = 21 amber-yellow */
    IM_COL32 ( 200, 136,   0, 255 ), /* EVENTLOG_CAT_QD            = 22 dark amber */
    IM_COL32 ( 136, 136,  68, 255 ), /* EVENTLOG_CAT_RD            = 23 olive */
    IM_COL32 ( 255, 255, 255, 255 ), /* EVENTLOG_CAT_SYS           = 24 white (lifecycle) */
};

/* ===========================================================================
 *  Helpers - decoder / formatter
 * =========================================================================== */

/**
 * @brief Dekóduje @c pxclk_in_screen na (scanline, px column).
 *
 * Filter modul drží šíři screenu interně (set přes
 * @c eventlog_filter_set_screen_width). Pro UI tabulku potřebujeme
 * stejné dekódování - duplikujeme dělení tady, ne přes filter API
 * (filter API nemá public getter na width).
 *
 * Width = celý pxclk per scanline VČETNĚ blanking (@c g_mzhal.video_screen_width).
 * Pro MZ-800 PAL: 80 (Hsync) + 106 (back porch) + 928 (display) + 22
 * (front porch) = 1136 pxclk/scanline. Decoded sline 0..311.
 *
 * @param pxclk_in_screen  Vstupní pozice v aktuálním snímku.
 * @param[out] sline       Scanline 0..g_mzhal.video_screen_height-1.
 * @param[out] px          Pixel column 0..g_mzhal.video_screen_width-1.
 */
static void evw_decode_raster ( uint32_t pxclk_in_screen,
                                 uint32_t *sline, uint32_t *px )
{
    const uint32_t width = (uint32_t) g_mzhal.video_screen_width;
    *sline = pxclk_in_screen / width;
    *px    = pxclk_in_screen % width;
}


/**
 * @brief Zformátuje krátký subtype label per kategorie (Sub sloupec).
 *
 * Vlna 3 Commit 22: dispatch na @c eventlog_decode_subtype_short() v
 * pure-C decoderu (viz @c debugger/eventlog_decoder.h). UI wrapper
 * zachovaný jen pro kompatibilitu s call-sites z Vlny 1/2 v tomto
 * souboru - logika kompletně v decoder modulu (= testovatelná z
 * @c tests/debugger/test_eventlog_decoder.c).
 *
 * Výstup je vždy max 8 viditelných znaků; pro kategorie bez smysluplného
 * subtype vrací @c "-".
 *
 * @param category  @ref en_EVENTLOG_CATEGORY hodnota.
 * @param subtype   Per-kategorie subtype hodnota.
 * @param buf       Output buffer.
 * @param buf_len   Velikost output bufferu (doporučeno >= 16 B).
 */
static void evw_format_subtype ( uint8_t category, uint8_t subtype,
                                  char *buf, size_t buf_len )
{
    eventlog_decode_subtype_short ( category, subtype, buf, buf_len );
}


/**
 * @brief Zformátuje plný popis (kategorie, subtype) pro hover tooltip.
 *
 * Vlna 3 Commit 22: thin wrapper nad
 * @c eventlog_decode_subtype_full(). Volá se z Log tabu při hover nad
 * Sub buňkou a ze Strip tooltipu jako čitelnější varianta zkratky.
 *
 * @param category  @ref en_EVENTLOG_CATEGORY hodnota.
 * @param subtype   Per-kategorie subtype hodnota.
 * @param buf       Output buffer.
 * @param buf_len   Velikost output bufferu (doporučeno >= 64 B).
 */
static void evw_format_subtype_full ( uint8_t category, uint8_t subtype,
                                       char *buf, size_t buf_len )
{
    eventlog_decode_subtype_full ( category, subtype, buf, buf_len );
}


/**
 * @brief Zformátuje Detail sloupec per kategorie (rich decoder, Vlna 3).
 *
 * Thin wrapper nad @c eventlog_decode_detail() z @c debugger/eventlog_decoder.h.
 * Decoder modul je pure C bez ImGui dependencies (= testovatelný z
 * @c test_eventlog_decoder.c).
 *
 * Per-kategorie formát výstupu viz @c eventlog_decoder.h hlavička.
 *
 * @param e        Pointer na event z ringu.
 * @param buf      Output buffer.
 * @param buf_len  Velikost bufferu (doporučeno 128 B).
 */
static void evw_format_detail ( const st_EVENTLOG_EVENT *e,
                                 char *buf, size_t buf_len )
{
    eventlog_decode_detail ( e, buf, buf_len );
}


/**
 * @brief Provede reparse @c s_state.filter_text -> @c s_state.filter_handle.
 *
 * Volá se při @c s_state.filter_dirty == @c true. Předchozí handle se
 * uvolní. Při OOM ponechá handle @c NULL (= @c eventlog_filter_match
 * vrátí @c false pro všechny eventy, UI to logicky interpretuje jako
 * "match všeho NEbude" - ale stejné chování pro syntax error
 * zachovává handle s error stringem, kdy match také vrací @c false).
 */
static void evw_reparse_filter ( void )
{
    if ( s_state.filter_handle ) {
        eventlog_filter_free ( s_state.filter_handle );
        s_state.filter_handle = NULL;
    }
    s_state.filter_handle = eventlog_filter_parse ( s_state.filter_text );
    s_state.filter_dirty = false;
}

/* ===========================================================================
 *  Pause-on-match trigger (Commit 19)
 * =========================================================================== */

/**
 * @brief State pause-on-match triggeru.
 *
 * Driven UI checkboxem "Pause on match" + textboxem s filter expression
 * v Events toolbaru. Když je @c enabled a @c parsed valid, eventlog
 * hot-path při každém zapsaném eventu volá @c evw_pause_callback() (přes
 * @ref g_eventlog_pause_callback). Callback eval filter na nově zapsaný
 * event a při match volá @c emulator_pause(true) (= halt) + zaznamená
 * frame/pxclk pro "Last match" indikátor.
 *
 * Cross-thread safety:
 *   - UI vlákno mutuje @c expr / @c parsed (= edit textboxu, toggle gate)
 *   - EMU vlákno čte @c parsed v @c evw_pause_callback
 *   - Race race je acceptable V1: max 1-2 missed / extra eventy než UI
 *     re-parse dokončí. Důvod: lock by zavedl per-event overhead i v
 *     OFF stavu (gate test by musel být uvnitř locku).
 *   - @c last_match_* polí jsou aktualizována jen z EMU vlákna při
 *     match; UI čte v render loopu (= úmyslně relaxed - mírný tearing
 *     na 32-bit hodnotách je vizuálně nezachytitelný).
 *
 * @field enabled        UI checkbox stav. Když @c false, @c
 *                       g_eventlog_pause_trigger_active musí být @c 0.
 * @field expr           ImGui InputText buffer pro pause filter
 *                       expression. NULL-terminated, max
 *                       @c EVW_FILTER_BUF_LEN.
 * @field parsed         Parsovaný filter z @c expr (alokovaný
 *                       @c eventlog_filter_parse). NULL pokud parse
 *                       OOM nebo prázdný expr; non-NULL i pro syntax
 *                       error (= @c eventlog_filter_get_error vrátí msg).
 * @field dirty          One-shot flag pro reparse v render loopu (=
 *                       user změnil text v textboxu).
 * @field has_match      @c true pokud trigger fíroval alespoň jednou
 *                       v této session. Resetováno @c Clear matches
 *                       tlačítkem.
 * @field last_match_frame   @c screens_total posledního matche.
 * @field last_match_pxclk   @c pxclk_in_screen posledního matche.
 * @field last_match_event_idx  Logický index v ringu pro scroll na
 *                       event v Log tabu. @c -1 = no valid idx (= overflow
 *                       může index zneplatnit, ale UI to bere jen jako
 *                       informativní; click řeší @c want_scroll_to_idx).
 */
typedef struct st_EVW_PAUSE_TRIGGER {
    bool                enabled;
    char                expr[EVW_FILTER_BUF_LEN];
    st_EVENTLOG_FILTER *parsed;
    bool                dirty;
    bool                has_match;
    uint32_t            last_match_frame;
    uint32_t            last_match_pxclk;
    int64_t             last_match_event_idx;
} st_EVW_PAUSE_TRIGGER;

static st_EVW_PAUSE_TRIGGER s_pause_trigger = {};

/**
 * @brief One-shot flag - eventlog callback je registrovaný.
 *
 * Registrace probíhá lazy při prvním renderu toolbaru, aby se neopírala
 * o pořadí cfg propagate / window init. Idempotentní (= druhé volání
 * nepřepíše pointer).
 */
static bool s_pause_callback_registered = false;

/**
 * @brief Pause-on-match callback volaný z EMU vlákna (eventlog hot path).
 *
 * Eval @c s_pause_trigger.parsed na nově zapsaný event. Match ->
 * zaznamenat metadata + volat @c emulator_pause(true) (= async halt
 * skrze MZ800_MAIN_SET_EVENT pattern, breakpoints používají stejnou
 * cestu).
 *
 * Defenzivní guard: pokud user vypnul gate mezi hot-path testem a
 * tímto callbackem, gracefully no-op. @c parsed může být NULL při OOM
 * - taky no-op.
 *
 * @param e  Pointer na právě zapsaný event v ringu (read-only).
 */
static void evw_pause_callback ( const st_EVENTLOG_EVENT *e )
{
    if ( !e ) return;
    if ( !s_pause_trigger.enabled ) return;
    if ( !s_pause_trigger.parsed ) return;
    if ( !eventlog_filter_match ( s_pause_trigger.parsed, e ) ) return;

    /* Match - zaznamenat + požádat o pause.
     *
     * Index v ringu = (head - 1) modulo capacity, protože eventlog_record
     * už head inkrementoval. Při wrap-around odpovídá fyzický index
     * logickému idx = count-1 (= aktuálně nejnovější event). */
    s_pause_trigger.has_match = true;
    s_pause_trigger.last_match_frame = e->screens_total;
    s_pause_trigger.last_match_pxclk = e->pxclk_in_screen;
    if ( g_eventlog.count > 0 ) {
        s_pause_trigger.last_match_event_idx = (int64_t) ( g_eventlog.count - 1 );
    } else {
        s_pause_trigger.last_match_event_idx = -1;
    }

    /* emulator_pause(true) z EMU vlákna je safe - viz breakpoints.c
     * pattern (= breakpoints_enforce volá emulator_pause(true) z hot
     * path. Funkce nastaví g_emulator.paused = true a MZEVENT_BREAK
     * event, který mzarch loop vyhodnotí v dalším checkpointu. */
    emulator_pause ( true );
}

/**
 * @brief Idempotentní registrace pause callbacku v eventlog hot-path.
 *
 * Volat při prvním renderu Events okna. Druhé volání no-op.
 */
static void evw_pause_register_callback_once ( void )
{
    if ( s_pause_callback_registered ) return;
    g_eventlog_pause_callback = evw_pause_callback;
    s_pause_callback_registered = true;
}

/**
 * @brief Re-parse pause filteru @c s_pause_trigger.expr -> @c parsed.
 *
 * Atomicky vůči hot-path: před re-parse vypneme gate (=
 * @c g_eventlog_pause_trigger_active = 0), pak free + new parse, pak
 * gate znovu nahodíme pokud user enabled a filter je validní.
 *
 * Důvod: kdyby gate zůstal zapnutý při free parsed, hot-path callback
 * by mohl číst right-after-free pointer. Vypnutí gate na pár cyklů je
 * acceptable (= max 1-2 missed eventy během re-parse).
 */
static void evw_pause_reparse ( void )
{
    int saved_gate = g_eventlog_pause_trigger_active;
    g_eventlog_pause_trigger_active = 0;

    if ( s_pause_trigger.parsed ) {
        eventlog_filter_free ( s_pause_trigger.parsed );
        s_pause_trigger.parsed = NULL;
    }
    s_pause_trigger.parsed = eventlog_filter_parse ( s_pause_trigger.expr );
    s_pause_trigger.dirty = false;

    /* Obnovit gate jen pokud user enabled a parse je platný (= bez
     * syntax error). Prázdný filter = match all = NEpovolujeme jako
     * trigger (= způsobilo by halt na první event), takže taky OFF.
     * Filter s temporal node-em (Vlna 4 Commit 26) taky NEpovolujeme -
     * callback běží z emu vlákna a temporal eval vyžaduje ring kontext
     * + ctx scope; toto je analytická vrstva pro UI, ne hot-path. */
    bool can_fire = false;
    if ( saved_gate && s_pause_trigger.enabled && s_pause_trigger.parsed ) {
        const char *err = eventlog_filter_get_error ( s_pause_trigger.parsed );
        if ( !err && s_pause_trigger.expr[0] != '\0'
             && !eventlog_filter_has_temporal ( s_pause_trigger.parsed ) ) {
            can_fire = true;
        }
    }
    g_eventlog_pause_trigger_active = can_fire ? 1 : 0;
}

/**
 * @brief Vykreslí Pause-on-match toolbar řádek (Commit 19).
 *
 * Layout:
 *   [ ] Pause on match: [filter expr textbox]  [status badge]  [Last: frame=N pxclk=M] [Clear]
 *
 * Side effects:
 *   - Toggle checkboxu nebo edit textboxu reparseuje filter +
 *     aktualizuje @c g_eventlog_pause_trigger_active (= hot-path gate).
 *   - Click na "Last match" indikátor scrollne Log tab na ten event
 *     (viz @c s_state.want_scroll_to_idx + want_switch_tab).
 *   - Click "Clear" vyresetuje @c has_match (= indikátor zmizí).
 */
static void evw_render_pause_trigger_row ( void )
{
    /* Lazy register callback v hot-path. */
    evw_pause_register_callback_once ( );

    /* Checkbox "Pause on match" - aktualizuje enabled + gate. */
    bool en = s_pause_trigger.enabled;
    if ( ImGui::Checkbox ( _L("Pause on match##evw_pause_match_en"), &en ) ) {
        s_pause_trigger.enabled = en;
        /* Promítnutí do gate: aktivace vyžaduje i platný parse + non-empty
         * expr. evw_pause_reparse() to vyhodnotí. */
        s_pause_trigger.dirty = true;
    }

    /* Textbox s filter expression. */
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 260.0f );
    if ( ImGui::InputTextWithHint ( _L("##evw_pause_match_expr"),
                                     _("cat:gdg_colors sym:isr_*"),
                                     s_pause_trigger.expr,
                                     sizeof ( s_pause_trigger.expr ) ) ) {
        s_pause_trigger.dirty = true;
    }

    /* Re-parse pokud user změnil text NEBO toggle enabled. */
    if ( s_pause_trigger.dirty ) {
        /* Pro re-parse potřebujeme stav gate před hovorem - když user
         * toggluje enabled, evw_pause_reparse() přečte enabled a podle
         * něj rozhodne. Trik s "saved_gate" v reparse zajistí, že
         * obnovení gate respektuje aktuální enabled. */
        g_eventlog_pause_trigger_active = s_pause_trigger.enabled ? 1 : 0;
        evw_pause_reparse ( );
    }

    /* Status badge - zelená "OK", červená "Syntax error", šedá "(off)". */
    ImGui::SameLine ( );
    const char *err = eventlog_filter_get_error ( s_pause_trigger.parsed );
    if ( s_pause_trigger.expr[0] == '\0' ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 140, 140, 140, 255 ) );
        ImGui::TextUnformatted ( _("(empty)") );
        ImGui::PopStyleColor ( );
    } else if ( err ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 220, 80, 80, 255 ) );
        ImGui::TextUnformatted ( _("[Syntax error]") );
        ImGui::PopStyleColor ( );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", err );
        }
    } else if ( s_pause_trigger.enabled ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 90, 200, 90, 255 ) );
        ImGui::TextUnformatted ( _("[Armed]") );
        ImGui::PopStyleColor ( );
    } else {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 180, 180, 180, 255 ) );
        ImGui::TextUnformatted ( _("[OK]") );
        ImGui::PopStyleColor ( );
    }

    /* Last match indikátor + click. */
    if ( s_pause_trigger.has_match ) {
        ImGui::SameLine ( );
        char lbl[96];
        snprintf ( lbl, sizeof ( lbl ),
                   "Last: frame=%u pxclk=%u##evw_pause_lastmatch",
                   s_pause_trigger.last_match_frame,
                   s_pause_trigger.last_match_pxclk );
        if ( ImGui::SmallButton ( lbl ) ) {
            if ( s_pause_trigger.last_match_event_idx >= 0 ) {
                s_state.selected_idx       = s_pause_trigger.last_match_event_idx;
                s_state.want_scroll_to_idx = s_pause_trigger.last_match_event_idx;
                s_state.want_switch_tab    = 0; /* Log tab */
                s_state.follow_tail        = false;
                s_state.cfg_follow_tail    = 0u;
            }
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _("Click to scroll Log tab to last matched event.") );
        }

        ImGui::SameLine ( );
        if ( ImGui::SmallButton ( _L("Clear##evw_pause_clear") ) ) {
            s_pause_trigger.has_match = false;
            s_pause_trigger.last_match_event_idx = -1;
        }
    }
}


/* ===========================================================================
 *  Auto-mark on match trigger (Commit 20)
 * =========================================================================== */

/**
 * @brief Maximální délka user marker name (=  @c MARKLOG_NAME_MAX 64,
 *        včetně NUL terminatoru).
 *
 * Marklog při registraci truncuje delší jména s warningem, UI buffer
 * zde drží stejný limit aby uživatel viděl výsledek 1:1.
 */
#define EVW_AUTOMARK_NAME_BUF_LEN 64

/**
 * @brief State auto-mark on match triggeru.
 *
 * Driven UI checkboxem "Auto-mark on match" + textboxem s user
 * marker name + textboxem s filter expression v Events toolbaru.
 * Když je @c enabled, @c parsed valid a @c name non-empty, eventlog
 * hot-path při každém zapsaném eventu volá @c evw_automark_callback()
 * (přes @ref g_eventlog_automark_callback). Callback eval filter a
 * při match volá @c marklog_record() (= synthetic @c USER_MARK event
 * v ringu + zápis do tlog markeru, pokud běží recording).
 *
 * Cross-thread safety: identicky s @ref st_EVW_PAUSE_TRIGGER (= UI
 * mutuje pod gate vypnutým, EMU čte při gate zapnutém).
 *
 * Use cases (= dokumentační, viz @ref evw_render_automark_trigger_row):
 *  - name "psg_writes" + expr "cat:psg" - každý PSG write je marker
 *    (= Strip canvas ukáže pozice PSG writes v rámci snímku).
 *  - name "bord_change" + expr "cat:gdg_colors sub:bord" - tracking
 *    BORDER writes (= debug raster effects).
 *  - name "isr_call" + expr "cat:cpu_int sym:isr_main" - specific
 *    ISR dispatch.
 *
 * @field enabled            UI checkbox stav.
 * @field name               User-supplied marker name, max
 *                           @c EVW_AUTOMARK_NAME_BUF_LEN. Prázdný =
 *                           gate OFF (= jména s 0 bytes marklog odmítá).
 * @field expr               Filter expression buffer, max
 *                           @c EVW_FILTER_BUF_LEN.
 * @field parsed             Parsovaný filter (= @c eventlog_filter_parse).
 * @field dirty              One-shot flag pro reparse + re-register.
 * @field cached_marker_id   Marker id po prvním fire (=
 *                           @c marklog_register). @c MARKLOG_INVALID_ID
 *                           = ještě nebylo registrováno.
 * @field cached_for_name    Jméno, pro které je @c cached_marker_id
 *                           platný. Invalidate při změně @c name.
 * @field has_match          @c true při alespoň jednom fire.
 * @field total_marks        Counter fire počtů (= "N fires" v UI).
 */
typedef struct st_EVW_AUTOMARK_TRIGGER {
    bool                enabled;
    char                name[EVW_AUTOMARK_NAME_BUF_LEN];
    char                expr[EVW_FILTER_BUF_LEN];
    st_EVENTLOG_FILTER *parsed;
    bool                dirty;
    uint16_t            cached_marker_id;
    char                cached_for_name[EVW_AUTOMARK_NAME_BUF_LEN];
    bool                has_match;
    uint64_t            total_marks;
} st_EVW_AUTOMARK_TRIGGER;

static st_EVW_AUTOMARK_TRIGGER s_automark_trigger = {
    .cached_marker_id = MARKLOG_INVALID_ID,
};

/**
 * @brief One-shot flag - eventlog automark callback je registrovaný.
 *
 * Lazy registrace při prvním renderu, identicky s pause callbackem.
 */
static bool s_automark_callback_registered = false;

/**
 * @brief Auto-mark callback volaný z EMU vlákna (eventlog hot path).
 *
 * Eval @c s_automark_trigger.parsed na nově zapsaný event. Match ->
 * @c marklog_register (lazy, jen poprvé per name) + @c marklog_record
 * (= 24B zápis do markerlog tlog + paralelní fan-out do eventlog ringu
 * jako @c EVENTLOG_CAT_USER_MARK event).
 *
 * Re-entry guard: @c marklog_record() vyvolá @c eventlog_record() s
 * kategorií @c USER_MARK -> callback by se volal znovu. Skip pomocí
 * @c if (e->category == USER_MARK) return; (= USER_MARK se nikdy
 * neaut-markuje znovu, infinite loop prevented).
 *
 * @param e  Pointer na právě zapsaný event (read-only).
 */
static void evw_automark_callback ( const st_EVENTLOG_EVENT *e )
{
    if ( !e ) return;
    if ( !s_automark_trigger.enabled ) return;
    if ( !s_automark_trigger.parsed ) return;
    if ( s_automark_trigger.name[0] == '\0' ) return;

    /* Re-entry guard: USER_MARK event byl vygenerován naším vlastním
     * marklog_record() voláním. Skip - jinak infinite re-entry, dokud
     * stack neselže. */
    if ( e->category == EVENTLOG_CAT_USER_MARK ) return;

    if ( !eventlog_filter_match ( s_automark_trigger.parsed, e ) ) return;

    /* MATCH! Lazy register marker_id - jen pokud ještě nebyl nebo se
     * změnilo jméno mezi předchozím fire a tímto. Re-register při
     * změně name je iniciovaný UI (= clear cached_marker_id při dirty
     * + name change), tady jen lazy fallback. */
    if ( s_automark_trigger.cached_marker_id == MARKLOG_INVALID_ID
         || strcmp ( s_automark_trigger.cached_for_name,
                     s_automark_trigger.name ) != 0 ) {
        s_automark_trigger.cached_marker_id =
            marklog_register ( s_automark_trigger.name );
        /* Uložit jméno pro budoucí porovnání. Pokud register selhal
         * (= MARKLOG_INVALID_ID), uložíme stejně - aby se nezkoušelo
         * register znovu při každém match (= stderr warning by spamoval). */
        snprintf ( s_automark_trigger.cached_for_name,
                   sizeof ( s_automark_trigger.cached_for_name ),
                   "%s", s_automark_trigger.name );
    }

    if ( s_automark_trigger.cached_marker_id == MARKLOG_INVALID_ID ) {
        /* Register selhal (= overflow registru, alloc fail nebo prázdné
         * jméno). UI to zobrazí jako "(registration failed)" - hot path
         * sám nic víc neudělá. */
        return;
    }

    /* marklog_record: synchronní write do markerlog binárního souboru
     * (pokud writer běží) + paralelní fan-out do eventlog ringu jako
     * USER_MARK event. Druhá cesta re-entry callbacku, ale skipnutá
     * guardem výše. */
    marklog_record ( s_automark_trigger.cached_marker_id );
    s_automark_trigger.has_match = true;
    s_automark_trigger.total_marks++;
}

/**
 * @brief Idempotentní registrace automark callbacku v eventlog hot-path.
 */
static void evw_automark_register_callback_once ( void )
{
    if ( s_automark_callback_registered ) return;
    g_eventlog_automark_callback = evw_automark_callback;
    s_automark_callback_registered = true;
}

/**
 * @brief Re-parse automark filteru + invalidate cached marker_id při
 *        změně jména.
 *
 * Atomicky vůči hot-path: gate OFF, free + new parse, gate ON pokud
 * (enabled && parsed valid && non-empty name && non-empty expr).
 *
 * Cached marker_id se invaliduje pokud user změnil jméno - další fire
 * zaregistruje pod novým jménem (= dostane nový id).
 */
static void evw_automark_reparse ( void )
{
    int saved_gate = g_eventlog_automark_trigger_active;
    g_eventlog_automark_trigger_active = 0;

    if ( s_automark_trigger.parsed ) {
        eventlog_filter_free ( s_automark_trigger.parsed );
        s_automark_trigger.parsed = NULL;
    }
    s_automark_trigger.parsed = eventlog_filter_parse ( s_automark_trigger.expr );
    s_automark_trigger.dirty = false;

    /* Pokud user změnil name, zrušit cache (= re-register při dalším
     * fire). Counter total_marks zachovat (= UI vidí kumulativní fire
     * count, reset jen explicit Clear button). */
    if ( strcmp ( s_automark_trigger.cached_for_name,
                  s_automark_trigger.name ) != 0 ) {
        s_automark_trigger.cached_marker_id = MARKLOG_INVALID_ID;
    }

    /* Gate ON jen pokud user enabled a všechny vstupy validní. Temporal
     * filtry jsou vyřazené (Vlna 4 Commit 26) - viz pause_reparse. */
    bool can_fire = false;
    if ( saved_gate
         && s_automark_trigger.enabled
         && s_automark_trigger.parsed
         && s_automark_trigger.name[0] != '\0'
         && s_automark_trigger.expr[0] != '\0'
         && !eventlog_filter_has_temporal ( s_automark_trigger.parsed ) ) {
        const char *err = eventlog_filter_get_error ( s_automark_trigger.parsed );
        if ( !err ) {
            can_fire = true;
        }
    }
    g_eventlog_automark_trigger_active = can_fire ? 1 : 0;
}

/**
 * @brief Vykreslí Auto-mark on match toolbar řádek (Commit 20).
 *
 * Layout (= podobný jako pause sekce, ale s name + filter):
 *   [ ] Auto-mark on match: name=[textbox] expr=[textbox]
 *     [status badge]  [Marker ID: N  (M fires)]  [Clear]
 *
 * Side effects:
 *   - Toggle / edit reparseuje filter + aktualizuje gate.
 *   - Click "Clear" vyresetuje has_match + total_marks + invaliduje
 *     cached_marker_id (= další fire registruje znovu).
 *
 * Re-entry guard pro infinite loop je v @c evw_automark_callback
 * (skip @c USER_MARK kategorie).
 *
 * Use case examples (= komentáře pro budoucí čtenáře dokumentace):
 *   - "psg_write" + "cat:psg" -> každý PSG write je marker
 *   - "bord_change" + "cat:gdg_colors sub:bord" -> BORDER write tracking
 *   - "isr_call" + "cat:cpu_int sym:isr_main" -> specific ISR dispatch
 */
static void evw_render_automark_trigger_row ( void )
{
    evw_automark_register_callback_once ( );

    /* Checkbox. */
    bool en = s_automark_trigger.enabled;
    if ( ImGui::Checkbox ( _L("Auto-mark on match##evw_automark_en"), &en ) ) {
        s_automark_trigger.enabled = en;
        s_automark_trigger.dirty = true;
    }

    /* Name textbox (uzší než filter). */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "%s", _("name:") );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 120.0f );
    if ( ImGui::InputTextWithHint ( _L("##evw_automark_name"),
                                     _("psg_writes"),
                                     s_automark_trigger.name,
                                     sizeof ( s_automark_trigger.name ) ) ) {
        s_automark_trigger.dirty = true;
    }

    /* Filter expression textbox. */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "%s", _("expr:") );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 220.0f );
    if ( ImGui::InputTextWithHint ( _L("##evw_automark_expr"),
                                     _("cat:psg"),
                                     s_automark_trigger.expr,
                                     sizeof ( s_automark_trigger.expr ) ) ) {
        s_automark_trigger.dirty = true;
    }

    /* Reparse + gate update. */
    if ( s_automark_trigger.dirty ) {
        g_eventlog_automark_trigger_active = s_automark_trigger.enabled ? 1 : 0;
        evw_automark_reparse ( );
    }

    /* Status badge. */
    ImGui::SameLine ( );
    const char *err = eventlog_filter_get_error ( s_automark_trigger.parsed );
    if ( s_automark_trigger.expr[0] == '\0'
         || s_automark_trigger.name[0] == '\0' ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 140, 140, 140, 255 ) );
        ImGui::TextUnformatted ( _("(empty)") );
        ImGui::PopStyleColor ( );
    } else if ( err ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 220, 80, 80, 255 ) );
        ImGui::TextUnformatted ( _("[Syntax error]") );
        ImGui::PopStyleColor ( );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", err );
        }
    } else if ( s_automark_trigger.enabled ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 90, 200, 90, 255 ) );
        ImGui::TextUnformatted ( _("[Armed]") );
        ImGui::PopStyleColor ( );
    } else {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 180, 180, 180, 255 ) );
        ImGui::TextUnformatted ( _("[OK]") );
        ImGui::PopStyleColor ( );
    }

    /* Marker ID indikátor + fire counter. */
    if ( s_automark_trigger.cached_marker_id != MARKLOG_INVALID_ID ) {
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "%s %u  (%llu fires)",
                              _("Marker ID:"),
                              (unsigned) s_automark_trigger.cached_marker_id,
                              (unsigned long long) s_automark_trigger.total_marks );
    } else if ( s_automark_trigger.enabled
                && s_automark_trigger.name[0] != '\0'
                && s_automark_trigger.expr[0] != '\0' ) {
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "%s", _("(not yet fired)") );
    }

    /* Clear button (= zobrazit jen pokud byly nějaké fires). */
    if ( s_automark_trigger.has_match ) {
        ImGui::SameLine ( );
        if ( ImGui::SmallButton ( _L("Clear##evw_automark_clear") ) ) {
            s_automark_trigger.has_match = false;
            s_automark_trigger.total_marks = 0;
            /* Cache marker_id nezruším - další fire by jen znovu
             * registrovalo pod stejným jménem (= dostane stejné id z
             * idempotentního marklog_register). Counter restartuje od
             * nuly = informativně přehlednější. */
        }
    }
}


/* ===========================================================================
 *  Quick filter presets
 * =========================================================================== */

/**
 * @brief Quick filter preset.
 *
 * Toolbar tlačítko @c label při kliku přepíše @c s_state.filter_text
 * na @c expr a invaliduje filter cache.
 */
typedef struct st_EVW_QUICK_PRESET {
    const char *label;
    const char *expr;
} st_EVW_QUICK_PRESET;

/**
 * @brief Předdefinované quick filter buttony toolbaru.
 *
 * "Hide noise" filtr v Commit 7 vypíná jen @c gdg_video kategorii
 * (= časté HBLN/VS/HS edge eventy). Dokumentační poznámka: další noisy
 * subtypes (např. časté CTC reload writes) přidá Commit 20 jakmile budou
 * subtypes pojmenované.
 */
static const st_EVW_QUICK_PRESET k_quick_presets[] = {
    /* HW Events groups - zobrazení per-typ HW aktivity. */
    { N_("Only IRQ"),       "cat:cpu_int,cpu_pin_edge,irq_ack_im2" },
    { N_("Only banking"),   "cat:gdg_banking,memext" },
    { N_("Only video"),     "cat:gdg_mode,gdg_hwscroll,gdg_colors,gdg_video,gdg_wfrf" },
    { N_("Only PSG"),       "cat:psg" },
    { N_("Only FDC"),       "cat:fdc" },
    { N_("Only memory"),    "cat:mmio_r,mmio_w" },
    { N_("Only SYS"),       "cat:sys" },
    /* Code scope - filtruje podle PC range nebo debugger markerů. */
    { N_("Only marks/BPs"), "cat:user_mark,bp_fire" },
    { N_("ISR scope (PC 0038-00FF)"), "pc:38-FF" },
    /* Hide / reset. */
    { N_("Hide noise"),     "!cat:gdg_video" },
    { N_("Clear filter"),   "" },
};


/**
 * @brief Aplikuje quick preset = zkopíruje @c expr do filter bufferu
 *        a invaliduje parsed handle.
 */
static void evw_apply_quick_preset ( const char *expr )
{
    if ( !expr ) return;
    g_strlcpy ( s_state.filter_text, expr, sizeof ( s_state.filter_text ) );
    s_state.filter_dirty = true;
}

/* ===========================================================================
 *  Toolbar rendering
 * =========================================================================== */

/**
 * @brief Vykreslí Mode combo (OFF / WHEN_WINDOW_OPEN / ALWAYS).
 *
 * Při změně volá @c eventlog_recompute_active() aby se nová politika
 * okamžitě promítla do @c g_eventlog_active.
 */
static void evw_render_mode_combo ( void )
{
    const char *items[] = { "OFF", "WHEN_WINDOW_OPEN", "ALWAYS" };
    int current = (int) g_eventlog_config.mode;
    if ( current < 0 || current > 2 ) current = 0;

    ImGui::SetNextItemWidth ( 180.0f );
    if ( ImGui::BeginCombo ( _L("Mode##evw_mode"), items[current] ) ) {
        for ( int i = 0; i < 3; i++ ) {
            bool sel = ( i == current );
            if ( ImGui::Selectable ( items[i], sel ) ) {
                g_eventlog_config.mode = (en_EVENTLOG_MODE) i;
                eventlog_recompute_active ( 0 );
            }
            if ( sel ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }
}


/**
 * @brief Vykreslí Capacity input + Clear button.
 *
 * Capacity ImGui InputInt s clamp do [MIN..MAX]. Klik "Apply" volá
 * @c eventlog_set_capacity() (= zahodí stávající data ringu). Bez
 * explicitního Apply změna InputIntu jen mutuje lokální buffer, ne
 * @c g_eventlog_config.capacity (= safety proti náhodnému resize během
 * editace).
 */
static void evw_render_capacity_controls ( void )
{
    static int s_pending_cap = -1;
    if ( s_pending_cap < 0 ) {
        s_pending_cap = (int) g_eventlog_config.capacity;
    }

    ImGui::SetNextItemWidth ( 120.0f );
    if ( ImGui::InputInt ( _L("Capacity##evw_cap"), &s_pending_cap, 0, 0 ) ) {
        if ( s_pending_cap < (int) EVENTLOG_MIN_CAPACITY )
            s_pending_cap = (int) EVENTLOG_MIN_CAPACITY;
        if ( s_pending_cap > (int) EVENTLOG_MAX_CAPACITY )
            s_pending_cap = (int) EVENTLOG_MAX_CAPACITY;
    }

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L("Apply##evw_cap_apply") ) ) {
        if ( (unsigned) s_pending_cap != g_eventlog_config.capacity ) {
            eventlog_set_capacity ( (size_t) s_pending_cap );
            g_eventlog_config.capacity = (unsigned) s_pending_cap;
        }
    }

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L("Clear##evw_clear") ) ) {
        eventlog_clear ( );
    }
}


/* ===========================================================================
 *  Export / Import toolbar controls (Vlna 4 Commit 29)
 * =========================================================================== */

/** Default cesta v Export popup-u (= relativní k current dir). */
static char s_export_path[ 512 ] = "eventlog_dump.evlog";

/** Default cesta v Import popup-u (= relativní k current dir). */
static char s_import_path[ 512 ] = "eventlog_dump.evlog";

/** Last toast text (= zobrazí se 4 s po Export/Import operaci). */
/* 600 = source msg[600] z evw_render_export_import_controls obsahuje
 * s_export_path[512] + prefix přes %s; toast buffer musí pojmout stejný worst-case. */
static char  s_evw_io_toast[ 600 ] = { 0 };

/** Last toast deadline v ImGui frame čase (= getTime + 4 s). */
static double s_evw_io_toast_until = 0.0;

/** Pre-import zjištěný count (= pro confirm dialog). */
static size_t s_pending_import_record_count = 0;


/**
 * @brief Zobrazí toast notifikaci po Export / Import operaci.
 *
 * Volá se z render path pokud @c s_evw_io_toast_until > ImGui::GetTime().
 * Layout: jednoduchý ImGui::Text v hlavním okně pod toolbarem. Bez
 * decay animace, jen on/off (= V1).
 */
static void evw_render_io_toast ( void )
{
    if ( s_evw_io_toast[0] == '\0' ) return;
    if ( ImGui::GetTime ( ) >= s_evw_io_toast_until ) {
        s_evw_io_toast[0] = '\0';
        return;
    }
    ImGui::SameLine ( );
    ImGui::TextColored ( ImVec4 ( 0.6f, 1.0f, 0.6f, 1.0f ), "%s", s_evw_io_toast );
}


/**
 * @brief Zaznamenat toast text + deadline 4 s.
 *
 * @param text Lokalizovaný / hotový text k zobrazení (kopíruje se do bufferu).
 */
static void evw_set_io_toast ( const char *text )
{
    if ( text == NULL ) text = "";
    snprintf ( s_evw_io_toast, sizeof ( s_evw_io_toast ), "%s", text );
    s_evw_io_toast_until = ImGui::GetTime ( ) + 4.0;
}


/**
 * @brief Peek hlavičky import souboru pro confirm dialog (= record_count).
 *
 * Otevře soubor, načte 32 B hlavičku, vrátí @c record_count nebo @c 0
 * při jakékoliv chybě. Plné importu provede až vlastní
 * @c eventlog_import_from_file po confirm.
 */
static size_t evw_peek_import_record_count ( const char *path )
{
    if ( path == NULL || path[0] == '\0' ) return 0;
    FILE *fp = fopen ( path, "rb" );
    if ( fp == NULL ) return 0;
    st_EVENTLOG_EXPORT_HEADER hdr;
    size_t rc = fread ( &hdr, sizeof ( hdr ), 1, fp );
    fclose ( fp );
    if ( rc != 1 ) return 0;
    if ( memcmp ( hdr.magic, EVENTLOG_EXPORT_MAGIC, EVENTLOG_EXPORT_MAGIC_LEN ) != 0 ) return 0;
    if ( hdr.version != EVENTLOG_EXPORT_VERSION ) return 0;
    if ( hdr.record_size != (uint32_t) sizeof ( st_EVENTLOG_EVENT ) ) return 0;
    return (size_t) hdr.record_count;
}


/**
 * @brief Vykreslí Export / Import tlačítka a jejich modal popupy.
 *
 * Layout (SameLine s Clear button v capacity controls):
 *
 *   [Export...]  [Import...]
 *
 * Export popup: file path InputText (default "eventlog_dump.evlog") +
 * Save / Cancel. Po Save zavolá @c eventlog_export_to_file(path) a
 * zobrazí toast s počtem zapsaných events.
 *
 * Import popup: file path InputText + Load / Cancel. Po Load proběhne
 * 2-stage confirm: peek hlavičky -> "Replace ring (N) with file (M)?"
 * -> Replace / Cancel. Replace volá @c eventlog_import_from_file(path).
 *
 * Pro V1 prostý ImGui popup - file picker (ImGuiFileDialog) je follow-up.
 */
static void evw_render_export_import_controls ( void )
{
    /* Export tlačítko - otevře "Export events" popup. */
    if ( ImGui::Button ( _L("Export...##evw_export_btn") ) ) {
        ImGui::OpenPopup ( "Export events##evw_export_popup" );
    }

    ImGui::SameLine ( );

    if ( ImGui::Button ( _L("Import...##evw_import_btn") ) ) {
        ImGui::OpenPopup ( "Import events##evw_import_popup" );
    }

    /* --- Export popup ---------------------------------------------------- */
    if ( ImGui::BeginPopupModal ( "Export events##evw_export_popup", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( "%s",
                      _("Export the in-memory event ring to a binary file.") );
        ImGui::Spacing ( );

        ImGui::SetNextItemWidth ( 400.0f );
        ImGui::InputText ( _L("File path##evw_export_path"),
                            s_export_path, sizeof ( s_export_path ) );

        ImGui::Text ( "%s %zu",
                      _("Events to write:"), eventlog_get_count ( ) );

        ImGui::Spacing ( );

        if ( ImGui::Button ( _L("Save##evw_export_save"), ImVec2 ( 120, 0 ) ) ) {
            int rc = eventlog_export_to_file ( s_export_path );
            /* 600 = "Exported %zu events to " (~25) + s_export_path max 511 + rezerva. */
            char msg[ 600 ];
            if ( rc == 0 ) {
                snprintf ( msg, sizeof ( msg ), "Exported %zu events to %s",
                           eventlog_get_count ( ), s_export_path );
            } else {
                snprintf ( msg, sizeof ( msg ), "Export FAILED: %s", s_export_path );
            }
            evw_set_io_toast ( msg );
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##evw_export_cancel"), ImVec2 ( 120, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::EndPopup ( );
    }

    /* --- Import popup --------------------------------------------------- */
    if ( ImGui::BeginPopupModal ( "Import events##evw_import_popup", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( "%s",
                      _("Replace the in-memory event ring with a previously exported file.") );
        ImGui::Spacing ( );

        ImGui::SetNextItemWidth ( 400.0f );
        ImGui::InputText ( _L("File path##evw_import_path"),
                            s_import_path, sizeof ( s_import_path ) );

        ImGui::Spacing ( );

        if ( ImGui::Button ( _L("Load##evw_import_load"), ImVec2 ( 120, 0 ) ) ) {
            s_pending_import_record_count = evw_peek_import_record_count ( s_import_path );
            if ( s_pending_import_record_count == 0 ) {
                /* Hlavička invalid - rovnou error toast, popup necháme otevřený
                 * aby user mohl upravit cestu. */
                evw_set_io_toast ( "Import FAILED: invalid file or header" );
            } else {
                ImGui::CloseCurrentPopup ( );
                ImGui::OpenPopup ( "Confirm import##evw_import_confirm" );
            }
        }
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##evw_import_cancel"), ImVec2 ( 120, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::EndPopup ( );
    }

    /* --- Confirm import popup ------------------------------------------- */
    if ( ImGui::BeginPopupModal ( "Confirm import##evw_import_confirm", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( "%s",
                      _("Replace the current ring with the file contents?") );
        ImGui::Text ( "%s %zu", _("Current ring:"), eventlog_get_count ( ) );
        ImGui::Text ( "%s %zu", _("File records:"), s_pending_import_record_count );
        ImGui::Spacing ( );

        if ( ImGui::Button ( _L("Replace##evw_import_replace"), ImVec2 ( 120, 0 ) ) ) {
            int rc = eventlog_import_from_file ( s_import_path );
            /* 600 = "Exported %zu events to " (~25) + s_export_path max 511 + rezerva. */
            char msg[ 600 ];
            if ( rc == 0 ) {
                snprintf ( msg, sizeof ( msg ), "Imported %zu events from %s",
                           eventlog_get_count ( ), s_import_path );
            } else {
                snprintf ( msg, sizeof ( msg ), "Import FAILED: %s", s_import_path );
            }
            evw_set_io_toast ( msg );
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##evw_import_confirm_cancel"), ImVec2 ( 120, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::EndPopup ( );
    }
}


/**
 * @brief Vykreslí Categories expand sekci s 24 checkboxy.
 *
 * Bity v @c g_eventlog_active_mask odpovídají hodnotě
 * @ref en_EVENTLOG_CATEGORY. Změna bitu se okamžitě promítne do
 * hot-path gate (= další eventy té kategorie nepojdou do ringu).
 *
 * Layout: 4 sloupce x 6 řádků, label = lowercase jméno z
 * @c eventlog_filter_cat_to_name().
 */
static void evw_render_categories_section ( void )
{
    if ( !ImGui::CollapsingHeader ( _L("Categories##evw_cats_hdr"),
                                     s_state.show_categories
                                        ? ImGuiTreeNodeFlags_DefaultOpen
                                        : 0 ) ) {
        return;
    }

    ImGui::Columns ( 4, "##evw_cat_cols", false );
    for ( int i = 0; i < (int) EVENTLOG_CAT_COUNT; i++ ) {
        const char *name = eventlog_filter_cat_to_name ( (uint8_t) i );
        if ( !name ) name = "?";

        bool on = ( g_eventlog_active_mask & ( UINT64_C(1) << i ) ) != 0;
        char id[64];
        snprintf ( id, sizeof ( id ), "%s##evw_cat_%d", name, i );
        if ( ImGui::Checkbox ( id, &on ) ) {
            if ( on ) {
                g_eventlog_active_mask |= ( UINT64_C(1) << i );
            } else {
                g_eventlog_active_mask &= ~( UINT64_C(1) << i );
            }
            g_eventlog_config.categories_mask = g_eventlog_active_mask;
        }
        ImGui::NextColumn ( );
    }
    ImGui::Columns ( 1 );
}


/**
 * @brief Vykreslí quick filter preset dropdown.
 *
 * Místo řady buttonů (= horizontální stres v toolbaru) je preset list
 * v ComboBox. ComboBox sám neukládá vybrané = po kliku na položku se
 * filter nasadí a combo se zavře (= "akční dropdown"). Reset/Clear
 * položka je součástí seznamu.
 */
static void evw_render_quick_filters ( void )
{
    ImGui::SetNextItemWidth ( 220.0f );
    /* Preview text combo - statický placeholder, ne dynamický (= combo
     * je akční, nevybrané hodnoty si nedrží). */
    if ( ImGui::BeginCombo ( _L("Quick filter##evw_qf_combo"),
                              _("(choose preset)"),
                              ImGuiComboFlags_HeightLarge ) ) {
        const int n = (int) ( sizeof ( k_quick_presets )
                              / sizeof ( k_quick_presets[0] ) );
        for ( int i = 0; i < n; i++ ) {
            char id[80];
            snprintf ( id, sizeof ( id ), "%s##evw_qf_item_%d",
                       k_quick_presets[i].label, i );
            if ( ImGui::Selectable ( _L(id) ) ) {
                evw_apply_quick_preset ( k_quick_presets[i].expr );
            }
            /* Tooltip s expression - user vidí co preset reálně dělá. */
            if ( ImGui::IsItemHovered ( ) ) {
                const char *e = k_quick_presets[i].expr;
                if ( !e || !*e ) e = "(empty - clears filter)";
                ImGui::SetTooltip ( "%s", e );
            }
        }
        ImGui::EndCombo ( );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _("Apply predefined filter expression.  Hover items for syntax preview.") );
    }
}


/* ===========================================================================
 *  Saved filter presets (Vlna 2 Commit 15)
 * =========================================================================== */

/**
 * @brief Maximální délka jména saved presetu (= INI klíč buffer).
 */
#define EVW_SAVED_PRESET_NAME_LEN     64u

/**
 * @brief Max počet uložených presetů per session (= max 32 cfg slotů).
 *
 * Limit prevention memory bloat. UI tlačítko "Save current as..." je
 * disabled pokud již @c s_saved_presets dosáhly tohoto limitu.
 */
#define EVW_SAVED_PRESET_MAX_COUNT    32u

/**
 * @brief User-defined saved filter preset.
 *
 * Drží name + filter expression string. Vektor @c s_saved_presets je
 * SSOT v runtime. Cfg persistence funguje přes fixní 32 slotů
 * @c preset_NN_name + @c preset_NN_expr v sekci @c [EVENT_LOG_FILTERS],
 * serializace probíhá při mutaci (Save / Delete) a před cfg save.
 *
 * @field name  User-friendly jméno (max 63 chars + NUL). Prázdné = slot
 *              nepoužitý (filter pro cfg propagation).
 * @field expr  Filter expression buffer (stejná velikost jako
 *              @c filter_text - lze ho do/z něj kopírovat 1:1).
 */
typedef struct st_EVW_SAVED_PRESET {
    char name[EVW_SAVED_PRESET_NAME_LEN];
    char expr[EVW_FILTER_BUF_LEN];
} st_EVW_SAVED_PRESET;

/** @brief Runtime SSOT vektor saved presetů (= user library). */
static std::vector<st_EVW_SAVED_PRESET> s_saved_presets;

/**
 * @brief Cfg-vlastněné string buffery pro perzistované sloty.
 *
 * Velikost @ref EVW_SAVED_PRESET_MAX_COUNT * 2 = 64 pointerů (name +
 * expr per slot). Lifecycle stejný jako @c s_state.cfg_filter_text -
 * cfgmodule vlastní paměť, my jen čteme po propagate a píšeme přes
 * @c cfgelement_set_value_text() před save.
 */
static char *s_cfg_saved_preset_name[EVW_SAVED_PRESET_MAX_COUNT] = { 0 };
static char *s_cfg_saved_preset_expr[EVW_SAVED_PRESET_MAX_COUNT] = { 0 };

/**
 * @brief Index aktuálně načteného presetu v combo preview (= -1 žádný).
 *
 * Mutuje se při kliku na položku v combo + při Save (= nový vybraný) +
 * při Delete (= reset na -1). Klik na Quick filter combo nebo manuální
 * edit filter textboxu hodnotu NEresetuje (= preview může lhát po
 * manuální editaci, což je akceptovatelné - user vidí name jako "co
 * naposledy načetl").
 */
static int s_saved_preset_active = -1;

/**
 * @brief Jednorázové flagy pro popupy "Save as" / "Delete" / "Replace".
 */
static bool s_saved_open_save_popup    = false;
static bool s_saved_open_delete_popup  = false;
static bool s_saved_open_replace_popup = false;

/**
 * @brief Buffer pro InputText "Preset name" v Save popupu.
 */
static char s_saved_name_buf[EVW_SAVED_PRESET_NAME_LEN] = { 0 };

/**
 * @brief Index slotu k nahrazení (Save popup zjistí, že name již
 *        existuje, otevře Replace confirm popup a uloží sem idx).
 */
static int s_saved_replace_idx = -1;


/**
 * @brief Vrátí počet aktuálně uložených presetů.
 */
static size_t evw_saved_preset_count ( void )
{
    return s_saved_presets.size ( );
}


/**
 * @brief Najde index presetu podle jména (= case-sensitive equal).
 *
 * @param name Hledané jméno (nesmí být @c NULL).
 * @return Index v @c s_saved_presets nebo @c -1 pokud nenalezeno.
 */
static int evw_saved_preset_find ( const char *name )
{
    if ( !name ) return -1;
    for ( size_t i = 0; i < s_saved_presets.size ( ); i++ ) {
        if ( strcmp ( s_saved_presets[i].name, name ) == 0 ) {
            return (int) i;
        }
    }
    return -1;
}


/**
 * @brief Přidá nový preset nebo nahradí existující se shodným jménem.
 *
 * Validation:
 *   - Empty / NULL @c name = no-op (caller má validovat).
 *   - Name > 63 chars = truncate.
 *   - Existing name = nahradí @c expr (= editace presetu).
 *   - Capacity full (@ref EVW_SAVED_PRESET_MAX_COUNT) = no-op pokud
 *     name neexistuje.
 *
 * Side effects:
 *   - Mutace @c s_saved_presets.
 *   - Po mutaci volá @c event_viewer_window_sync_filters_to_cfg() aby
 *     cfg buffer odpovídal stavu (= další save se uloží správně).
 *
 * @param name Jméno (max 63 chars, bude truncated).
 * @param expr Filter expression (může být prázdný = match all).
 * @return Index přidaného / nahrazeného presetu nebo @c -1 při chybě.
 */
static int evw_saved_preset_add ( const char *name, const char *expr )
{
    if ( !name || !name[0] ) return -1;
    if ( !expr ) expr = "";

    int existing = evw_saved_preset_find ( name );
    if ( existing >= 0 ) {
        g_strlcpy ( s_saved_presets[existing].expr, expr,
                    sizeof ( s_saved_presets[existing].expr ) );
        event_viewer_window_sync_filters_to_cfg ( );
        return existing;
    }

    if ( s_saved_presets.size ( ) >= EVW_SAVED_PRESET_MAX_COUNT ) {
        return -1;
    }

    st_EVW_SAVED_PRESET p;
    memset ( &p, 0, sizeof ( p ) );
    g_strlcpy ( p.name, name, sizeof ( p.name ) );
    g_strlcpy ( p.expr, expr, sizeof ( p.expr ) );
    s_saved_presets.push_back ( p );
    event_viewer_window_sync_filters_to_cfg ( );
    return (int) ( s_saved_presets.size ( ) - 1 );
}


/**
 * @brief Odstraní preset na daném indexu.
 *
 * Side effects:
 *   - Mutace @c s_saved_presets (erase).
 *   - Pokud @c s_saved_preset_active ukazoval na tento nebo vyšší
 *     index, posune se zpět (= reset na @c -1 pokud byl active mazaný).
 *   - Sync cfg.
 *
 * @param idx Index (out of range = no-op).
 */
static void evw_saved_preset_remove ( int idx )
{
    if ( idx < 0 || (size_t) idx >= s_saved_presets.size ( ) ) return;
    s_saved_presets.erase ( s_saved_presets.begin ( ) + idx );

    if ( s_saved_preset_active == idx ) {
        s_saved_preset_active = -1;
    } else if ( s_saved_preset_active > idx ) {
        s_saved_preset_active--;
    }
    event_viewer_window_sync_filters_to_cfg ( );
}


/**
 * @brief Const accessor pro položku vektoru saved presetů.
 *
 * @param idx Index (musí být v range).
 * @return Pointer na @c st_EVW_SAVED_PRESET nebo @c NULL při out of range.
 */
static const st_EVW_SAVED_PRESET * evw_saved_preset_get ( int idx )
{
    if ( idx < 0 || (size_t) idx >= s_saved_presets.size ( ) ) return NULL;
    return &s_saved_presets[(size_t) idx];
}


/**
 * @brief Vykreslí Saved filters combo + Save / Delete tlačítka.
 *
 * Layout (SameLine s ostatními toolbar prvky):
 *   [Saved filters: name v] [Save as...] [Delete]
 *
 * Combo preview = jméno aktivního presetu nebo "(none)". Klik na item
 * nasadí @c expr do @c s_state.filter_text + označí item jako active.
 * "Save as..." otevře InputText popup; pokud name již existuje, popup
 * se přepne na Replace confirm. "Delete" otevře confirm popup pro
 * aktivní preset (disabled pokud active < 0).
 */
static void evw_render_saved_presets ( void )
{
    ImGui::SetNextItemWidth ( 220.0f );

    const char *preview = _("(none)");
    if ( s_saved_preset_active >= 0
         && (size_t) s_saved_preset_active < s_saved_presets.size ( ) ) {
        preview = s_saved_presets[(size_t) s_saved_preset_active].name;
    }

    if ( ImGui::BeginCombo ( _L("Saved filters##evw_saved_combo"),
                              preview, ImGuiComboFlags_HeightLarge ) ) {
        const int n = (int) s_saved_presets.size ( );
        if ( n == 0 ) {
            ImGui::TextDisabled ( "%s",
                _("(no presets - use 'Save as...' to create one)") );
        }
        for ( int i = 0; i < n; i++ ) {
            /* 128 = name max EVW_SAVED_PRESET_NAME_LEN (64) + "##evw_saved_item_" (17) + %d (10) + rezerva. */
            char id[128];
            snprintf ( id, sizeof ( id ), "%s##evw_saved_item_%d",
                       s_saved_presets[(size_t) i].name, i );
            bool sel = ( i == s_saved_preset_active );
            if ( ImGui::Selectable ( id, sel ) ) {
                /* Nasaď expression do live filter bufferu + invalidate. */
                g_strlcpy ( s_state.filter_text,
                            s_saved_presets[(size_t) i].expr,
                            sizeof ( s_state.filter_text ) );
                s_state.filter_dirty = true;
                /* Sync cfg_filter_text aby další save zachoval text. */
                if ( s_state.cfg_filter_text ) {
                    /* cfgmodule vlastní paměť - bezpečnější je nechat
                     * cfg propagate při příští save (= save_handler
                     * pointer čte přímo z s_state.filter_text? Ne -
                     * je to oddělený buffer s vlastním g_strdup. Místo
                     * toho budeme číst přes cfgelement_get_text_save.
                     * Pro jednoduchost: s_state.cfg_filter_text se
                     * synchronizuje v _sync_filters_to_cfg(). Filter
                     * textbox má vlastní cfg klíč filter_expression
                     * v [EVENT_VIEWER_WINDOW] - ten se aktualizuje
                     * jiným handlerem (cfgmodule používá save_value_pointer
                     * = same pointer). Pro běžný flow stačí, že
                     * filter_text drží správnou hodnotu - save callback
                     * ji při shutdown přečte. */
                }
                s_saved_preset_active = i;
            }
            /* Tooltip s expression preview. */
            if ( ImGui::IsItemHovered ( ) ) {
                const char *e = s_saved_presets[(size_t) i].expr;
                if ( !e || !*e ) e = "(empty - matches all events)";
                ImGui::SetTooltip ( "%s", e );
            }
        }
        ImGui::EndCombo ( );
    }
    if ( ImGui::IsItemHovered ( ) && !ImGui::IsItemActive ( ) ) {
        ImGui::SetTooltip ( "%s",
            _("Apply user-saved filter expression. Save current with 'Save as...' button.") );
    }

    ImGui::SameLine ( );
    const bool save_disabled =
        ( s_saved_presets.size ( ) >= EVW_SAVED_PRESET_MAX_COUNT );
    if ( save_disabled ) ImGui::BeginDisabled ( );
    if ( ImGui::Button ( _L("Save as...##evw_saved_save_btn") ) ) {
        /* Reset InputText buffer + otevři popup. */
        s_saved_name_buf[0] = '\0';
        s_saved_open_save_popup = true;
    }
    if ( save_disabled ) ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        if ( save_disabled ) {
            ImGui::SetTooltip ( "%s",
                _("Maximum 32 presets - delete one to make space.") );
        } else {
            ImGui::SetTooltip ( "%s",
                _("Save current filter expression as a named preset.") );
        }
    }

    ImGui::SameLine ( );
    const bool delete_disabled = ( s_saved_preset_active < 0 );
    if ( delete_disabled ) ImGui::BeginDisabled ( );
    if ( ImGui::Button ( _L("Delete##evw_saved_del_btn") ) ) {
        s_saved_open_delete_popup = true;
    }
    if ( delete_disabled ) ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) && !delete_disabled ) {
        ImGui::SetTooltip ( "%s",
            _("Delete the currently selected preset.") );
    }
}


/**
 * @brief Render Save / Delete / Replace popupy (modal).
 *
 * Volá se z hlavního render flow po toolbaru. Tři popupy jsou nezávislé:
 *   - "Save preset" - InputText "Preset name" + Save/Cancel buttony.
 *     Validace: empty name = Save disabled. Tooltip varování pokud
 *     filter_text je prázdný. Pokud name již existuje, přepne se na
 *     Replace popup.
 *   - "Replace preset?" - confirm popup pokud user zadá duplicate name.
 *   - "Delete preset?" - confirm popup s name aktivního presetu.
 *
 * Pattern: @c s_saved_open_*_popup flag se nastaví v UI handleru,
 * @c OpenPopup() se volá zde JEN když flag je @c true, pak se flag
 * clearuje (= jednorázové otevření).
 */
static void evw_render_saved_presets_popups ( void )
{
    /* Volat SetKeyboardFocusHere() na InputText JEN při prvním renderu
     * popup okna. Jinak focus se každý frame vrací do textboxu a klik
     * na Save/Cancel se nikdy nedoručí (= ImGui ztratí button focus
     * než hover→click cyklus dokončí). */
    static bool s_save_popup_first_frame = false;

    /* ---- Save preset popup ---- */
    if ( s_saved_open_save_popup ) {
        ImGui::OpenPopup ( "###evw_saved_save_popup" );
        s_saved_open_save_popup = false;
        s_save_popup_first_frame = true;
    }
    if ( ImGui::BeginPopupModal ( _L("Save preset###evw_saved_save_popup"),
                                    NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( "%s", _("Preset name:") );
        ImGui::SetNextItemWidth ( 320.0f );
        if ( s_save_popup_first_frame ) {
            ImGui::SetKeyboardFocusHere ( );
            s_save_popup_first_frame = false;
        }
        ImGui::InputText ( "###evw_saved_save_input",
                            s_saved_name_buf, sizeof ( s_saved_name_buf ) );

        const bool name_empty = ( s_saved_name_buf[0] == '\0' );
        const bool expr_empty = ( s_state.filter_text[0] == '\0' );

        if ( name_empty ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.6f, 0.2f, 1.0f ),
                "%s", _("Name cannot be empty.") );
        }
        if ( expr_empty ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.8f, 0.2f, 1.0f ),
                "%s",
                _("Warning: saving an empty filter (matches all events).") );
        }

        ImGui::Text ( "%s %s", _("Expression:"),
                       s_state.filter_text[0] ? s_state.filter_text
                                              : _("(empty)") );

        ImGui::Separator ( );
        if ( name_empty ) ImGui::BeginDisabled ( );
        if ( ImGui::Button ( _L("Save##evw_saved_save_ok"),
                              ImVec2 ( 120.0f, 0.0f ) ) ) {
            int existing = evw_saved_preset_find ( s_saved_name_buf );
            if ( existing >= 0 ) {
                /* Duplicate - přepni na Replace confirm. */
                s_saved_replace_idx = existing;
                s_saved_open_replace_popup = true;
                ImGui::CloseCurrentPopup ( );
            } else {
                int new_idx = evw_saved_preset_add ( s_saved_name_buf,
                                                      s_state.filter_text );
                if ( new_idx >= 0 ) {
                    s_saved_preset_active = new_idx;
                }
                ImGui::CloseCurrentPopup ( );
            }
        }
        if ( name_empty ) ImGui::EndDisabled ( );
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##evw_saved_save_cancel"),
                              ImVec2 ( 120.0f, 0.0f ) ) ) {
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::EndPopup ( );
    }

    /* ---- Replace preset popup ---- */
    if ( s_saved_open_replace_popup ) {
        ImGui::OpenPopup ( "###evw_saved_replace_popup" );
        s_saved_open_replace_popup = false;
    }
    if ( ImGui::BeginPopupModal ( _L("Replace preset###evw_saved_replace_popup"),
                                    NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize ) ) {
        const char *existing_name =
            ( s_saved_replace_idx >= 0
              && (size_t) s_saved_replace_idx < s_saved_presets.size ( ) )
            ? s_saved_presets[(size_t) s_saved_replace_idx].name
            : "?";
        ImGui::Text ( _("Preset '%s' already exists. Replace?"),
                       existing_name );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L("Replace##evw_saved_repl_ok"),
                              ImVec2 ( 120.0f, 0.0f ) ) ) {
            int new_idx = evw_saved_preset_add ( s_saved_name_buf,
                                                  s_state.filter_text );
            if ( new_idx >= 0 ) {
                s_saved_preset_active = new_idx;
            }
            s_saved_replace_idx = -1;
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##evw_saved_repl_cancel"),
                              ImVec2 ( 120.0f, 0.0f ) ) ) {
            s_saved_replace_idx = -1;
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::EndPopup ( );
    }

    /* ---- Delete preset popup ---- */
    if ( s_saved_open_delete_popup ) {
        ImGui::OpenPopup ( "###evw_saved_delete_popup" );
        s_saved_open_delete_popup = false;
    }
    if ( ImGui::BeginPopupModal ( _L("Delete preset###evw_saved_delete_popup"),
                                    NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize ) ) {
        const char *active_name =
            ( s_saved_preset_active >= 0
              && (size_t) s_saved_preset_active < s_saved_presets.size ( ) )
            ? s_saved_presets[(size_t) s_saved_preset_active].name
            : "?";
        ImGui::Text ( _("Delete preset '%s'?"), active_name );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L("Delete##evw_saved_del_ok"),
                              ImVec2 ( 120.0f, 0.0f ) ) ) {
            evw_saved_preset_remove ( s_saved_preset_active );
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##evw_saved_del_cancel"),
                              ImVec2 ( 120.0f, 0.0f ) ) ) {
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::EndPopup ( );
    }
}


/**
 * @brief Vykreslí bookmark control řádek v toolbaru (Commit 14).
 *
 * Layout (SameLine):
 *   [★ N]   Prev   Next   [v] Show only ★   Clear all
 *
 * @c [★ N]    Statický text "Bookmarks (N)" s počtem.
 * @c Prev    Skok na předchozí bookmark od @c s_state.selected_idx.
 * @c Next    Skok na další bookmark od @c s_state.selected_idx.
 * @c Show only ★  Toggle filter override - skryje neoznačené eventy.
 * @c Clear all   Otevře confirm popup "Remove N bookmarks?".
 *
 * Side effects:
 *   - Prev/Next mutace @c s_state.selected_idx + nastavení
 *     @c want_scroll_to_idx (= Log tab po renderu auto-scrolluje).
 *   - "Show only ★" mutuje @c s_state.show_only_bookmarked.
 *   - "Clear all" otevře popup, který v případě potvrzení volá
 *     @c evw_bookmark_clear_all().
 *
 * Tlačítka Prev/Next jsou disabled pokud @c s_bookmarks.empty().
 * "Clear all" je disabled pokud nejsou žádné bookmarky.
 */
static void evw_render_bookmark_controls ( void )
{
    const size_t bm_count = evw_bookmark_count ( );

    /* Žlutý text "★ N" - informativní counter (žlutá pro vizuální
     * harmonii s bookmark glyfy v Log tab + outline ve Strip). */
    ImGui::PushStyleColor ( ImGuiCol_Text,
                             IM_COL32 ( 255, 220, 0, 255 ) );
    ImGui::Text ( "\xE2\x98\x85 %zu", bm_count );
    ImGui::PopStyleColor ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _("Bookmarks count.") );
    }

    ImGui::SameLine ( );
    ImGui::BeginDisabled ( bm_count == 0 );
    if ( ImGui::SmallButton ( _L("< Prev##evw_bm_prev") ) ) {
        const int idx = evw_bookmark_find_event_idx ( -1 );
        if ( idx >= 0 ) {
            s_state.selected_idx       = (int64_t) idx;
            s_state.want_scroll_to_idx = (int64_t) idx;
            s_state.follow_tail        = false;
            s_state.cfg_follow_tail    = 0u;
        }
    }
    ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _("Jump to previous bookmark.  Hotkey: Ctrl+Shift+B") );
    }

    ImGui::SameLine ( );
    ImGui::BeginDisabled ( bm_count == 0 );
    if ( ImGui::SmallButton ( _L("Next >##evw_bm_next") ) ) {
        const int idx = evw_bookmark_find_event_idx ( +1 );
        if ( idx >= 0 ) {
            s_state.selected_idx       = (int64_t) idx;
            s_state.want_scroll_to_idx = (int64_t) idx;
            s_state.follow_tail        = false;
            s_state.cfg_follow_tail    = 0u;
        }
    }
    ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _("Jump to next bookmark.  Hotkey: Ctrl+B") );
    }

    ImGui::SameLine ( );
    {
        bool sob = s_state.show_only_bookmarked;
        if ( ImGui::Checkbox ( _L("Show only \xE2\x98\x85###evw_bm_show_only"),
                                &sob ) ) {
            s_state.show_only_bookmarked = sob;
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _("Show only bookmarked events (overrides filter expression).") );
        }
    }

    ImGui::SameLine ( );
    ImGui::BeginDisabled ( bm_count == 0 );
    if ( ImGui::SmallButton ( _L("Clear all##evw_bm_clear") ) ) {
        s_state.confirm_clear_bookmarks_open = true;
    }
    ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _("Remove all bookmarks (confirmation required).") );
    }

    /* Confirm popup - "Remove N bookmarks?". Pattern OpenPopup +
     * BeginPopupModal - musí být ve scope dokud popup je otevřen.
     * Otevření per one-shot flag z Clear all tlačítka výše. */
    if ( s_state.confirm_clear_bookmarks_open ) {
        ImGui::OpenPopup ( "##evw_bm_clear_confirm" );
        s_state.confirm_clear_bookmarks_open = false;
    }
    if ( ImGui::BeginPopupModal ( "##evw_bm_clear_confirm", NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( _("Remove %zu bookmarks?"), bm_count );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L("Yes##evw_bm_clear_yes"),
                              ImVec2 ( 80, 0 ) ) ) {
            evw_bookmark_clear_all ( );
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##evw_bm_clear_no"),
                              ImVec2 ( 80, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        }
        ImGui::EndPopup ( );
    }
}


/**
 * @brief Vykreslí "Follow tail" toggle checkbox v toolbaru.
 *
 * Toggle button kontroluje auto-scroll Log tabulky na konec. Default ON
 * (= typický @c tail-f live debug UX). Klik na řádek tabulky / context
 * menu click-pause akce toggle vypne automaticky - vrátí se na ON jen
 * explicit klik tady. Hodnota persistovaná v cfg klíči @c follow_tail.
 */
static void evw_render_follow_tail_toggle ( void )
{
    bool ft = s_state.follow_tail;
    if ( ImGui::Checkbox ( _L("Follow tail###evw_follow"), &ft ) ) {
        s_state.follow_tail = ft;
        s_state.cfg_follow_tail = ft ? 1u : 0u;
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _("Auto-scroll Log table to newest event.  Hotkey: F") );
    }

    /* Row coloring toggle - barvy řádků v Log tabulce dle kategorie
     * (sdílí color picker se Strip tab). Default ON. */
    ImGui::SameLine ( );
    bool rc = s_state.row_coloring;
    if ( ImGui::Checkbox ( _L("Color rows###evw_row_color"), &rc ) ) {
        s_state.row_coloring = rc;
        s_state.cfg_row_coloring = rc ? 1u : 0u;
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _("Tint Log table rows by event category color (shared with Strip tab).") );
    }

    /* Group by combo (Vlna 4 Commit 27) - chronologický vs aggregate
     * pohled v Log tabu. Default None. Cfg klíč group_by. */
    ImGui::SameLine ( );
    const char *group_labels[EVW_GROUP_COUNT] = {
        N_("None"),
        N_("Frame"),
        N_("Category"),
        N_("PC"),
    };
    int gb_idx = s_state.group_by;
    if ( gb_idx < 0 || gb_idx >= (int) EVW_GROUP_COUNT ) gb_idx = EVW_GROUP_NONE;
    ImGui::SetNextItemWidth ( 110.0f );
    if ( ImGui::BeginCombo ( _L("Group by###evw_group_by"),
                              _( group_labels[gb_idx] ) ) ) {
        for ( int i = 0; i < (int) EVW_GROUP_COUNT; i++ ) {
            bool sel = ( i == gb_idx );
            /* Stable per-item ID nutný kvůli překladu labelu - dva různé
             * překlady stejného slova by jinak měly stejné ImGui ID. */
            char item_id[48];
            snprintf ( item_id, sizeof ( item_id ), "%s###evw_grp_item_%d",
                       _( group_labels[i] ), i );
            if ( ImGui::Selectable ( item_id, sel ) ) {
                s_state.group_by = i;
                s_state.cfg_group_by = (unsigned) i;
                /* Group by != None vypne follow_tail - per-group
                 * CollapsingHeadery nemají smysluplný "tail" stav. */
                if ( i != EVW_GROUP_NONE && s_state.follow_tail ) {
                    s_state.follow_tail = false;
                    s_state.cfg_follow_tail = 0u;
                }
            }
            if ( sel ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _("Group Log entries by Frame / Category / PC, or show chronologically (None).") );
    }

    /* Heatmap toggle (Vlna 4 Commit 28) - per-frame histogram nad Log
     * tabulkou ukazující distribuci eventů v čase (X = pxclk bin index,
     * Y = count, color stack per kategorie). Default OFF aby region
     * nezabíral místo když ho user nepoužívá. Cfg klíč heatmap. */
    ImGui::SameLine ( );
    bool hm = s_state.heatmap_enabled;
    if ( ImGui::Checkbox ( _L("Heatmap###evw_heatmap"), &hm ) ) {
        s_state.heatmap_enabled = hm;
        s_state.cfg_heatmap_enabled = hm ? 1u : 0u;
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _("Show per-frame event distribution histogram above Log table.") );
    }
}


/**
 * @brief Vykreslí filter řádek (InputText) + error badge + counters.
 */
static void evw_render_filter_row ( void )
{
    ImGui::SetNextItemWidth ( 280.0f );
    /* Placeholder hint zobrazený pokud je buffer prázdný (= rychlý hint
     * co tam napsat). Plný syntax help je v "?" tlačítku napravo. */
    if ( ImGui::InputTextWithHint ( _L("Filter##evw_filter"),
                                      _("cat:cpu_int pc:4000-40FF (! for negate)"),
                                      s_state.filter_text,
                                      sizeof ( s_state.filter_text ) ) ) {
        s_state.filter_dirty = true;
    }

    /* Help popup s plnou syntax referencí - "?" ikonka. */
    ImGui::SameLine ( );
    if ( ImGui::SmallButton ( _L("?##evw_filter_help") ) ) {
        ImGui::OpenPopup ( "##evw_filter_help_popup" );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _("Filter syntax help") );
    }
    if ( ImGui::BeginPopup ( "##evw_filter_help_popup" ) ) {
        ImGui::TextUnformatted ( _("Filter syntax (Tier 1):") );
        ImGui::Separator ( );
        ImGui::TextUnformatted (
            _("  cat:NAME[,NAME]   - per-category (cpu_int, gdg_mode, ...)") );
        ImGui::TextUnformatted (
            _("  sub:N[,N]         - per-subtype (uint8)") );
        ImGui::TextUnformatted (
            _("  pc:HEX            - PC exact match (e.g. pc:4042)") );
        ImGui::TextUnformatted (
            _("  pc:HEX-HEX        - PC range (e.g. pc:4000-40FF)") );
        ImGui::TextUnformatted (
            _("  frame:N / >N / <N - frame number") );
        ImGui::TextUnformatted (
            _("  cycle:N[M|k] / >N - pxclk range, M/k suffix") );
        ImGui::TextUnformatted (
            _("  sline:N-N         - scanline range (0..311 PAL)") );
        ImGui::TextUnformatted (
            _("  px:N-N            - pixel column range") );
        ImGui::TextUnformatted (
            _("  payload:HEX       - payload uint32 match") );
        ImGui::TextUnformatted (
            _("  !TOKEN            - negation (e.g. !cat:gdg_video)") );
        ImGui::TextUnformatted (
            _("  ( T1 or T2 )      - OR group (parens required)") );
        ImGui::TextUnformatted (
            _("  TOKEN TOKEN       - AND (whitespace separator)") );
        ImGui::Separator ( );
        ImGui::TextUnformatted ( _("Tier 2 (symbol-aware):") );
        ImGui::TextUnformatted (
            _("  sym:NAME / sym:PREFIX_*       - PC == addr of symbol(s)") );
        ImGui::TextUnformatted (
            _("  from_sym:A to_sym:B           - PC in [A.addr, B.addr]") );
        ImGui::Separator ( );
        ImGui::TextUnformatted ( _("Tier 3 (Vlna 4) - state-aware:") );
        ImGui::TextUnformatted (
            _("  if iff1:0|1                   - CPU interrupt enable") );
        ImGui::TextUnformatted (
            _("  if im:0|1|2                   - Z80 IM mode") );
        ImGui::TextUnformatted (
            _("  if reason:NAME                - BP fire reason") );
        ImGui::TextUnformatted (
            _("    (reset/ei/di/int_ack/nmi_ack/reti/retn/none)") );
        ImGui::TextUnformatted (
            _("  if banking:NAME               - memory banking summary") );
        ImGui::TextUnformatted (
            _("    (default/all_ram/rom_low_off/rom_high_off/") );
        ImGui::TextUnformatted (
            _("     cgrom/vram_640/pcg_high/other)") );
        ImGui::Separator ( );
        ImGui::TextUnformatted ( _("Tier 3 (Vlna 4) - temporal:") );
        ImGui::TextUnformatted (
            _("  before(N) <ref>               - events in last N pxclk before match") );
        ImGui::TextUnformatted (
            _("  after(M) <ref>                - events in M pxclk after match") );
        ImGui::TextUnformatted (
            _("  near(K) <ref>                 - events in window +-K around match") );
        ImGui::TextUnformatted (
            _("    N/M/K: dec, 'k' (=1000) / 'M' (=10^6) suffix") );
        ImGui::TextUnformatted (
            _("    <ref>: any sub-expression (cat:/sym:/if/...)") );
        ImGui::TextUnformatted (
            _("    Max nesting depth = 2 levels") );
        ImGui::Separator ( );
        ImGui::TextDisabled (
            _("Example: cat:bp_fire,user_mark ( pc:4000-40FF or pc:5000 )") );
        ImGui::TextDisabled (
            _("Example: cat:cpu_int if iff1:0  (= INT taken while IFF1=0)") );
        ImGui::TextDisabled (
            _("Example: before(1k) cat:irq_ack_im2  (= what preceded IRQ)") );
        ImGui::Separator ( );
        ImGui::TextUnformatted ( _("UI: Group by (Log tab toolbar):") );
        ImGui::TextUnformatted (
            _("  None / Frame / Category / PC - aggregates filtered") );
        ImGui::TextUnformatted (
            _("  events into collapsible groups. Filter syntax above") );
        ImGui::TextUnformatted (
            _("  applies first; grouping operates on filter result.") );
        ImGui::EndPopup ( );
    }

    /* Reparse pokud user změnil text. */
    if ( s_state.filter_dirty ) {
        evw_reparse_filter ( );
    }

    /* Syntax error badge (= červený text vedle textboxu). */
    const char *err = eventlog_filter_get_error ( s_state.filter_handle );
    if ( err ) {
        ImGui::SameLine ( );
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 220, 80, 80, 255 ) );
        ImGui::TextUnformatted ( _("[Syntax error]") );
        ImGui::PopStyleColor ( );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", err );
        }
    }

    /* Counters - cache per frame. */
    int frame_no = ImGui::GetFrameCount ( );
    if ( s_state.cached_frame != frame_no ) {
        s_state.cached_total    = eventlog_get_count ( );
        /* Temporal filter ctx (Vlna 4 Commit 26) - rozsah viditelných
         * eventů pro reference lookup. events=NULL = ctx_event_at()
         * deleguje na eventlog_get_event() (= read-only iterace ringem). */
        st_EVENTLOG_FILTER_CTX flt_ctx;
        flt_ctx.events = NULL;
        flt_ctx.count  = s_state.cached_total;

        size_t filtered = 0;
        for ( size_t i = 0; i < s_state.cached_total; i++ ) {
            const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
            if ( !e ) continue;
            /* Per-kategorie visibility gate (UI checkboxy). */
            if ( !( g_eventlog_active_mask
                    & ( UINT64_C(1) << e->category ) ) ) continue;
            if ( !eventlog_filter_match_ctx ( s_state.filter_handle, e, &flt_ctx ) ) continue;
            filtered++;
        }
        s_state.cached_filtered = filtered;
        s_state.cached_frame    = frame_no;
    }

    ImGui::SameLine ( );
    ImGui::Text ( _("Total: %zu  Filtered: %zu  Mask: 0x%016llX"),
                  s_state.cached_total, s_state.cached_filtered,
                  (unsigned long long) g_eventlog_active_mask );
}

/* ===========================================================================
 *  Click-pause helpers
 * =========================================================================== */

/**
 * @brief Provede click-pause akci na vybraný event.
 *
 * Side effects:
 *   - Pošle @c DBGAPI_CMD_PAUSE přes synchronní helper @c dbg_ui_pause().
 *     Pattern shodný s "Pause" tlačítkem v iconbaru (= F5 / Ctrl+F5).
 *   - Pokud @c focus_disasm je @c true, zavolá @c dbg_disasm_show_in_slot()
 *     na PC eventu (slot 0 = hlavní disasm instance).
 *   - Vypne @c follow_tail (= user chce konkrétní řádek, ne ho ztratit
 *     auto-scrollem).
 *
 * @param e             Pointer na event (nesmí být @c NULL).
 * @param focus_disasm  Pokud @c true, otevři disasm okno na @c e->pc.
 *
 * Thread safety: pouze UI vlákno. @c dbg_ui_pause() volá synchronní
 * CMDRQ s 200 ms timeoutem; pokud selže, UI flag @c g_debugger_paused se
 * dorovná na příštím framu standardní propagací (= side effect ignorován).
 */
static void evw_click_pause ( const st_EVENTLOG_EVENT *e, bool focus_disasm )
{
    if ( !e ) return;

    /* Pauza emu - synchronní helper (= pattern stejný jako iconbar
     * Pause tlačítko / Ctrl+F5 shortcut). */
    dbg_ui_pause ( );

    /* Disasm focus na PC eventu - reuse cross-window pattern z Commit 7. */
    if ( focus_disasm ) {
        dbg_disasm_show_in_slot ( 0, e->pc );
    }

    /* Vypnout Follow tail - user explicit vybral konkrétní řádek a
     * nechce ho ztratit příštím auto-scroll na konec. Vrátí se na ON
     * jen explicit toggle v toolbaru. */
    s_state.follow_tail = false;
    s_state.cfg_follow_tail = 0u;
}

/* ===========================================================================
 *  Log tab rendering
 * =========================================================================== */

/**
 * @brief Context popup obsah pro klik pravým tlačítkem na řádek tabulky.
 *
 * Položky:
 *   - "Pause emu + show in disasm" - kombinovaná click-pause akce
 *     (DBGAPI_CMD_PAUSE + @c dbg_disasm_show_in_slot na PC eventu).
 *   - "Pause emu here" - jen pause, bez disasm focus.
 *   - "Show in disasm" - @c dbg_disasm_show_in_slot(0, e->pc) (= focus
 *     hlavní disasm instance) bez pauzy.
 *   - "Show port in Overview" - jen pro IORQ_* / MMIO_* kategorie -
 *     otevře I/O Ports okno (@c g_gui->showIoWindow = true).
 *   - "Copy event to clipboard" - raw textový dump.
 *
 * @param e    Event pro který se renderuje popup.
 * @param idx  Index v ringu (= součást stable popup ID).
 */
static void evw_render_row_popup ( const st_EVENTLOG_EVENT *e, size_t idx )
{
    if ( !e ) return;

    char popup_id[64];
    snprintf ( popup_id, sizeof ( popup_id ), "##evw_row_ctx_%zu", idx );

    if ( ImGui::BeginPopup ( popup_id ) ) {
        if ( ImGui::MenuItem ( _L("Pause emu + show in disasm###evw_pause_disasm") ) ) {
            evw_click_pause ( e, true );
        }
        if ( ImGui::MenuItem ( _L("Pause emu here###evw_pause_here") ) ) {
            evw_click_pause ( e, false );
        }

        ImGui::Separator ( );

        if ( ImGui::MenuItem ( _L("Show in disasm##evw_row_disasm") ) ) {
            dbg_disasm_show_in_slot ( 0, e->pc );
        }

        bool is_io_or_mmio = ( e->category == EVENTLOG_CAT_IORQ_IN
                            || e->category == EVENTLOG_CAT_IORQ_OUT
                            || e->category == EVENTLOG_CAT_MMIO_R
                            || e->category == EVENTLOG_CAT_MMIO_W );
        if ( is_io_or_mmio ) {
            if ( ImGui::MenuItem ( _L("Show port in Overview##evw_row_io") ) ) {
                /* Otevři I/O Ports okno - okno samo načte aktuální stav
                 * portů; uživatel manuálně vyhledá konkrétní port v
                 * Overview tabulce. Hlubší cross-window propagace
                 * (= prefill filter na konkrétní port) přidá pozdější
                 * commit, až se sjednotí API se goto-port-overview
                 * mutantem. */
                g_gui->showIoWindow = true;
            }
        }

        ImGui::Separator ( );

        if ( ImGui::MenuItem ( _L("Copy to clipboard##evw_row_copy") ) ) {
            const char *cat_name = eventlog_filter_cat_to_name ( e->category );
            if ( !cat_name ) cat_name = "?";
            char detail[128];
            evw_format_detail ( e, detail, sizeof ( detail ) );
            char dump[256];
            uint32_t sline = 0, px = 0;
            evw_decode_raster ( e->pxclk_in_screen, &sline, &px );
            snprintf ( dump, sizeof ( dump ),
                       "frame=%u cycle=%llu sline=%u px=%u pc=0x%04X "
                       "cat=%s sub=%u %s",
                       (unsigned) e->screens_total,
                       (unsigned long long) e->pxclk_total,
                       (unsigned) sline, (unsigned) px,
                       (unsigned) e->pc, cat_name,
                       (unsigned) e->subtype, detail );
            ImGui::SetClipboardText ( dump );
        }

        ImGui::EndPopup ( );
    }
}


/**
 * @brief Vykreslí Log tab - tabulku všech eventů po filteru.
 *
 * Implementace iteruje sekvenčně přes celý ring - bez clipperu, protože
 * po filteru může být počet zobrazených řádků mnohem menší než
 * @c eventlog_get_count(). Pro velké ringy s žádným filterem to může
 * znamenat několik tisíc řádků renderovaných ImGuiem; pokud se ukáže
 * jako perf bottleneck, follow-up commit přidá clipper s pre-filtered
 * list cache.
 */
/**
 * @brief Bitmaska sloupců, které mají být v Log tabulce vynechány.
 *
 * Group by režim vynechá redundantní sloupec (= klíč skupiny zobrazený
 * v CollapsingHeader). Hodnoty jsou nezávislé bity → lze kombinovat
 * (zatím se kombinace nepoužívá, ale architektura je připravená).
 */
enum en_EVW_LOG_SKIP {
    EVW_LOG_SKIP_NONE  = 0,
    EVW_LOG_SKIP_FRAME = 1u << 0,
    EVW_LOG_SKIP_PC    = 1u << 1,
    EVW_LOG_SKIP_CAT   = 1u << 2,
};


/**
 * @brief Vykreslí jeden řádek Log tabulky pro daný event.
 *
 * Tělo bylo původně inline v @c evw_render_log_tab(). Vyčleněno pro
 * Group by režim (Vlna 4 Commit 27), kde se identická row logika
 * spouští z per-group tabulky se @c skip_cols maskou (= vynechá sloupce
 * redundantní s group key).
 *
 * Side effects:
 *   - Volá @c ImGui::TableNextRow() (= caller musí být uvnitř BeginTable).
 *   - Aplikuje per-row tinting podle @c s_state.row_coloring.
 *   - Mutuje @c s_state.selected_idx na klik / right-click.
 *   - Mutuje @c s_state.want_scroll_to_idx (= konzumace cross-tab requestu).
 *   - Otevírá row context popup přes @c evw_render_row_popup().
 *
 * @param e          Pointer na event, NESMÍ být NULL.
 * @param i          Globální event index v ringu (= stable ImGui ID).
 * @param skip_cols  Bitmaska sloupců k vynechání (@ref en_EVW_LOG_SKIP).
 */
static void evw_render_log_row ( const st_EVENTLOG_EVENT *e, size_t i,
                                  unsigned skip_cols )
{
    ImGui::TableNextRow ( );

    /* Row coloring - barevné pozadí řádku per kategorie (sdílí
     * s_strip_state.category_colors[] se Strip tab color pickerem).
     * Alpha 96 = výrazně viditelné, text bílý zůstává čitelný.
     * Zebra striping (RowBg flag) je vypnut když row_coloring ON,
     * jinak by přepsal override na lichých řádcích. */
    if ( s_state.row_coloring ) {
        ImU32 base = s_strip_state.category_colors[e->category];
        ImU32 r = ( base       ) & 0xFFu;
        ImU32 g = ( base >>  8 ) & 0xFFu;
        ImU32 b = ( base >> 16 ) & 0xFFu;
        ImU32 tinted = IM_COL32 ( r, g, b, 96 );
        ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg0, tinted );
    }

    uint32_t sline = 0, px = 0;
    evw_decode_raster ( e->pxclk_in_screen, &sline, &px );

    /* První sloupec = Pxclk + Selectable spanning přes celý řádek (=
     * clickable hit area pro click-pause UX). */
    ImGui::TableNextColumn ( );
    char row_label[48];
    snprintf ( row_label, sizeof ( row_label ), "%llu##evw_row_%zu",
               (unsigned long long) e->pxclk_total, i );
    bool is_selected = ( s_state.selected_idx == (int64_t) i );
    ImGuiSelectableFlags sel_flags = ImGuiSelectableFlags_SpanAllColumns
                                   | ImGuiSelectableFlags_AllowDoubleClick
                                   | ImGuiSelectableFlags_AllowOverlap;
    if ( ImGui::Selectable ( row_label, is_selected, sel_flags ) ) {
        s_state.selected_idx = (int64_t) i;
        if ( ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) ) {
            evw_click_pause ( e, true );
        }
    }
    if ( ImGui::IsItemHovered ( ) && !ImGui::IsMouseDown ( ImGuiMouseButton_Left ) ) {
        ImGui::SetTooltip ( "%s",
            _("Double-click to pause emu + show in disasm") );
    }

    /* Frame (lze vynechat v Group by Frame). */
    if ( !( skip_cols & EVW_LOG_SKIP_FRAME ) ) {
        ImGui::TableNextColumn ( );
        ImGui::Text ( "%u", (unsigned) e->screens_total );
    }

    ImGui::TableNextColumn ( );
    ImGui::Text ( "%u", (unsigned) sline );

    ImGui::TableNextColumn ( );
    ImGui::Text ( "%u", (unsigned) px );

    /* PC (lze vynechat v Group by PC). */
    if ( !( skip_cols & EVW_LOG_SKIP_PC ) ) {
        ImGui::TableNextColumn ( );
        ImGui::Text ( "0x%04X", (unsigned) e->pc );
    }

    /* Bookmark column - klikatelná hvězda. */
    ImGui::TableNextColumn ( );
    {
        const bool is_bm = evw_bookmark_contains ( e->screens_total,
                                                   e->pxclk_in_screen );
        const ImU32 col_on  = IM_COL32 ( 255, 220,  0, 255 );
        const ImU32 col_off = IM_COL32 ( 110, 110, 110, 200 );
        ImGui::PushStyleColor ( ImGuiCol_Text,
                                 is_bm ? col_on : col_off );
        char bm_label[40];
        snprintf ( bm_label, sizeof ( bm_label ),
                   "\xE2\x98\x85##evw_bm_%zu", i );
        if ( ImGui::Selectable ( bm_label, false,
                                  ImGuiSelectableFlags_None,
                                  ImVec2 ( 18.0f, 0.0f ) ) ) {
            evw_bookmark_toggle ( e->screens_total,
                                   e->pxclk_in_screen );
        }
        ImGui::PopStyleColor ( );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                is_bm ? _("Remove bookmark")
                      : _("Add bookmark") );
        }
    }

    /* Cat (lze vynechat v Group by Category). */
    if ( !( skip_cols & EVW_LOG_SKIP_CAT ) ) {
        ImGui::TableNextColumn ( );
        const char *cat_name = eventlog_filter_cat_to_name ( e->category );
        ImGui::TextUnformatted ( cat_name ? cat_name : "?" );
    }

    ImGui::TableNextColumn ( );
    char sub_label[16];
    evw_format_subtype ( e->category, e->subtype, sub_label,
                          sizeof ( sub_label ) );
    ImGui::TextUnformatted ( sub_label );
    if ( ImGui::IsItemHovered ( ) ) {
        char sub_full[ 96 ];
        evw_format_subtype_full ( e->category, e->subtype,
                                   sub_full, sizeof ( sub_full ) );
        ImGui::SetTooltip ( "%s", sub_full );
    }

    ImGui::TableNextColumn ( );
    char detail[128];
    evw_format_detail ( e, detail, sizeof ( detail ) );
    ImGui::TextUnformatted ( detail );

    /* Right-click detekce na posledním cellu řádku (= Detail text)
     * pro context popup. Stejné chování jako Vlna 1 Commit 7. */
    char popup_id[64];
    snprintf ( popup_id, sizeof ( popup_id ), "##evw_row_ctx_%zu", i );
    if ( ImGui::IsItemHovered ( )
         && ImGui::IsMouseClicked ( ImGuiMouseButton_Right ) ) {
        s_state.selected_idx = (int64_t) i;
        ImGui::OpenPopup ( popup_id );
    }
    evw_render_row_popup ( e, i );

    /* Cross-tab scroll request konzumace. */
    if ( s_state.want_scroll_to_idx == (int64_t) i ) {
        ImGui::SetScrollHereY ( 0.5f );
        s_state.want_scroll_to_idx = -1;
    }
}


/**
 * @brief Vykreslí TableSetupColumn hlavičky podle @c skip_cols masky.
 *
 * Pro každý ID musí být ImGui ID stable per call - @c id_suffix slouží
 * jako differentiator (= různé tabulky v různých group-by skupinách
 * dostanou různé IDs aby ImGui neresetoval per-sloupcové width).
 *
 * @param skip_cols  Bitmaska sloupců k vynechání (@ref en_EVW_LOG_SKIP).
 * @param id_suffix  String suffix pro stable ID per tabulka (např.
 *                   "main" pro chronological, "g42" pro group 42).
 *                   Smí být NULL = použije se "main".
 */
static void evw_log_setup_columns ( unsigned skip_cols, const char *id_suffix )
{
    if ( !id_suffix ) id_suffix = "main";
    char buf[64];

    ImGui::TableSetupScrollFreeze ( 0, 1 );

    snprintf ( buf, sizeof ( buf ), "%s##evw_col_pxclk_%s", _("Pxclk"), id_suffix );
    ImGui::TableSetupColumn ( buf, ImGuiTableColumnFlags_WidthFixed, 100.0f );

    if ( !( skip_cols & EVW_LOG_SKIP_FRAME ) ) {
        snprintf ( buf, sizeof ( buf ), "%s##evw_col_frame_%s", _("Frame"), id_suffix );
        ImGui::TableSetupColumn ( buf, ImGuiTableColumnFlags_WidthFixed, 56.0f );
    }

    snprintf ( buf, sizeof ( buf ), "%s##evw_col_sline_%s", _("Sline"), id_suffix );
    ImGui::TableSetupColumn ( buf, ImGuiTableColumnFlags_WidthFixed, 48.0f );

    snprintf ( buf, sizeof ( buf ), "%s##evw_col_px_%s", _("Px"), id_suffix );
    ImGui::TableSetupColumn ( buf, ImGuiTableColumnFlags_WidthFixed, 44.0f );

    if ( !( skip_cols & EVW_LOG_SKIP_PC ) ) {
        snprintf ( buf, sizeof ( buf ), "%s##evw_col_pc_%s", _("PC"), id_suffix );
        ImGui::TableSetupColumn ( buf, ImGuiTableColumnFlags_WidthFixed, 60.0f );
    }

    /* Bookmark sloupec - vždy přítomen. */
    snprintf ( buf, sizeof ( buf ), "\xE2\x98\x85##evw_col_bm_%s", id_suffix );
    ImGui::TableSetupColumn ( buf, ImGuiTableColumnFlags_WidthFixed, 26.0f );

    if ( !( skip_cols & EVW_LOG_SKIP_CAT ) ) {
        snprintf ( buf, sizeof ( buf ), "%s##evw_col_cat_%s", _("Cat"), id_suffix );
        ImGui::TableSetupColumn ( buf, ImGuiTableColumnFlags_WidthFixed, 110.0f );
    }

    snprintf ( buf, sizeof ( buf ), "%s##evw_col_sub_%s", _("Sub"), id_suffix );
    ImGui::TableSetupColumn ( buf, ImGuiTableColumnFlags_WidthFixed, 80.0f );

    snprintf ( buf, sizeof ( buf ), "%s##evw_col_detail_%s", _("Detail"), id_suffix );
    ImGui::TableSetupColumn ( buf, ImGuiTableColumnFlags_WidthStretch );

    ImGui::TableHeadersRow ( );
}


/**
 * @brief Spočítá počet sloupců tabulky pro daný @c skip_cols.
 *
 * Plná tabulka = 9 sloupců (Pxclk, Frame, Sline, Px, PC, ★, Cat, Sub, Detail).
 * Každý bit v @c skip_cols ubere jeden sloupec.
 *
 * @param skip_cols  Bitmaska sloupců k vynechání.
 * @return  Počet sloupců pro @c ImGui::BeginTable().
 */
static int evw_log_column_count ( unsigned skip_cols )
{
    int n = 9;
    if ( skip_cols & EVW_LOG_SKIP_FRAME ) n--;
    if ( skip_cols & EVW_LOG_SKIP_PC )    n--;
    if ( skip_cols & EVW_LOG_SKIP_CAT )   n--;
    return n;
}


/**
 * @brief Jedna skupina eventů pro Group by režim.
 *
 * Naplnění probíhá per render frame v @c evw_render_log_tab() po průchodu
 * filter pipeline (= category mask AND filter match AND show-only-★).
 * Sort kritérium podle @ref s_state.group_by:
 *   - @c EVW_GROUP_FRAME    -> @c key = @c screens_total, ascending.
 *   - @c EVW_GROUP_CATEGORY -> @c key = @c category, enum order.
 *   - @c EVW_GROUP_PC       -> @c key = @c pc, ascending.
 *
 * @field key      Hodnota klíče skupiny (= sjednocené pole 32 bitů).
 * @field indices  Globální indexy eventů v ringu, v původním pořadí
 *                 (= chronologicky uvnitř skupiny).
 */
struct st_EVW_GROUP {
    uint32_t key;
    std::vector<size_t> indices;
};


/**
 * @brief Vykreslí chronologický (= Group by None) výpis Log tabulky.
 *
 * Side effects: viz @ref evw_render_log_row().
 */
static void evw_render_log_chronological ( void )
{
    ImGuiTableFlags flags = ImGuiTableFlags_Borders
                          | ImGuiTableFlags_Resizable
                          | ImGuiTableFlags_ScrollY
                          | ImGuiTableFlags_SizingStretchProp;
    if ( !s_state.row_coloring ) {
        flags |= ImGuiTableFlags_RowBg;
    }

    if ( !ImGui::BeginTable ( "##evw_log_table", 9, flags ) ) return;

    evw_log_setup_columns ( EVW_LOG_SKIP_NONE, "main" );

    size_t total = eventlog_get_count ( );
    /* Temporal filter ctx (Vlna 4 Commit 26) - viz comments u counter cache. */
    st_EVENTLOG_FILTER_CTX flt_ctx;
    flt_ctx.events = NULL;
    flt_ctx.count  = total;
    size_t rendered_count = 0;
    for ( size_t i = 0; i < total; i++ ) {
        const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
        if ( !e ) continue;

        if ( !( g_eventlog_active_mask
                & ( UINT64_C(1) << e->category ) ) ) continue;
        if ( !eventlog_filter_match_ctx ( s_state.filter_handle, e, &flt_ctx ) ) continue;
        if ( s_state.show_only_bookmarked
             && !evw_bookmark_contains ( e->screens_total,
                                          e->pxclk_in_screen ) ) {
            continue;
        }

        evw_render_log_row ( e, i, EVW_LOG_SKIP_NONE );
        rendered_count++;
    }

    /* Auto-scroll na konec při follow_tail = ON. */
    if ( s_state.follow_tail && rendered_count > 0 ) {
        ImGui::SetScrollHereY ( 1.0f );
    }

    ImGui::EndTable ( );
}


/**
 * @brief Vrátí klíč skupiny pro daný event a aktuální @c group_by.
 *
 * @param e         Pointer na event (nesmí být NULL).
 * @param group_by  Aktuální režim (@ref en_EVW_GROUP_BY).
 * @return  32-bit klíč použitelný jako sort kritérium + map key.
 */
static uint32_t evw_event_group_key ( const st_EVENTLOG_EVENT *e, int group_by )
{
    switch ( group_by ) {
        case EVW_GROUP_FRAME:    return (uint32_t) e->screens_total;
        case EVW_GROUP_CATEGORY: return (uint32_t) e->category;
        case EVW_GROUP_PC:       return (uint32_t) e->pc;
        default:                 return 0u;
    }
}


/**
 * @brief Sestaví header label pro CollapsingHeader skupiny.
 *
 * Formát podle @c group_by:
 *   - Frame:    "Frame N (M events)"
 *   - Category: "CAT_NAME (M events)"
 *   - PC:       "0xXXXX (M events)"
 *
 * @param group_by    Režim grupování.
 * @param key         Klíč skupiny.
 * @param count       Počet eventů ve skupině.
 * @param idx_in_seq  Sekvenční index skupiny (= stable suffix pro ImGui ID).
 * @param buf         Výstupní buffer (min 96 B doporučeno).
 * @param buf_size    Velikost @c buf.
 */
static void evw_format_group_header ( int group_by, uint32_t key, size_t count,
                                       size_t idx_in_seq, char *buf, size_t buf_size )
{
    switch ( group_by ) {
        case EVW_GROUP_FRAME:
            snprintf ( buf, buf_size, "%s %u (%zu %s)###evw_grp_%zu",
                       _("Frame"), (unsigned) key, count, _("events"), idx_in_seq );
            break;
        case EVW_GROUP_CATEGORY: {
            const char *cn = eventlog_filter_cat_to_name ( (uint8_t) key );
            snprintf ( buf, buf_size, "%s (%zu %s)###evw_grp_%zu",
                       cn ? cn : "?", count, _("events"), idx_in_seq );
            break;
        }
        case EVW_GROUP_PC:
            snprintf ( buf, buf_size, "0x%04X (%zu %s)###evw_grp_%zu",
                       (unsigned) key, count, _("events"), idx_in_seq );
            break;
        default:
            snprintf ( buf, buf_size, "?###evw_grp_%zu", idx_in_seq );
            break;
    }
}


/**
 * @brief Vykreslí Log tabulku v Group by režimu (Frame / Category / PC).
 *
 * Algoritmus:
 *   1. Pass 1: scan ringu, pro každý visible+matched event akumuluj
 *      do hash mapy @c key->indices.
 *   2. Sort skupiny podle klíče (ascending; pro Category = enum order).
 *   3. Pass 2: per skupina render CollapsingHeader + BeginTable + řádky.
 *
 * Per-frame O(N log G) kde N = visible events, G = počet skupin.
 * Pro N=50000 reálně <16 ms (= acceptable pro 60 fps render budget).
 * Cache mezi rendery = follow-up, V1 spoléhá na ImGui MultiThreaded
 * skip pokud frame nemá co měnit.
 *
 * @param group_by  Aktivní režim (@ref en_EVW_GROUP_BY), nesmí být
 *                  @c EVW_GROUP_NONE (= caller volá chronological path).
 */
static void evw_render_log_grouped ( int group_by )
{
    size_t total = eventlog_get_count ( );
    st_EVENTLOG_FILTER_CTX flt_ctx;
    flt_ctx.events = NULL;
    flt_ctx.count  = total;

    /* Pass 1: bucket events do skupin. Použijeme prostý vector + sekvenční
     * hledání klíče - pro typický počet skupin (<300) je rychlejší než
     * std::map / std::unordered_map (= hash overhead u malé G). */
    std::vector<st_EVW_GROUP> groups;
    groups.reserve ( 32 );

    for ( size_t i = 0; i < total; i++ ) {
        const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
        if ( !e ) continue;

        if ( !( g_eventlog_active_mask
                & ( UINT64_C(1) << e->category ) ) ) continue;
        if ( !eventlog_filter_match_ctx ( s_state.filter_handle, e, &flt_ctx ) ) continue;
        if ( s_state.show_only_bookmarked
             && !evw_bookmark_contains ( e->screens_total,
                                          e->pxclk_in_screen ) ) {
            continue;
        }

        uint32_t key = evw_event_group_key ( e, group_by );

        /* Lineární hledání v groups vektoru - pro >300 skupin by se to
         * vyplatilo nahradit hashmapou; pro typický debug session
         * (10-100 skupin) je linear OK. */
        bool found = false;
        for ( size_t g = 0; g < groups.size ( ); g++ ) {
            if ( groups[g].key == key ) {
                groups[g].indices.push_back ( i );
                found = true;
                break;
            }
        }
        if ( !found ) {
            st_EVW_GROUP grp;
            grp.key = key;
            grp.indices.push_back ( i );
            groups.push_back ( grp );
        }
    }

    /* Sort skupiny ascending podle klíče (Frame/PC numeric, Category
     * enum order = také ascending uint hodnota). Stable sort není
     * nutný - klíče jsou unikátní v rámci skupin. */
    std::sort ( groups.begin ( ), groups.end ( ),
                [] ( const st_EVW_GROUP &a, const st_EVW_GROUP &b ) {
                    return a.key < b.key;
                } );

    /* Skip mask pro per-group tabulku - vynechá sloupec redundantní
     * s group key (= zobrazený v CollapsingHeader). */
    unsigned skip_cols = EVW_LOG_SKIP_NONE;
    switch ( group_by ) {
        case EVW_GROUP_FRAME:    skip_cols = EVW_LOG_SKIP_FRAME; break;
        case EVW_GROUP_CATEGORY: skip_cols = EVW_LOG_SKIP_CAT;   break;
        case EVW_GROUP_PC:       skip_cols = EVW_LOG_SKIP_PC;    break;
        default: break;
    }
    int col_count = evw_log_column_count ( skip_cols );

    /* Pass 2: render per skupina jako CollapsingHeader + tabulka. Každá
     * tabulka má vlastní stable ID (= "g42") aby ImGui per-sloupcové
     * width zachoval mezi rendery samostatně. */
    for ( size_t gi = 0; gi < groups.size ( ); gi++ ) {
        const st_EVW_GROUP &g = groups[gi];
        char header[96];
        evw_format_group_header ( group_by, g.key, g.indices.size ( ),
                                   gi, header, sizeof ( header ) );

        if ( !ImGui::CollapsingHeader ( header ) ) continue;

        char tbl_id[64];
        snprintf ( tbl_id, sizeof ( tbl_id ), "##evw_log_table_g%zu", gi );

        ImGuiTableFlags flags = ImGuiTableFlags_Borders
                              | ImGuiTableFlags_Resizable
                              | ImGuiTableFlags_SizingStretchProp;
        if ( !s_state.row_coloring ) {
            flags |= ImGuiTableFlags_RowBg;
        }

        if ( !ImGui::BeginTable ( tbl_id, col_count, flags ) ) continue;

        char id_suffix[32];
        snprintf ( id_suffix, sizeof ( id_suffix ), "g%zu", gi );
        evw_log_setup_columns ( skip_cols, id_suffix );

        for ( size_t k = 0; k < g.indices.size ( ); k++ ) {
            size_t i = g.indices[k];
            const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
            if ( !e ) continue;
            evw_render_log_row ( e, i, skip_cols );
        }

        ImGui::EndTable ( );
    }
}


/**
 * @brief File-static buffer per-frame heatmap binů.
 *
 * Resetovaný @c memset(0) na začátku každého renderu heatmapy. Po
 * naplnění (= scan visible+matched eventů) se použije pro vykreslení
 * stack chart bars. Lifecycle per render Log tabu, žádná cross-frame
 * cache (= V1 dělá O(N) scan, optimalizace přijde pokud profiling
 * ukáže potřebu).
 */
static st_EVW_HEATMAP_BIN s_heatmap[EVW_HEATMAP_BIN_COUNT];


/**
 * @brief Vykreslí per-frame heatmapu nad Log tabulkou (Vlna 4 Commit 28).
 *
 * Algoritmus:
 *   1. Reset @c s_heatmap (memset 0).
 *   2. Scan ringu - pro každý visible+matched event spočítá bin index z
 *      @c pxclk_in_screen a inkrementuje @c total + @c per_cat[category].
 *   3. Najde max bin total (= self-normalizing Y-osa).
 *   4. Render bars přes @c ImDrawList::AddRectFilled v invisible BeginChild
 *      regionu. Color stack per kategorie (= reuse
 *      @c s_strip_state.category_colors[]).
 *   5. Hover detekce + tooltip s bin range (pxclk + sline) + total + top
 *      kategorie. Klik = scroll Log na první event v binu (= reuse
 *      @c want_scroll_to_idx mechanismus).
 *
 * Filter pipeline shodný s @c evw_render_log_chronological() - aplikuje
 * category mask, filter expression i show_only_bookmarked. Volí stejnou
 * sadu eventů jako co tabulka pod heatmapou ukáže (= konzistence).
 *
 * Side effects:
 *   - Mutuje @c s_heatmap (file-static buffer).
 *   - Klik na bin: nastaví @c s_state.want_scroll_to_idx + vypne
 *     @c follow_tail (= aby scroll nepřeskočil zpět na konec).
 *
 * @pre  @c s_state.heatmap_enabled == @c true (caller guard).
 * @post @c s_heatmap obsahuje histogram pro tento render.
 */
static void evw_render_log_heatmap ( void )
{
    /* Bin size v pxclk - celý frame (= g_mzhal.video_screen_width * HEIGHT)
     * rozdělený na EVW_HEATMAP_BIN_COUNT stejných binů. */
    const uint32_t bin_size_pxclk =
        ( (uint32_t) g_mzhal.video_screen_width * (uint32_t) g_mzhal.video_screen_height )
        / EVW_HEATMAP_BIN_COUNT;

    /* Pass 1: reset bins. */
    memset ( s_heatmap, 0, sizeof ( s_heatmap ) );

    /* Pass 2: scan ringu, akumulace visible+matched eventů. Filter
     * pipeline shodný s evw_render_log_chronological. */
    size_t total = eventlog_get_count ( );
    st_EVENTLOG_FILTER_CTX flt_ctx;
    flt_ctx.events = NULL;
    flt_ctx.count  = total;

    for ( size_t i = 0; i < total; i++ ) {
        const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
        if ( !e ) continue;

        if ( !( g_eventlog_active_mask
                & ( UINT64_C(1) << e->category ) ) ) continue;
        if ( !eventlog_filter_match_ctx ( s_state.filter_handle, e, &flt_ctx ) ) continue;
        if ( s_state.show_only_bookmarked
             && !evw_bookmark_contains ( e->screens_total,
                                          e->pxclk_in_screen ) ) {
            continue;
        }

        uint32_t bin_idx = ( bin_size_pxclk > 0u )
                            ? ( e->pxclk_in_screen / bin_size_pxclk )
                            : 0u;
        if ( bin_idx >= EVW_HEATMAP_BIN_COUNT ) {
            bin_idx = EVW_HEATMAP_BIN_COUNT - 1u;
        }
        s_heatmap[bin_idx].total++;
        if ( (int) e->category < (int) EVENTLOG_CAT_COUNT ) {
            s_heatmap[bin_idx].per_cat[e->category]++;
        }
    }

    /* Pass 3a: max bin total pro Y normalizaci (= self-normalizing). */
    uint32_t max_count = 0;
    for ( int i = 0; i < EVW_HEATMAP_BIN_COUNT; i++ ) {
        if ( s_heatmap[i].total > max_count ) max_count = s_heatmap[i].total;
    }

    /* Pass 3b: render bars v invisible child regionu. Borders aby uživatel
     * měl jasný rámeček heatmapy i když je prázdná. */
    ImGui::BeginChild ( "##evw_heatmap", ImVec2 ( 0.0f, EVW_HEATMAP_HEIGHT_PX ),
                        ImGuiChildFlags_Borders );

    ImVec2 origin    = ImGui::GetCursorScreenPos ( );
    ImDrawList *dl   = ImGui::GetWindowDrawList ( );
    float avail_w    = ImGui::GetContentRegionAvail ( ).x;
    float child_h    = ImGui::GetContentRegionAvail ( ).y;
    if ( child_h < 8.0f ) child_h = 8.0f;
    float bin_w      = ( avail_w > 0.0f )
                         ? ( avail_w / (float) EVW_HEATMAP_BIN_COUNT )
                         : 0.0f;

    if ( max_count == 0u ) {
        /* Žádné visible eventy v tomto framu - jen placeholder text. */
        ImGui::TextDisabled ( "%s", _("(no events in heatmap range)") );
    } else if ( bin_w > 0.0f ) {
        const float y_bottom_base = origin.y + child_h - 2.0f;
        const float bar_max_h     = child_h - 4.0f;
        for ( int i = 0; i < EVW_HEATMAP_BIN_COUNT; i++ ) {
            if ( s_heatmap[i].total == 0u ) continue;

            float x0 = origin.x + (float) i * bin_w;
            float x1 = x0 + bin_w - 1.0f;
            if ( x1 <= x0 ) x1 = x0 + 1.0f;

            /* Stack per kategorie zdola nahoru. Použije sdílené Strip
             * barvy aby koleace mezi histogramem a Strip canvasem byla
             * jasná. */
            float y_bottom = y_bottom_base;
            for ( int cat = 0; cat < (int) EVENTLOG_CAT_COUNT; cat++ ) {
                uint32_t c = s_heatmap[i].per_cat[cat];
                if ( c == 0u ) continue;
                float seg_h = ( (float) c / (float) max_count ) * bar_max_h;
                if ( seg_h < 1.0f ) seg_h = 1.0f;
                ImU32 color = s_strip_state.initialized
                                ? s_strip_state.category_colors[cat]
                                : s_default_category_colors[cat];
                dl->AddRectFilled ( ImVec2 ( x0, y_bottom - seg_h ),
                                    ImVec2 ( x1, y_bottom ),
                                    color );
                y_bottom -= seg_h;
            }
        }
    }

    /* Hover/click detekce - invisible button přes celou plochu pro
     * IsItemHovered + IsItemClicked. Pozici myši nad ním přepočítáme
     * na bin index. */
    ImGui::SetCursorScreenPos ( origin );
    ImGui::InvisibleButton ( "##evw_heatmap_hit",
                              ImVec2 ( avail_w > 0.0f ? avail_w : 1.0f,
                                       child_h ) );
    bool hit_hovered = ImGui::IsItemHovered ( );
    bool hit_clicked = ImGui::IsItemClicked ( ImGuiMouseButton_Left );

    if ( hit_hovered && bin_w > 0.0f ) {
        ImVec2 mouse = ImGui::GetIO ( ).MousePos;
        int hover_bin = (int) ( ( mouse.x - origin.x ) / bin_w );
        if ( hover_bin >= 0 && hover_bin < EVW_HEATMAP_BIN_COUNT ) {
            uint64_t pxclk_lo = (uint64_t) hover_bin * (uint64_t) bin_size_pxclk;
            uint64_t pxclk_hi = pxclk_lo + (uint64_t) bin_size_pxclk - 1u;
            uint32_t sline_lo = (uint32_t) ( pxclk_lo / (uint64_t) g_mzhal.video_screen_width );
            uint32_t sline_hi = (uint32_t) ( pxclk_hi / (uint64_t) g_mzhal.video_screen_width );

            if ( s_heatmap[hover_bin].total > 0u ) {
                ImGui::BeginTooltip ( );
                ImGui::Text ( _("Bin %d: pxclk %llu..%llu (sline %u..%u)"),
                              hover_bin,
                              (unsigned long long) pxclk_lo,
                              (unsigned long long) pxclk_hi,
                              (unsigned) sline_lo, (unsigned) sline_hi );
                ImGui::Text ( _("Total: %u events"),
                              (unsigned) s_heatmap[hover_bin].total );
                ImGui::Separator ( );
                for ( int cat = 0; cat < (int) EVENTLOG_CAT_COUNT; cat++ ) {
                    uint32_t c = s_heatmap[hover_bin].per_cat[cat];
                    if ( c == 0u ) continue;
                    const char *name = eventlog_filter_cat_to_name ( (uint8_t) cat );
                    ImGui::Text ( "  %s: %u", name ? name : "?",
                                  (unsigned) c );
                }
                ImGui::EndTooltip ( );

                /* Klik = scroll Log na první event v binu. */
                if ( hit_clicked ) {
                    uint32_t pxclk_lo_u = (uint32_t) pxclk_lo;
                    uint32_t pxclk_hi_u = (uint32_t) pxclk_hi;
                    size_t total_ev = eventlog_get_count ( );
                    for ( size_t i = 0; i < total_ev; i++ ) {
                        const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
                        if ( !e ) continue;
                        if ( !( g_eventlog_active_mask
                                & ( UINT64_C(1) << e->category ) ) ) continue;
                        if ( !eventlog_filter_match_ctx ( s_state.filter_handle,
                                                            e, &flt_ctx ) ) continue;
                        if ( s_state.show_only_bookmarked
                             && !evw_bookmark_contains ( e->screens_total,
                                                          e->pxclk_in_screen ) ) {
                            continue;
                        }
                        if ( e->pxclk_in_screen >= pxclk_lo_u
                             && e->pxclk_in_screen <= pxclk_hi_u ) {
                            s_state.selected_idx       = (int64_t) i;
                            s_state.want_scroll_to_idx = (int64_t) i;
                            s_state.follow_tail        = false;
                            s_state.cfg_follow_tail    = 0u;
                            break;
                        }
                    }
                }
            }
        }
    }

    ImGui::EndChild ( );
}


static void evw_render_log_tab ( void )
{
    /* Heatmapa (Vlna 4 Commit 28) - před tabulkou, jen pokud user
     * zapne toggle v toolbaru. Region zabírá ~70 px nad tabulkou,
     * proto default OFF. */
    if ( s_state.heatmap_enabled ) {
        evw_render_log_heatmap ( );
    }

    if ( s_state.group_by == EVW_GROUP_NONE ) {
        evw_render_log_chronological ( );
    } else {
        evw_render_log_grouped ( s_state.group_by );
    }
}

/* ===========================================================================
 *  Strip tab - helpers (Vlna 2 Commit 10)
 * =========================================================================== */

/**
 * @brief One-shot init Strip state (= default colors + mode + zoom).
 *
 * Volá se z @c evw_render_strip_tab() při prvním vstupu do Strip tabu.
 * Idempotentní přes @c s_strip_state.initialized guard. Persistence
 * per-kategorie barev v cfg je follow-up (= zatím defaulty per session).
 */
static void evw_init_strip_state_once ( void )
{
    if ( s_strip_state.initialized ) return;

    s_strip_state.fit_to_window       = true;
    s_strip_state.zoom                = 1.0f;
    s_strip_state.show_colors         = false;
    s_strip_state.selected_idx        = -1;
    s_strip_state.hovered_idx         = -1;
    s_strip_state.popup_idx           = -1;
    /* Propagace cfg -> live: cfg_show_previous_frame byl naplněn cfg
     * load loaderem (default 0). Při prvním vstupu do Strip tabu se
     * hodnota zkopíruje, dál UI mutace zapisuje zpět do cfg fieldu
     * (= viz toolbar checkbox handler níže). */
    s_strip_state.show_previous_frame =
        ( s_strip_state.cfg_show_previous_frame != 0u );
    /* Grid toggle - default OFF, propagace cfg -> live identicky jako
     * show_previous_frame. */
    s_strip_state.show_grid =
        ( s_strip_state.cfg_show_grid != 0u );
    /* Legend toggle - default ON (zadání: "Legend default ON"),
     * cfg propagate stejným patternem. */
    s_strip_state.show_legend =
        ( s_strip_state.cfg_show_legend != 0u );
    for ( int i = 0; i < (int) EVENTLOG_CAT_COUNT; i++ ) {
        s_strip_state.category_colors[i] = s_default_category_colors[i];
    }
    s_strip_state.initialized = true;
}


/**
 * @brief Vrátí barvu bodu pro danou kategorii.
 *
 * @param category  Kategorie eventu (@ref en_EVENTLOG_CATEGORY hodnota).
 * @return @c ImU32 barva ze @c s_strip_state.category_colors, fallback
 *         neutrální bílá pro out-of-range index.
 */
static ImU32 evw_category_color ( uint8_t category )
{
    if ( (int) category >= (int) EVENTLOG_CAT_COUNT ) {
        return IM_COL32 ( 255, 255, 255, 255 );
    }
    return s_strip_state.category_colors[category];
}


/**
 * @brief Vrátí poloměr bodu (= velikost) per kategorie + subtype.
 *
 * Heuristika z README Vlny 2:
 *   - @c BP_FIRE / @c USER_MARK = 4 px (velké body, debugger eventy).
 *   - @c IRQ_ACK_IM2 = 3 px (středně velké, IRQ vector).
 *   - @c CPU_INT / @c CPU_CTRL = 2.5 px (střední, HALT / RST / state).
 *   - @c GDG_VIDEO (HBLN/HS) = 1 px (často, malé aby nepřebily strip).
 *   - default = 1.5 px (rozumný kompromis viditelnost vs hustota).
 *
 * @param cat  Kategorie eventu.
 * @param sub  Subtype (zatím nepoužitý - rezerva pro per-subtype
 *             rozlišení, např. RETI bigger než EI, či NMI bigger
 *             než INT). Connection-aware velikost přijde s rich
 *             decodery v Commit 20.
 * @return Poloměr v ImGui pixelech (= screen px po násobení scale).
 */
static float evw_subtype_radius ( uint8_t cat, uint8_t sub )
{
    (void) sub;
    if ( cat == EVENTLOG_CAT_BP_FIRE || cat == EVENTLOG_CAT_USER_MARK ) {
        return 4.0f;
    }
    if ( cat == EVENTLOG_CAT_SYS ) {
        /* Vlna 5 Commit 31 - SYS lifecycle eventy jsou rare a důležité,
         * kreslíme je jako "large markers" stejně jako BP_FIRE / USER_MARK. */
        return 4.0f;
    }
    if ( cat == EVENTLOG_CAT_IRQ_ACK_IM2 ) {
        return 3.0f;
    }
    if ( cat == EVENTLOG_CAT_CPU_INT || cat == EVENTLOG_CAT_CPU_CTRL ) {
        return 2.5f;
    }
    if ( cat == EVENTLOG_CAT_GDG_VIDEO ) {
        return 1.0f;
    }
    return 1.5f;
}


/**
 * @brief Vrátí aktuální frame number z g_gdg (= screens_total counter).
 *
 * Strip tab kreslí eventy z aktuálního snímku (= @c screens_total
 * shodný s tímto vráceným číslem) a volitelně z předchozího
 * (= @c current_screen - 1) pokud je toggle "Show previous frame"
 * zapnutý (Commit 12).
 *
 * Implementace: thin wrapper nad @c tlog_common_get_screens_total(),
 * která čte @c g_gdg.total_elapsed.screens. Toto je thread-safe public
 * API pattern shodný s ostatními trace subsystems (intlog/marklog/...).
 * Důvod přechodu z heuristiky "screens_total z posledního eventu v
 * ringu" (Commit 10): pokud user pauzne emu uprostřed framu N+1, ale
 * poslední event je z framu N, heuristika by hlásila current=N a
 * "previous" by ukazovalo N-1 - místo aby Strip ukázal N+1 jako
 * (možná prázdný) current a N jako previous.
 *
 * @return Číslo aktuálního snímku (= shodné s @c screens_total
 *         field v eventech, které právě teď generuje hot-path).
 */
static uint32_t evw_get_current_screen ( void );


/**
 * @brief Vykreslí Strip toolbar (mode combo + zoom slider + Colors).
 *
 * Layout:
 *   - 1. řádek: Mode combo ("Fit to window" / "1:1"), Zoom slider
 *     (0.5..4x), Frame number, Events drawn počet.
 *   - 2. řádek (volitelný): Colors collapsible sekce s ColorEdit3 per
 *     kategorie ve 4 sloupcích (= analogie Categories sekce v hlavním
 *     toolbaru, aby UX bylo konzistentní).
 *
 * Side effects:
 *   - Mutace @c s_strip_state.fit_to_window, @c .zoom, @c .show_colors,
 *     @c .category_colors[].
 *
 * @param current_screen  Aktuální frame number (= pro display).
 * @param drawn_count     Počet bodů vykreslených v poslední iteraci
 *                        (= pro display, hodnota z předchozího framu).
 */
static void evw_render_strip_toolbar ( uint32_t current_screen,
                                        size_t drawn_count )
{
    /* Mode combo - "Fit to window" vs "1:1". */
    const char *mode_items[2] = { "Fit to window", "1:1" };
    int mode_idx = s_strip_state.fit_to_window ? 0 : 1;
    ImGui::SetNextItemWidth ( 160.0f );
    if ( ImGui::BeginCombo ( _L("Mode##evw_strip_mode"),
                              mode_items[mode_idx] ) ) {
        for ( int i = 0; i < 2; i++ ) {
            bool sel = ( i == mode_idx );
            if ( ImGui::Selectable ( mode_items[i], sel ) ) {
                s_strip_state.fit_to_window = ( i == 0 );
            }
            if ( sel ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }

    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 180.0f );
    /* Ve "Fit to window" módu je zoom slider grayed out (= Fit znamená
     * vždy fit, žádné zoom násobení). V "1:1" módu funguje 0.5..4x. */
    ImGui::BeginDisabled ( s_strip_state.fit_to_window );
    ImGui::SliderFloat ( _L("Zoom##evw_strip_zoom"),
                          &s_strip_state.zoom, 0.5f, 4.0f, "%.2fx" );
    ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        if ( s_strip_state.fit_to_window ) {
            ImGui::SetTooltip ( "%s",
                _("Switch Mode to '1:1' to enable zoom.  In '1:1' you can also Ctrl+wheel over canvas.") );
        } else {
            ImGui::SetTooltip ( "%s",
                _("Zoom 0.5x..4x.  Ctrl+mouse wheel over canvas = zoom in/out.") );
        }
    }

    ImGui::SameLine ( );
    /* "Show previous frame" toggle (Commit 12). Při ON render iteruje
     * eventy z (current, current-1) screens_total a previous frame se
     * kreslí s alpha/2 (= ghost overlay). Cfg klíč strip_show_prev. */
    if ( ImGui::Checkbox ( _L("Show previous frame##evw_strip_prev"),
                            &s_strip_state.show_previous_frame ) ) {
        s_strip_state.cfg_show_previous_frame =
            s_strip_state.show_previous_frame ? 1u : 0u;
    }

    ImGui::SameLine ( );
    /* "Grid" toggle - vykreslí jemnou mřížku v logical Strip souřadnicích
     * (sekundární čáry každých 64 px / 32 sline, primární zvýrazněné
     * každých 256 px / 128 sline). Default OFF. Cfg klíč strip_grid. */
    if ( ImGui::Checkbox ( _L("Grid##evw_strip_grid"),
                            &s_strip_state.show_grid ) ) {
        s_strip_state.cfg_show_grid =
            s_strip_state.show_grid ? 1u : 0u;
    }

    ImGui::SameLine ( );
    /* "Legend" toggle - per-kategorie color key + jméno aktivních
     * kategorií jako collapsible sekce pod canvasem. Default ON.
     * Cfg klíč strip_legend. */
    if ( ImGui::Checkbox ( _L("Legend##evw_strip_legend"),
                            &s_strip_state.show_legend ) ) {
        s_strip_state.cfg_show_legend =
            s_strip_state.show_legend ? 1u : 0u;
    }

    ImGui::SameLine ( );
    ImGui::Text ( _("Frame: %u  Events drawn: %zu"),
                  (unsigned) current_screen, drawn_count );

    /* Colors collapsible - per-kategorie ColorEdit3. */
    if ( ImGui::CollapsingHeader ( _L("Strip colors##evw_strip_colors_hdr"),
                                    s_strip_state.show_colors
                                       ? ImGuiTreeNodeFlags_DefaultOpen
                                       : 0 ) ) {
        /* "Reset" tlačítko = obnoví všechny barvy na default Mesen
         * scheme (= s_default_category_colors). */
        if ( ImGui::Button ( _L("Reset to defaults##evw_strip_colors_reset") ) ) {
            for ( int i = 0; i < (int) EVENTLOG_CAT_COUNT; i++ ) {
                s_strip_state.category_colors[i] = s_default_category_colors[i];
            }
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _("Restore all Strip colors to default Mesen-inspired scheme.") );
        }
        ImGui::Separator ( );
        ImGui::Columns ( 4, "##evw_strip_color_cols", false );
        for ( int i = 0; i < (int) EVENTLOG_CAT_COUNT; i++ ) {
            const char *name = eventlog_filter_cat_to_name ( (uint8_t) i );
            if ( !name ) name = "?";

            /* ColorEdit3 očekává float[3] - konverze z ImU32 a zpět. */
            ImU32 col_u32 = s_strip_state.category_colors[i];
            float col_f[3];
            col_f[0] = ( ( col_u32       ) & 0xFF ) / 255.0f;
            col_f[1] = ( ( col_u32 >>  8 ) & 0xFF ) / 255.0f;
            col_f[2] = ( ( col_u32 >> 16 ) & 0xFF ) / 255.0f;

            char id[64];
            snprintf ( id, sizeof ( id ), "%s##evw_strip_color_%d", name, i );
            if ( ImGui::ColorEdit3 ( id, col_f,
                                      ImGuiColorEditFlags_NoInputs
                                      | ImGuiColorEditFlags_NoLabel
                                      | ImGuiColorEditFlags_AlphaPreview ) ) {
                uint32_t r = (uint32_t) ( col_f[0] * 255.0f + 0.5f );
                uint32_t g = (uint32_t) ( col_f[1] * 255.0f + 0.5f );
                uint32_t b = (uint32_t) ( col_f[2] * 255.0f + 0.5f );
                if ( r > 255 ) r = 255;
                if ( g > 255 ) g = 255;
                if ( b > 255 ) b = 255;
                s_strip_state.category_colors[i] =
                    IM_COL32 ( r, g, b, 255 );
            }
            ImGui::SameLine ( );
            ImGui::TextUnformatted ( name );
            ImGui::NextColumn ( );
        }
        ImGui::Columns ( 1 );
    }
}


/**
 * @brief Hit-test lineárním scanem nad @c s_strip_drawn cache.
 *
 * Pro každý vykreslený bod změří euklidovskou vzdálenost @c mouse_pos
 * od jeho středu. Vrátí @c event_idx prvního bodu, kde @c distance
 * <= @c radius + @c EVW_STRIP_HIT_SLACK_PX. Pokud žádný bod nesedí,
 * vrátí @c -1.
 *
 * Komplexita: O(N) kde N = počet vykreslených bodů v posledním framu
 * (typicky <2000 pro běžný HW emulator). Pro N < 10000 je scan pod
 * 0.5 ms na běžném CPU - grid binning přidá až pokud profiling ukáže
 * jako bottleneck.
 *
 * @param mouse_pos  Absolutní pozice myši v ImGui screen space.
 * @return Index eventu v ringu, nebo @c -1 pokud žádný bod není v hit
 *         radius pod kurzorem.
 */
#define EVW_STRIP_HIT_SLACK_PX  2.0f    /**< Tolerance hit-testu nad
                                            kreslený radius (= jemné
                                            kulaté body se chytí i mírně
                                            mimo střed). */

static int64_t evw_strip_hit_test ( ImVec2 mouse_pos )
{
    int64_t hit = -1;
    float best_dist2 = 1e9f;
    const size_t n = s_strip_drawn.size ( );
    for ( size_t i = 0; i < n; i++ ) {
        const st_EVW_STRIP_DRAWN &d = s_strip_drawn[i];
        const float dx = mouse_pos.x - d.screen_x;
        const float dy = mouse_pos.y - d.screen_y;
        const float dist2 = dx * dx + dy * dy;
        const float tol = d.radius + EVW_STRIP_HIT_SLACK_PX;
        if ( dist2 <= tol * tol && dist2 < best_dist2 ) {
            best_dist2 = dist2;
            hit = (int64_t) d.event_idx;
        }
    }
    return hit;
}


/**
 * @brief Vykreslí Strip canvas tooltip s detaily eventu.
 *
 * Layout:
 *   0. (volitelně) "(previous frame)" indikátor + separator, pokud event
 *      pochází z předchozího framu (Commit 12 "Show previous frame").
 *   1. Frame / Pxclk / Sline / Px / PC.
 *   2. Kategorie + subtype (textový label, ne číslo).
 *   3. Decoded detail (= @c evw_format_detail výstup).
 *
 * Reuse formátovacích helperů z Vlny 1 Log tabu (= konzistence labelů).
 *
 * @param e               Pointer na event (nesmí být @c NULL).
 * @param from_prev_frame @c true = event je z předchozího framu (zobrazí
 *                        se hlavička "(previous frame)"). @c false = z
 *                        aktuálního.
 */
static void evw_render_strip_tooltip ( const st_EVENTLOG_EVENT *e,
                                        bool from_prev_frame )
{
    if ( !e ) return;

    uint32_t sline = 0, px = 0;
    evw_decode_raster ( e->pxclk_in_screen, &sline, &px );

    const char *cat_name = eventlog_filter_cat_to_name ( e->category );
    if ( !cat_name ) cat_name = "?";

    char sub_label[16];
    evw_format_subtype ( e->category, e->subtype, sub_label,
                          sizeof ( sub_label ) );

    /* Vlna 3 Commit 22: plný popis (= tooltip čtenář nemá hover nad Sub
     * buňkou jako v Log tabu, proto plný text přímo). Pokud short ==
     * full nebo full začíná na short, vynecháme redundanci. */
    char sub_full[ 96 ];
    evw_format_subtype_full ( e->category, e->subtype,
                               sub_full, sizeof ( sub_full ) );

    char detail[128];
    evw_format_detail ( e, detail, sizeof ( detail ) );

    ImGui::BeginTooltip ( );
    if ( from_prev_frame ) {
        ImGui::TextDisabled ( "%s", _("(previous frame)") );
        ImGui::Separator ( );
    }
    ImGui::Text ( "Pxclk: %llu", (unsigned long long) e->pxclk_total );
    ImGui::Text ( "Frame: %u  Sline: %u  Px: %u",
                  (unsigned) e->screens_total,
                  (unsigned) sline, (unsigned) px );
    ImGui::Text ( "PC: 0x%04X", (unsigned) e->pc );
    ImGui::Separator ( );
    ImGui::Text ( "Cat: %s", cat_name );
    /* Krátká zkratka + plný popis v závorkách - kontext pro uživatele,
     * který si zatím nepamatuje všechny zkratky. */
    if ( strcmp ( sub_label, sub_full ) == 0 ) {
        ImGui::Text ( "Sub: %s", sub_label );
    } else {
        ImGui::Text ( "Sub: %s  (%s)", sub_label, sub_full );
    }
    ImGui::Separator ( );
    ImGui::TextUnformatted ( detail );
    ImGui::EndTooltip ( );
}


/**
 * @brief Context popup pro pravý klik na bod ve Strip tabu.
 *
 * Položky shodné s Log tabem + 1 navíc ("Show in Log tab"):
 *   - "Pause emu + show in disasm" - kombinovaná pause + disasm focus.
 *   - "Pause emu here" - jen pause.
 *   - "Show in Log tab" - cross-tab switch + scroll na řádek.
 *   - "Copy event to clipboard" - textový dump (= shodný formát s Log tab).
 *
 * @param e          Event pod popupem (nesmí být @c NULL).
 * @param event_idx  Index v ringu (= cíl scrollu při "Show in Log tab").
 */
static void evw_render_strip_popup ( const st_EVENTLOG_EVENT *e,
                                       size_t event_idx )
{
    if ( !e ) return;

    if ( ImGui::BeginPopup ( "##evw_strip_ctx" ) ) {
        if ( ImGui::MenuItem ( _L("Pause emu + show in disasm###evw_strip_ctx_pause_disasm") ) ) {
            evw_click_pause ( e, true );
        }
        if ( ImGui::MenuItem ( _L("Pause emu here###evw_strip_ctx_pause_here") ) ) {
            evw_click_pause ( e, false );
        }

        ImGui::Separator ( );

        /* Bookmark toggle (Commit 14) - label podle aktuálního stavu. */
        {
            const bool is_bm = evw_bookmark_contains (
                e->screens_total, e->pxclk_in_screen );
            if ( !is_bm ) {
                if ( ImGui::MenuItem ( _L("Bookmark this event##evw_strip_ctx_bookmark") ) ) {
                    evw_bookmark_toggle ( e->screens_total,
                                           e->pxclk_in_screen );
                }
            } else {
                if ( ImGui::MenuItem ( _L("Remove bookmark##evw_strip_ctx_unbookmark") ) ) {
                    evw_bookmark_toggle ( e->screens_total,
                                           e->pxclk_in_screen );
                }
            }
        }

        ImGui::Separator ( );

        if ( ImGui::MenuItem ( _L("Show in Log tab##evw_strip_ctx_log") ) ) {
            s_state.want_switch_tab   = 0;            /* 0 = Log */
            s_state.want_scroll_to_idx = (int64_t) event_idx;
            s_state.selected_idx       = (int64_t) event_idx;
        }

        ImGui::Separator ( );

        if ( ImGui::MenuItem ( _L("Copy event##evw_strip_ctx_copy") ) ) {
            const char *cat_name = eventlog_filter_cat_to_name ( e->category );
            if ( !cat_name ) cat_name = "?";
            char detail[128];
            evw_format_detail ( e, detail, sizeof ( detail ) );
            uint32_t sline = 0, px = 0;
            evw_decode_raster ( e->pxclk_in_screen, &sline, &px );
            char dump[256];
            snprintf ( dump, sizeof ( dump ),
                       "frame=%u pxclk=%llu sline=%u px=%u pc=0x%04X "
                       "cat=%s sub=%u %s",
                       (unsigned) e->screens_total,
                       (unsigned long long) e->pxclk_total,
                       (unsigned) sline, (unsigned) px,
                       (unsigned) e->pc, cat_name,
                       (unsigned) e->subtype, detail );
            ImGui::SetClipboardText ( dump );
        }

        ImGui::EndPopup ( );
    }
}


/**
 * @brief Vykreslí overlay - bílý obrys viditelné display area + šedý obrys
 *        aktivní pixel oblasti (canvas) v logical Strip souřadnicích.
 *
 * Display area = (0, 0) - (mzhal_beam_display_last_column(), mzhal_beam_display_last_row())
 * = viditelná část obrazu včetně borderu. Pro MZ-800 (0, 0)-(927, 287).
 *
 * Canvas (active pixel area) = (mzhal_beam_canvas_first_column(),
 * mzhal_beam_canvas_first_row()) - (mzhal_beam_canvas_last_column(),
 * mzhal_beam_canvas_last_row()). Pro MZ-800 (154, 46)-(793, 245) = 640x200.
 *
 * Rendering pořadí: pozadí canvasu (čisté logical screen) je už vykreslen
 * v evw_render_strip_tab. Overlay rect = tenké rámečky na top, takže
 * nepřebijí kreslené body (alpha < 255).
 *
 * @param dl              ImDrawList (= canvas drawlist).
 * @param canvas_origin   Absolutní screen pozice (0,0) logical canvasu.
 * @param scale_x         Logical px -> screen px škálování (X).
 * @param scale_y         Logical px -> screen px škálování (Y).
 */
static void evw_render_strip_visible_canvas_overlay ( ImDrawList *dl,
                                                       ImVec2 canvas_origin,
                                                       float scale_x,
                                                       float scale_y )
{
    if ( !dl ) return;

    /* Display area (= visible incl. border) - tenký bílý polotransparentní
     * rámeček. Začátek (0,0) = canvas_origin. */
    const ImVec2 disp_min ( canvas_origin.x,
                            canvas_origin.y );
    const ImVec2 disp_max (
        canvas_origin.x + (float) ( mzhal_beam_display_last_column() + 1 ) * scale_x,
        canvas_origin.y + (float) ( mzhal_beam_display_last_row()    + 1 ) * scale_y );
    dl->AddRect ( disp_min, disp_max,
                   IM_COL32 ( 255, 255, 255, 128 ),
                   0.0f, 0, 1.0f );

    /* Canvas (= active pixel area, bez borderu) - světle šedý
     * polotransparentní rámeček, menší a uvnitř display. */
    const ImVec2 cv_min (
        canvas_origin.x + (float) mzhal_beam_canvas_first_column() * scale_x,
        canvas_origin.y + (float) mzhal_beam_canvas_first_row()    * scale_y );
    const ImVec2 cv_max (
        canvas_origin.x + (float) ( mzhal_beam_canvas_last_column() + 1 ) * scale_x,
        canvas_origin.y + (float) ( mzhal_beam_canvas_last_row()    + 1 ) * scale_y );
    dl->AddRect ( cv_min, cv_max,
                   IM_COL32 ( 180, 180, 180, 96 ),
                   0.0f, 0, 1.0f );
}


/**
 * @brief Vykreslí grid lines (= mřížku) v logical Strip souřadnicích.
 *
 * Sekundární grid: vertikální čáry každých @c GRID_STEP_X_SEC = 64 px,
 * horizontální každých @c GRID_STEP_Y_SEC = 32 sline. Barva
 * @c IM_COL32(100,100,100,80) = velmi jemný šedý.
 *
 * Primární grid: zvýrazněné čáry každých @c GRID_STEP_X_PRI = 256 px,
 * horizontální každých @c GRID_STEP_Y_PRI = 128 sline. Barva
 * @c IM_COL32(140,140,140,140) = sytější odstín pro hlavní orientaci.
 *
 * Linie se kreslí jen v rozsahu [0, g_mzhal.video_screen_width-1] x
 * [0, g_mzhal.video_screen_height-1] - mimo screen je blanking, kde se eventy
 * nezobrazují.
 *
 * @param dl             ImDrawList canvas drawlistu.
 * @param canvas_origin  Absolutní screen pozice (0,0) logical canvasu.
 * @param scale_x        Logical -> screen px škálování (X).
 * @param scale_y        Logical -> screen px škálování (Y).
 */
static void evw_render_strip_grid ( ImDrawList *dl,
                                     ImVec2 canvas_origin,
                                     float scale_x,
                                     float scale_y )
{
    if ( !dl ) return;

    /* Hranice gridu v logical souřadnicích (= 0..SCREEN_WIDTH/HEIGHT). */
    const float full_w = (float) g_mzhal.video_screen_width  * scale_x;
    const float full_h = (float) g_mzhal.video_screen_height * scale_y;

    const int GRID_STEP_X_SEC = 64;
    const int GRID_STEP_Y_SEC = 32;
    const int GRID_STEP_X_PRI = 256;
    const int GRID_STEP_Y_PRI = 128;

    const ImU32 col_sec = IM_COL32 ( 100, 100, 100,  80 );
    const ImU32 col_pri = IM_COL32 ( 140, 140, 140, 140 );

    /* Vertikální čáry (cols). Primární test bere přednost = jedna čára
     * jeden render (jinak by primary linka byla překreslena secondary). */
    for ( int x = GRID_STEP_X_SEC; x < (int) g_mzhal.video_screen_width;
          x += GRID_STEP_X_SEC ) {
        const bool primary = ( ( x % GRID_STEP_X_PRI ) == 0 );
        const float sx = canvas_origin.x + (float) x * scale_x;
        dl->AddLine ( ImVec2 ( sx, canvas_origin.y ),
                       ImVec2 ( sx, canvas_origin.y + full_h ),
                       primary ? col_pri : col_sec, 1.0f );
    }

    /* Horizontální čáry (rows). */
    for ( int y = GRID_STEP_Y_SEC; y < (int) g_mzhal.video_screen_height;
          y += GRID_STEP_Y_SEC ) {
        const bool primary = ( ( y % GRID_STEP_Y_PRI ) == 0 );
        const float sy = canvas_origin.y + (float) y * scale_y;
        dl->AddLine ( ImVec2 ( canvas_origin.x,           sy ),
                       ImVec2 ( canvas_origin.x + full_w, sy ),
                       primary ? col_pri : col_sec, 1.0f );
    }

    (void) full_w; /* Variable used by horizontal lines only - keep
                     definition for readability. */
}


/**
 * @brief Vykreslí scanline cursor - cross-hair na aktuální beam pozici.
 *
 * Beam pozice se čte přes thread-safe accessory:
 *   - @c tlog_common_get_screens_total() = aktuální screen number (unused
 *     tady - cursor patří k zobrazenému canvasu, nemusí být per-screen
 *     gated).
 *   - @c tlog_common_get_pxclk_in_screen() = pxclk pozice 0..SCREEN_TICKS-1
 *     uvnitř právě skenovaného framu.
 *
 * Z @c pxclk_in_screen dekódujeme (sline, px) přes @c evw_decode_raster.
 * Vykreslí se tenká vertikální čára na @c px (= "v tomto pixelu právě
 * teď beam") a horizontální na @c sline (= "tento řádek právě teď
 * beam"). Žlutá barva s alpha 200 pro dobrý kontrast nad tmavým canvasem.
 *
 * Label "sline=N px=M" se renderuje vedle průsečíku malým textem.
 *
 * Cursor se kreslí vždy (= "kde se právě teď řeže obraz" - užitečné i bez
 * pauzy). Pokud je emu pauznutý (@c EMULATOR_TEST_PAUSED), pozice stojí
 * a uživatel vidí přesné místo, kam emu dorazil.
 *
 * @param dl             ImDrawList canvas drawlistu.
 * @param canvas_origin  Absolutní screen pozice (0,0) logical canvasu.
 * @param scale_x        Logical -> screen px škálování (X).
 * @param scale_y        Logical -> screen px škálování (Y).
 */
static void evw_render_strip_scanline_cursor ( ImDrawList *dl,
                                                ImVec2 canvas_origin,
                                                float scale_x,
                                                float scale_y )
{
    if ( !dl ) return;

    /* Pxclk pozice v aktuálním framu. Hot-path emu vlákno průběžně mění
     * g_gdg.beam_row + g_gdg.total_elapsed.ticks; tlog_common_*
     * accessor čte stejný field, žádný extra lock - krátký data race
     * vůči concurrent update je akceptovatelný (cursor blikne max o 1
     * frame nesprávně, irrelevantní). */
    const uint32_t pxclk_in_screen = tlog_common_get_pxclk_in_screen ( );

    uint32_t sline = 0, px = 0;
    evw_decode_raster ( pxclk_in_screen, &sline, &px );

    /* Defensive clip - pxclk_in_screen by se měl pohybovat v rozsahu
     * 0..VIDEO_SCREEN_TICKS-1 (= sline 0..SCREEN_HEIGHT-1), ale guard
     * proti corner case bezprostředně po reset / před prvním screen
     * done. */
    if ( sline >= (uint32_t) g_mzhal.video_screen_height ) sline = g_mzhal.video_screen_height - 1;
    if ( px    >= (uint32_t) g_mzhal.video_screen_width  ) px    = g_mzhal.video_screen_width  - 1;

    const float sx_full = (float) g_mzhal.video_screen_width  * scale_x;
    const float sy_full = (float) g_mzhal.video_screen_height * scale_y;

    const float cx = canvas_origin.x + (float) px    * scale_x;
    const float cy = canvas_origin.y + (float) sline * scale_y;

    /* Cross-hair = žlutá s alpha 200. Tenká 1 px tloušťka aby cursor
     * nepřekryl příliš mnoho bodů pod sebou. */
    const ImU32 col = IM_COL32 ( 255, 220, 0, 200 );

    /* Vertikální čára (= aktuální pixel column). */
    dl->AddLine ( ImVec2 ( cx, canvas_origin.y ),
                   ImVec2 ( cx, canvas_origin.y + sy_full ),
                   col, 1.0f );
    /* Horizontální čára (= aktuální scanline). */
    dl->AddLine ( ImVec2 ( canvas_origin.x,           cy ),
                   ImVec2 ( canvas_origin.x + sx_full, cy ),
                   col, 1.0f );

    /* Label "sline=N px=M" - malý text vedle průsečíku. Pokud by label
     * uplýtnul mimo canvas (kurzor blízko pravého/dolního okraje),
     * posun do druhého kvadrantu. */
    char label[48];
    snprintf ( label, sizeof ( label ), "sline=%u px=%u",
               (unsigned) sline, (unsigned) px );

    const ImVec2 text_size = ImGui::CalcTextSize ( label );
    float lx = cx + 4.0f;
    float ly = cy + 2.0f;
    if ( lx + text_size.x > canvas_origin.x + sx_full ) {
        lx = cx - text_size.x - 4.0f;
    }
    if ( ly + text_size.y > canvas_origin.y + sy_full ) {
        ly = cy - text_size.y - 2.0f;
    }
    /* Stín pro čitelnost nad tmavým + barevným pozadím. */
    dl->AddText ( ImVec2 ( lx + 1.0f, ly + 1.0f ),
                   IM_COL32 ( 0, 0, 0, 200 ), label );
    dl->AddText ( ImVec2 ( lx, ly ), col, label );
}


/**
 * @brief Vykreslí legendu (= per-kategorie color key) pod canvasem.
 *
 * Renderuje se jako collapsible sekce stejným patternem jako "Strip
 * colors" - umístění na konci Strip tabu (= po canvasu). Zobrazí pouze
 * kategorie, které jsou aktuálně viditelné (bit ON v
 * @c g_eventlog_active_mask) - schované kategorie nemají v legendě smysl.
 *
 * Layout: 4 sloupce, v každé buňce barevný čtvereček (12x12 px
 * @c AddRectFilled přes @c ImDrawList) + jméno kategorie. Stejný styl
 * jako Categories sekce v hlavním toolbaru pro UX konzistenci.
 */
static void evw_render_strip_legend ( void )
{
    if ( !ImGui::CollapsingHeader ( _L("Legend##evw_strip_legend_hdr"),
                                     ImGuiTreeNodeFlags_DefaultOpen ) ) {
        return;
    }

    /* Vysvětlení velikostí bodů - každý řádek symbol + popisek
     * (= user vidí "tečka v této velikosti znamená X"). */
    ImGui::TextDisabled ( "%s", _("Dot size = subtype priority:") );
    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    const float text_h = ImGui::GetTextLineHeight ( );
    struct {
        float radius;
        const char *label;
        const char *cats;
    } size_legend[] = {
        { 4.0f, N_("large"),  N_("BP_FIRE, USER_MARK (debug markers)") },
        { 3.0f, N_("medium"), N_("IRQ_ACK_IM2 (IM 2 dispatch)") },
        { 2.5f, N_("normal"), N_("CPU_INT, CPU_CTRL (IM/IFF + HALT/RST)") },
        { 1.5f, N_("small"),  N_("default (most HW events)") },
        { 1.0f, N_("tiny"),   N_("GDG_VIDEO (HBLN/HS edges, dense)") },
    };
    const int n_size_rows = (int) ( sizeof ( size_legend ) / sizeof ( size_legend[0] ) );
    for ( int s = 0; s < n_size_rows; s++ ) {
        ImVec2 p = ImGui::GetCursorScreenPos ( );
        float cx = p.x + 8.0f;
        float cy = p.y + text_h * 0.5f;
        dl->AddCircleFilled ( ImVec2 ( cx, cy ),
                               size_legend[s].radius,
                               IM_COL32 ( 220, 220, 220, 255 ) );
        ImGui::Dummy ( ImVec2 ( 20.0f, text_h ) );
        ImGui::SameLine ( );
        ImGui::Text ( "%s - %s", _( size_legend[s].label ),
                                   _( size_legend[s].cats ) );
    }

    ImGui::Separator ( );
    ImGui::TextDisabled ( "%s", _("Color = category (visible only):") );

    /* Per-kategorie color key. Nakreslíme kruh skutečné base velikosti
     * (= evw_subtype_radius pro subtype 0) v dané kategorii barvě, takže
     * user vidí jak vypadá konkrétní typ eventu ve Stripu. */
    ImGui::Columns ( 3, "##evw_strip_legend_cols", false );
    for ( int i = 0; i < (int) EVENTLOG_CAT_COUNT; i++ ) {
        /* Pouze viditelné kategorie. */
        if ( !( g_eventlog_active_mask & ( UINT64_C(1) << i ) ) ) continue;

        const char *name = eventlog_filter_cat_to_name ( (uint8_t) i );
        if ( !name ) name = "?";

        /* Kruh ve velikosti default subtype 0 - reprezentuje "typický
         * bod této kategorie na Stripu". */
        const float radius = evw_subtype_radius ( (uint8_t) i, 0u );
        ImVec2 p = ImGui::GetCursorScreenPos ( );
        float cx = p.x + 8.0f;
        float cy = p.y + text_h * 0.5f;
        dl->AddCircleFilled ( ImVec2 ( cx, cy ), radius,
                               s_strip_state.category_colors[i] );

        ImGui::Dummy ( ImVec2 ( 20.0f, text_h ) );
        ImGui::SameLine ( );
        ImGui::TextUnformatted ( name );

        ImGui::NextColumn ( );
    }
    ImGui::Columns ( 1 );
}


/**
 * @brief Vykreslí Strip tab - 2D canvas s body per-event.
 *
 * Render pipeline:
 *   1. Lazy init Strip state (defaulty colors + mode).
 *   2. Zjisti aktuální frame number (@c g_gdg.total_elapsed.screens).
 *   3. Spočítej canvas dimenze (logické = g_mzhal.video_screen_width x HEIGHT,
 *      fyzické = logical * scale).
 *   4. Vykresli rámec canvasu (= tmavé pozadí pro kontrast bodů).
 *   5. Iteruj eventy v ringu; pro každý s @c screens_total ==
 *      current_screen + per-kategorie visibility on + filter match
 *      kresli @c AddCircleFilled na (px, sline) pozici.
 *   6. Toolbar zobrazí drawn_count.
 *
 * Implementační rozhodnutí:
 *   - Per-frame ring iteration je O(N), nikoliv O(N) per pixel - na
 *     50000 eventech max ~10000 v jednom frame, ImDrawList zvládá.
 *   - @c AddCircleFilled má vyšší overhead než @c AddRectFilled, ale
 *     vizuálně lepší. Switch je triviální pokud se ukáže perf problém.
 *   - Aspect ratio v "Fit to window" se škáluje proporcionálně podle
 *     menší dimenze regionu (= celý frame se vejde se zachovaným ratio).
 *   - "1:1" mode obalí canvas do ChildWindow se scrollbars, aby šel
 *     prohlížet i full-size 1136x312 frame na malém okně.
 *
 * @note Hit-test, hover tooltip, single / double / right click handling
 *       přibyly v Commit 11 (Vlna 2). Hit-test je lineární scan nad
 *       per-frame draw cache @c s_strip_drawn (= O(N), pro typický
 *       <2000 visible events <0.5 ms). InvisibleButton overlayed přes
 *       celý canvas slouží jako hit region pro ImGui hover/click queries.
 */
static void evw_render_strip_tab ( void )
{
    evw_init_strip_state_once ( );

    /* Logické dimenze canvasu - per-arch (g_mzhal.video_screen_width/HEIGHT
     * makra rezolvují podle compile-time MZARCH + PAL/NTSC variant). */
    const float logical_w = (float) g_mzhal.video_screen_width;
    const float logical_h = (float) g_mzhal.video_screen_height;

    /* Toolbar. Drawn count je z PŘEDCHOZÍHO renderu (= cached v static)
     * aby uživatel nemusel čekat na další frame na update; pro hodnotu
     * tohoto framu by se musel toolbar renderovat AŽ po canvasu, což
     * by rozbilo layout (toolbar nad canvasem). */
    static size_t s_last_drawn = 0;
    const uint32_t current_screen = evw_get_current_screen ( );
    evw_render_strip_toolbar ( current_screen, s_last_drawn );

    /* Legenda (= per-kategorie color/size key). Renderujeme PŘED
     * canvasem aby zabrala vlastní prostor v layoutu; canvas níže
     * vezme jen zbývající ContentRegion. Kdyby byla po EndChild,
     * canvas BeginChild s ImVec2(0, 0) by zabral celý zbytek a
     * legenda by spadla pod viewport (= scrollbar Events okna).
     * Toggle z toolbaru (default OFF). */
    if ( s_strip_state.show_legend ) {
        evw_render_strip_legend ( );
    }

    /* Wrap do ChildWindow.
     *  - "1:1" mode má scrollbars zapnuté (= canvas > avail je běžné).
     *  - "Fit to window" má scrollbars i borders vypnuté (= canvas má
     *    být přesně vnitřní region; border/scrollbar reservation by
     *    canvas posunuly přes hranice + projevily by se vertikálním
     *    scrollerem i v Fit režimu). */
    ImGuiWindowFlags child_flags = ImGuiWindowFlags_NoScrollbar;
    ImGuiChildFlags  child_extras = 0;
    if ( !s_strip_state.fit_to_window ) {
        child_flags = ImGuiWindowFlags_HorizontalScrollbar;
        child_extras = ImGuiChildFlags_Borders;
    }
    ImGui::BeginChild ( "##evw_strip_child", ImVec2 ( 0, 0 ),
                         child_extras, child_flags );

    /* Compute canvas physical size UVNITŘ BeginChild - GetContentRegionAvail
     * tady reflektuje skutečný vnitřní prostor child window (= ne včetně
     * border/scrollbar padding rodiče, který způsoboval že canvas v Fit
     * režimu přesahoval o pár pixelů → vertikální scroll). */
    ImVec2 avail = ImGui::GetContentRegionAvail ( );
    if ( avail.x < 50.0f ) avail.x = 50.0f;
    if ( avail.y < 30.0f ) avail.y = 30.0f;

    float scale_x = 1.0f;
    float scale_y = 1.0f;

    if ( s_strip_state.fit_to_window ) {
        /* "Fit to window" = vyplnit celý vnitřní region NEZÁVISLE na
         * aspect ratio. Canvas má aspect 1136:312 (~3.64:1), aspect-
         * preserving fit by nechával velké prázdné pásy. Pro Strip view
         * jsou (px, sline) abstraktní souřadnice eventu, ne pixel-perfect
         * render obrazu - distortion nemění význam, zaplnění je
         * intuitivnější UX. User chce přesný aspect → "1:1" mode. */
        scale_x = avail.x / logical_w;
        scale_y = avail.y / logical_h;
    } else {
        /* 1:1 mode - logický px = fyzický px (* zoom). */
        scale_x = 1.0f * s_strip_state.zoom;
        scale_y = 1.0f * s_strip_state.zoom;
    }

    const float canvas_w = logical_w * scale_x;
    const float canvas_h = logical_h * scale_y;

    /* Ctrl+wheel zoom nad canvasem (jen v 1:1 mode). */
    if ( !s_strip_state.fit_to_window
         && ImGui::IsWindowHovered ( ) ) {
        ImGuiIO &io = ImGui::GetIO ( );
        if ( io.KeyCtrl && io.MouseWheel != 0.0f ) {
            float new_zoom = s_strip_state.zoom * ( 1.0f + io.MouseWheel * 0.1f );
            if ( new_zoom < 0.5f ) new_zoom = 0.5f;
            if ( new_zoom > 4.0f ) new_zoom = 4.0f;
            s_strip_state.zoom = new_zoom;
        }
    }

    /* Tmavé pozadí canvasu pro kontrast s body. */
    ImVec2 canvas_origin = ImGui::GetCursorScreenPos ( );
    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    dl->AddRectFilled ( canvas_origin,
                         ImVec2 ( canvas_origin.x + canvas_w,
                                  canvas_origin.y + canvas_h ),
                         IM_COL32 ( 16, 16, 24, 255 ) );
    dl->AddRect ( canvas_origin,
                   ImVec2 ( canvas_origin.x + canvas_w,
                            canvas_origin.y + canvas_h ),
                   IM_COL32 ( 80, 80, 96, 255 ) );

    /* Grid lines (= pod overlays / event body). Toggle z toolbaru. */
    if ( s_strip_state.show_grid ) {
        evw_render_strip_grid ( dl, canvas_origin, scale_x, scale_y );
    }

    /* Visible display area + active canvas rect overlay - tenké obrysy
     * pomáhají uživateli zorientovat se kde je border vs pixel area. */
    evw_render_strip_visible_canvas_overlay ( dl, canvas_origin,
                                                scale_x, scale_y );

    /* Reset draw cache (= per-frame populace). Capacity se neuvolňuje,
     * další framy reuse buffer. */
    s_strip_drawn.clear ( );

    /* Previous frame number pro double buffer overlay (Commit 12).
     * UINT32_MAX = sentinel "žádný previous frame" pro current_screen==0,
     * kdy by current-1 wraplo na 0xFFFFFFFF a omylem matchovalo
     * eventy z extrémních screens_total hodnot. */
    const uint32_t prev_screen = ( current_screen > 0 )
                                   ? ( current_screen - 1 )
                                   : UINT32_MAX;

    /* Iteruj ring a kresli body z aktuálního (+ volitelně předchozího) framu. */
    size_t drawn = 0;
    const size_t total = eventlog_get_count ( );
    /* Temporal filter ctx (Vlna 4 Commit 26) - reference scope je celý ring,
     * ne jen aktuální frame (= před(N) může najít reference v předchozím
     * snímku). */
    st_EVENTLOG_FILTER_CTX flt_ctx;
    flt_ctx.events = NULL;
    flt_ctx.count  = total;
    for ( size_t i = 0; i < total; i++ ) {
        const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
        if ( !e ) continue;

        /* Per-frame filter - current + volitelně previous (Commit 12). */
        const bool is_current  = ( e->screens_total == current_screen );
        const bool is_previous = s_strip_state.show_previous_frame
                                  && ( e->screens_total == prev_screen );
        if ( !is_current && !is_previous ) continue;

        /* Per-kategorie visibility (sdílené s Log tab). */
        if ( !( g_eventlog_active_mask
                & ( UINT64_C(1) << e->category ) ) ) continue;

        /* Filter expression match (sdílené s Log tab). */
        if ( !eventlog_filter_match_ctx ( s_state.filter_handle, e, &flt_ctx ) ) continue;

        /* "Show only ★" override (Commit 14) - viz Log tab. */
        if ( s_state.show_only_bookmarked
             && !evw_bookmark_contains ( e->screens_total,
                                          e->pxclk_in_screen ) ) {
            continue;
        }

        uint32_t sline = 0, px = 0;
        evw_decode_raster ( e->pxclk_in_screen, &sline, &px );

        /* Safety: pxclk_in_screen by neměl překročit screen dimenze,
         * ale defensive clip aby out-of-bounds bod neuteklo ven. */
        if ( sline >= (uint32_t) g_mzhal.video_screen_height ) continue;
        if ( px    >= (uint32_t) g_mzhal.video_screen_width  ) continue;

        ImVec2 pos = ImVec2 (
            canvas_origin.x + (float) px    * scale_x,
            canvas_origin.y + (float) sline * scale_y );
        ImU32 color = evw_category_color ( e->category );
        /* Radius škálujeme společně s canvas scale = body rostou
         * proporcionálně se zoomem. Min 1.0 (= subpixel by zmizel),
         * max 8.0 (= velký bod by zaplavil obraz). Scale faktor
         * = průměr scale_x/scale_y (= aspect ratio je zachován ve
         * "Fit to window" + "1:1" modech). */
        float scale_factor = ( scale_x + scale_y ) * 0.5f;
        float radius = evw_subtype_radius ( e->category, e->subtype )
                       * scale_factor;
        if ( radius < 1.0f ) radius = 1.0f;
        if ( radius > 8.0f ) radius = 8.0f;

        /* Previous frame = "ghost" overlay - alpha / 2 (= zachová odstín,
         * sníží viditelnost aby current dominoval). IM_COL32 layout je
         * ABGR (= alpha v top byte). */
        if ( is_previous ) {
            uint32_t a = ( color >> IM_COL32_A_SHIFT ) & 0xFFu;
            a = ( a >> 1 );
            color = ( color & ~( (ImU32) 0xFFu << IM_COL32_A_SHIFT ) )
                    | ( a << IM_COL32_A_SHIFT );
        }

        /* AddCircleFilled - vizuálně lepší než Rect, perf při <10k
         * bodů per frame OK. Pokud benchmark ukáže problém, swap
         * na AddRectFilled (+/- radius / 2 around pos). */
        dl->AddCircleFilled ( pos, radius, color );

        /* Zápis do hit-test cache. */
        st_EVW_STRIP_DRAWN d;
        d.event_idx       = i;
        d.screen_x        = pos.x;
        d.screen_y        = pos.y;
        d.radius          = radius;
        d.from_prev_frame = is_previous;
        s_strip_drawn.push_back ( d );

        drawn++;
    }

    /* Hit region overlay - InvisibleButton přes celý canvas slouží jako
     * ImGui item pro hover / click queries. Cursor je teď za canvasem
     * (= za pozadím rectu), vrátíme ho na origin a InvisibleButton ho
     * zase posune za canvas (= ChildWindow scroll size dorovnán). */
    ImGui::SetCursorScreenPos ( canvas_origin );
    ImGui::InvisibleButton ( "##evw_strip_hit",
                              ImVec2 ( canvas_w, canvas_h ),
                              ImGuiButtonFlags_MouseButtonLeft
                              | ImGuiButtonFlags_MouseButtonRight );

    /* Hit-test pouze pokud kurzor je nad canvas itemem. */
    s_strip_state.hovered_idx = -1;
    if ( ImGui::IsItemHovered ( ) ) {
        const ImVec2 mouse = ImGui::GetIO ( ).MousePos;
        const int64_t hit  = evw_strip_hit_test ( mouse );
        s_strip_state.hovered_idx = hit;

        if ( hit >= 0 ) {
            const st_EVENTLOG_EVENT *he = eventlog_get_event ( (size_t) hit );
            if ( he ) {
                /* from_prev_frame flag dohledáme v draw cache - lineární
                 * scan stejnou metodou jako hit-test (cache je malá). */
                bool hit_from_prev = false;
                for ( const auto &d : s_strip_drawn ) {
                    if ( (int64_t) d.event_idx == hit ) {
                        hit_from_prev = d.from_prev_frame;
                        break;
                    }
                }
                evw_render_strip_tooltip ( he, hit_from_prev );
            }

            /* Single click levým = select highlight. */
            if ( ImGui::IsMouseClicked ( ImGuiMouseButton_Left ) ) {
                s_strip_state.selected_idx = hit;
            }
            /* Double click levým = pause + disasm focus (= shodné s
             * Log tabem double-click UX). */
            if ( ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) ) {
                if ( he ) {
                    evw_click_pause ( he, true );
                }
            }
            /* Pravý klik = context popup. ImGui rezerva popup pod globálním
             * ID "##evw_strip_ctx" - viz @ref evw_render_strip_popup. */
            if ( ImGui::IsMouseClicked ( ImGuiMouseButton_Right ) ) {
                s_strip_state.popup_idx = hit;
                ImGui::OpenPopup ( "##evw_strip_ctx" );
            }
        } else {
            /* Klik mimo bod = zruš selection (= UX jako file manageru). */
            if ( ImGui::IsMouseClicked ( ImGuiMouseButton_Left ) ) {
                s_strip_state.selected_idx = -1;
            }
        }
    }

    /* Bookmark outline (Commit 14) - žlutý kroužek kolem každého
     * bookmarkovaného bodu. Kreslíme PŘED selected outline aby bílý
     * select highlight přebil žlutou bookmark indikaci u dvojného
     * stavu (bookmarked + selected). */
    if ( !s_bookmarks.empty ( ) ) {
        const ImU32 bm_col = IM_COL32 ( 255, 220, 0, 255 );
        for ( const auto &d : s_strip_drawn ) {
            const st_EVENTLOG_EVENT *be =
                eventlog_get_event ( d.event_idx );
            if ( !be ) continue;
            if ( !evw_bookmark_contains ( be->screens_total,
                                           be->pxclk_in_screen ) ) {
                continue;
            }
            dl->AddCircle ( ImVec2 ( d.screen_x, d.screen_y ),
                             d.radius + 2.0f, bm_col, 0, 1.5f );
        }
    }

    /* Render outline pro selected bod (přes všechny body = na top).
     * Bílý 2px ring kolem středu s radius + 2px = jasně viditelný i pro
     * jemné body (radius 1.0..1.5). */
    if ( s_strip_state.selected_idx >= 0 ) {
        for ( const auto &d : s_strip_drawn ) {
            if ( (int64_t) d.event_idx != s_strip_state.selected_idx ) continue;
            dl->AddCircle ( ImVec2 ( d.screen_x, d.screen_y ),
                             d.radius + 2.0f,
                             IM_COL32 ( 255, 255, 255, 255 ),
                             0, 2.0f );
            break;
        }
    }

    /* Hover indicator - jemný gray ring kolem bodu pod kurzorem, jen
     * pokud není shodný se selected (= výrazný outline by stačil). */
    if ( s_strip_state.hovered_idx >= 0
         && s_strip_state.hovered_idx != s_strip_state.selected_idx ) {
        for ( const auto &d : s_strip_drawn ) {
            if ( (int64_t) d.event_idx != s_strip_state.hovered_idx ) continue;
            dl->AddCircle ( ImVec2 ( d.screen_x, d.screen_y ),
                             d.radius + 2.0f,
                             IM_COL32 ( 180, 180, 180, 200 ),
                             0, 1.0f );
            break;
        }
    }

    /* Scanline cursor (beam cross-hair) - vykreslen nad body a outline
     * (= žluté čáry vidí uživatel nad veškerým obsahem canvasu).
     * Zobrazuje se vždy (= živá pozice beam při běhu, statická při pauze).
     * Side effect žádný, čte přes thread-safe accessory. */
    evw_render_strip_scanline_cursor ( dl, canvas_origin, scale_x, scale_y );

    /* Hotkey Alt+P (pause toggle) je řešen na úrovni
     * event_viewer_window_render() = funguje v obou tabech, ne jen
     * Strip. Viz tam. */

    /* Context popup - rendered always; ImGui sám interně testuje "is
     * open" flag a renderuje obsah jen pokud user OpenPopup zavolal. */
    if ( s_strip_state.popup_idx >= 0 ) {
        const st_EVENTLOG_EVENT *pe =
            eventlog_get_event ( (size_t) s_strip_state.popup_idx );
        if ( pe ) {
            evw_render_strip_popup ( pe, (size_t) s_strip_state.popup_idx );
        }
    }

    ImGui::EndChild ( );

    /* Legenda byla přesunuta NAD canvas (viz výše), aby canvas nezatlačil
     * legendu pod viewport. */

    s_last_drawn = drawn;
}


/**
 * @brief Implementace deklarovaná výše - get current screen number.
 *
 * Commit 12 přechod: čistý accessor @c tlog_common_get_screens_total()
 * (= thin wrapper nad @c g_gdg.total_elapsed.screens) místo
 * heuristiky "screens_total posledního eventu v ringu" z Commit 10.
 * Důvod viz Doxygen forward deklarace - rozlišení current vs prev
 * frame v "Show previous frame" toggle vyžaduje hodnotu nezávislou
 * na obsahu ringu (pauznutý emu uprostřed framu N+1 s posledním
 * eventem ve framu N).
 *
 * Strategie: scan ringu, najdi MAX @c screens_total mezi všemi aktuálně
 * uloženými eventy. Vrátí tento snímek = "snímek z něhož máme nejnovější
 * data".
 *
 * Důvod oproti @c tlog_common_get_screens_total() (= g_gdg counter):
 *  - Když user pauzne emu, @c g_gdg.total_elapsed.screens stojí na N+1
 *    (= další frame začal před pauzou), ale eventy z N+1 ještě nejsou
 *    v ringu (= nebo jsou, ale ne všechny). Render hledá events s
 *    @c screens_total == N+1 a vidí prázdný canvas.
 *  - Scan ringu vrátí to, co user reálně vidí jako "poslední data".
 *
 * Performance: O(N) per UI frame, ale N <= capacity (default 50000) a
 * scan je jen čtení 4 B per event. Při typické load <2000 events/frame
 * je to <100 us. Pokud N velké a UI pomalé, fallback = cache `static
 * uint32_t s_last_max_screen` + aktualizace jen pokud `count` se změnil.
 *
 * Thread safety: per-event čtení screens_total je 32-bit atomic read.
 * Hot-path zapisuje eventlog_record v emu vlákně, UI scan v UI vlákně.
 * Krátký data race vůči concurrent write je akceptovatelný (= max ukáže
 * starší hodnotu o 1 frame, irrelevantní pro live UX).
 */
static uint32_t evw_get_current_screen ( void )
{
    uint32_t max_screen = 0;
    size_t count = eventlog_get_count ( );
    for ( size_t i = 0; i < count; i++ ) {
        const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
        if ( !e ) continue;
        if ( e->screens_total > max_screen ) max_screen = e->screens_total;
    }
    return max_screen;
}

/* ===========================================================================
 *  Public API
 * =========================================================================== */

extern "C" void event_viewer_window_request_focus ( void )
{
    s_focus_pending = true;
}


extern "C" void event_viewer_window_render ( bool *p_open )
{
    if ( !p_open ) return;

    /* Open / close edge detekce - lifecycle do eventlog. */
    if ( *p_open != s_prev_open ) {
        eventlog_notify_window_open ( *p_open ? 1 : 0 );
        s_prev_open = *p_open;
        s_state.cfg_window_open = *p_open ? 1u : 0u;
    }

    if ( !*p_open ) return;

    /* Inicializace Strip state (= category_colors[] defaulty) i pokud
     * user otevře jen Log tab. Bez tohoto by row coloring v Log tabu
     * tintoval s nulovými (transparentními) barvami. Idempotentní guard
     * uvnitř. */
    evw_init_strip_state_once ( );

    /* První render po open - sync filter handle z perzistovaného textu
     * + nastav screen width. */
    if ( !s_screen_width_initialized ) {
        /* g_mzhal.video_screen_width = total pxclk/scanline včetně blanking
         * (= compile-time per-arch konstanta, MZ-800 PAL 1136). */
        eventlog_filter_set_screen_width ( (uint32_t) g_mzhal.video_screen_width );
        s_screen_width_initialized = true;
        /* Pokud cfg propagate naplnil cfg_filter_text, zkopíruj do live
         * bufferu a parsuj (jednou). */
        if ( s_state.cfg_filter_text && s_state.cfg_filter_text[0] ) {
            g_strlcpy ( s_state.filter_text, s_state.cfg_filter_text,
                        sizeof ( s_state.filter_text ) );
        }
        s_state.filter_dirty = true;
        /* Propagace cfg_follow_tail -> live follow_tail. Pokud cfg
         * propagate proběhl, hodnota odpovídá uložené konfiguraci;
         * jinak zůstává default 1 z register_persistence. Při close+open
         * (= subsequent open) tento blok běží jen jednou (init guard),
         * takže user zachovaný follow_tail po manuálním vypnutí
         * NEztratí. Per zadání: "Pokud Events okno se zavře a znovu
         * otevře -> vrátí se na default ON" - explicit reset níže. */
        s_state.follow_tail = ( s_state.cfg_follow_tail != 0u );
        s_state.row_coloring = ( s_state.cfg_row_coloring != 0u );
        /* group_by propagace cfg -> live (Vlna 4 Commit 27). Clamp na
         * platný rozsah (= ochrana proti out-of-range cfg hodnotě). */
        {
            unsigned gb = s_state.cfg_group_by;
            if ( gb >= (unsigned) EVW_GROUP_COUNT ) gb = EVW_GROUP_NONE;
            s_state.group_by = (int) gb;
        }
        /* heatmap_enabled propagace cfg -> live (Vlna 4 Commit 28). */
        s_state.heatmap_enabled = ( s_state.cfg_heatmap_enabled != 0u );
        /* selected_idx reset při čerstvém otevření okna. */
        s_state.selected_idx = -1;
    }

    /* Reset selected_idx + obnov default follow_tail při close->open
     * transition (= druhé a další otevření okna v rámci běhu emu).
     * Persistovaný follow_tail v cfg odpovídá uživatelově poslední
     * volbě ze session - zadání ale říká "při znovuotevření default
     * ON". Per spec: ON unless explicit toggle. Implementace volí
     * návrat na ON aby user neměl "překvapivě OFF follow_tail po
     * zavření okna z předchozí click-pause akce". */
    if ( *p_open && !s_prev_open ) {
        s_state.selected_idx = -1;
        s_state.follow_tail = true;
        s_state.cfg_follow_tail = 1u;
    }

    /* Cross-window focus request - pattern shodný s memmap_window. */
    if ( s_focus_pending ) {
        ImGui::SetNextWindowFocus ( );
    }

    /* Default size jen při prvním renderu - imgui.ini layout dál spravuje. */
    auto_layout_first_use_portrait ( "Events", 720.0f, 600.0f );

    /* Stable ID s ###suffix (= dynamic title ready, viz memory note
     * "ImGui stable ID je ###suffix"). Window title je překládán přes
     * _L() na úrovni ID; ImGui interně ID hashuje z části za "###". */
    if ( !ImGui::Begin ( _L("Events###event_viewer_window"), p_open, ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::End ( );
        /* X tlačítko v titlebaru mohlo nastavit *p_open = false i při
         * collapsed okně - lifecycle dolů. */
        if ( !*p_open && s_prev_open ) {
            eventlog_notify_window_open ( 0 );
            s_prev_open = false;
            s_state.cfg_window_open = 0u;
        }
        return;
    }

    /* Krok 2 cross-window focus: po Begin víme do kterého platform
     * viewportu okno patří. V multi-viewport módu raise OS-level okno. */
    if ( s_focus_pending ) {
        ImGuiViewport *vp      = ImGui::GetWindowViewport ( );
        ImGuiViewport *main_vp = ImGui::GetMainViewport ( );
        ImGuiPlatformIO &pio   = ImGui::GetPlatformIO ( );
        if ( vp && vp != main_vp && vp->PlatformWindowCreated
             && pio.Platform_SetWindowFocus ) {
            pio.Platform_SetWindowFocus ( vp );
        }
        s_focus_pending = false;
    }

    /* Hotkey F (= Follow tail toggle) je řešen na konci render funkce
     * (po EndTabBar) - viz tam. */

    /* ---- Toolbar -------------------------------------------------------- */
    evw_render_mode_combo ( );
    ImGui::SameLine ( );
    evw_render_capacity_controls ( );
    ImGui::SameLine ( );
    /* Vlna 4 Commit 29 - Export/Import binárního dumpu ringu. */
    evw_render_export_import_controls ( );
    ImGui::SameLine ( );
    /* Toast po Export/Import operaci (4 s decay). */
    evw_render_io_toast ( );
    ImGui::SameLine ( );
    evw_render_follow_tail_toggle ( );

    evw_render_categories_section ( );
    evw_render_quick_filters ( );
    ImGui::SameLine ( );
    evw_render_saved_presets ( );
    ImGui::SameLine ( );
    evw_render_bookmark_controls ( );
    evw_render_filter_row ( );

    /* Pause-on-match trigger řádek (Commit 19) - samostatný sub-řádek
     * pod hlavním filter textboxem. Killer feature Vlny 3 - stream-level
     * BP přes filter expression. */
    evw_render_pause_trigger_row ( );

    /* Auto-mark on match trigger řádek (Commit 20) - druhý stream
     * trigger, místo pauznutí generuje synthetic USER_MARK event s
     * uživatelským jménem (= Strip canvas markery). */
    evw_render_automark_trigger_row ( );

    /* Popupy saved presets (Save / Replace / Delete) - vykreslí se vždy,
     * vlastní zobrazení řídí OpenPopup() v evw_render_saved_presets. */
    evw_render_saved_presets_popups ( );

    ImGui::Separator ( );

    /* ---- TabBar -------------------------------------------------------- */
    if ( ImGui::BeginTabBar ( "###evw_tabbar" ) ) {
        /* Cross-tab switch request (= "Show in Log tab" ze Strip popupu
         * nastaví want_switch_tab=0). Flag se konzumuje jednorázově. */
        ImGuiTabItemFlags log_flags = 0;
        if ( s_state.want_switch_tab == 0 ) {
            log_flags |= ImGuiTabItemFlags_SetSelected;
            s_state.want_switch_tab = -1;
        }
        if ( ImGui::BeginTabItem ( _L("Log###evw_tab_log"), NULL, log_flags ) ) {
            evw_render_log_tab ( );
            ImGui::EndTabItem ( );
        }

        ImGuiTabItemFlags strip_flags = 0;
        if ( s_state.want_switch_tab == 1 ) {
            strip_flags |= ImGuiTabItemFlags_SetSelected;
            s_state.want_switch_tab = -1;
        }
        if ( ImGui::BeginTabItem ( _L("Strip###evw_tab_strip"), NULL,
                                    strip_flags ) ) {
            evw_render_strip_tab ( );
            ImGui::EndTabItem ( );
        }
        ImGui::EndTabBar ( );
    }

    /* Hotkey Alt+P = pause toggle. Propagujeme na úrovni Events okna
     * (= funguje v obou tabech Log + Strip, dokud má okno focus). Globální
     * Alt+P binding v topmenu.cpp je RouteFocused-only - když má focus
     * Events okno, topmenu shortcut se nedostane. Duplikace tady zajistí
     * konzistentní UX. F hotkey (= Follow tail toggle) viz níže. */
    if ( ImGui::Shortcut ( ImGuiMod_Alt | ImGuiKey_P,
                            ImGuiInputFlags_RouteFocused ) ) {
        dbg_ui_pause_toggle ( );
    }

    /* Hotkey Ctrl+B = next bookmark, Ctrl+Shift+B = prev bookmark
     * (Commit 14). RouteFocused = aktivní jen když má Events okno
     * focus (= žádný globální clash). Test Ctrl+Shift+B PŘED Ctrl+B,
     * jinak by Ctrl+B match na Ctrl+Shift+B (= Shift v ImGui jako
     * další modifier neblokuje base match). */
    if ( ImGui::Shortcut ( ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B,
                            ImGuiInputFlags_RouteFocused ) ) {
        const int idx = evw_bookmark_find_event_idx ( -1 );
        if ( idx >= 0 ) {
            s_state.selected_idx       = (int64_t) idx;
            s_state.want_scroll_to_idx = (int64_t) idx;
            s_state.follow_tail        = false;
            s_state.cfg_follow_tail    = 0u;
        }
    } else if ( ImGui::Shortcut ( ImGuiMod_Ctrl | ImGuiKey_B,
                                    ImGuiInputFlags_RouteFocused ) ) {
        const int idx = evw_bookmark_find_event_idx ( +1 );
        if ( idx >= 0 ) {
            s_state.selected_idx       = (int64_t) idx;
            s_state.want_scroll_to_idx = (int64_t) idx;
            s_state.follow_tail        = false;
            s_state.cfg_follow_tail    = 0u;
        }
    }

    /* Hotkey F = Follow tail toggle. Guard !IsAnyItemActive() zabrání
     * toggle při psaní 'f' do filter textboxu. */
    if ( !ImGui::IsAnyItemActive ( )
         && ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows )
         && ImGui::IsKeyPressed ( ImGuiKey_F, false ) ) {
        s_state.follow_tail = !s_state.follow_tail;
        s_state.cfg_follow_tail = s_state.follow_tail ? 1u : 0u;
    }

    ImGui::End ( );

    /* X tlačítko v titulbaru - po End. Sync cfg_window_open + lifecycle. */
    if ( !*p_open && s_prev_open ) {
        eventlog_notify_window_open ( 0 );
        s_prev_open = false;
        s_state.cfg_window_open = 0u;
    }
}

/* ===========================================================================
 *  Cfg persistence
 * =========================================================================== */

extern "C" void event_viewer_window_register_persistence ( void *cmod_void )
{
    if ( !cmod_void ) return;

    st_CFGMODULE *cmod = (st_CFGMODULE *) cmod_void;
    st_CFGELEMENT *elm;

    /* window_open - default 0 (= okno zavřené po čerstvé instalaci). */
    elm = cfgmodule_register_new_element ( cmod, (char *) "window_open",
                                            CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm,
                              (void *) &s_state.cfg_window_open,
                              (void *) &s_state.cfg_window_open );

    /* filter_expression - default prázdný (= match all). cfgmodule
     * vlastní string, my v render loopu zkopírujeme do
     * @c s_state.filter_text. */
    elm = cfgmodule_register_new_element ( cmod, (char *) "filter_expression",
                                            CFGENTYPE_TEXT, (char *) "" );
    cfgelement_bind ( elm, (void *) &s_state.cfg_filter_text );

    /* follow_tail - default 1 (= live debug UX, auto-scroll dolů).
     * Hodnota se propaguje do @c s_state.follow_tail při prvním
     * renderu (= stejný lifecycle jako @c cfg_filter_text). */
    s_state.cfg_follow_tail = 1u;
    s_state.follow_tail = true;
    elm = cfgmodule_register_new_element ( cmod, (char *) "follow_tail",
                                            CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm,
                              (void *) &s_state.cfg_follow_tail,
                              (void *) &s_state.cfg_follow_tail );

    /* row_coloring - default 1 (= ON, barvy řádků v Log tabulce dle
     * kategorie, sdílí color picker se Strip tab). */
    s_state.cfg_row_coloring = 1u;
    s_state.row_coloring = true;
    elm = cfgmodule_register_new_element ( cmod, (char *) "row_coloring",
                                            CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm,
                              (void *) &s_state.cfg_row_coloring,
                              (void *) &s_state.cfg_row_coloring );

    /* group_by (Vlna 4 Commit 27) - default 0 (= EVW_GROUP_NONE,
     * chronologický výpis). Hodnota z @ref en_EVW_GROUP_BY. Propagace
     * cfg -> live při prvním renderu (= identický pattern jako
     * cfg_follow_tail). */
    s_state.cfg_group_by = 0u;
    s_state.group_by = EVW_GROUP_NONE;
    elm = cfgmodule_register_new_element ( cmod, (char *) "group_by",
                                            CFGENTYPE_UNSIGNED, 0 );
    cfgelement_set_handlers ( elm,
                              (void *) &s_state.cfg_group_by,
                              (void *) &s_state.cfg_group_by );

    /* heatmap (Vlna 4 Commit 28) - default 0 (= OFF, heatmap region
     * zabírá ~70 px nad Log tabulkou, user zapne explicit). Cfg propagace
     * při prvním renderu - viz blok níže u cfg_filter_text. */
    s_state.cfg_heatmap_enabled = 0u;
    s_state.heatmap_enabled = false;
    elm = cfgmodule_register_new_element ( cmod, (char *) "heatmap",
                                            CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm,
                              (void *) &s_state.cfg_heatmap_enabled,
                              (void *) &s_state.cfg_heatmap_enabled );

    /* strip_show_prev - default 0 (= OFF, Commit 12). Hodnota se
     * propaguje do @c s_strip_state.show_previous_frame při prvním
     * vstupu do Strip tabu (= @c evw_init_strip_state_once). UI mutace
     * (checkbox v toolbaru) zapisuje zpět do @c cfg_show_previous_frame
     * aby další cfg save zachoval stav. */
    s_strip_state.cfg_show_previous_frame = 0u;
    s_strip_state.show_previous_frame = false;
    elm = cfgmodule_register_new_element ( cmod, (char *) "strip_show_prev",
                                            CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm,
                              (void *) &s_strip_state.cfg_show_previous_frame,
                              (void *) &s_strip_state.cfg_show_previous_frame );

    /* strip_grid - default 0 (= OFF). Grid lines mřížka, persistence
     * identicky jako strip_show_prev. */
    s_strip_state.cfg_show_grid = 0u;
    s_strip_state.show_grid = false;
    elm = cfgmodule_register_new_element ( cmod, (char *) "strip_grid",
                                            CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm,
                              (void *) &s_strip_state.cfg_show_grid,
                              (void *) &s_strip_state.cfg_show_grid );

    /* strip_legend - default 0 (= OFF). Legenda zabírá vertikální
     * prostor nad canvasem a default ON překvapil uživatele = canvas
     * v malém okně byl příliš zmenšený. User explicit zapne pokud chce. */
    s_strip_state.cfg_show_legend = 0u;
    s_strip_state.show_legend = false;
    elm = cfgmodule_register_new_element ( cmod, (char *) "strip_legend",
                                            CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm,
                              (void *) &s_strip_state.cfg_show_legend,
                              (void *) &s_strip_state.cfg_show_legend );

    /* Init selected_idx -> "žádný výběr". */
    s_state.selected_idx = -1;

    /* Cross-tab request flagy - default "nic neaktivní". */
    s_state.want_switch_tab    = -1;
    s_state.want_scroll_to_idx = -1;
}


extern "C" void event_viewer_window_apply_persisted ( void )
{
    if ( !g_gui ) return;
    g_gui->showEventViewerWindow = ( s_state.cfg_window_open != 0 );
    /* Pokud okno startuje otevřené, nastav s_prev_open=false (default)
     * tak, aby první render edge detekoval open transition a zavolal
     * eventlog_notify_window_open(1). */
}


/* ===========================================================================
 *  Cfg persistence - [EVENT_LOG_FILTERS] (Vlna 2 Commit 15)
 * =========================================================================== */

/**
 * @brief Element handle per slot pro pozdější @c cfgelement_set_text_value().
 *
 * Lifecycle: alokované v
 * @c event_viewer_window_register_filters_persistence(), držené pro
 * sync-on-save v @c event_viewer_window_sync_filters_to_cfg().
 */
static st_CFGELEMENT *s_cfg_preset_name_elm[EVW_SAVED_PRESET_MAX_COUNT] = { 0 };
static st_CFGELEMENT *s_cfg_preset_expr_elm[EVW_SAVED_PRESET_MAX_COUNT] = { 0 };


extern "C" void event_viewer_window_register_filters_persistence ( void *cmod_void )
{
    if ( !cmod_void ) return;

    st_CFGMODULE *cmod = (st_CFGMODULE *) cmod_void;

    /* Předregistrovaných 32 slotů - každý slot má 2 klíče:
     * preset_NN_name + preset_NN_expr. Default hodnota prázdná =
     * slot neaktivní. cfgelement_bind drží pointer na slot v poli
     * cfg-vlastněných stringů; cfgmodule_propagate naalokuje hodnotu
     * a uloží ji do *pointer. */
    for ( unsigned i = 0; i < EVW_SAVED_PRESET_MAX_COUNT; i++ ) {
        char key_name[32];
        char key_expr[32];
        snprintf ( key_name, sizeof ( key_name ), "preset_%02u_name", i );
        snprintf ( key_expr, sizeof ( key_expr ), "preset_%02u_expr", i );

        st_CFGELEMENT *en = cfgmodule_register_new_element ( cmod,
                                key_name, CFGENTYPE_TEXT, (char *) "" );
        cfgelement_bind ( en, (void *) &s_cfg_saved_preset_name[i] );
        s_cfg_preset_name_elm[i] = en;

        st_CFGELEMENT *ee = cfgmodule_register_new_element ( cmod,
                                key_expr, CFGENTYPE_TEXT, (char *) "" );
        cfgelement_bind ( ee, (void *) &s_cfg_saved_preset_expr[i] );
        s_cfg_preset_expr_elm[i] = ee;
    }
}


extern "C" void event_viewer_window_apply_filters_persisted ( void )
{
    s_saved_presets.clear ( );
    for ( unsigned i = 0; i < EVW_SAVED_PRESET_MAX_COUNT; i++ ) {
        const char *nm = s_cfg_saved_preset_name[i];
        const char *ex = s_cfg_saved_preset_expr[i];
        if ( !nm || !nm[0] ) continue;
        if ( !ex ) ex = "";

        st_EVW_SAVED_PRESET p;
        memset ( &p, 0, sizeof ( p ) );
        g_strlcpy ( p.name, nm, sizeof ( p.name ) );
        g_strlcpy ( p.expr, ex, sizeof ( p.expr ) );
        s_saved_presets.push_back ( p );
        if ( s_saved_presets.size ( ) >= EVW_SAVED_PRESET_MAX_COUNT ) break;
    }
    s_saved_preset_active = -1;
}


extern "C" void event_viewer_window_sync_filters_to_cfg ( void )
{
    /* Pro každý slot 0..MAX nastav cfg element value text. Pokud
     * je vektor kratší, zbylé sloty dostanou prázdné stringy
     * (= cfg save zapíše empty value, načtení vrátí slot prázdný). */
    for ( unsigned i = 0; i < EVW_SAVED_PRESET_MAX_COUNT; i++ ) {
        if ( !s_cfg_preset_name_elm[i] || !s_cfg_preset_expr_elm[i] ) continue;
        const char *nm = "";
        const char *ex = "";
        if ( i < s_saved_presets.size ( ) ) {
            nm = s_saved_presets[i].name;
            ex = s_saved_presets[i].expr;
        }
        cfgelement_set_text_value ( s_cfg_preset_name_elm[i], nm );
        cfgelement_set_text_value ( s_cfg_preset_expr_elm[i], ex );
    }
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
