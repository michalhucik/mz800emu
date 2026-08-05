/*
 * io_window.cpp - I/O Ports panel (D.7 / V1.5 Sprint 1 redesign).
 *
 * Layout (per UX spec ux-io-ports.md schvaleny 2026-05-04):
 *   +-------------------------------------------------------------------+
 *   | I/O Ports                                                  [X]   |
 *   +-------------------------------------------------------------------+
 *   | [Overview] [History]                                              |
 *   +-------------------------------------------------------------------+
 *   | Sticky header:                                                    |
 *   |   Filter:[___________] [Clear]   (visible/total)                  |
 *   |   [Reset Activity] | Capacity:[10000] Auto-follow                 |
 *   +-------------------------------------------------------------------+
 *   | Section: GDG                                            [v]       |
 *   |   Addr   | Name             | Hex | Bin    | R/W | Activity       |
 *   |   0xCC   | GDG - WF (W)     | 42  | 010... | W   |   42 hits/s    |
 *   |   ...                                                              |
 *   +-------------------------------------------------------------------+
 *
 * Sprint 1 = Overview tab (data + UI), History tab placeholder.
 * Sprint 2 = History tab UI rendering, persistence, filter syntax.
 *
 * Pattern dle Variables panel (vars_window.cpp):
 *  - Sticky header s filter input + tlacitka inline
 *  - Tooltipy pres IsItemHovered + SetTooltip (zadne (?) markery)
 *  - CollapsingHeader per chip group (default expanded)
 *
 * Tracking flag g_io_window_tracking_active:
 *  - Sets na 1 pri otevreni panelu (begin = open)
 *  - Sets na 0 pri zavreni
 *  - Hot-path hook v port_*_with_logging_cb je gated pres tento flag
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs/imgui/imgui.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

#include "io_window.h"
#include "ui-imgui/debugger/breakpoints/bpt_edit_panel.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/debugger/debugger_state.h"
#include "ui-imgui/debugger/memmap/memmap_window.h"

extern "C"
{
#include "emulator/debugger/io_catalog.h"
#include "emulator/debugger/io_activity.h"
#include "emulator/debugger/io_history.h"
#include "emulator/debugger/io_history_filter.h"
#include "emulator/debugger/debugger.h"
#include "emulator/debugger/symbols/sym_db.h"
#include "emulator/debugger/bookmarks/bookmarks.h"
/* GDG include pro g_gdg (= total_elapsed.screens cur_frame v fix #10
 * last value cache age), per-arch dispatch jako v io_catalog.c. */
#include "emulator/hw-generic/gdg/gdg_state.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
}


/**
 * @brief Konverze IO katalog adresy (Sharp notace) na 16-bit IORQ bus addr.
 *
 * V1.5 fix #1: io_activity[] indexuje plnou 16-bit IORQ adresu = obsah
 * BC při IN/OUT instrukci.
 *
 * 8-bit katalog porty (addr <= 0xFF): bus low byte = catalog addr,
 *   high byte (B reg) je libovolný. UI agreguje přes 256 high-byte slotů
 *   (= io_activity_*_8bit helpers), tato funkce vrací jen low byte.
 *
 * 16-bit katalog porty (addr > 0xFF, např. 0xCF01..0xCF07):
 *   Sharp notace 0xCF<RR> = "port 0xCF, sub-register RR".
 *   Skutečná Z80 bus addr = (RR << 8) | 0xCF (= např. 0x01CF pro 0xCF01).
 *   Funkce provede byte-swap.
 *
 * @param catalog_addr  Adresa z IO katalogu (port->addr).
 * @return 16-bit Z80 IORQ bus adresa pro indexování g_io_activity[].
 */
static inline uint16_t io_catalog_to_bus_addr ( uint16_t catalog_addr )
{
    if ( catalog_addr <= 0xFF ) return catalog_addr;
    /*
     * V1.5.E: MZ-700 mem-mapped IO 0xE000-0xE008 (= MMIO mirror, ne IORQ).
     * Activity tracking ulozi pod klic = MMIO addr (memory hook predava
     * addr primo). Tj. zde NEdelat byte-swap, vratit addr beze zmeny.
     */
    if ( catalog_addr >= 0xE000 && catalog_addr <= 0xE008 ) {
        return catalog_addr;
    }
    /* 0xCF<RR> family: byte-swap katalog 0xCF<RR> -> bus 0x<RR>CF. */
    uint8_t lo = (uint8_t) ( catalog_addr >> 8 );
    uint8_t hi = (uint8_t) ( catalog_addr & 0xFF );
    return (uint16_t) ( ( (uint16_t) hi << 8 ) | lo );
}


/* ========================================================================= */
/*  UI state                                                                 */
/* ========================================================================= */

namespace {

/**
 * @brief Identifikátor section group (pro collapse persist).
 *
 * Pořadí v poli definuje pořadí render. UI iteruje skupiny + filtruje
 * porty per group_name match na port name prefix.
 */
struct SectionGroup {
    const char *id;          /* stable ID pro cfg persist (V1.5 Sprint 2) */
    const char *display;     /* zobrazený nadpis */
    const char *match_prefix;/* substring match na port->name pro filtraci */
};


/**
 * @brief V1.5 Sprint 1 sekce - hardcoded order.
 *
 * Sprint 2 přidá cfg persist collapse_<id>. Pořadí cca podle adresy.
 */
static const SectionGroup g_sections[] = {
    { "gdg",       "GDG video",                "GDG -" },
    { "ppi8255",   "8255 PPI",                 "8255 PPI -" },
    { "ctc8253",   "8253 CTC",                 "8253 CTC -" },
    { "fdc",       "FDC (standard)",           "FDC -" },
    { "fdc1",      "FDC1 (secondary)",         "FDC1 -" },
    { "memory",    "Memory banking",           "Memory bank -" },
    /* MemExt = oddělená HW karta (paměťová extenze 64 kB -> 512 kB SRAM),
     * jediný port 0xE7. NENÍ to banking - Sharp banking porty (0xE0-0xE6)
     * jsou v sekci "Memory banking" výše. Reference: mz800-knowledge
     * reference/agent/hw/23-memext.md. */
    { "memext",    "Memory expansion (MemExt)", "Memory ext -" },
    { "psg",       "PSG sound",                "PSG -" },
    /* MZ-1500 ma dva SN76489 (stereo L+R) na portech 0xE9/0xF2/0xF3.
     * Distinkcni prefix "PSG stereo -" v io_catalog.c sem zaradi jen
     * MZ-1500 PSG entries; v MZ-800/MZ-700 buildech bude sec_visible==0
     * a sekce se skryje (viz io_window_render Overview tab). */
    { "psg_1500",  "PSG (stereo SN76489 x2)", "PSG stereo -" },
    { "joystick",  "Joystick",                 "JOY" },
    { "pioz80",    "Z80 PIO",                  "Z80 PIO -" },
    /* MMIO sekce - 9 entries 0xE000-0xE008 = mem-mapped PPI/CTC/GDG.
     * Na MZ-800/MZ-1500 to je MZ-700 compat mode mirror, na MZ-700 native
     * to je primarni (jediny) zpusob pristupu. Display nazev se per-arch
     * uzpusobuje v render kodu (g_sections.display). */
#if MZARCH == 700
    { "mmio",      "Mem-mapped IO",            "MMIO -" },
#else
    { "mmio",      "MZ-700 mem-mapped IO",     "MMIO -" },
#endif
    /* V1.5.F: chybejici peripherie - cmthack, Unicard, Ramdisk, IDE8, QDISK */
    { "cmthack",   "CMT loader hack",          "CMT hack -" },
    { "unicard",   "Unicard (SD/FAT storage)", "Unicard -" },
    { "ramdisk",   "Ramdisk (Pezik / Std)",    "Ramdisk -" },
    { "ide8",      "IDE8 (8-bit ATA)",         "IDE8 -" },
    { "qdisk",     "QDISK (Z80 SIO)",          "QDISK -" },
};
static const size_t g_sections_count = sizeof(g_sections) / sizeof(g_sections[0]);


/**
 * @brief Persistent UI state pro I/O Ports panel (V1.5 Sprint 1 in-memory).
 *
 * Sprint 2 přidá cfg persist [IO_PORTS_PANEL] roundtrip.
 */
struct IoUiState {
    char filter[128] = "";
    int  tracking_enable = 1;        /* user toggle - default ON pri panel open
                                       * (int kvuli cfgmodule BOOL handler). */
    int  history_capacity = (int) IO_HISTORY_DEFAULT_CAPACITY;
    int  auto_follow = 1;            /* int pro cfgmodule BOOL handler. */
    /* Tracking opt-in z cfg (= user persist preferenci). */
    int  cfg_tracking_active = 1;    /* default ON - tracking se vykonává jen
                                        při otevřeném okně, takže overhead se
                                        projeví jen v UI session. User si může
                                        odškrtnout a uložit do cfg jako 0. */
    /* Collapse state per section - default expanded. int pro cfgmodule. */
    int  section_collapsed[16] = { 0 };  /* zarovnano na 16 = max sections */
    /* Track že okno bylo poprvé otevřeno - prevence opakovaného
     * tracking_active = 1 přepsání. */
    bool was_open_last_frame = false;

    /* === History tab state (Sprint 2 Fáze 3) === */
    /**
     * Filter syntax string. Prázdný = všechny eventy. Parser ve
     * io_history_filter.c, viz docs/cz/debugger/io-ports.md pro
     * syntax (port:CE, pc:4042, frame:>100, plain text = name).
     */
    char history_filter[128] = "";
    /**
     * Index vybraného řádku v aktuálním (filtered) view, nebo -1.
     * Index je do filtered seznamu, ne do raw ringu - drží
     * highlight i při změně filtru (= ne, drop highlight on filter change).
     */
    int  history_selected_visible = -1;
    /**
     * Logical event index uloženého výběru (= io_history_get index).
     * Slouží pro persist selection přes scroll - reselect po novém renderu.
     */
    int  history_selected_logical = -1;
    /**
     * Cache poslední scroll pozice History tabulky pro detekci user
     * scroll up. Detect logika: pokud auto_follow je 1 a user posune
     * scrollbar (= scroll_y se změní jiným způsobem než SetScrollHereY
     * skok dolů), ten frame zachytíme rozdíl proti očekávané "u dna"
     * pozici a vypneme auto_follow. Fix #3 detection robustness.
     */
    float history_last_scroll_y = 0.0f;
    float history_last_scroll_max_y = 0.0f;
    /**
     * Request flag pro switch na History tab z Overview context menu
     * (= "Show in History tab" položka). UI při dalším render vidí
     * true a předá ImGuiTabItemFlags_SetSelected do BeginTabItem
     * History; pak resetuje flag. Fix #4.
     */
    bool want_switch_to_history = false;
    /**
     * Request flag pro switch na Overview tab z History context menu
     * (= "Show port in Overview" položka). UI při dalším render vidí
     * true a předá ImGuiTabItemFlags_SetSelected do BeginTabItem
     * Overview; pak resetuje flag. CHECKLIST 2.8.
     */
    bool want_switch_to_overview = false;

    /**
     * Vyska Selected event detail panelu v History tabu (V1.5.D fix #1).
     *
     * Drive byl panel fixed-height 100 px (V1.5 round1 fix #7). Detail
     * presahoval - chybel jeden radek pro Description (zejmena Cycle).
     * Po V1.5.D je default 160 px + horizontal splitter umoznuje user
     * resize 60..400 px.
     *
     * Persist v cfg modulu IO_PORTS_PANEL pod klicem
     * "detail_panel_height" jako UNSIGNED (= integer px).
     */
    int detail_panel_height = 160;

    /**
     * @brief Trigger flag pro otevreni RMB popupu na PC v Selected Event detailu.
     *
     * Nastaveno true v render scopu pri RMB klik na PC value; OpenPopup
     * se vola mimo SameLine retezec aby se ID popupu nehashovalo s widget
     * stack contextem (analogie patternu z bm_window.cpp). Po
     * vyhodnoceni v render scope se resetuje na false.
     */
    bool     pc_popup_open = false;

    /**
     * @brief Cilova PC adresa pro RMB popup akce.
     *
     * Snapshot e->pc v okamziku RMB kliku. Drzime samostatne, protoze
     * popup se renderuje az po BulletText/SameLine retezci a `e` uz
     * nemusi byt v scope (i kdyz typicky je - safety pres explicit copy).
     */
    uint16_t pc_popup_addr = 0;

    /**
     * @brief Práh hits/s, od kterého se text radku barví zeleně (= aktivní port).
     *
     * Default 1 = jakýkoliv hit → zelený text. Uživatel může nastavit
     * vyšší práh aby filtroval šum (např. 10 = ignorovat ojedinělé probes
     * a zvýraznit jen porty s pravidelnou aktivitou).
     *
     * Persist v cfg [IO_PORTS_PANEL] pod klíčem "heat_text_active".
     * Konfigurace přes inline popup "Heat..." v toolbaru.
     *
     * V1.7+ CHECKLIST.md 2.7 (debugger-fixes-5).
     */
    int heat_text_active_threshold = 1;

    /**
     * @brief Práh hits/s, od kterého se radek tintuje jemně červeně (= bottleneck).
     *
     * Default 10000 = původní hardcoded hodnota (fix #9, Michal preference
     * = subtilní tint, nezatahuje pozornost). Uživatel může snížit (např.
     * 1000) aby spotřeboval bottlenecks dřív, nebo zvýšit (např. 50000)
     * pokud pracuje s vysoko-frekvenčním IO patternem.
     *
     * Persist v cfg [IO_PORTS_PANEL] pod klíčem "heat_bg_hot".
     * Konfigurace přes inline popup "Heat..." v toolbaru.
     *
     * V1.7+ CHECKLIST.md 2.7 (debugger-fixes-5).
     */
    int heat_bg_hot_threshold = 10000;
};


}  /* anonymous namespace */


/** Globalní UI state instance. */
static IoUiState g_io_ui;


/* ========================================================================= */
/*  Helpers                                                                  */
/* ========================================================================= */

/**
 * @brief MZ_AVAIL_* mask pro aktuální architekturu (runtime z g_mzhal).
 */
static unsigned io_window_current_arch_mask(void)
{
    switch (g_mzhal.arch) {
        case 800:  return MZ_AVAIL_800;
        case 1500: return MZ_AVAIL_1500;
        case 700:  return MZ_AVAIL_700;
        default:   return MZ_AVAIL_ALL;
    }
}


/**
 * @brief Naformatuj 8-bit hodnotu jako binarni string (LSB vpravo).
 */
static void io_window_format_bin(uint8_t value, char *out)
{
    for (int i = 7; i >= 0; --i)
    {
        out[7 - i] = ((value >> i) & 1) ? '1' : '0';
    }
    out[8] = '\0';
}


/**
 * @brief Lidsky citelny string pro smer portu.
 */
static const char *io_window_dir_str(en_IO_PORT_DIR dir)
{
    switch (dir)
    {
    case IO_PORT_DIR_R:  return "R";
    case IO_PORT_DIR_W:  return "W";
    case IO_PORT_DIR_RW: return "R/W";
    default: return "?";
    }
}


/**
 * @brief Case-insensitive substring match.
 */
static bool io_str_contains_ci ( const char *haystack, const char *needle )
{
    if ( !needle || !*needle ) return true;
    if ( !haystack ) return false;
    size_t hl = strlen ( haystack );
    size_t nl = strlen ( needle );
    if ( nl > hl ) return false;
    for ( size_t i = 0; i + nl <= hl; i++ ) {
        size_t j = 0;
        for ( ; j < nl; j++ ) {
            char a = haystack[ i + j ];
            char b = needle[ j ];
            if ( a >= 'A' && a <= 'Z' ) a = (char) ( a - 'A' + 'a' );
            if ( b >= 'A' && b <= 'Z' ) b = (char) ( b - 'A' + 'a' );
            if ( a != b ) break;
        }
        if ( j == nl ) return true;
    }
    return false;
}


/**
 * @brief Filter match: na port name, addr hex string, direction.
 *
 * Match se aplikuje OR (= jakákoliv shoda projde).
 *
 * @param port  Port descriptor.
 * @param filter  Filter string z UI (g_io_ui.filter).
 * @return true pokud port projde filtrem nebo filter prázdný.
 */
static bool io_filter_match ( const st_IO_PORT_DESC *port, const char *filter )
{
    if ( !filter || !filter[ 0 ] ) return true;

    /* Match na name */
    if ( io_str_contains_ci ( port->name, filter ) ) return true;

    /* Match na addr hex (4-digit + 2-digit) */
    char addr_hex4[ 8 ];
    snprintf ( addr_hex4, sizeof ( addr_hex4 ), "%04X", (unsigned) port->addr );
    if ( io_str_contains_ci ( addr_hex4, filter ) ) return true;
    char addr_hex2[ 4 ];
    snprintf ( addr_hex2, sizeof ( addr_hex2 ), "%02X",
               (unsigned)( port->addr & 0xFF ) );
    if ( io_str_contains_ci ( addr_hex2, filter ) ) return true;

    /* Match na direction (R / W / R/W) */
    if ( io_str_contains_ci ( io_window_dir_str ( port->direction ),
                              filter ) ) return true;

    return false;
}


/**
 * @brief Vyber background tint barvu pro radek dle hits/s.
 *
 * Fix #9 (Michal preference): bg tint zruseny / decentni - pouze
 * pro velmi aktivni porty (>= g_io_ui.heat_bg_hot_threshold) jemny
 * cerveny tint pro spotting performance bottlenecks. Aktivita se
 * primarne indikuje barvou textu v io_activity_text_color (zelena).
 *
 * Práh je user-configurable (V1.7+ 2.7), default 10000 = puvodni
 * hardcoded hodnota.
 *
 * @param hits_per_sec  Aktualni hits/s pro port.
 * @return ImU32 barva (0 = bez barvy).
 */
static ImU32 io_activity_bg_color ( uint32_t hits_per_sec )
{
    uint32_t threshold = (uint32_t) g_io_ui.heat_bg_hot_threshold;
    if ( hits_per_sec < threshold ) return 0;
    return IM_COL32 ( 255, 80, 80, 30 );  /* very light red - bottleneck warn */
}


/**
 * @brief Barva textu radky podle aktivity (fix #9).
 *
 * Aktivni port (= hits/s >= g_io_ui.heat_text_active_threshold): light
 * green = jasna vizualni indikace "tady se neco deje". Pod prahem:
 * vraci false (= UI pouzije default text barvu).
 *
 * Práh je user-configurable (V1.7+ 2.7), default 1 = puvodni chovani
 * "jakýkoliv nenulový hit aktivuje zelený text".
 *
 * @param hits_per_sec  Aktualni hits/s.
 * @param out_color     Vystupni ImVec4 barva (validni jen pri true).
 * @return true pokud ma byt push barvy, false = nepushovat.
 */
static bool io_activity_text_color ( uint32_t hits_per_sec,
                                      ImVec4 *out_color )
{
    uint32_t threshold = (uint32_t) g_io_ui.heat_text_active_threshold;
    if ( threshold == 0 ) threshold = 1;  /* clamp - 0 by hned ukázalo zeleně i neaktivní */
    if ( hits_per_sec < threshold ) return false;
    *out_color = ImVec4 ( 0.5f, 1.0f, 0.5f, 1.0f );  /* light green */
    return true;
}


/* ========================================================================= */
/*  Bit detail render                                                        */
/* ========================================================================= */

/**
 * @brief Render bit-by-bit detailu jednoho portu (uvnitr collapse).
 */
static void io_window_render_bit_details ( const st_IO_PORT_DESC *port,
                                            uint8_t value )
{
    if ( !port->bits || port->bit_count == 0 ) {
        ImGui::TextDisabled ( "%s", _( "(no bit-level decode available)" ) );
        return;
    }

    ImGui::Indent ( );
    for ( size_t i = 0; i < port->bit_count; ++i ) {
        const st_IO_BIT_DESC *bit = &port->bits[ i ];

        if ( bit->bit_count == 1 ) {
            bool b = ( ( value >> bit->bit_index ) & 1 ) != 0;
            /* '0'/'1' misto ' '/'X' - v proporcionalnim fontu by 'X' a
             * mezera mely jinou sirku a zbytek radku by skakal. */
            ImGui::Text ( "bit %u [%c] : %-8s - %s",
                          (unsigned) bit->bit_index,
                          b ? '1' : '0',
                          bit->label ? bit->label : "",
                          bit->description ? bit->description : "" );
        } else {
            unsigned mask = ( 1u << bit->bit_count ) - 1u;
            unsigned field_val = ( value >> bit->bit_index ) & mask;
            unsigned msb = bit->bit_index + bit->bit_count - 1u;

            char binbuf[ 16 ];
            int bidx = 0;
            for ( int b = (int) bit->bit_count - 1; b >= 0; --b ) {
                binbuf[ bidx++ ] = ( ( field_val >> b ) & 1 ) ? '1' : '0';
            }
            binbuf[ bidx ] = '\0';

            ImGui::Text ( "bits %u..%u [%s] = %u (0x%X) : %-8s - %s",
                          (unsigned) bit->bit_index, msb,
                          binbuf, field_val, field_val,
                          bit->label ? bit->label : "",
                          bit->description ? bit->description : "" );
        }
    }

    if ( port->decode ) {
        const char *txt = port->decode ( value );
        if ( txt ) {
            ImGui::Spacing ( );
            ImGui::Text ( "decoded: %s", txt );
        }
    }
    ImGui::Unindent ( );
}


/**
 * @brief Format addr per V1.5 konvence.
 *
 * 8-bit IO porty: zobrazi 0x00..0xFF (= low byte).
 * 16-bit IO porty (0xCF<RR>): zobrazi 0xCF01..0xCF07 (= MZ konvence).
 *
 * @param addr  Port address.
 * @param out   Cilovy buffer (min 8 chars).
 */
static void io_format_addr ( uint16_t addr, char *out, size_t out_size )
{
    if ( addr >= 0xCF01 && addr <= 0xCF07 ) {
        snprintf ( out, out_size, "0x%04X", (unsigned) addr );
    } else if ( addr >= 0xE000 && addr <= 0xE008 ) {
        /* V1.5.E: MZ-700 MMIO mirror - 16-bit MMIO adresa zobrazena plne. */
        snprintf ( out, out_size, "0x%04X", (unsigned) addr );
    } else {
        snprintf ( out, out_size, "0x%02X", (unsigned)( addr & 0xFF ) );
    }
}


/* ========================================================================= */
/*  Render row                                                               */
/* ========================================================================= */

/**
 * @brief Render jednoho radku tabulky.
 *
 * Sloupce: Addr (collapsible), Name, Hex, Bin, R/W, Activity.
 * Right-click na adresu otevre context menu.
 */
static void io_window_render_port_row ( size_t idx, const st_IO_PORT_DESC *port )
{
    ImGui::PushID ( (int) idx );
    ImGui::TableNextRow ( );

    /* Background tint per activity hits/s (fix #9: decentni - jen >10000/s).
     * V1.5 fix #1: 16-bit indexing. 8-bit porty agregujeme přes 256
     * high-byte slotů, 16-bit porty (catalog > 0xFF) lookup na bus addr.
     *
     * V1.5.D fix #2: counter je per smer (IN vs OUT). Volime smer podle
     * port->direction:
     *   - DIR_R  => is_in=true  (jen IN counter)
     *   - DIR_W  => is_in=false (jen OUT counter)
     *   - DIR_RW => secteme oba smery (= jeden katalogovy radek pokryva
     *               oba) - rozliseni je na zodpovednost UI checkbox/toggle
     *               v V1.7+, V1.5 zobrazuje aggregat. */
    uint32_t hps;
    if ( port->direction == IO_PORT_DIR_RW ) {
        if ( port->addr <= 0xFF ) {
            hps = io_activity_get_hits_per_sec_8bit ( (uint8_t) port->addr, true )
                + io_activity_get_hits_per_sec_8bit ( (uint8_t) port->addr, false );
        } else {
            uint16_t bus = io_catalog_to_bus_addr ( port->addr );
            hps = io_activity_get_hits_per_sec ( bus, true )
                + io_activity_get_hits_per_sec ( bus, false );
        }
    } else {
        bool dir_in = ( port->direction == IO_PORT_DIR_R );
        if ( port->addr <= 0xFF ) {
            hps = io_activity_get_hits_per_sec_8bit ( (uint8_t) port->addr,
                                                       dir_in );
        } else {
            hps = io_activity_get_hits_per_sec (
                io_catalog_to_bus_addr ( port->addr ), dir_in );
        }
    }
    ImU32 tint = io_activity_bg_color ( hps );
    if ( tint != 0 ) {
        ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg0, tint );
    }

    /* Fix #9: aktivni port (hits/s > 0) -> zelena barva textu cele radky.
     * Push pred prvnim sloupcem; Pop az pred PopID na konci funkce. */
    ImVec4 act_color;
    bool pushed_text_color = io_activity_text_color ( hps, &act_color );
    if ( pushed_text_color ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, act_color );
    }

    /* Sloupec 0: Adresa (= TreeNode pokud má bit details) */
    ImGui::TableNextColumn ( );
    char addr_str[ 16 ];
    io_format_addr ( port->addr, addr_str, sizeof ( addr_str ) );

    bool has_details = ( port->bits && port->bit_count > 0 ) ||
                       ( port->decode != NULL );

    bool is_open = false;
    /* 48 = addr_str max 15 + "##port_" 7 + %zu max 20 + rezerva. */
    char addr_label[ 48 ];
    snprintf ( addr_label, sizeof ( addr_label ), "%s##port_%zu",
               addr_str, idx );

    if ( has_details ) {
        is_open = ImGui::TreeNodeEx ( addr_label,
                                       ImGuiTreeNodeFlags_SpanFullWidth );
    } else {
        ImGui::TextUnformatted ( addr_str );
    }

    /* Tooltip pro 0xCF entries */
    if ( port->addr >= 0xCF01 && port->addr <= 0xCF07
         && ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip (
            "%s",
            _( "16-bit CRTC family port. Bus addr is 0x<RR>CF "
               "(Z80 OUT (C),A pattern with B=register index, C=0xCF). "
               "MZ notation 0xCF<RR> is human-readable display." ) );
    }

    /* V1.5 fix #2: popup menu nesmi dedit row-level zelenou barvu.
     * Pop pred Begin, re-push po End. */
    if ( pushed_text_color ) {
        ImGui::PopStyleColor ( );
    }

    /* Right-click context menu */
    if ( ImGui::BeginPopupContextItem ( "##iorq_ctx" ) ) {
        /* V1.5.E: MMIO entries (0xE000-0xE008) nepouzivaji IORQ BP - jdou
         * pres MREQ -> nabidnout MEM_R / MEM_W BP misto IORQ R/W.
         * V1.5.F: pridany Add MEM R/W BP polozky (drive jen TextDisabled). */
        bool is_mmio_entry = ( port->addr >= 0xE000 && port->addr <= 0xE008 );
        if ( !is_mmio_entry ) {
            if ( ImGui::MenuItem ( _L ( "Add IORQ R BP##ioctx" ) ) ) {
                bpt_edit_panel_open_new_iorq ( port->addr, false );
            }
            if ( ImGui::MenuItem ( _L ( "Add IORQ W BP##ioctx" ) ) ) {
                bpt_edit_panel_open_new_iorq ( port->addr, true );
            }
        } else {
            if ( ImGui::MenuItem ( _L ( "Add MEM R BP##ioctx" ) ) ) {
                bpt_edit_panel_open_new_mem ( port->addr, false );
            }
            if ( ImGui::MenuItem ( _L ( "Add MEM W BP##ioctx" ) ) ) {
                bpt_edit_panel_open_new_mem ( port->addr, true );
            }
        }
        ImGui::Separator ( );
        if ( ImGui::MenuItem ( _L ( "Reset activity counter##ioctx" ) ) ) {
            /* V1.5 fix #1: per 16-bit slot reset. 8-bit porty agreguj
             * přes 256 high-byte slotů. */
            if ( port->addr <= 0xFF ) {
                io_activity_reset_port_8bit ( (uint8_t) port->addr );
            } else {
                io_activity_reset_port ( io_catalog_to_bus_addr ( port->addr ) );
            }
        }
        if ( ImGui::MenuItem ( _L ( "Show in History tab##ioctx" ) ) ) {
            /* Fix #4: prefill history filter + switch na History tab.
             *
             * 16-bit porty (catalog addr 0xCF01..0xCF07): bus addr =
             * (RR << 8) | 0xCF, kde RR = 0x01..0x07. Filter `port16:RRCF`
             * matchuje e->port == 0xRRCF přesně (full 16-bit).
             *
             * 8-bit porty: bus addr = (B << 8) | low_byte, kde B
             * je arbitrary (IN A,(N) má B=A, IN A,(C) má B=cokoliv).
             * V1.5.D+ má parser `port:` prefix s low-byte-only match
             * (= ignoruje high byte), takže matchuje VŠECHNY IORQ s
             * daným low byte bez ohledu na random B reg. */
            if ( port->addr >= 0xE000 && port->addr <= 0xE008 ) {
                /* V1.5.E: MMIO entry - addr: filter (= jen MMIO eventy). */
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "addr:%04X", (unsigned) port->addr );
            } else if ( port->addr >= 0xCF01 && port->addr <= 0xCF07 ) {
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "port16:%04X",
                           (unsigned) io_catalog_to_bus_addr ( port->addr ) );
            } else {
                /* 8-bit port - low byte match přes port: prefix. */
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "port:%02X",
                           (unsigned)( port->addr & 0xFF ) );
            }
            g_io_ui.want_switch_to_history = true;
        }
        /* Cross-window navigation: hint katalogu na související debug okno.
         * Banking porty 0xE0-0xE7 ukazují g_memory.map mirror (resp. MemExt
         * je per-page state v g_memext.map[16]) - plný decoded view (= per
         * 4 kB stránka Z80 prostoru) drží Memory Map debug okno. */
        if ( port->cross_target == IO_CROSS_MEMORY_MAP ) {
            ImGui::Separator ( );
            if ( ImGui::MenuItem ( _L ( "Show in Memory Map##ioctx" ) ) ) {
                g_gui->showMemoryMapWindow = true;
                memmap_window_request_focus ( );
            }
        }
        ImGui::EndPopup ( );
    }

    /* V1.5 fix #2: re-push row-level zelenou pro zbyle sloupce. */
    if ( pushed_text_color ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, act_color );
    }

    /* Sloupec 1: Name */
    ImGui::TableNextColumn ( );
    ImGui::TextUnformatted ( port->name ? port->name : "" );
    if ( port->long_name && port->long_name[ 0 ]
         && ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", port->long_name );
    }

    /* Live hodnota */
    uint8_t value = 0;
    bool have_value = false;
    if ( port->read_value ) {
        value = port->read_value ( );
        have_value = true;
    }

    /* Fix #10: pokud nemame real mirror, zkusime last value cache.
     * Distinct per smer: pro DIR_R volame is_in=true cache, jinak write.
     * Pro DIR_RW preferujeme write cache (= typicky se nastavuje hodnota
     * kterou pak emulator pouziva, write je relevantnejsi pro debugging). */
    bool cached = false;
    bool cached_stale = false;
    uint32_t cache_age = 0;
    if ( !have_value ) {
        bool prefer_in = ( port->direction == IO_PORT_DIR_R );
        uint32_t cur_frame = (uint32_t) g_gdg.total_elapsed.screens;
        uint8_t cv = 0;
        /* V1.5 fix #1: 16-bit indexing. 8-bit porty agregujeme přes
         * 256 high-byte slotů (= io_activity_get_last_value_8bit).
         * 16-bit porty (catalog > 0xFF): bus addr lookup. */
        bool got;
        if ( port->addr <= 0xFF ) {
            got = io_activity_get_last_value_8bit (
                (uint8_t) port->addr, prefer_in,
                cur_frame, &cv, &cache_age );
        } else {
            got = io_activity_get_last_value (
                io_catalog_to_bus_addr ( port->addr ), prefer_in,
                cur_frame, &cv, &cache_age );
        }
        if ( got ) {
            value = cv;
            have_value = true;  /* uz mame value (= z cache), pokracuj normalne */
            cached = true;
            /* 10s timeout @ 50Hz = ~500 frames. */
            cached_stale = ( cache_age >= 500 );
        } else if ( port->direction == IO_PORT_DIR_RW
                    || port->direction == IO_PORT_DIR_W ) {
            /* Pro RW zkus jeste read cache jako fallback. */
            bool got2;
            if ( port->addr <= 0xFF ) {
                got2 = io_activity_get_last_value_8bit (
                    (uint8_t) port->addr, true,
                    cur_frame, &cv, &cache_age );
            } else {
                got2 = io_activity_get_last_value (
                    io_catalog_to_bus_addr ( port->addr ), true,
                    cur_frame, &cv, &cache_age );
            }
            if ( got2 ) {
                value = cv;
                have_value = true;
                cached = true;
                cached_stale = ( cache_age >= 500 );
            }
        }
    }

    /* Sloupec 2: Hex
     * Fix #10:
     *  - Real mirror -> normalni hex
     *  - Cached < 500 frames -> normalni hex + tooltip "cached, age N frames"
     *  - Cached >= 500 frames -> dimmed (TextDisabled) + age tooltip
     *  - No data -> "??" + tooltip "no data captured" */
    ImGui::TableNextColumn ( );
    if ( have_value ) {
        if ( cached_stale ) {
            ImGui::TextDisabled ( "0x%02X", (unsigned) value );
        } else {
            ImGui::Text ( "0x%02X", (unsigned) value );
        }
        if ( cached && ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( _( "Last cached value %u frames ago" ),
                                 (unsigned) cache_age );
        }
    } else {
        ImGui::TextDisabled ( "??" );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "No data captured (port has no mirror and no IORQ event yet)" ) );
        }
    }

    /* Sloupec 3: Binary */
    ImGui::TableNextColumn ( );
    if ( have_value ) {
        char binbuf[ 9 ];
        io_window_format_bin ( value, binbuf );
        if ( cached_stale ) {
            ImGui::TextDisabled ( "%s", binbuf );
        } else {
            ImGui::TextUnformatted ( binbuf );
        }
    } else {
        ImGui::TextDisabled ( "--------" );
    }

    /* Sloupec 4: R/W */
    ImGui::TableNextColumn ( );
    ImGui::TextUnformatted ( io_window_dir_str ( port->direction ) );

    /* Sloupec 5: Activity */
    ImGui::TableNextColumn ( );
    if ( g_io_window_tracking_active ) {
        ImGui::Text ( "%u/s", (unsigned) hps );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip (
                "%s",
                _( "Hits per second over last 1s window (50 buckets)" ) );
        }
    } else {
        ImGui::TextDisabled ( "--" );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip (
                "%s", _( "Tracking disabled (panel closed or toggled off)" ) );
        }
    }

    /* Sloupec 6: Rec - V1.7+ 2.6 selective per-port history capture.
     *
     * Flag indexovany low byte adresy (= 8-bit I/O space). Pro MMIO
     * entries (0xE000-0xE008) je checkbox disabled - MMIO eventy
     * NEJSOU filtrovany v io_history_record_mem (= cely 0xE000-0xE008
     * rozsah jde do ringu vzdy, low-byte filtr by je nesmyslne mappnul
     * do 8-bit I/O prostoru).
     *
     * Default vsech portu = 1 (= back-compat).
     */
    ImGui::TableNextColumn ( );
    {
        bool is_mmio = ( port->addr >= 0xE000 && port->addr <= 0xE008 );
        if ( is_mmio ) {
            bool rec_dummy = true;
            ImGui::BeginDisabled ( true );
            ImGui::Checkbox ( "##io_rec", &rec_dummy );
            ImGui::EndDisabled ( );
            if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ) ) {
                ImGui::SetTooltip ( "%s",
                    _( "MMIO events (0xE000-0xE008) are always recorded "
                       "(per-port filter applies to 8-bit IORQ space only)" ) );
            }
        } else {
            uint8_t lb = (uint8_t) ( port->addr & 0xFF );
            bool rec_enabled = ( g_io_history_record_enabled[ lb ] != 0 );
            if ( ImGui::Checkbox ( "##io_rec", &rec_enabled ) ) {
                /* Atomic byte write - emu vlakno vidi novou hodnotu okamzite. */
                g_io_history_record_enabled[ lb ] = rec_enabled ? 1u : 0u;
            }
            if ( ImGui::IsItemHovered ( ) ) {
                ImGui::BeginTooltip ( );
                ImGui::PushTextWrapPos ( ImGui::GetFontSize ( ) * 35.0f );
                ImGui::TextUnformatted (
                    _( "Record IORQ events for this port into History "
                       "ring buffer. Uncheck to skip high-frequency ports "
                       "and preserve ring capacity for events of interest." ) );
                ImGui::PopTextWrapPos ( );
                ImGui::EndTooltip ( );
            }
        }
    }

    /* Bit detail (uvnitr expanderu) */
    if ( has_details && is_open ) {
        ImGui::TableNextRow ( );
        ImGui::TableNextColumn ( );
        ImGui::TableSetColumnIndex ( 1 );
        io_window_render_bit_details ( port, have_value ? value : 0 );
        ImGui::TreePop ( );
    }

    if ( pushed_text_color ) {
        ImGui::PopStyleColor ( );  /* uzavri row text color (fix #9). */
    }
    ImGui::PopID ( );
}


/* ========================================================================= */
/*  Render Overview tab                                                      */
/* ========================================================================= */

/**
 * @brief Vrati true pokud port name pasuje na section match_prefix.
 *
 * Poznamka: 0xCF<RR> entries maji name "GDG - SOF0 (W)" atd. = match na
 * "GDG -" prefix => spadnou do "gdg" sekce.
 */
static bool io_port_in_section ( const st_IO_PORT_DESC *port,
                                  const SectionGroup *sec )
{
    if ( !port->name || !sec || !sec->match_prefix ) return false;
    /* Prefix match na zacatku name. */
    size_t pl = strlen ( sec->match_prefix );
    return strncmp ( port->name, sec->match_prefix, pl ) == 0;
}


/**
 * @brief Render Overview tab - section grouping + tabulka.
 */
static void io_window_render_overview_tab ( void )
{
    unsigned arch_mask = io_window_current_arch_mask ( );

    /* Spočítej kolik portů je viditelných pro arch + filter */
    size_t total_arch = 0;
    size_t visible = 0;
    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        const st_IO_PORT_DESC *p = &g_io_ports[ i ];
        if ( !p->name ) continue;
        if ( p->available_for_mzarch != 0
             && ( p->available_for_mzarch & arch_mask ) == 0 ) continue;
        total_arch++;
        if ( io_filter_match ( p, g_io_ui.filter ) ) visible++;
    }

    /* === Sticky header (radek 1: filter) === */
    ImGui::SetNextItemWidth ( 240.0f );
    ImGui::InputTextWithHint ( "##io_filter",
                                _( "Filter..." ),
                                g_io_ui.filter, sizeof ( g_io_ui.filter ) );
    /* Plný filter syntax na hover (= placeholder zkracen na minimum,
     * fix #1). */
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Filter visible ports (case-insensitive substring match on "
               "name, address hex, or direction r/w/rw)." ) );
    }
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Clear##io_clear_filter" ) ) ) {
        g_io_ui.filter[ 0 ] = '\0';
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Clear filter" ) );
    }
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "(%zu/%zu)", visible, total_arch );

    /* === Sticky header (radek 2: tracking + capacity + auto-follow) === */
    if ( ImGui::Button ( _L ( "Reset Activity##io_reset_act" ) ) ) {
        io_activity_reset_all ( );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Clear activity counters for all 256 ports" ) );
    }
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    ImGui::TextUnformatted ( _( "Capacity:" ) );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 90.0f );
    /* Dropdown s 5 hodnotami - Sprint 2 Fáze 4.3 (per Michalovo
     * rozhodnutí #6: inline dropdown, ne modal Settings). */
    static const int s_capacity_options[]    = { 1000, 5000, 10000, 25000, 50000 };
    static const char *const s_capacity_labels[] = {
        "1000", "5000", "10000", "25000", "50000"
    };
    const int s_capacity_count = (int)( sizeof ( s_capacity_options )
                                       / sizeof ( s_capacity_options[ 0 ] ) );
    /* Najdi current option (= nejbližší match, fallback default 10000). */
    int sel_idx = 2;  /* default 10000 */
    for ( int i = 0; i < s_capacity_count; i++ ) {
        if ( g_io_ui.history_capacity == s_capacity_options[ i ] ) {
            sel_idx = i;
            break;
        }
    }
    if ( ImGui::BeginCombo ( "##io_capacity",
                              s_capacity_labels[ sel_idx ] ) ) {
        for ( int i = 0; i < s_capacity_count; i++ ) {
            bool is_sel = ( i == sel_idx );
            if ( ImGui::Selectable ( s_capacity_labels[ i ], is_sel ) ) {
                g_io_ui.history_capacity = s_capacity_options[ i ];
                io_history_set_capacity ( (size_t) g_io_ui.history_capacity );
            }
            if ( is_sel ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "History buffer size. Resize discards existing events." ) );
    }
    /* Auto-follow checkbox + Latest button přemístěny do History tab
     * sticky header (= fix #3, logicky tam patří). */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    bool track_b = ( g_io_ui.tracking_enable != 0 );
    if ( ImGui::Checkbox ( _L ( "Track##io_track" ), &track_b ) ) {
        g_io_ui.tracking_enable = track_b ? 1 : 0;
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Enable IORQ activity + history capture (gated). "
               "Default ON when panel is open. Toggle off to reduce overhead." ) );
    }

    /* Heat thresholds inline popup (V1.7+ 2.7).
     *
     * Globální prahy pro coloring Activity sloupce:
     *   - text_active_threshold: hits/s od kterého se text zbarví zeleně
     *   - bg_hot_threshold:      hits/s od kterého se radek tintuje červeně
     * Default 1 / 10000 = původní hardcoded chování (fix #9).
     *
     * Per Michalovo rozhodnutí #6: inline popup, ne modal Settings. */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Heat...##io_heat_btn" ) ) ) {
        ImGui::OpenPopup ( "io_heat_popup" );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Configure activity heat coloring thresholds." ) );
    }
    if ( ImGui::BeginPopup ( "io_heat_popup" ) ) {
        ImGui::TextUnformatted ( _( "Heat coloring thresholds (hits/s):" ) );
        ImGui::Separator ( );
        ImGui::SetNextItemWidth ( 120.0f );
        if ( ImGui::InputInt ( _L ( "Text active (green)##io_heat_text" ),
                                &g_io_ui.heat_text_active_threshold, 1, 10 ) ) {
            if ( g_io_ui.heat_text_active_threshold < 1 ) {
                g_io_ui.heat_text_active_threshold = 1;
            }
            if ( g_io_ui.heat_text_active_threshold > 1000000 ) {
                g_io_ui.heat_text_active_threshold = 1000000;
            }
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Minimum hits/s to color row text green. Default 1." ) );
        }
        ImGui::SetNextItemWidth ( 120.0f );
        if ( ImGui::InputInt ( _L ( "Bg hot tint (red)##io_heat_bg" ),
                                &g_io_ui.heat_bg_hot_threshold, 100, 1000 ) ) {
            if ( g_io_ui.heat_bg_hot_threshold < 1 ) {
                g_io_ui.heat_bg_hot_threshold = 1;
            }
            if ( g_io_ui.heat_bg_hot_threshold > 10000000 ) {
                g_io_ui.heat_bg_hot_threshold = 10000000;
            }
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Minimum hits/s for red row background tint "
                   "(bottleneck warn). Default 10000." ) );
        }
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "Reset to defaults##io_heat_reset" ) ) ) {
            g_io_ui.heat_text_active_threshold = 1;
            g_io_ui.heat_bg_hot_threshold      = 10000;
        }
        ImGui::EndPopup ( );
    }

    /* Apply tracking flag - propagate UI checkbox to hot-path flag.
     * Také aktualizuj cfg klíč (= persist user preference při exit).
     *
     * KRITICKÉ: při změně g_io_window_tracking_active (= 0→1 nebo 1→0)
     * je nutné přepočítat debugger callbacky přes dbg_ui_debugger_state_recompute()
     * (= mzarch_platform_fn_debugger_state_changed na EMU vlákně), aby
     * Z80 CPU pře-bindovala port_read/write callbacks na logging variantu
     * (jinak zůstanou default callbacks bez io_activity / io_history hooku
     * a Activity column + History tab zůstanou prázdné). Recompute jde přes
     * dbgapi na emu vlákno (ne přímo z UI vlákna) kvůli thread-safety.
     *
     * Volat jen na edge change, ne každý frame (= zbytečné z80_set_pread
     * každých 16 ms). */
    {
        uint8_t want = g_io_ui.tracking_enable ? 1u : 0u;
        if ( g_io_window_tracking_active != want ) {
            g_io_window_tracking_active = want;
            dbg_ui_debugger_state_recompute ( );
        }
    }
    g_io_ui.cfg_tracking_active = g_io_ui.tracking_enable;

    ImGui::Separator ( );

    /* === Section grouping + tabulka === */
    ImGuiTableFlags table_flags = ImGuiTableFlags_Borders
                                | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_Resizable
                                | ImGuiTableFlags_SizingStretchProp
                                | ImGuiTableFlags_ScrollY;

    /* Pokud žádný filter match - zobraz hint místo tabulky */
    if ( visible == 0 && g_io_ui.filter[ 0 ] != '\0' ) {
        ImGui::TextDisabled ( "%s", _( "No ports match filter" ) );
        return;
    }

    /* Vyska tabulky - vyplni dostupny prostor */
    ImVec2 avail = ImGui::GetContentRegionAvail ( );

    if ( ! ImGui::BeginTable ( "##io_tbl", 7, table_flags,
                                ImVec2 ( 0.0f, avail.y ) ) ) {
        return;
    }

    /* Headery anglicky natvrdo (= bypass cs překlad "Název" apod.,
     * konzistentně s Variables / Symbols panely V1.5.B/D). */
    ImGui::TableSetupColumn ( "Addr",
                               ImGuiTableColumnFlags_WidthStretch, 0.10f );
    ImGui::TableSetupColumn ( "Name",
                               ImGuiTableColumnFlags_WidthStretch, 0.30f );
    ImGui::TableSetupColumn ( "Hex",
                               ImGuiTableColumnFlags_WidthStretch, 0.07f );
    ImGui::TableSetupColumn ( "Binary",
                               ImGuiTableColumnFlags_WidthStretch, 0.16f );
    ImGui::TableSetupColumn ( "R/W",
                               ImGuiTableColumnFlags_WidthStretch, 0.06f );
    ImGui::TableSetupColumn ( "Activity",
                               ImGuiTableColumnFlags_WidthStretch, 0.13f );
    ImGui::TableSetupColumn ( "Rec",
                               ImGuiTableColumnFlags_WidthFixed, 40.0f );
    ImGui::TableHeadersRow ( );

    /* Render per section */
    for ( size_t s = 0; s < g_sections_count; s++ ) {
        const SectionGroup *sec = &g_sections[ s ];

        /* Spočítej kolik portů v této section je viditelných */
        size_t sec_visible = 0;
        for ( size_t i = 0; i < g_io_ports_count; i++ ) {
            const st_IO_PORT_DESC *p = &g_io_ports[ i ];
            if ( !p->name ) continue;
            if ( p->available_for_mzarch != 0
                 && ( p->available_for_mzarch & arch_mask ) == 0 ) continue;
            if ( !io_port_in_section ( p, sec ) ) continue;
            if ( !io_filter_match ( p, g_io_ui.filter ) ) continue;
            sec_visible++;
        }
        if ( sec_visible == 0 ) continue;

        /* Section header jako pseudo-row pres CollapsingHeader.
         * BeginTable + CollapsingHeader nelze stackovat primo, takze
         * TableNextRow + Selectable s manualnim collapse state. */
        ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );
        ImGui::TableNextColumn ( );
        ImGui::PushID ( (int) ( 1000 + s ) );
        char hdr_label[ 96 ];
        snprintf ( hdr_label, sizeof ( hdr_label ), "%s %s (%zu)",
                   g_io_ui.section_collapsed[ s ] ? "[+]" : "[-]",
                   sec->display, sec_visible );
        if ( ImGui::Selectable ( hdr_label, false,
                                  ImGuiSelectableFlags_SpanAllColumns ) ) {
            g_io_ui.section_collapsed[ s ] = !g_io_ui.section_collapsed[ s ];
        }
        ImGui::PopID ( );

        if ( g_io_ui.section_collapsed[ s ] ) continue;

        /* Render port rows v této section */
        for ( size_t i = 0; i < g_io_ports_count; i++ ) {
            const st_IO_PORT_DESC *p = &g_io_ports[ i ];
            if ( !p->name ) continue;
            if ( p->available_for_mzarch != 0
                 && ( p->available_for_mzarch & arch_mask ) == 0 ) continue;
            if ( !io_port_in_section ( p, sec ) ) continue;
            if ( !io_filter_match ( p, g_io_ui.filter ) ) continue;
            io_window_render_port_row ( i, p );
        }
    }

    ImGui::EndTable ( );
}


/* ========================================================================= */
/*  Render History tab                                                       */
/* ========================================================================= */

/**
 * @brief Najde port name v g_io_ports[] dle adresy, směru a typu události.
 *
 * Pro multi-entry (= 0xCE má R i W entry) vybírá podle is_in:
 *   is_in==true  -> hledá direction == R nebo RW
 *   is_in==false -> hledá direction == W nebo RW
 *
 * Match strategy:
 *   - is_mem==true  (MR/MW pro 0xE000-0xE008) -> full 16-bit addr match
 *     (catalog drží MMIO entries s addr = 0xE000..0xE008). 8-bit fallback
 *     by spadnul na IORQ entries se stejným low byte (= 0x01 -> CMT hack
 *     misidentifikace), proto pro MMIO events fallback NEDĚLÁME.
 *   - is_mem==false (IORQ IN/OUT) -> existující strategie:
 *     16-bit GDG family 0xCF<RR> match na full addr, ostatní 8-bit match
 *     na low byte (high byte = junk z B registru BC IORQ instrukce).
 *
 * @param port   16-bit IORQ / MMIO adresa.
 * @param is_in  true = IN / MR (CPU read), false = OUT / MW.
 * @param is_mem true = memory-mapped event (MR/MW pro 0xE000-0xE008),
 *               false = IORQ event.
 * @return Pointer na statický name string nebo NULL.
 */
static const char* io_history_lookup_port_name ( uint16_t port, bool is_in,
                                                  bool is_mem )
{
    /* MMIO event = full 16-bit match na catalog MMIO entries (0xE000-0xE008).
     * Žádný 8-bit fallback - sdílí low byte s IORQ entries (0x01, 0x02 = CMT
     * hack), takže by sváděl na špatnou identifikaci. */
    if ( is_mem ) {
        const st_IO_PORT_DESC *fallback = NULL;
        for ( size_t i = 0; i < g_io_ports_count; i++ ) {
            const st_IO_PORT_DESC *p = &g_io_ports[ i ];
            if ( !p->name ) continue;
            if ( p->addr != port ) continue;

            bool dir_ok = is_in
                ? ( p->direction == IO_PORT_DIR_R
                    || p->direction == IO_PORT_DIR_RW )
                : ( p->direction == IO_PORT_DIR_W
                    || p->direction == IO_PORT_DIR_RW );
            if ( dir_ok ) return p->name;
            if ( !fallback ) fallback = p;
        }
        return fallback ? fallback->name : NULL;
    }

    /* IORQ event = 16-bit GDG family 0xCF<RR> nebo 8-bit low match. */
    bool need_16bit = ( ( port & 0xFF ) == 0xCF && ( port >> 8 ) >= 0x01
                        && ( port >> 8 ) <= 0x07 );
    /* MZ notace 0xCF01..0xCF07 v katalogu */
    uint16_t key16 = need_16bit
        ? (uint16_t) ( 0xCF00 | ( port >> 8 ) )
        : 0;
    uint16_t key8 = (uint16_t) ( port & 0xFF );

    const st_IO_PORT_DESC *fallback = NULL;
    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        const st_IO_PORT_DESC *p = &g_io_ports[ i ];
        if ( !p->name ) continue;

        bool match = false;
        if ( need_16bit ) {
            if ( p->addr == key16 ) match = true;
        } else {
            /* 8-bit IO porty mají addr <= 0xFF; přeskoč 16-bit entries
             * (= MMIO 0xE000+ a GDG 0xCF<RR>). */
            if ( p->addr <= 0xFF && p->addr == key8 ) match = true;
        }
        if ( !match ) continue;

        /* Direction filter dle is_in */
        bool dir_ok = false;
        if ( is_in ) {
            dir_ok = ( p->direction == IO_PORT_DIR_R
                       || p->direction == IO_PORT_DIR_RW );
        } else {
            dir_ok = ( p->direction == IO_PORT_DIR_W
                       || p->direction == IO_PORT_DIR_RW );
        }
        if ( dir_ok ) return p->name;
        if ( !fallback ) fallback = p;
    }
    return fallback ? fallback->name : NULL;
}


/**
 * @brief Smart description event radku v History tabu (fix #8).
 *
 * Vraci kratky lidsky citelny popis udalosti pro nejdulezitejsi porty
 * (banking E0-E7, GDG DMD/BCOL/palette, PPI) s value-decoded informaci
 * kde to dava smysl. Pro porty bez explicitniho dekoderu se vraci NULL
 * (= UI fallback na port->name).
 *
 * Stratiegie: explicit lookup per port low byte. Pro porty bez spec
 * descriptor vraci NULL. Pouzity staticky buffer (= thread unsafe, jen
 * UI vlakno).
 *
 * @param port    Bus addr (full 16-bit z e->port).
 * @param value   IORQ / MMIO value.
 * @param is_in   true = IN / MR (CPU read), false = OUT / MW.
 * @param is_mem  true = MMIO event (0xE000-0xE008) - full addr decode,
 *                false = IORQ event - decode na low byte.
 * @return Pointer na statiky buffer s popisem nebo NULL.
 */
static const char* io_history_describe ( uint16_t port, uint8_t value,
                                          bool is_in, bool is_mem )
{
    static char buf[ 96 ];

    /* MMIO event = full 16-bit decode (0xE000-0xE008 mirror PIO/CTC/GDG).
     * Pro vsechny ostatni MMIO addresy vraci NULL -> fallback na catalog. */
    if ( is_mem ) {
        switch ( port ) {
            case 0xE000:
                return is_in ? "MMIO PPI Port A read" : "MMIO PPI Port A write";
            case 0xE001:
                return is_in ? "MMIO PPI Port B read" : "MMIO PPI Port B write";
            case 0xE002:
                return is_in ? "MMIO PPI Port C read" : "MMIO PPI Port C write";
            case 0xE003:
                return is_in ? NULL : "MMIO PPI Control word";
            case 0xE004:
                return "MMIO CTC counter 0 (audio)";
            case 0xE005:
                return "MMIO CTC counter 1 (HSYNC div)";
            case 0xE006:
                return "MMIO CTC counter 2 (50Hz INT)";
            case 0xE007:
                return is_in ? NULL : "MMIO CTC control";
            case 0xE008:
                if ( is_in ) {
                    return "MMIO GDG Status (HBLN/TEMPO/JOY)";
                }
                /* OUT 0xE008 NENI DMD! Zapis na 0xE008 jde do gdg_write_byte
                 * case 0x08 (mz800_gdg.c), kde se z hodnoty bere jen bit 0 a
                 * ridi se jim CTC0 GATE0 (ctc8253_gate(0, value & 1)). DMD
                 * registr je naopak IORQ port 0xCE. Drivejsi dekodovani jako
                 * "GDG DMD: 320x200x16" bylo chybne (napr. XOR a; LD (0E008h),a
                 * = GATE0 off, ne zadny videorezim). */
                snprintf ( buf, sizeof ( buf ),
                           "MMIO CTC0 GATE0 = %u (audio gate)",
                           (unsigned)( value & 0x01 ) );
                return buf;
            default:
                return NULL;
        }
    }

    uint8_t low = (uint8_t)( port & 0xFF );

    /* 16-bit GDG family 0xCF<RR> - rozlisovat per RR. */
    if ( low == 0xCF ) {
        unsigned rr = (unsigned)( port >> 8 );
        switch ( rr ) {
            case 0x01:
                snprintf ( buf, sizeof ( buf ), "GDG SOF0 = %u", value );
                return buf;
            case 0x02:
                snprintf ( buf, sizeof ( buf ),
                           "GDG SOF1 = %u (high 2 bits)",
                           (unsigned)( value & 0x03 ) );
                return buf;
            case 0x03:
                snprintf ( buf, sizeof ( buf ), "GDG SW = %u", value );
                return buf;
            case 0x04:
                snprintf ( buf, sizeof ( buf ), "GDG SSA = %u", value );
                return buf;
            case 0x05:
                snprintf ( buf, sizeof ( buf ), "GDG SEA = %u", value );
                return buf;
            case 0x06:
                snprintf ( buf, sizeof ( buf ), "GDG BCOL = %u",
                           (unsigned)( value & 0x0F ) );
                return buf;
            case 0x07:
                return "GDG CKSW (superimpose)";
            default:
                return NULL;
        }
    }

    switch ( low ) {
        /* === GDG === */
        case 0xCC:
            return is_in ? NULL : "GDG WF (write format)";
        case 0xCD:
            return is_in ? NULL : "GDG RF (read format)";
        case 0xCE: {
            if ( is_in ) {
                return "GDG Status (VBLN/HBLN/VSY/HSY)";
            }
            /* DMD value decoded */
            const char *mode;
            switch ( ( value >> 2 ) & 0x03 ) {
                case 0: mode = "320x200x16 (MZ-800)"; break;
                case 1: mode = "640x200x4 (MZ-800)"; break;
                case 2: mode = "MZ-700 mode"; break;
                default: mode = "illegal"; break;
            }
            snprintf ( buf, sizeof ( buf ), "GDG DMD: %s", mode );
            return buf;
        }
        /* === Memory banking === (E0-E4 dispatchuji na addr, value irrelevant) */
        case 0xE0:
            return is_in
                ? "MEM: map CGROM/VRAM at 0x1000"
                : "MEM: unmap ROM 0000 + ROM 1000";
        case 0xE1:
            return is_in
                ? "MEM: unmap CGROM/VRAM at 0x1000"
                : "MEM: unmap ROM E000";
        case 0xE2:
            return is_in ? NULL : "MEM: map ROM 0000";
        case 0xE3:
            return is_in ? NULL : "MEM: map ROM E000";
        case 0xE4:
            return is_in ? NULL : "MEM: reset to default mapping";
        case 0xE6:
            if ( !is_in ) {
                snprintf ( buf, sizeof ( buf ), "MEM: PEHU bank %u", value );
                return buf;
            }
            return NULL;
        case 0xE7:
            if ( !is_in ) {
                snprintf ( buf, sizeof ( buf ),
                           "MEM: MEMEXT bank %u", value );
                return buf;
            }
            return NULL;
        /* === GDG palette / JOY === */
        case 0xF0:
            if ( is_in ) return "JOY0 read";
            snprintf ( buf, sizeof ( buf ),
                       "GDG Palette write (value=0x%02X)", value );
            return buf;
        case 0xF1:
            return is_in ? "JOY1 read" : "GDG palette/colour group";
        /* === PSG === */
        case 0xF2:
            return is_in ? NULL : "PSG SN76489 write";
        case 0xF3:
            return is_in ? NULL : "PSG stereo left write";
        case 0xF9:
            return is_in ? NULL : "PSG stereo right write";
        /* === SIO QD (F4-F7) === */
        case 0xF4: case 0xF5: case 0xF6: case 0xF7:
            return "Z80 SIO (Quick Disk)";
        /* === PPI 8255 === */
        case 0xD0:
            return is_in
                ? "PPI Port A (keyboard col read)"
                : "PPI Port A (keyboard col select)";
        case 0xD1:
            return is_in
                ? "PPI Port B (keyboard row read)"
                : "PPI Port B write";
        case 0xD2:
            return is_in
                ? "PPI Port C (CMT/INT in)"
                : "PPI Port C (CMT/INT out)";
        case 0xD3:
            return is_in ? NULL : "PPI Control word";
        /* === CTC 8253 === */
        case 0xD4:
            return "CTC counter 0 (audio)";
        case 0xD5:
            return "CTC counter 1 (HSYNC div)";
        case 0xD6:
            return "CTC counter 2 (50Hz INT)";
        case 0xD7:
            return is_in ? NULL : "CTC control";
        /* === FDC === */
        case 0xD8:
            return is_in ? "FDC status" : "FDC command";
        case 0xD9:
            return "FDC track";
        case 0xDA:
            return "FDC sector";
        case 0xDB:
            return "FDC data";
        /* 0xDC-0xDF jsou externi Sharp logika, ne WD279x chip. Write-only. */
        case 0xDC:
            return is_in ? NULL : "FDC motor / drive select";
        case 0xDD:
            return is_in ? NULL : "FDC side";
        case 0xDE:
            return is_in ? NULL : "FDC density";
        case 0xDF:
            return is_in ? NULL : "FDC HD Patch EINT";
        /* === FDC1 (sekundarni, 0x58-0x5F) === */
        case 0x58:
            return is_in ? "FDC1 status" : "FDC1 command";
        case 0x59:
            return "FDC1 track";
        case 0x5A:
            return "FDC1 sector";
        case 0x5B:
            return "FDC1 data";
        /* 0x5C-0x5F jsou externi Sharp logika, ne WD279x chip. Write-only. */
        case 0x5C:
            return is_in ? NULL : "FDC1 motor / drive select";
        case 0x5D:
            return is_in ? NULL : "FDC1 side";
        case 0x5E:
            return is_in ? NULL : "FDC1 density";
        case 0x5F:
            return is_in ? NULL : "FDC1 HD Patch EINT";
        /* === Z80 PIO === */
        case 0xFC:
            return "Z80 PIO Port A control";
        case 0xFD:
            return "Z80 PIO Port B control";
        case 0xFE:
            return "Z80 PIO Port A data";
        case 0xFF:
            return "Z80 PIO Port B data";
        default:
            return NULL;
    }
}


/**
 * @brief Format adresy portu pro History tab (= MZ notace).
 *
 * 0xCF<RR> z bus addr (= port & 0xFF == 0xCF + port >> 8 in 1..7) zobrazí
 * jako "0xCF01" (= MZ konvence). Ostatní 8-bit jako "0xCC".
 *
 * @param port  16-bit BC IORQ addr.
 * @param out   Cilovy buffer (min 8 chars).
 * @param sz    Velikost out bufferu.
 */
static void io_history_format_port ( uint16_t port, char *out, size_t sz )
{
    uint8_t low = (uint8_t)( port & 0xFF );
    uint8_t high = (uint8_t)( port >> 8 );
    if ( low == 0xCF && high >= 0x01 && high <= 0x07 ) {
        /* MZ notace = 0xCF<RR> */
        snprintf ( out, sz, "0xCF%02X", (unsigned) high );
    } else {
        snprintf ( out, sz, "0x%02X", (unsigned) low );
    }
}


/**
 * @brief Render detail panelu pro vybraný event.
 *
 * V1.5 fix #7: Panel je v fixed-height BeginChild s vlastnim scrollbar
 * pro pripad, ze obsah presahne (= delsi descriptions). Multiline format
 * misto jedne dlouhe BulletText radky. NULL event = "(no selection)"
 * placeholder.
 *
 * V1.5.D fix #1: pridan horizontal splitter mezi tabulkou a detail
 * panelem. User muze drag splitter vertikalne pro resize. Vyska persist
 * v g_io_ui.detail_panel_height (60..400 px). Default 160 px (= 5 radku
 * + padding) pro pohodlne zobrazeni Cycle pole pridaneho v fix #3.
 *
 * @param e  Event nebo NULL (= nic nezobrazí).
 */
static void io_history_render_selected_detail ( const st_IO_HISTORY_EVENT *e )
{
    ImGui::Separator ( );

    /* V1.5.D fix #1: drag splitter pro resize panelu. ImGui nema native
     * splitter widget, implementujeme pres tenky Button + IsItemActive
     * + MouseDelta.y. Vertical drag posouva detail_panel_height. */
    {
        ImGui::PushStyleColor ( ImGuiCol_Button,
                                  ImVec4 ( 0.30f, 0.30f, 0.32f, 1.0f ) );
        ImGui::PushStyleColor ( ImGuiCol_ButtonHovered,
                                  ImVec4 ( 0.45f, 0.45f, 0.50f, 1.0f ) );
        ImGui::PushStyleColor ( ImGuiCol_ButtonActive,
                                  ImVec4 ( 0.55f, 0.55f, 0.60f, 1.0f ) );
        ImGui::Button ( "##io_hist_splitter", ImVec2 ( -1.0f, 4.0f ) );
        ImGui::PopStyleColor ( 3 );

        if ( ImGui::IsItemActive ( ) ) {
            float dy = ImGui::GetIO ( ).MouseDelta.y;
            int nh = g_io_ui.detail_panel_height - (int) dy;
            if ( nh < 60 )  nh = 60;
            if ( nh > 400 ) nh = 400;
            g_io_ui.detail_panel_height = nh;
        }
        if ( ImGui::IsItemHovered ( ) || ImGui::IsItemActive ( ) ) {
            ImGui::SetMouseCursor ( ImGuiMouseCursor_ResizeNS );
        }
    }

    /* Dynamic-height child s vlastni scroll - panel uz nikdy ne-prelije
     * hlavni window. Vyska z g_io_ui.detail_panel_height (60..400 px). */
    int dh = g_io_ui.detail_panel_height;
    if ( dh < 60 )  dh = 60;
    if ( dh > 400 ) dh = 400;
    if ( !ImGui::BeginChild ( "##io_hist_sel_detail",
                               ImVec2 ( 0.0f, (float) dh ),
                               false,
                               ImGuiWindowFlags_NoSavedSettings ) ) {
        ImGui::EndChild ( );
        return;
    }

    if ( !e ) {
        ImGui::TextDisabled ( "%s", _( "(no selection)" ) );
        ImGui::TextDisabled ( "%s", _( "Click a row to see details" ) );
        ImGui::EndChild ( );
        return;
    }

    bool is_in  = ( ( e->flags & IO_HISTORY_FLAG_READ ) != 0 );
    bool is_mem = ( ( e->flags & IO_HISTORY_FLAG_MEMORY ) != 0 );
    const char *pname = io_history_lookup_port_name ( e->port, is_in, is_mem );
    const char *descr = io_history_describe ( e->port, e->value, is_in, is_mem );
    char addr_str[ 16 ];
    io_history_format_port ( e->port, addr_str, sizeof ( addr_str ) );

    ImGui::Text ( "%s", _( "Selected event:" ) );
    /* V1.5 fix #7: multiline format - kazdy klic na vlastnim radku misto
     * jedne dlouhe BulletText, ktera se v uzkem okne orezavala.
     * V1.5.D fix #3: Cycle pridan na prvni radek.
     *
     * disasm-upgrade: PC value je interaktivni widget (focus to disasm,
     * RMB popup s "Focus to..." submenu, Add to bookmarks, Add breakpoint).
     * Z toho duvodu se prvni radek nerendruje jako jeden BulletText, ale
     * jako sekvence Bullet + Text + SameLine + tlacitko PC. */
    ImGui::Bullet ( );
    /* AlignTextToFramePadding aby textovy obsah radku fluiy zarovnal
     * s tlacitkem PC (= jinak by text seděl výše než button glyph). */
    ImGui::AlignTextToFramePadding ( );
    ImGui::Text ( "Frame: %u  Scanline: %u  px: %u  Cycle: %u  ",
                   (unsigned) e->frame,
                   (unsigned) e->scanline,
                   (unsigned) e->px,
                   (unsigned) e->cpu_cycle );
    ImGui::SameLine ( 0.0f, 0.0f );
    ImGui::TextUnformatted ( "PC:" );
    ImGui::SameLine ( );

    /* Klikatelny PC jako tlacitko. LMB -> dbg_disasm_show_in_slot(0);
     * RMB -> trigger popup mimo widget scope (popup ID nesmi zaviset
     * na widget stacku). Tooltip popis dual chovani. */
    {
        char pc_btn[ 24 ];
        snprintf ( pc_btn, sizeof ( pc_btn ), "0x%04X###io_pc_btn",
                   (unsigned) e->pc );
        if ( ImGui::SmallButton ( pc_btn ) ) {
            dbg_disasm_show_in_slot ( 0, e->pc );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Focus to primary disassembly. "
                   "Right-click for additional actions." ) );
        }
        if ( ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
            g_io_ui.pc_popup_open = true;
            g_io_ui.pc_popup_addr = e->pc;
        }
    }

    /* V1.5.F fix: rozlisit IORQ vs MMIO event v detail panelu.
     * IORQ: "Port: 0xCE  Type: IN/OUT"
     * MMIO: "Addr: 0xE001 Type: MR/MW" (Port nedava smysl - neni IORQ adresa). */
    if ( is_mem ) {
        ImGui::BulletText ( "Addr: 0x%04X  Type: %s  Value: 0x%02X",
                             (unsigned) e->port,
                             is_in ? "MR" : "MW",
                             (unsigned) e->value );
    } else {
        ImGui::BulletText ( "Port: %s  Type: %s  Value: 0x%02X",
                             addr_str,
                             is_in ? "IN" : "OUT",
                             (unsigned) e->value );
    }
    ImGui::BulletText ( "Description: %s",
                         descr ? descr : ( pname ? pname : "(unknown)" ) );

    /* === RMB popup pro klikatelne PC ===
     * OpenPopup volame az tady mimo SameLine retezec a hover scope, aby
     * popup ID nezavislelo na widget stacku (= stejny pattern jako
     * bm_window rmb_popup_open flag). Popup nabizi:
     *   - Focus to ... ▶ (5x slot disasm #1..#5)
     *   - Add to bookmarks
     *   - Add breakpoint (PC_EXEC)
     */
    if ( g_io_ui.pc_popup_open ) {
        ImGui::OpenPopup ( "io_pc_popup" );
        g_io_ui.pc_popup_open = false;
    }
    if ( ImGui::BeginPopup ( "io_pc_popup" ) ) {
        uint16_t addr = g_io_ui.pc_popup_addr;

        if ( ImGui::BeginMenu ( _L ( "Focus to..." ) ) ) {
            for ( int slot = 0; slot < 5; slot++ ) {
                char buf[ 64 ];
                snprintf ( buf, sizeof ( buf ),
                           "Disassembly #%d###io_pc_show_%d",
                           slot + 1, slot );
                if ( ImGui::MenuItem ( buf ) ) {
                    dbg_disasm_show_in_slot ( slot, addr );
                }
            }
            ImGui::EndMenu ( );
        }

        ImGui::Separator ( );

        /* Add to bookmarks - kopiruje 1:1 vzor z dbg_disassembled.cpp
         * (= symbol jmeno pokud existuje, jinak hex literal #XXXX;
         * comment empty - user doplni v Bookmarks okne; po pridani
         * okno otevreme = visual feedback). */
        if ( ImGui::MenuItem ( _L ( "Add to bookmarks" ) ) ) {
            const st_SYMBOL *sym = sym_db_lookup_by_addr ( addr, 0 );
            char input_buf[ 64 ];
            if ( sym && sym->name && sym->name[ 0 ] ) {
                snprintf ( input_buf, sizeof ( input_buf ),
                           "%s", sym->name );
            } else {
                snprintf ( input_buf, sizeof ( input_buf ),
                           "#%04X", (unsigned) addr );
            }
            bookmarks_add ( input_buf, "" );
            g_gui->showBookmarksWindow = true;
        }

        /* Add breakpoint - PC_EXEC fixed addr. Pokud BP na adrese uz
         * existuje, dbg_ui_bp_add stejne zavola CMD_BP_ADD a vznikne
         * druhy/N-ty BP; chovani identicke jako "Set Breakpoint" v
         * dbg_disassembled.cpp (= zadna deduplikace na UI vrstve, BP
         * core to pripousti). User-facing edge case je akceptovany. */
        if ( ImGui::MenuItem ( _L ( "Add breakpoint" ) ) ) {
            dbg_ui_bp_add ( addr, NULL );
            dbg_refresh_request ( );
        }

        ImGui::EndPopup ( );
    }

    ImGui::EndChild ( );
}


/**
 * @brief Append token do history filter bufferu se separátorem.
 *
 * Pokud je `g_io_ui.history_filter` prázdný, zapíše jen samotný formátovaný
 * token. Jinak nejprve prefixuje separátorem (`" "` pro AND chain, `" | "`
 * pro OR chain), pak naformátovaný token. Truncate-safe (snprintf clamp);
 * pokud by append nepřežil v buffer kapacitě, funkce mlčky neudělá nic
 * (= quick-action je silent fail, uživatel vidí že se filter nezměnil
 * a může napsat ručně).
 *
 * Helper extrahovaný z 18× duplikovaného snprintf+strlen patternu v
 * `Add AND:` / `Add OR:` quick-action menu položkách (V1.7+ položka 5.2
 * v CHECKLIST.md, debugger-fixes-5).
 *
 * @param sep        Separátor mezi current bufferem a tokenem (`" "` nebo `" | "`).
 * @param token_fmt  printf-style formát tokenu (např. `"port:%02X"`).
 */
static void io_history_filter_append ( const char *sep,
                                       const char *token_fmt, ... )
{
    size_t cur = strlen ( g_io_ui.history_filter );
    size_t avail = sizeof ( g_io_ui.history_filter ) - cur;
    if ( avail == 0 ) return;
    char *p = g_io_ui.history_filter + cur;
    if ( cur > 0 ) {
        int n = snprintf ( p, avail, "%s", sep );
        if ( n < 0 || (size_t) n >= avail ) return;
        p += n;
        avail -= (size_t) n;
    }
    va_list ap;
    va_start ( ap, token_fmt );
    vsnprintf ( p, avail, token_fmt, ap );
    va_end ( ap );
}


/**
 * @brief File-scope History tab filter (lazy init, free přes atexit).
 *
 * Single instance per proces - History tab renderer je volaný z hlavního
 * vlákna a okno existuje nejvýše v jedné kopii. Lazy init při prvním
 * render_history_tab volání; uvolnění registrované přes atexit aby
 * valgrind nehlásil reachable block (V1.7+ položka 5.3 v CHECKLIST.md).
 */
static st_IO_HISTORY_FILTER *s_io_history_filter = NULL;


/**
 * @brief atexit callback - uvolní s_io_history_filter pokud byl alokován.
 *
 * Registruje se jednou při první lazy-init v io_window_render_history_tab().
 * Idempotentní (NULL check), volání po free znova nic neudělá.
 */
static void io_window_history_filter_cleanup ( void )
{
    if ( s_io_history_filter ) {
        io_history_filter_free ( s_io_history_filter );
        s_io_history_filter = NULL;
    }
}


/**
 * @brief Render History tab - plná 7-sloupcová tabulka events.
 *
 * Layout:
 *   Sticky header: filter input + counter + Latest button
 *   Tabulka: Frame, Cycle, Type, Port, Value, PC, Direction
 *   Detail panel: vybraný event s lookup name z io_catalog
 *
 * Filter syntax (Fáze 3.2/3.3): "port:CE", "pc:4042", "frame:>100",
 * "in"/"out", plain text = name match.
 *
 * Auto-follow: při g_io_ui.auto_follow == true scrolluje na poslední
 * řádek. User scroll up = automaticky disable; tlačítko [Latest] re-enable.
 *
 * Click row = highlight + detail panel pod tabulkou (= žádný goto disasm
 * v V1.5, viz Michalovo rozhodnutí #2).
 */
static void io_window_render_history_tab ( void )
{
    /* === Parse filter syntax (Fáze 3.3) ===
     *
     * V1.5.F (boolean-expr refactor Fáze 1): filter je nyní opaque AST
     * + arena alokovaný heap. Single static instance per okno (lazy init,
     * lifetime aplikace - free přes atexit, viz s_io_history_filter +
     * io_window_history_filter_cleanup). Arena se recykluje uvnitř
     * parse() / reset(), takže žádný leak per-frame. */
    if ( !s_io_history_filter ) {
        s_io_history_filter = io_history_filter_new ( );
        atexit ( io_window_history_filter_cleanup );
    }
    st_IO_HISTORY_FILTER *s_hf = s_io_history_filter;
    char hf_err[ 80 ] = "";
    bool hf_ok = ( s_hf != NULL )
                 && io_history_filter_parse ( g_io_ui.history_filter, s_hf,
                                               hf_err, sizeof ( hf_err ) );
    bool hf_uses_name = io_history_filter_uses_name ( s_hf );

    /* === Sticky header (řádek 1: filter + counter + Latest) === */
    ImGui::SetNextItemWidth ( 280.0f );
    ImGui::InputTextWithHint ( "##io_hist_filter",
        _( "Filter..." ),
        g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ) );
    /* Plný filter syntax na hover (= placeholder zkracen na minimum,
     * fix #1). */
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Filter syntax (boolean expression):\n"
               "Atoms (leaf tokens):\n"
               "  port:CE       low byte match (8-bit IORQ, ignores random B reg)\n"
               "  port:C0-CF    low byte range\n"
               "  port16:00CE   full 16-bit match (explicit BC pattern)\n"
               "  port16:CF00-CF0F  16-bit range\n"
               "  pc:4042 / pc:4000-40FF          PC match / range\n"
               "  value:42 / value:00-7F          value match / range\n"
               "  frame:>100 / frame:<100 / frame:50-150\n"
               "  cycle:>1000000 / cycle:1000-2000\n"
               "  addr:E008 / addr:E000-E008      MMIO address match / range\n"
               "  in / out      IORQ direction\n"
               "  mr / mw       MMIO direction\n"
               "  text          name substring (case-insensitive)\n"
               "Operators (high to low precedence):\n"
               "  ( )           grouping\n"
               "  !             NOT (over leaf or parenthesized expression)\n"
               "  space & AND   AND (AND keyword is uppercase only)\n"
               "  | OR          OR  (OR keyword is uppercase only)\n"
               "Examples:\n"
               "  port:CE pc:4042            both must match\n"
               "  port:CE | port:CF          either port\n"
               "  (port:CE pc:42) | port:CF  group OR group\n"
               "  !(in | out)                neither IN nor OUT" ) );
    }
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Clear##io_hist_clear" ) ) ) {
        g_io_ui.history_filter[ 0 ] = '\0';
    }
    ImGui::SameLine ( );

    /* Counter visible/total - vypocitame az pri renderingu, zatim placeholder. */
    size_t total = g_io_history.count;
    /* visible spocitam below pri prvnim prochodu (= 2x loop). Pro rozumny
     * default tady pouzij total a pak Update inline. */
    /* Pro presny counter projedeme eventy 1x predem. */
    size_t visible = 0;
    if ( hf_ok ) {
        for ( size_t k = 0; k < total; k++ ) {
            const st_IO_HISTORY_EVENT *e = io_history_get ( k );
            if ( !e ) continue;
            const char *pname = NULL;
            if ( hf_uses_name ) {
                pname = io_history_lookup_port_name (
                    e->port,
                    ( e->flags & IO_HISTORY_FLAG_READ ) != 0,
                    ( e->flags & IO_HISTORY_FLAG_MEMORY ) != 0 );
            }
            if ( io_history_filter_match ( s_hf, e, pname ) ) visible++;
        }
    }
    ImGui::TextDisabled ( "(%zu/%zu)", visible, total );

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    /* Latest tlačítko = re-enable auto-follow + immediate scroll to bottom
     * (= request signal pro render kód níže). Fix #3. */
    bool want_jump_latest = false;
    if ( ImGui::Button ( _L ( "Latest##io_hist_latest" ) ) ) {
        g_io_ui.auto_follow = 1;
        want_jump_latest = true;
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Re-enable auto-follow and scroll to latest event" ) );
    }
    ImGui::SameLine ( );
    bool af_hist_b = ( g_io_ui.auto_follow != 0 );
    if ( ImGui::Checkbox ( _L ( "Auto-follow##io_hist_af" ), &af_hist_b ) ) {
        g_io_ui.auto_follow = af_hist_b ? 1 : 0;
        /* Při toggle na ON treat jako [Latest] click = bypass scroll detect
         * (jinak by detection logika vypla auto_follow ihned protože scroll_y
         * != scroll_max při toggle uprostřed tabulky). */
        if ( af_hist_b ) {
            want_jump_latest = true;
        }
    }
    /* Fix #2: smysluplny tooltip (= Sprint 2 blabol nahrazen). */
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Auto-scroll History tab to latest event. User scroll up "
               "disables follow; click [Latest] re-enables." ) );
    }
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Clear history##io_hist_purge" ) ) ) {
        io_history_clear ( );
        g_io_ui.history_selected_visible = -1;
        g_io_ui.history_selected_logical = -1;
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Discard all recorded IORQ events" ) );
    }

    /* === V1.5 fix #3: druhý řádek - Capacity + Track ===
     *
     * Stejny widget jako v Overview sticky header, sdileny stav
     * g_io_ui.history_capacity / g_io_ui.tracking_enable. Logicky
     * zde patri (= Capacity je primarne pro History, Track ovlivnuje
     * History capture). */
    ImGui::TextUnformatted ( _( "Capacity:" ) );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 90.0f );
    static const int s_hcap_options[] = { 1000, 5000, 10000, 25000, 50000 };
    static const char *const s_hcap_labels[] = {
        "1000", "5000", "10000", "25000", "50000"
    };
    const int s_hcap_count = (int)( sizeof ( s_hcap_options )
                                    / sizeof ( s_hcap_options[ 0 ] ) );
    int hsel_idx = 2;
    for ( int i = 0; i < s_hcap_count; i++ ) {
        if ( g_io_ui.history_capacity == s_hcap_options[ i ] ) {
            hsel_idx = i;
            break;
        }
    }
    if ( ImGui::BeginCombo ( "##io_hist_capacity",
                              s_hcap_labels[ hsel_idx ] ) ) {
        for ( int i = 0; i < s_hcap_count; i++ ) {
            bool is_sel = ( i == hsel_idx );
            if ( ImGui::Selectable ( s_hcap_labels[ i ], is_sel ) ) {
                g_io_ui.history_capacity = s_hcap_options[ i ];
                io_history_set_capacity ( (size_t) g_io_ui.history_capacity );
            }
            if ( is_sel ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "History buffer size. Resize discards existing events." ) );
    }
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    bool track_h_b = ( g_io_ui.tracking_enable != 0 );
    if ( ImGui::Checkbox ( _L ( "Track##io_hist_track" ), &track_h_b ) ) {
        g_io_ui.tracking_enable = track_h_b ? 1 : 0;
        /* Edge change → propagate hot-path flag (stejne jako Overview). */
        uint8_t want = g_io_ui.tracking_enable ? 1u : 0u;
        if ( g_io_window_tracking_active != want ) {
            g_io_window_tracking_active = want;
            dbg_ui_debugger_state_recompute ( );
        }
        g_io_ui.cfg_tracking_active = g_io_ui.tracking_enable;
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Enable IORQ activity + history capture (gated). "
               "Default ON when panel is open. Toggle off to reduce overhead." ) );
    }

    /* Parse error indikace pod headerem (red text). */
    if ( !hf_ok ) {
        ImVec4 red ( 1.0f, 0.4f, 0.4f, 1.0f );
        ImGui::TextColored ( red, "%s: %s", _( "Filter error" ),
                              hf_err[ 0 ] ? hf_err : "?" );
    }

    ImGui::Separator ( );

    /* === Tabulka events === */
    ImGuiTableFlags hflags = ImGuiTableFlags_Borders
                           | ImGuiTableFlags_RowBg
                           | ImGuiTableFlags_ScrollY
                           | ImGuiTableFlags_Resizable
                           | ImGuiTableFlags_NoSavedSettings;

    /* Vyhradime prostor: tabulka vyplni az na detail panel.
     * V1.5 fix #7 -> V1.5.D fix #1: panel je dynamic-height z
     * g_io_ui.detail_panel_height + Separator + splitter (4 px) +
     * spacing => rezerva = detail_h + 19 px (Separator ~3, splitter
     * 4, internal padding ~12). */
    int dh = g_io_ui.detail_panel_height;
    if ( dh < 60 )  dh = 60;
    if ( dh > 400 ) dh = 400;
    float detail_h = (float) dh + 19.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail ( );
    float tbl_h = avail.y - detail_h;
    if ( tbl_h < 100.0f ) tbl_h = 100.0f;

    /* Fix #6: Direction column odstranen (= duplicita s Type IN/OUT).
     * Fix #8: pridan 7. sloupec Description (smart decode per port).
     * V1.5 fix #6: pridan 8. sloupec px (pixel position v scanline).
     * V1.5.D fix #3: pridan CPU Cycle (4. sloupec) + reorder PC pred Type.
     * V1.5.E: pridan 10. sloupec Addr pro MMIO eventy (= IORQ events maji
     * "Port", MMIO eventy maji "Addr"; dle flags se naplnuje jen jeden,
     * druhy je "-").
     * Sloupcu celkem 10 (Frame, Scanline, px, Cycle, PC, Type, Port, Addr,
     * Value, Description). */
    if ( !ImGui::BeginTable ( "##io_hist_tbl", 10, hflags,
                               ImVec2 ( 0.0f, tbl_h ) ) ) {
        return;
    }

    /* Headery anglicky natvrdo (= bypass cs překlad konzistentně s Overview). */
    ImGui::TableSetupScrollFreeze ( 0, 1 );  /* zmraz header při scroll */
    ImGui::TableSetupColumn ( "Frame",
                               ImGuiTableColumnFlags_WidthFixed, 80.0f );
    /* Fix #5: "Cycle" -> "Scanline" (zaznam g_gdg.beam_row, 0..311). */
    ImGui::TableSetupColumn ( "Scanline",
                               ImGuiTableColumnFlags_WidthFixed, 60.0f );
    /* V1.5 fix #6: px = pixel column ve scanline pro presnejsi raster timing. */
    ImGui::TableSetupColumn ( "px",
                               ImGuiTableColumnFlags_WidthFixed, 50.0f );
    /* V1.5.D fix #3: CPU Cycle = kumulativni T-state counter
     * (cpu->total_cycles) v okamziku zaznamu. Decimal format. */
    ImGui::TableSetupColumn ( "CPU Cycle",
                               ImGuiTableColumnFlags_WidthFixed, 100.0f );
    /* V1.5.D fix #3: PC reordered pred Type (drive na konci pred Description). */
    ImGui::TableSetupColumn ( "PC",
                               ImGuiTableColumnFlags_WidthFixed, 80.0f );
    ImGui::TableSetupColumn ( "Type",
                               ImGuiTableColumnFlags_WidthFixed, 50.0f );
    ImGui::TableSetupColumn ( "Port",
                               ImGuiTableColumnFlags_WidthFixed, 80.0f );
    /* V1.5.E: novy sloupec Addr pro MMIO eventy 0xE000-0xE008. */
    ImGui::TableSetupColumn ( "Addr",
                               ImGuiTableColumnFlags_WidthFixed, 80.0f );
    ImGui::TableSetupColumn ( "Value",
                               ImGuiTableColumnFlags_WidthFixed, 70.0f );
    ImGui::TableSetupColumn ( "Description",
                               ImGuiTableColumnFlags_WidthStretch );
    ImGui::TableHeadersRow ( );

    /* Render eventy s filter aplikací. */
    int new_selected = g_io_ui.history_selected_visible;
    size_t cnt = g_io_history.count;
    for ( size_t k = 0; k < cnt; k++ ) {
        const st_IO_HISTORY_EVENT *e = io_history_get ( k );
        if ( !e ) continue;

        /* Filter (Fáze 3.3) - skip non-matching. */
        if ( hf_ok ) {
            const char *pname_for_filter = NULL;
            if ( hf_uses_name ) {
                pname_for_filter = io_history_lookup_port_name (
                    e->port,
                    ( e->flags & IO_HISTORY_FLAG_READ ) != 0,
                    ( e->flags & IO_HISTORY_FLAG_MEMORY ) != 0 );
            }
            if ( !io_history_filter_match ( s_hf, e, pname_for_filter ) ) continue;
        }

        ImGui::TableNextRow ( );

        /* Fix #7: per-row barva textu podle smeru.
         *  - IN  = light green (rgba 0.5, 1.0, 0.5, 1.0)
         *  - OUT = light orange (rgba 1.0, 0.7, 0.4, 1.0)
         * Push pred prvnim sloupcem, Pop na konci radku. */
        bool ev_is_read = ( ( e->flags & IO_HISTORY_FLAG_READ ) != 0 );
        bool ev_is_mem  = ( ( e->flags & IO_HISTORY_FLAG_MEMORY ) != 0 );
        /* V1.5.E: 4-color matrix podle (mem, read):
         *   IORQ IN  = light green   (R, !mem)
         *   IORQ OUT = light orange  (W, !mem)
         *   MR       = light cyan    (R,  mem)
         *   MW       = light yellow  (W,  mem) */
        ImVec4 row_color;
        if ( ev_is_mem ) {
            row_color = ev_is_read
                ? ImVec4 ( 0.5f, 0.95f, 0.95f, 1.0f )   /* MR: cyan */
                : ImVec4 ( 0.95f, 0.95f, 0.5f, 1.0f );  /* MW: yellow */
        } else {
            row_color = ev_is_read
                ? ImVec4 ( 0.5f, 1.0f, 0.5f, 1.0f )     /* IN:  green */
                : ImVec4 ( 1.0f, 0.7f, 0.4f, 1.0f );    /* OUT: orange */
        }
        ImGui::PushStyleColor ( ImGuiCol_Text, row_color );

        /* Frame (= sloupec 0) je Selectable s SpanAllColumns pro click row. */
        ImGui::TableNextColumn ( );
        bool is_selected = ( (int) k == g_io_ui.history_selected_visible );
        /* 64 = uint32 frame (max 10) + "##io_hist_row_" (14) + size_t k (max 20) + null. */
        char sel_label[ 64 ];
        snprintf ( sel_label, sizeof ( sel_label ), "%u##io_hist_row_%zu",
                   (unsigned) e->frame, k );
        if ( ImGui::Selectable ( sel_label, is_selected,
                                  ImGuiSelectableFlags_SpanAllColumns ) ) {
            new_selected = (int) k;
            g_io_ui.history_selected_logical = (int) k;
            /* User klik = pause auto-follow (= chce inspect). */
            g_io_ui.auto_follow = 0;
        }

        /* V1.5 fix #5: right-click context menu pro filter quick-actions.
         * Pop barvu pred popupem (= menu items default barva, jako #2). */
        ImGui::PopStyleColor ( );
        /* 64 = "##io_hist_ctx_" (14) + size_t k (max 20) + rezerva. */
        char ctx_id[ 64 ];
        snprintf ( ctx_id, sizeof ( ctx_id ), "##io_hist_ctx_%zu", k );
        if ( ImGui::BeginPopupContextItem ( ctx_id ) ) {
            /* 128 = prefix (~16) + tok max 47 (sizeof tok[48]) + "##xxx_yyyy_" max 12 + %zu max 20 + rezerva. */
            char buf[ 128 ];
            ImGui::TextDisabled ( "%s",
                _( "Filter actions for this event:" ) );
            ImGui::Separator ( );
            /* V1.5.E: rozdelene quick actions per event kind.
             * MMIO event = addr: filter (jen MMIO eventy s presnou adresou).
             * IORQ event = port: / port16: jak drive. */
            bool ev_is_mem_ctx = ( ( e->flags & IO_HISTORY_FLAG_MEMORY ) != 0 );
            unsigned port_lo = (unsigned)( e->port & 0xFFu );
            if ( ev_is_mem_ctx ) {
                snprintf ( buf, sizeof ( buf ), "Set: addr:%04X##sf_addr_%zu",
                           (unsigned) e->port, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    snprintf ( g_io_ui.history_filter,
                               sizeof ( g_io_ui.history_filter ),
                               "addr:%04X", (unsigned) e->port );
                }
                snprintf ( buf, sizeof ( buf ), "Set: !addr:%04X##sf_naddr_%zu",
                           (unsigned) e->port, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    snprintf ( g_io_ui.history_filter,
                               sizeof ( g_io_ui.history_filter ),
                               "!addr:%04X", (unsigned) e->port );
                }
            } else {
                /* Set filter: port (low byte 8-bit) - vhodné pro 8-bit IORQ
                 * (OUT (n),A / IN A,(n)) ignoruje random high byte na bus. */
                snprintf ( buf, sizeof ( buf ), "Set: port:%02X##sf_port_%zu",
                           port_lo, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    snprintf ( g_io_ui.history_filter,
                               sizeof ( g_io_ui.history_filter ),
                               "port:%02X", port_lo );
                }
                snprintf ( buf, sizeof ( buf ), "Set: !port:%02X##sf_nport_%zu",
                           port_lo, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    snprintf ( g_io_ui.history_filter,
                               sizeof ( g_io_ui.history_filter ),
                               "!port:%02X", port_lo );
                }
                /* Set filter: port16 (full 16-bit) - rozlišuje konkrétní
                 * BC pattern (= 0xCF family sub-registry GDG/I/O, nebo
                 * explicitní LD BC,nn pred OUT (C),A). */
                snprintf ( buf, sizeof ( buf ), "Set: port16:%04X##sf_port16_%zu",
                           (unsigned) e->port, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    snprintf ( g_io_ui.history_filter,
                               sizeof ( g_io_ui.history_filter ),
                               "port16:%04X", (unsigned) e->port );
                }
                snprintf ( buf, sizeof ( buf ), "Set: !port16:%04X##sf_nport16_%zu",
                           (unsigned) e->port, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    snprintf ( g_io_ui.history_filter,
                               sizeof ( g_io_ui.history_filter ),
                               "!port16:%04X", (unsigned) e->port );
                }
            }
            /* Set filter: pc */
            snprintf ( buf, sizeof ( buf ), "Set: pc:%04X##sf_pc_%zu",
                       (unsigned) e->pc, k );
            if ( ImGui::MenuItem ( buf ) ) {
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "pc:%04X", (unsigned) e->pc );
            }
            snprintf ( buf, sizeof ( buf ), "Set: !pc:%04X##sf_npc_%zu",
                       (unsigned) e->pc, k );
            if ( ImGui::MenuItem ( buf ) ) {
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "!pc:%04X", (unsigned) e->pc );
            }
            /* Set filter: type keyword. V1.5.E: MMIO event preferuje
             * mr/mw (= jen memory eventy stejneho smeru), IORQ event
             * pouziva sjednocene in/out (matchuje IORQ + opacny smer
             * MMIO se nestane v praxi pri vybrane konkretni adrese). */
            const char *type_kw, *type_kw_n;
            if ( ev_is_mem_ctx ) {
                type_kw   = ( e->flags & IO_HISTORY_FLAG_READ ) ? "mr"  : "mw";
                type_kw_n = ( e->flags & IO_HISTORY_FLAG_READ ) ? "!mr" : "!mw";
            } else {
                type_kw   = ( e->flags & IO_HISTORY_FLAG_READ ) ? "in"  : "out";
                type_kw_n = ( e->flags & IO_HISTORY_FLAG_READ ) ? "!in" : "!out";
            }
            snprintf ( buf, sizeof ( buf ), "Set: %s##sf_type_%zu",
                       type_kw, k );
            if ( ImGui::MenuItem ( buf ) ) {
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "%s", type_kw );
            }
            snprintf ( buf, sizeof ( buf ), "Set: %s##sf_ntype_%zu",
                       type_kw_n, k );
            if ( ImGui::MenuItem ( buf ) ) {
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "%s", type_kw_n );
            }
            /* Set filter: value */
            snprintf ( buf, sizeof ( buf ), "Set: value:%02X##sf_val_%zu",
                       (unsigned) e->value, k );
            if ( ImGui::MenuItem ( buf ) ) {
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "value:%02X", (unsigned) e->value );
            }
            snprintf ( buf, sizeof ( buf ), "Set: !value:%02X##sf_nval_%zu",
                       (unsigned) e->value, k );
            if ( ImGui::MenuItem ( buf ) ) {
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "!value:%02X", (unsigned) e->value );
            }
            /* V1.5.D fix #3: cycle:N a !cycle:N quick-actions. */
            snprintf ( buf, sizeof ( buf ), "Set: cycle:%u##sf_cyc_%zu",
                       (unsigned) e->cpu_cycle, k );
            if ( ImGui::MenuItem ( buf ) ) {
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "cycle:%u", (unsigned) e->cpu_cycle );
            }
            snprintf ( buf, sizeof ( buf ), "Set: !cycle:%u##sf_ncyc_%zu",
                       (unsigned) e->cpu_cycle, k );
            if ( ImGui::MenuItem ( buf ) ) {
                snprintf ( g_io_ui.history_filter,
                           sizeof ( g_io_ui.history_filter ),
                           "!cycle:%u", (unsigned) e->cpu_cycle );
            }
            ImGui::Separator ( );
            /* Add AND: port / !port / port16 / !port16 / pc / !pc - append
             * do current filter. port: = low byte (8-bit IORQ),
             * port16: = full 16-bit (přesný BC pattern). */
            snprintf ( buf, sizeof ( buf ), "Add AND: port:%02X##af_port_%zu",
                       port_lo, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " ", "port:%02X", port_lo );
            }
            snprintf ( buf, sizeof ( buf ), "Add AND: !port:%02X##af_nport_%zu",
                       port_lo, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " ", "!port:%02X", port_lo );
            }
            snprintf ( buf, sizeof ( buf ), "Add AND: port16:%04X##af_port16_%zu",
                       (unsigned) e->port, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " ", "port16:%04X",
                                           (unsigned) e->port );
            }
            snprintf ( buf, sizeof ( buf ), "Add AND: !port16:%04X##af_nport16_%zu",
                       (unsigned) e->port, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " ", "!port16:%04X",
                                           (unsigned) e->port );
            }
            snprintf ( buf, sizeof ( buf ), "Add AND: pc:%04X##af_pc_%zu",
                       (unsigned) e->pc, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " ", "pc:%04X", (unsigned) e->pc );
            }
            snprintf ( buf, sizeof ( buf ), "Add AND: !pc:%04X##af_npc_%zu",
                       (unsigned) e->pc, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " ", "!pc:%04X", (unsigned) e->pc );
            }
            /* V1.5.D fix #3: cycle Add AND. */
            snprintf ( buf, sizeof ( buf ), "Add AND: cycle:%u##af_cyc_%zu",
                       (unsigned) e->cpu_cycle, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " ", "cycle:%u",
                                           (unsigned) e->cpu_cycle );
            }
            snprintf ( buf, sizeof ( buf ), "Add AND: !cycle:%u##af_ncyc_%zu",
                       (unsigned) e->cpu_cycle, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " ", "!cycle:%u",
                                           (unsigned) e->cpu_cycle );
            }
            ImGui::Separator ( );
            /* F4: Add OR: varianty - paralelně k Add AND: výše.
             * Append na konec current filteru se separátorem " | " pokud
             * je buffer neprázdný, jinak jen bare token (= "match cokoliv
             * z bare tokenu, žádné leading |").
             *
             * Příklad: buffer = "port:CE", klik na "Add OR: port:CF"
             *          → buffer = "port:CE | port:CF".
             *
             * Buffer overflow: snprintf je truncate-safe, ale výsledek
             * by mohl být nezavřený token. Quick-action je tichý fail
             * pokud by append nepřežil v 128 B - uživatel viděl, že se
             * filter nezměnil, může napsat ručně.
             *
             * ImGui ID prefix "of_" (= "or filter") liší se od Add AND
             * "af_" prefixu - zabráníme ID kolizi v rámci popupu. */
            if ( ev_is_mem_ctx ) {
                /* Add OR: addr (MMIO event). */
                snprintf ( buf, sizeof ( buf ), "Add OR: addr:%04X##of_addr_%zu",
                           (unsigned) e->port, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_append ( " | ", "addr:%04X",
                                               (unsigned) e->port );
                }
                snprintf ( buf, sizeof ( buf ), "Add OR: !addr:%04X##of_naddr_%zu",
                           (unsigned) e->port, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_append ( " | ", "!addr:%04X",
                                               (unsigned) e->port );
                }
            } else {
                /* Add OR: port / !port (low byte 8-bit IORQ). */
                snprintf ( buf, sizeof ( buf ), "Add OR: port:%02X##of_port_%zu",
                           port_lo, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_append ( " | ", "port:%02X", port_lo );
                }
                snprintf ( buf, sizeof ( buf ), "Add OR: !port:%02X##of_nport_%zu",
                           port_lo, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_append ( " | ", "!port:%02X", port_lo );
                }
                /* Add OR: port16 / !port16 (full 16-bit BC pattern). */
                snprintf ( buf, sizeof ( buf ), "Add OR: port16:%04X##of_port16_%zu",
                           (unsigned) e->port, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_append ( " | ", "port16:%04X",
                                               (unsigned) e->port );
                }
                snprintf ( buf, sizeof ( buf ), "Add OR: !port16:%04X##of_nport16_%zu",
                           (unsigned) e->port, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_append ( " | ", "!port16:%04X",
                                               (unsigned) e->port );
                }
            }
            /* Add OR: pc / !pc (společné pro IORQ i MMIO). */
            snprintf ( buf, sizeof ( buf ), "Add OR: pc:%04X##of_pc_%zu",
                       (unsigned) e->pc, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " | ", "pc:%04X", (unsigned) e->pc );
            }
            snprintf ( buf, sizeof ( buf ), "Add OR: !pc:%04X##of_npc_%zu",
                       (unsigned) e->pc, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " | ", "!pc:%04X", (unsigned) e->pc );
            }
            /* Add OR: cycle / !cycle. */
            snprintf ( buf, sizeof ( buf ), "Add OR: cycle:%u##of_cyc_%zu",
                       (unsigned) e->cpu_cycle, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " | ", "cycle:%u",
                                           (unsigned) e->cpu_cycle );
            }
            snprintf ( buf, sizeof ( buf ), "Add OR: !cycle:%u##of_ncyc_%zu",
                       (unsigned) e->cpu_cycle, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_append ( " | ", "!cycle:%u",
                                           (unsigned) e->cpu_cycle );
            }
            ImGui::Separator ( );
            /* V1.7+ položka 5.1: Add AND/OR group quick-actions.
             *
             * Rozdíl proti "Add AND:" / "Add OR:" nahoře: tato varianta
             * obalí CELÝ dosavadní filter do závorky a teprve pak
             * připojí token spojkou "&" / "|". Smysl: operator precedence
             * v parseru je "&" > "|", takže např. buf="port:CE | port:CF"
             * + "Add AND: pc:42" dá nepříjemně "port:CE | port:CF pc:42"
             * = port:CE OR (port:CF AND pc:42). Group wrap udělá správně
             * "(port:CE | port:CF) & pc:42".
             *
             * Při prázdném filteru "group wrap" degraduje na bare token
             * (= match-all AND X == X), takže UX je beze ztráty.
             *
             * ImGui ID prefix "afg_" / "ofg_" liší se od "af_" / "of_"
             * v Add AND / Add OR sekcích výše -> žádná ID kolize. */
            char tok[ 48 ];
            char tok_n[ 48 ];

            /* Tooltip texty pro AND/OR group items (= konvence vars_window:
             * žádné "(?)" markery, tooltip přímo na hover prvku). Vysvětlují
             * proč group varianta existuje vedle Add AND / Add OR výše. */
            const char *tip_and = _( "Wraps the current filter in parentheses, "
                                     "then appends ' & token'. Use to AND-narrow "
                                     "an OR chain (parser precedence: & binds "
                                     "tighter than |)." );
            const char *tip_or  = _( "Wraps the current filter in parentheses, "
                                     "then appends ' | token'. Use when the "
                                     "current filter is itself an AND chain "
                                     "that should be OR-extended as a whole." );

            if ( ev_is_mem_ctx ) {
                /* MMIO: addr / !addr. */
                snprintf ( tok,   sizeof ( tok   ), "addr:%04X",  (unsigned) e->port );
                snprintf ( tok_n, sizeof ( tok_n ), "!addr:%04X", (unsigned) e->port );

                snprintf ( buf, sizeof ( buf ), "Add AND group: %s##afg_addr_%zu",
                           tok, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_string_group_wrap (
                        g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                        IOFILT_GROUP_AND, tok );
                }
                if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_and );
                snprintf ( buf, sizeof ( buf ), "Add AND group: %s##afg_naddr_%zu",
                           tok_n, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_string_group_wrap (
                        g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                        IOFILT_GROUP_AND, tok_n );
                }
                if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_and );
                snprintf ( buf, sizeof ( buf ), "Add OR group: %s##ofg_addr_%zu",
                           tok, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_string_group_wrap (
                        g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                        IOFILT_GROUP_OR, tok );
                }
                if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_or );
                snprintf ( buf, sizeof ( buf ), "Add OR group: %s##ofg_naddr_%zu",
                           tok_n, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_string_group_wrap (
                        g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                        IOFILT_GROUP_OR, tok_n );
                }
                if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_or );
            } else {
                /* IORQ: port (low byte) + port16 (full 16-bit BC pattern). */
                snprintf ( tok,   sizeof ( tok   ), "port:%02X",  port_lo );
                snprintf ( tok_n, sizeof ( tok_n ), "!port:%02X", port_lo );

                snprintf ( buf, sizeof ( buf ), "Add AND group: %s##afg_port_%zu",
                           tok, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_string_group_wrap (
                        g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                        IOFILT_GROUP_AND, tok );
                }
                if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_and );
                snprintf ( buf, sizeof ( buf ), "Add AND group: %s##afg_nport_%zu",
                           tok_n, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_string_group_wrap (
                        g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                        IOFILT_GROUP_AND, tok_n );
                }
                if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_and );
                snprintf ( buf, sizeof ( buf ), "Add OR group: %s##ofg_port_%zu",
                           tok, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_string_group_wrap (
                        g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                        IOFILT_GROUP_OR, tok );
                }
                if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_or );
                snprintf ( buf, sizeof ( buf ), "Add OR group: %s##ofg_nport_%zu",
                           tok_n, k );
                if ( ImGui::MenuItem ( buf ) ) {
                    io_history_filter_string_group_wrap (
                        g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                        IOFILT_GROUP_OR, tok_n );
                }
                if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_or );
            }
            /* pc / !pc (společné IORQ i MMIO). */
            snprintf ( tok,   sizeof ( tok   ), "pc:%04X",  (unsigned) e->pc );
            snprintf ( tok_n, sizeof ( tok_n ), "!pc:%04X", (unsigned) e->pc );

            snprintf ( buf, sizeof ( buf ), "Add AND group: %s##afg_pc_%zu",
                       tok, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_string_group_wrap (
                    g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                    IOFILT_GROUP_AND, tok );
            }
            if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_and );
            snprintf ( buf, sizeof ( buf ), "Add AND group: %s##afg_npc_%zu",
                       tok_n, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_string_group_wrap (
                    g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                    IOFILT_GROUP_AND, tok_n );
            }
            if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_and );
            snprintf ( buf, sizeof ( buf ), "Add OR group: %s##ofg_pc_%zu",
                       tok, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_string_group_wrap (
                    g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                    IOFILT_GROUP_OR, tok );
            }
            if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_or );
            snprintf ( buf, sizeof ( buf ), "Add OR group: %s##ofg_npc_%zu",
                       tok_n, k );
            if ( ImGui::MenuItem ( buf ) ) {
                io_history_filter_string_group_wrap (
                    g_io_ui.history_filter, sizeof ( g_io_ui.history_filter ),
                    IOFILT_GROUP_OR, tok_n );
            }
            if ( ImGui::IsItemHovered ( ) ) ImGui::SetTooltip ( "%s", tip_or );
            ImGui::Separator ( );
            /* CHECKLIST 2.8: cross-tab navigation - "Show port in Overview".
             * Nastaví Overview tab filter na hex hodnotu portu + požádá
             * switch na Overview tab. Symetrický counterpart k akci
             * "Show in History tab" v Overview row context menu
             * (řádek ~671). */
            snprintf ( buf, sizeof ( buf ),
                       "Show port in Overview##sf_ov_%zu", k );
            if ( ImGui::MenuItem ( buf ) ) {
                if ( ev_is_mem_ctx ) {
                    /* MMIO event: full 4-digit hex (catalog addr == bus
                     * addr, např. "E001" matchuje addr_hex4 substring
                     * unique). */
                    snprintf ( g_io_ui.filter,
                               sizeof ( g_io_ui.filter ),
                               "%04X", (unsigned) e->port );
                } else {
                    /* IORQ non-MMIO: low byte 2-digit hex. Narrowuje
                     * na port_lo a případnou 0xCF family - user vidí
                     * všechny matching rows v Overview substring
                     * match. */
                    snprintf ( g_io_ui.filter,
                               sizeof ( g_io_ui.filter ),
                               "%02X", port_lo );
                }
                g_io_ui.want_switch_to_overview = true;
            }
            ImGui::EndPopup ( );
        }
        /* Re-push row barva pro zbytek sloupcu. */
        ImGui::PushStyleColor ( ImGuiCol_Text, row_color );

        /* Scanline (= g_gdg.beam_row v okamziku zaznamu, fix #5). */
        ImGui::TableNextColumn ( );
        ImGui::Text ( "%u", (unsigned) e->scanline );

        /* V1.5 fix #6: px = pixel column ve scanline. */
        ImGui::TableNextColumn ( );
        ImGui::Text ( "%u", (unsigned) e->px );

        /* V1.5.D fix #3: CPU Cycle (decimal). */
        ImGui::TableNextColumn ( );
        ImGui::Text ( "%u", (unsigned) e->cpu_cycle );

        /* V1.5.D fix #3: PC presunut pred Type. */
        ImGui::TableNextColumn ( );
        ImGui::Text ( "0x%04X", (unsigned) e->pc );

        /* Type (= IN / OUT / MR / MW). V1.5.E: memory-mapped event = MR/MW. */
        ImGui::TableNextColumn ( );
        const char *type_str;
        if ( ev_is_mem ) {
            type_str = ev_is_read ? "MR" : "MW";
        } else {
            type_str = ev_is_read ? "IN" : "OUT";
        }
        ImGui::TextUnformatted ( type_str );

        /* Port (= IORQ MZ notace) - MMIO event ma "-".
         * V1.5.E: rozdeleny na 2 sloupce (Port = IORQ, Addr = MMIO). */
        ImGui::TableNextColumn ( );
        char addr_str[ 16 ];
        if ( ev_is_mem ) {
            ImGui::TextUnformatted ( "-" );
        } else {
            io_history_format_port ( e->port, addr_str, sizeof ( addr_str ) );
            ImGui::TextUnformatted ( addr_str );
        }

        /* Addr (= MMIO addr 0xE000-0xE008) - IORQ event ma "-". */
        ImGui::TableNextColumn ( );
        if ( ev_is_mem ) {
            ImGui::Text ( "0x%04X", (unsigned) e->port );
        } else {
            ImGui::TextUnformatted ( "-" );
        }

        /* Value */
        ImGui::TableNextColumn ( );
        ImGui::Text ( "0x%02X", (unsigned) e->value );

        /* Description (fix #8) - smart decode per port + fallback name.
         * V1.5.F fix: is_mem flag rozlisuje IORQ vs MMIO event - bez nej
         * MR/MW eventy nesprávně padaly přes 8-bit low byte na IORQ
         * entries (0xE001 -> CMT hack 0x01 atd.). */
        ImGui::TableNextColumn ( );
        const char *descr = io_history_describe ( e->port, e->value,
                                                   ev_is_read, ev_is_mem );
        if ( descr ) {
            ImGui::TextUnformatted ( descr );
        } else {
            const char *pname = io_history_lookup_port_name (
                e->port, ev_is_read, ev_is_mem );
            ImGui::TextUnformatted ( pname ? pname : "" );
        }

        ImGui::PopStyleColor ( );  /* uzavri row_color (fix #7). */
    }
    g_io_ui.history_selected_visible = new_selected;

    /* Auto-follow logika (fix #3):
     *  - want_jump_latest = klik na [Latest] => bezpodminecny scroll dolu
     *    + auto_follow = 1 (uz nastaveno v handleru tlacitka).
     *  - auto_follow == 1 => SetScrollHereY(1.0) každý frame.
     *  - User scroll detect: pokud byl auto_follow == 1 v predchozim
     *    frame, frame N+1 vidi scroll_y = max (= ImGui drzel u dna).
     *    Pokud uzivatel manualne scrolluje pryc, scroll_y klesne pod
     *    max - 32 v tomto frame -> vypni auto_follow.
     *
     * Pouzivame cached last_scroll_max z prev frame - new max muze byt
     * jiny pokud pribyl event (= ImGui pohne scroll dolu automaticky pri
     * SetScrollHereY scenario, ale rucni user scroll fixuje y na old max
     * + pak novy max-y rozdil = vidim ze user není u dna).
     */
    float scroll_y = ImGui::GetScrollY ( );
    float scroll_max = ImGui::GetScrollMaxY ( );

    if ( want_jump_latest ) {
        /* [Latest] click - hard scroll. */
        ImGui::SetScrollHereY ( 1.0f );
    } else if ( g_io_ui.auto_follow ) {
        /* Detect: pokud user scrolloval pryč od dna (= scroll_y výrazně
         * pod max), vypni auto_follow A NEDOTLAČUJ tento frame zpět.
         * Threshold 32 px = cca 2 řádky tolerance. */
        if ( scroll_max > 0.0f && ( scroll_max - scroll_y ) > 32.0f ) {
            g_io_ui.auto_follow = 0;
        } else {
            ImGui::SetScrollHereY ( 1.0f );
        }
    }

    /* Cache pro pripadne dalsi frame detekce. */
    g_io_ui.history_last_scroll_y     = scroll_y;
    g_io_ui.history_last_scroll_max_y = scroll_max;

    ImGui::EndTable ( );

    /* === Detail panel (= vybraný event nebo hint) === */
    const st_IO_HISTORY_EVENT *sel = NULL;
    if ( g_io_ui.history_selected_visible >= 0
         && (size_t) g_io_ui.history_selected_visible < g_io_history.count ) {
        sel = io_history_get ( (size_t) g_io_ui.history_selected_visible );
    }
    io_history_render_selected_detail ( sel );
}


/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

void io_window_render ( bool *p_open )
{
    if ( !p_open || !*p_open ) {
        /* Panel closed - vypni tracking flag (zero overhead).
         * Při skutečné změně 1→0 musíme volat debugger_state_changed,
         * aby se CPU callbacks vrátily na default (bez logging overhead). */
        if ( g_io_window_tracking_active ) {
            g_io_window_tracking_active = 0;
            dbg_ui_debugger_state_recompute ( );
        }
        g_io_ui.was_open_last_frame = false;
        return;
    }

    /* Default 1297x918: 6 sloupců (Addr/Name/Hex/Binary/R/W/Activity)
     * + sticky header s 5 inline tlačítky vyžaduje šířku ~920px minimum.
     * Při menší šířce se Addr column ořezává. */
    ImGui::SetNextWindowSize ( ImVec2 ( 1297, 918 ), ImGuiCond_FirstUseEver );
    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 800, 400 ),
                                            ImVec2 ( FLT_MAX, FLT_MAX ) );

    /* Focus-on-open: detekce false -> true transition pomocí stávajícího
     * was_open_last_frame flagu. Při otevření okna jednorázově nastavíme
     * focus, aby se vykreslilo nad ostatními. Standardní z-order = klik
     * na jiné okno funguje normálně. */
    if ( !g_io_ui.was_open_last_frame )
        ImGui::SetNextWindowFocus ( );

    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *io_title = _L ( "I/O Ports###io_main" );
    auto_layout_first_use_portrait ( io_title, 1297.0f, 918.0f );
    if ( !ImGui::Begin ( io_title, p_open,
                          ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::End ( );
        return;
    }

    /* Při prvním otevření aplikuj cfg preferenci tracking_active (default 1 =
     * tracking ON; uživatel může odškrtnout a uložit do cfg jako 0 pro
     * trvalé vypnutí). Tracking je gated na otevřené okno = overhead jen
     * při UI session.
     *
     * Změna g_io_window_tracking_active musí vyvolat recompute callbacků přes
     * dbg_ui_debugger_state_recompute() (= mzarch_platform_fn_debugger_state_changed
     * na emu vlákně), jinak hooky v port_*_with_logging_cb nikdy neběží. */
    if ( !g_io_ui.was_open_last_frame ) {
        g_io_ui.tracking_enable = g_io_ui.cfg_tracking_active;
        uint8_t want = g_io_ui.cfg_tracking_active ? 1u : 0u;
        if ( g_io_window_tracking_active != want ) {
            g_io_window_tracking_active = want;
            dbg_ui_debugger_state_recompute ( );
        }
    }
    g_io_ui.was_open_last_frame = true;

    /* === Tab system === */
    if ( ImGui::BeginTabBar ( "##io_tabs" ) ) {
        /* CHECKLIST 2.8: switch request z History "Show port in Overview"
         * mirroruje fix #4 pattern v opačném směru. Flag se konzumuje
         * (= reset na false) - switch proběhne pouze jednou. */
        ImGuiTabItemFlags ov_flags = 0;
        if ( g_io_ui.want_switch_to_overview ) {
            ov_flags |= ImGuiTabItemFlags_SetSelected;
            g_io_ui.want_switch_to_overview = false;
        }
        if ( ImGui::BeginTabItem ( _L ( "Overview##io_tab_ov" ),
                                    NULL, ov_flags ) ) {
            io_window_render_overview_tab ( );
            ImGui::EndTabItem ( );
        }
        /* Fix #4: pokud Overview "Show in History tab" requestnul switch,
         * předáme ImGuiTabItemFlags_SetSelected pro forced activation.
         * Flag se konzumuje (= reset na false) - aby switch proběhl
         * pouze jednou. */
        ImGuiTabItemFlags hist_flags = 0;
        if ( g_io_ui.want_switch_to_history ) {
            hist_flags |= ImGuiTabItemFlags_SetSelected;
            g_io_ui.want_switch_to_history = false;
        }
        if ( ImGui::BeginTabItem ( _L ( "History##io_tab_hist" ),
                                    NULL, hist_flags ) ) {
            io_window_render_history_tab ( );
            ImGui::EndTabItem ( );
        }
        ImGui::EndTabBar ( );
    }

    ImGui::End ( );
}


void io_window_show_hide ( void )
{
    g_gui->showIoWindow = !g_gui->showIoWindow;
}


/* ========================================================================= */
/*  Persistence (Sprint 2 Fáze 4.1)                                          */
/* ========================================================================= */

/**
 * @brief Helper - vrátí klíč collapse_<chip> podle indexu g_sections.
 *
 * Buffer musí mít alespoň 32 bajtů.
 */
static const char* io_window_cfg_collapse_key ( size_t section_idx,
                                                  char *buf, size_t bufsz )
{
    if ( section_idx >= g_sections_count ) {
        snprintf ( buf, bufsz, "collapse_unknown_%zu", section_idx );
        return buf;
    }
    snprintf ( buf, bufsz, "collapse_%s", g_sections[ section_idx ].id );
    return buf;
}


/* ========================================================================= */
/*  Record mask cfg callbacks (V1.7+ 2.6)                                    */
/* ========================================================================= */

/**
 * @brief Hex znak (0-9 / A-F / a-f) na 4-bit nibble. -1 pri chybe.
 */
static int io_window_hex_nibble ( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'A' && c <= 'F' ) return 10 + ( c - 'A' );
    if ( c >= 'a' && c <= 'f' ) return 10 + ( c - 'a' );
    return -1;
}


/**
 * @brief Propagate cb pro `record_mask` - parse 64-hex string -> 256 bool flagy.
 *
 * Format: 64 hex znaku, kazdy nibble = 4 porty. Bit 0 nejnizsiho nibblu =
 * port 0x00, bit 3 nejvyssiho nibblu = port 0xFF. (= little-endian bit
 * order, prirozene pro hex string ctený zleva doprava jako sekvence bajtu
 * 0x00..0xFF.)
 *
 * Pri parse chybe (kratky string, neplatne znaky) padne na default = vse 1.
 */
static void io_window_cfg_propagate_record_mask ( void *e, void *data )
{
    (void) data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *) e;
    const char *txt = cfgelement_get_text_value ( elm );
    if ( !txt || strlen ( txt ) != 64 ) {
        /* Empty / kratky / dlouhy = default vse aktivni (safe fallback). */
        io_history_record_enable_all ( );
        return;
    }
    for ( size_t byte = 0; byte < 32; byte++ ) {
        int hi = io_window_hex_nibble ( txt[ byte * 2 ] );
        int lo = io_window_hex_nibble ( txt[ byte * 2 + 1 ] );
        if ( hi < 0 || lo < 0 ) {
            io_history_record_enable_all ( );
            return;
        }
        uint8_t b = (uint8_t) ( ( hi << 4 ) | lo );
        for ( int bit = 0; bit < 8; bit++ ) {
            size_t port_idx = byte * 8 + bit;
            g_io_history_record_enabled[ port_idx ] =
                ( b >> bit ) & 1u;
        }
    }
}


/**
 * @brief Save cb pro `record_mask` - 256 flagu -> 64-hex string.
 *
 * Inverze parseru. Bit i v bajtu i/8 = port i record_enabled.
 */
static void io_window_cfg_save_record_mask ( void *e, void *data )
{
    (void) data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *) e;
    char buf[ 65 ];
    static const char hex[] = "0123456789ABCDEF";
    for ( size_t byte = 0; byte < 32; byte++ ) {
        uint8_t b = 0;
        for ( int bit = 0; bit < 8; bit++ ) {
            if ( g_io_history_record_enabled[ byte * 8 + bit ] ) {
                b |= (uint8_t) ( 1u << bit );
            }
        }
        buf[ byte * 2 ]     = hex[ ( b >> 4 ) & 0x0Fu ];
        buf[ byte * 2 + 1 ] = hex[ b & 0x0Fu ];
    }
    buf[ 64 ] = '\0';
    cfgelement_set_text_value ( elm, buf );
}


extern "C" void io_window_register_persistence ( void *cmod_void )
{
    if ( !cmod_void ) return;

    st_CFGMODULE *cmod = (st_CFGMODULE *) cmod_void;
    st_CFGELEMENT *elm;

    /* Per-chip collapse state. Default 0 = expanded (user vidí všechno).
     * Indexovány v g_sections pořadí - název klíče je "collapse_<id>". */
    for ( size_t s = 0; s < g_sections_count && s < 16; s++ ) {
        char keybuf[ 64 ];
        const char *key = io_window_cfg_collapse_key ( s, keybuf,
                                                        sizeof ( keybuf ) );
        elm = cfgmodule_register_new_element ( cmod, (char *) key,
                                                CFGENTYPE_BOOL, 0 );
        cfgelement_set_handlers ( elm,
            (void *) &g_io_ui.section_collapsed[ s ],
            (void *) &g_io_ui.section_collapsed[ s ] );
    }

    /* History buffer capacity (UNSIGNED 1000..50000, default 10000). */
    elm = cfgmodule_register_new_element ( cmod,
        (char *) "history_capacity",
        CFGENTYPE_UNSIGNED, (int) IO_HISTORY_DEFAULT_CAPACITY,
        (int) IO_HISTORY_MIN_CAPACITY, (int) IO_HISTORY_MAX_CAPACITY );
    cfgelement_set_handlers ( elm,
        (void *) &g_io_ui.history_capacity,
        (void *) &g_io_ui.history_capacity );

    /* Auto-follow History tab (BOOL, default 1). */
    elm = cfgmodule_register_new_element ( cmod,
        (char *) "history_auto_follow", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm,
        (void *) &g_io_ui.auto_follow,
        (void *) &g_io_ui.auto_follow );

    /* Tracking default ON (BOOL, default 1). Tracking je gated na otevřené
     * okno - overhead se projeví jen v UI session. User si může odškrtnout
     * a uložit do cfg jako 0 (= opt-out preference). */
    elm = cfgmodule_register_new_element ( cmod,
        (char *) "tracking_active", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm,
        (void *) &g_io_ui.cfg_tracking_active,
        (void *) &g_io_ui.cfg_tracking_active );

    /* V1.5.D fix #1: Selected event detail panel height (UNSIGNED 60..400,
     * default 160). User muze drag splitter v History tab pro resize;
     * hodnota persist mezi sessions. */
    elm = cfgmodule_register_new_element ( cmod,
        (char *) "detail_panel_height", CFGENTYPE_UNSIGNED, 160, 60, 400 );
    cfgelement_set_handlers ( elm,
        (void *) &g_io_ui.detail_panel_height,
        (void *) &g_io_ui.detail_panel_height );

    /* V1.7+ 2.7: Heat coloring thresholds (UNSIGNED).
     *
     * text_active = minimum hits/s pro zelený text (default 1).
     * bg_hot      = minimum hits/s pro červený bg tint (default 10000).
     * Konfigurovatelné v Heat... popupu v toolbaru I/O okna. */
    elm = cfgmodule_register_new_element ( cmod,
        (char *) "heat_text_active", CFGENTYPE_UNSIGNED, 1, 1, 1000000 );
    cfgelement_set_handlers ( elm,
        (void *) &g_io_ui.heat_text_active_threshold,
        (void *) &g_io_ui.heat_text_active_threshold );

    elm = cfgmodule_register_new_element ( cmod,
        (char *) "heat_bg_hot", CFGENTYPE_UNSIGNED, 10000, 1, 10000000 );
    cfgelement_set_handlers ( elm,
        (void *) &g_io_ui.heat_bg_hot_threshold,
        (void *) &g_io_ui.heat_bg_hot_threshold );

    /* V1.7+ 2.6: Selective per-port history record mask.
     *
     * Format = 64 hex znaku (TEXT), bit i v bajtu i/8 = port i (8-bit
     * IORQ space, low byte adresy). Default vsech 'F' = vse zaznamenavano
     * (= back-compat povodni chovani). Pri propagate parser nastavi
     * g_io_history_record_enabled[256]; save callback inverze.
     *
     * Pro 256 portu by jednotlive klice byly nečitelne v INI - bitmap
     * je kompaktni 64-byte string a slo by ho i ručne editovat.
     */
    elm = cfgmodule_register_new_element ( cmod,
        (char *) "record_mask", CFGENTYPE_TEXT,
        (char *) "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF" );
    cfgelement_set_propagate_cb ( elm,
        io_window_cfg_propagate_record_mask, NULL );
    cfgelement_set_save_cb ( elm,
        io_window_cfg_save_record_mask, NULL );
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
