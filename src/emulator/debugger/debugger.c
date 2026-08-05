/* 
 * File:   debugger.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 23. srpna 2015, 16:19
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

#include "mzarch/mzarch_config.h"

#include <stdio.h>
#include <string.h>

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "debugger.h"

/* NOT_HAVE_DEBUGGER_UI je historický guard ze starého GTK3 portu -
 * dnes vždy defined v debugger buildu (= odpovídající #ifndef bloky
 * jsou mrtvý kód, kandidát na další cleanup). POZOR: tento guard
 * NErozliší ImGui production vs headless test build - pro registrace,
 * které mají běžet v ImGui production ale ne v test core, použij
 * #ifndef MZTEST_HEADLESS. */
#define NOT_HAVE_DEBUGGER_UI

/*
 * ui_debugger_show/hide_main_window — implementovány v ImGui UI vrstvě
 * (src/ui-imgui/debugger/debugger_window.cpp).
 * Manipulují s g_gui->showDebuggerWindow.
 */
#include "ui-imgui/debugger/debugger_window.h"
#include "ui-imgui/debugger/breakpoints/bpt_window.h"
#include "ui-imgui/debugger/sections/dbg_extra_disasm.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/debugger/sections/dbg_stack_panel.h"
#include "ui-imgui/debugger/sections/dbg_callstack.h"

#include "mzarch/mzarch.h"
#include "mzarch/mzarch_platform_functions.h"
#include "libs/cpu-z80/z80.h"
#include "hw-generic/memory/memory.h"
#include "bptmap.h"
#include "breakpoints.h"
#include "mhmap.h"
#include "trace/reclife.h"
#include "trace/cputrack.h"
#include "trace/iorqlog.h"
#include "trace/intlog.h"
#include "trace/hwlog.h"
#include "trace/marklog.h"
#include "trace/eventlog.h"
#include "io_activity.h"
#include "io_history.h"
#include "callstack.h"
#include "profiler.h"
#include "symbols/sym_db.h"
#include "bookmarks/bookmarks.h"
#include "freeze/freeze.h"
#include "dbgapi_ui.h"
#include "mzarch/mzhal.h"
#include "dbgapi_cmdrq.h"
#include "stack_regions.h"
#include "stack_history.h"
#include "watch.h"
#include "watch_cache.h"  /* Phase E: per-row cache + snapshot baseline */
#include "watch_emu_cache.h"  /* V1.D.2.C: dispatch-side thread-safe mirror */
/* SENTINEL: debugger okna platí pro všechny známé architektury
 * (700/800/1500, garantováno mzhal.c) - při přidání nové architektury
 * tuto sadu PROVĚŘ. */
#include "ui-imgui/debugger/heatmap/mhmap_window.h"
#include "ui-imgui/debugger/ioview/io_window.h"
#include "ui-imgui/debugger/memmap/memmap_window.h"
#include "ui-imgui/debugger/dasm_window/dasm_window.h"
/* membrowser mutant V0 - Memory Browser hex MVP. */
#include "ui-imgui/debugger/membrowser/membrowser_window.h"
#include "ui-imgui/debugger/eventview/event_viewer_window.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "main.h"   /* g_sdlapp pro sdlapp_paths_resolve_* */
#include "libs/cfgfile/cfgtools.h"
#include "hw-generic/gdg/framebuffer.h"
#include "hw-generic/gdg/framebuffer_done.h"
#include "iface/iface_video.h"

#include "cfgmain.h"

st_DEBUGGER g_debugger;
st_DEBUGGER_HISTORY g_debugger_history;


void debugger_step_call ( unsigned value ) {

    value &= 1;
    if ( value == g_debugger.step_call ) return;

    if ( value ) {
#if 0
        if ( !TEST_EMULATION_PAUSED ) return;
#else
        /* Emulator neni v pauze, takze neprovedeme step, ale prozatim jen zastavime emulaci */
        if ( !EMULATOR_TEST_PAUSED ) {
            //mz800_pause_emulation ( 1 );
            return;
        };
#endif
        g_debugger.step_call = 1;
        //printf ( "Debugger step call - PC: 0x%04x.\n", z80_get_reg ( g_mzarch_main.cpu, Z80_REG_PC ) );
    } else {
        g_debugger.step_call = 0;
    };
}


/**
 * Test, zda jméno už končí přímo na ".json" (case-insensitive).
 *
 * Slouží k tomu, abychom k cdl_export_name typu "mzdos.json" nepřidali
 * další ".json" (vznikl by "mzdos.json.json" + matoucí region prefix
 * "mzdos.json_"). User pravděpodobně chce přesně to jméno které dal.
 */
static int cdl_name_has_json_ext ( const char *name ) {
    if ( !name ) return 0;
    size_t n = strlen ( name );
    if ( n < 5 ) return 0;
    return ( g_ascii_strcasecmp ( name + n - 5, ".json" ) == 0 ) ? 1 : 0;
}


/**
 * @brief CDL save callback pro sdílenou reclife vrstvu.
 *
 * Implementuje @c st_RECLIFE_DESC.fn_save pro CDL (mhmap). Sjednocuje CDL
 * se sdíleným recording lifecycle layerem, ale heuristiku sestavení cílové
 * cesty (cdl_name_has_json_ext, resolve_work, <dir>/<name>.json) drží zde v
 * CDL-specifickém kódu - generická vrstva ji NESMÍ obsahovat (jinak by
 * vznikl "name.json.json").
 *
 * @param path  Pokud nenulové, exportuje se přímo do této cesty (UI "Export
 *              now"). Pokud NULL, sestaví se default cesta z configu
 *              (cdl_export_dir/cdl_export_name) - chování shodné s
 *              původním exit-gate exportem.
 * @return 0 OK, -1 chyba nebo CDL export nepovolen/nenakonfigurován.
 */
static int cdl_reclife_save ( const char *path ) {
    if ( path && path[ 0 ] ) {
        return mhmap_export ( path );
    }

    /* path == NULL: sestav default cestu z configu. Stejná logika jako
     * původní debugger_exit gate. Pokud dir/name nejsou nastaveny, export
     * se neprovede (návrat -1). */
    if ( !g_debugger.cdl_export_dir || !g_debugger.cdl_export_dir[ 0 ]
         || !g_debugger.cdl_export_name || !g_debugger.cdl_export_name[ 0 ] ) {
        return -1;
    }
    /* Sestavíme plnou cestu meta.json: <dir>/<name>.json. Pokud user už dal
     * .json extenzi v --cdl-name (nebo přes UI), nepřidávat další - jinak
     * by vznikl "mzdos.json.json" a matoucí region prefix "mzdos.json_". */
    char *fname = cdl_name_has_json_ext ( g_debugger.cdl_export_name )
                  ? g_strdup ( g_debugger.cdl_export_name )
                  : g_strdup_printf ( "%s.json", g_debugger.cdl_export_name );
    /* Relativní cesty resolveme proti work_dir (export je výstupní operace
     * v "user work" location), absolutní pass through. */
    char *resolved_dir = sdlapp_paths_resolve_work ( g_sdlapp->paths,
                                                      g_debugger.cdl_export_dir );
    char *full = g_build_filename ( resolved_dir, fname, NULL );
    int ret = mhmap_export ( full );
    g_free ( full );
    g_free ( resolved_dir );
    g_free ( fname );
    return ret;
}


/* Sdílený lifecycle deskriptor pro CDL (mhmap). CDL používá z reclife jen
 * save vrstvu (fn_save); aktivace zůstává mode-driven přes makro
 * TEST_DEBUGGER_MHMAP_ACTIVE (mhmap nemá samostatný _active flag), proto
 * mode_ptr/active_ptr/fn_start/fn_stop nejsou pro CDL relevantní a CDL
 * NEpoužívá reclife_recompute_active ani reclife_finalize. fn_reset =
 * mhmap_reset pro budoucí použití (např. UI "Clear coverage"). */
static const st_RECLIFE_DESC s_cdl_reclife = {
    .subsys_name = "cdl",
    .mode_ptr    = NULL,
    .active_ptr  = NULL,
    .fn_start    = NULL,
    .fn_stop     = NULL,
    .fn_reset    = mhmap_reset,
    .fn_save     = cdl_reclife_save,
};


void debugger_exit ( void ) {
    /* CDL export při ukončení - dle nastaveného flag. Cesta a heuristika se
     * sestavují v cdl_reclife_save (NULL path = default z configu). Export
     * přes sdílenou reclife vrstvu, chování beze změny oproti původnímu
     * inline gate. */
    if ( g_debugger.cdl_export_on_exit ) {
        reclife_save ( &s_cdl_reclife, NULL );
    };

    /* trace-suite - flush + close pokud běží recording. */
    cputrack_finalize ( );
    iorqlog_finalize ( );
    intlog_finalize ( );
    hwlog_finalize ( );
    /* Event Viewer - destroy ringu před marklog_finalize (sjednocení s
     * pořadím init: marklog před eventlog -> destroy v opačném pořadí). */
    eventlog_destroy ( );
    marklog_finalize ( );

    /* Profiler subsystém (mutant profiler-experimental Fáze F1) - shutdown
     * před watch (profiler je listener nad callstack, žádná persistence
     * agregátoru = jen unregister listener + free hash mapa). */
    profiler_shutdown ( );

    /* Watch panel - auto-save (pokud cfg auto_save=1) PŘED shutdown.
     * Storage se uvolní až po save, aby se zachytily aktuální záznamy. */
    watch_cfg_auto_save ( );
    /* Phase E: cache shutdown PRED storage shutdown (cache referuje
     * jen row_id, takže technicky pořadí nezáleží, ale logicky cache
     * patří k UI snapshot lifecycle). */
    watch_cache_shutdown ( );
    /* V1.D.2.C: dispatch-side mirror shutdown (= thread-safe storage). */
    watch_emu_cache_shutdown ( );
    watch_shutdown ( );

    breakpoints_exit ( );
    bookmarks_cleanup ( );  /* Bookmarks (D.6) - auto-save + free */
    sym_db_cfg_auto_save ( ); /* V1.7+ 3.2: auto-save default .lbl */
    sym_db_destroy ( );    /* symbol DB (D.8) */
}


void debugger_reset_history ( void ) {
    memset ( &g_debugger_history, 0x00, sizeof (g_debugger_history ) );
}


void debugger_membrowser_selected_addr_propagatecfg_cb ( void *e, void *data ) {
    (void)data;
#ifdef NOT_HAVE_DEBUGGER_UI
    (void)e;
#else
    char *encoded_txt = cfgelement_get_text_value ( (CFGELM *) e );

    int ret = EXIT_FAILURE;
    int value = cfgtools_strtol ( encoded_txt, &ret );

    if ( ret == EXIT_FAILURE ) {
        value = 0;
    };

    g_membrowser.selected_addr = value;
#endif
}


void debugger_membrowser_selected_addr_save_cb ( void *e, void *data ) {
    (void)data;
#ifdef NOT_HAVE_DEBUGGER_UI
    (void)e;
#else
    char value_txt[20];
    sprintf ( value_txt, "0x%04x", g_membrowser.selected_addr );
    cfgelement_set_text_value ( (CFGELM *) e, value_txt );
#endif
}


void debugger_init ( void ) {
    g_debugger.state = DEBUGGER_STATE_NONE;
    g_debugger.active = 0;
    g_debugger.memop_call = 0;
    g_debugger.skip_bp_at_pc = -1;   /* BP enforcement skip neaktivní */
    g_debugger.user_cycle_origin = 0; /* V3.1: User cycle counter origin (= 0 od resetu) */
    debugger_step_call ( 0 );
    bptmap_init ( );
    breakpoints_init ( );    /* breakpoints modul — vyšší vrstva nad bptmap */
    bpt_state_init ( );      /* UI stav breakpointů — musí být před prvním Alt+B */
    sym_db_init ( );         /* symbol DB (D.8) - prázdná, populated user load akcí */
    sym_db_cfg_init ( );     /* V1.7+ 3.2: cfg [SYMBOLS] + auto-load .lbl */
    bookmarks_init ( );      /* Bookmarks - storage + cfg + auto-load */
    freeze_init ( );         /* V1: Freeze Bytes - cheat-engine style hot byte override */

    /* Watch panel (Phase A = L0 Address Watch MVP).
     * Storage init + cfg sekce [WATCH_PANEL] + případný auto-load
     * standalone .watch souboru. */
    watch_init ( );
    watch_cfg_init ( );
    /* Phase E: per-row cache + snapshot baseline storage. */
    watch_cache_init ( );
    /* V1.D.2.C: thread-safe dispatch-side mirror watch statistik
     * pro MCP Resource `emulator://watch/snapshot/{name}`. */
    watch_emu_cache_init ( );

    /* V6: persistence stack regions (definice name/base/limit, ne
     * watermark/counters). Registrace sekce [STACK_REGIONS] v cfgmain.ini
     * + propagate uložených hodnot do g_stack_regions[]. */
    stack_regions_cfg_init ( );

    /* V6: persistence stack history recording flag (= enabled / disabled
     * v sekci [STACK_HISTORY]). Ring buffer obsahu se NEpersistuje. */
    stack_history_cfg_init ( );

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "DEBUGGER" );

    CFGELM *elm;
    elm = cfgmodule_register_new_element ( cmod, "auto_save_breakpoints", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.auto_save_breakpoints, (void*) &g_debugger.auto_save_breakpoints );

    /* CPU Instruction History (cpuhist) mode - kdy zaznamenávat instrukční historii. */
    elm = cfgmodule_register_new_element ( cmod, "cpuhist_mode", CFGENTYPE_KEYWORD, DEBUGGER_CPUHIST_MODE_WITH_WINDOW_OR_BP,
                                           DEBUGGER_CPUHIST_MODE_WITH_WINDOW,       "WITH_WINDOW",
                                           DEBUGGER_CPUHIST_MODE_ALWAYS,            "ALWAYS",
                                           DEBUGGER_CPUHIST_MODE_OFF,               "OFF",
                                           DEBUGGER_CPUHIST_MODE_WITH_WINDOW_OR_BP, "WITH_WINDOW_OR_BP",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.cpuhist_mode, (void*) &g_debugger.cpuhist_mode );


    /* CDL (Memory Heatmap) mode - kdy zaznamenávat counters R/W/X. */
    elm = cfgmodule_register_new_element ( cmod, "cdl_mode", CFGENTYPE_KEYWORD, DEBUGGER_MHMAP_MODE_OFF,
                                           DEBUGGER_MHMAP_MODE_OFF,         "OFF",
                                           DEBUGGER_MHMAP_MODE_WITH_WINDOW, "WITH_WINDOW",
                                           DEBUGGER_MHMAP_MODE_ALWAYS,      "ALWAYS",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.mhmap_mode, (void*) &g_debugger.mhmap_mode );

    /* CDL export při ukončení emulátoru. Default ON (= sjednoceno s trace-suite
     * save_on_exit, viz cputrack/iorqlog/intlog/hwlog/marklog). Aplikuje se jen
     * na nové configy; existující .ini s explicitním klíčem si drží svou hodnotu. */
    elm = cfgmodule_register_new_element ( cmod, "cdl_export_on_exit", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.cdl_export_on_exit, (void*) &g_debugger.cdl_export_on_exit );

    /* CDL exportní adresář. Pokud neexistuje, mhmap_export ho vytvoří
     * (g_mkdir_with_parents). Existující soubory se přepisují.
     * Default je relativní k work_dir (výstupní operace). */
    elm = cfgmodule_register_new_element ( cmod, "cdl_export_dir", CFGENTYPE_TEXT, "cdl-export" );
    cfgelement_bind ( elm, (void*) &g_debugger.cdl_export_dir );

    /* CDL exportní basename (bez .json) - prefix pro region soubory.
     * Plná cesta meta.json = <cdl_export_dir>/<cdl_export_name>.json. */
    elm = cfgmodule_register_new_element ( cmod, "cdl_export_name", CFGENTYPE_TEXT, "cdl-export" );
    cfgelement_bind ( elm, (void*) &g_debugger.cdl_export_name );

#ifndef NOT_HAVE_DEBUGGER_UI
    elm = cfgmodule_register_new_element ( cmod, "focus_to_addr_history", CFGENTYPE_TEXT, "0x0000" );
    cfgelement_set_propagate_cb ( elm, ui_debugger_focus_to_addr_history_propagatecfg_cb, NULL );
    cfgelement_set_save_cb ( elm, ui_debugger_focus_to_addr_history_save_cb, NULL );
#endif

    elm = cfgmodule_register_new_element ( cmod, "screen_refresh_on_edit", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.screen_refresh_on_edit, (void*) &g_debugger.screen_refresh_on_edit );

    elm = cfgmodule_register_new_element ( cmod, "screen_refresh_at_step", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.screen_refresh_at_step, (void*) &g_debugger.screen_refresh_at_step );

    /* Disassembled tabulka - vizualizace skoků (branch arrows v ICONS gutteru).
     * Default ON: navigačně užitečné pro vidění toku řízení v okně. */
    elm = cfgmodule_register_new_element ( cmod, "disasm_show_branch_arrows", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.disasm_show_branch_arrows, (void*) &g_debugger.disasm_show_branch_arrows );

    /* DBG Workplace flagy - která auxiliary okna se otevřou/zavřou současně
     * s hlavním oknem debuggeru. Defaults dle uživatelského sezení:
     * CPU Registers + Memory Map = ON, ostatní = OFF. */
    elm = cfgmodule_register_new_element ( cmod, "wp_cpu_registers", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_cpu_registers, (void*) &g_debugger.wp_cpu_registers );

    elm = cfgmodule_register_new_element ( cmod, "wp_memory_map", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_memory_map, (void*) &g_debugger.wp_memory_map );

    elm = cfgmodule_register_new_element ( cmod, "wp_stack_monitor", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_stack_monitor, (void*) &g_debugger.wp_stack_monitor );

    elm = cfgmodule_register_new_element ( cmod, "wp_callstack", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_callstack, (void*) &g_debugger.wp_callstack );

    elm = cfgmodule_register_new_element ( cmod, "wp_breakpoints", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_breakpoints, (void*) &g_debugger.wp_breakpoints );

    elm = cfgmodule_register_new_element ( cmod, "wp_watch", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_watch, (void*) &g_debugger.wp_watch );

    /* membrowser mutant V0: Memory Browser hex MVP workplace slot. */
    elm = cfgmodule_register_new_element ( cmod, "wp_membrowser", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_membrowser, (void*) &g_debugger.wp_membrowser );

    /* V3 multi-view: workplace sloty pro sekundarni Memory Browser okna
     * #2..#5. Default 0 (opt-in). Index 0..3 = #2..#5. */
    elm = cfgmodule_register_new_element ( cmod, "wp_membrowser2", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_membrowser_extra[0], (void*) &g_debugger.wp_membrowser_extra[0] );
    elm = cfgmodule_register_new_element ( cmod, "wp_membrowser3", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_membrowser_extra[1], (void*) &g_debugger.wp_membrowser_extra[1] );
    elm = cfgmodule_register_new_element ( cmod, "wp_membrowser4", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_membrowser_extra[2], (void*) &g_debugger.wp_membrowser_extra[2] );
    elm = cfgmodule_register_new_element ( cmod, "wp_membrowser5", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_membrowser_extra[3], (void*) &g_debugger.wp_membrowser_extra[3] );

    elm = cfgmodule_register_new_element ( cmod, "wp_profiler", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_profiler, (void*) &g_debugger.wp_profiler );

    elm = cfgmodule_register_new_element ( cmod, "wp_bookmarks", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_bookmarks, (void*) &g_debugger.wp_bookmarks );

    elm = cfgmodule_register_new_element ( cmod, "wp_symbols", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_symbols, (void*) &g_debugger.wp_symbols );

    elm = cfgmodule_register_new_element ( cmod, "wp_variables", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_variables, (void*) &g_debugger.wp_variables );

    elm = cfgmodule_register_new_element ( cmod, "wp_disasm2", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_disasm_extra[0], (void*) &g_debugger.wp_disasm_extra[0] );

    elm = cfgmodule_register_new_element ( cmod, "wp_disasm3", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_disasm_extra[1], (void*) &g_debugger.wp_disasm_extra[1] );

    elm = cfgmodule_register_new_element ( cmod, "wp_disasm4", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_disasm_extra[2], (void*) &g_debugger.wp_disasm_extra[2] );

    elm = cfgmodule_register_new_element ( cmod, "wp_disasm5", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_disasm_extra[3], (void*) &g_debugger.wp_disasm_extra[3] );

    /* Per-chip-panels F1 scaffold: workplace registrace pro per-chip
     * detail okna. Default 0 = opt-in přes menu Debugger -> DBG Workplace. */
    elm = cfgmodule_register_new_element ( cmod, "wp_show_ctc", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_show_ctc, (void*) &g_debugger.wp_show_ctc );

    elm = cfgmodule_register_new_element ( cmod, "wp_show_ppi", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_show_ppi, (void*) &g_debugger.wp_show_ppi );

    if (g_mzhal.have_pioz80) { /* INI klic jen na platformach s PIO-Z80 (mzhal krok 8) */
    /* Z80 PIO workplace slot - jen MZ-800 / MZ-1500. MZ-700 čip nemá,
     * INI klíč se neregistruje. */
    elm = cfgmodule_register_new_element ( cmod, "wp_show_pioz80", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_show_pioz80, (void*) &g_debugger.wp_show_pioz80 );
    }

    if (g_mzhal.psg_count >= 1) { /* INI klice jen na platformach s PSG (mzhal krok 8) */
    /* PSG SN76489 workplace slot - jen MZ-800 / MZ-1500. MZ-700 čip nemá,
     * INI klíč se neregistruje. */
    elm = cfgmodule_register_new_element ( cmod, "wp_show_psg", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_show_psg, (void*) &g_debugger.wp_show_psg );

    /* psg-audio-scope mutant F1: PSG Audio Scope okno - workplace slot
     * (stejný arch gate HAVE_PSG >= 1, default 0). */
    elm = cfgmodule_register_new_element ( cmod, "wp_show_psg_audio_scope", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_show_psg_audio_scope, (void*) &g_debugger.wp_show_psg_audio_scope );
    }

    /* gdg-panel F1 scaffold: GDG state inspector workplace slot. GDG je
     * ve všech 3 archech (MZ-700 / MZ-800 / MZ-1500), žádný guard. */
    elm = cfgmodule_register_new_element ( cmod, "wp_show_gdg", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_debugger.wp_show_gdg, (void*) &g_debugger.wp_show_gdg );

#ifndef NOT_HAVE_DEBUGGER_UI
    /* Disassembled hlavní instance: persistence per-instance dynamic flagů
     * (disasm_main_follow_pc, disasm_main_show_tstates). */
    dbg_disassembled_register_cfg ( cmod );

    /* Sekundární Disassembly okna #2-#5: persistence focus_addr a per-instance
     * flagů (follow_pc, show_tstates) per okno.
     * Klíče disasm_extra<N>_focus / _follow_pc / _show_tstates.
     * Visibility se nepersistuje (default closed). */
    dbg_extra_disasm_register_cfg ( cmod );
#endif

#ifndef NOT_HAVE_DEBUGGER_UI
    // ui_membrowser
    elm = cfgmodule_register_new_element ( cmod, "membrowser_sharp_ascii", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.sh_ascii_conversion, (void*) &g_membrowser.sh_ascii_conversion );

    elm = cfgmodule_register_new_element ( cmod, "membrowser_show_comparative", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.show_comparative, (void*) &g_membrowser.show_comparative );

    elm = cfgmodule_register_new_element ( cmod, "membrowser_forced_screen_refresh", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.forced_screen_refresh, (void*) &g_membrowser.forced_screen_refresh );

    elm = cfgmodule_register_new_element ( cmod, "membrowser_memsrc", CFGENTYPE_KEYWORD, MEMBROWSER_SOURCE_MAPED,
                                           MEMBROWSER_SOURCE_MAPED, "MAPED",
                                           MEMBROWSER_SOURCE_RAM, "RAM",
                                           MEMBROWSER_SOURCE_VRAM, "VRAM",
                                           MEMBROWSER_SOURCE_MZ700ROM, "MZ700ROM",
                                           MEMBROWSER_SOURCE_CGROM, "CGROM",
                                           MEMBROWSER_SOURCE_MZ800ROM, "MZ800ROM",
                                           MEMBROWSER_SOURCE_PEZIK_E8, "PEZIK_E8",
                                           MEMBROWSER_SOURCE_PEZIK_68, "PEZIK_68",
                                           MEMBROWSER_SOURCE_MZ1R18, "MZ1R18",
                                           MEMBROWSER_SOURCE_MEMEXT_PEHU, "MEMEXT_PEHU",
                                           MEMBROWSER_SOURCE_MEMEXT_LUFTNER, "MEMEXT_LUFTNER",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.memsrc, (void*) &g_membrowser.memsrc );
#endif
    /*
        elm = cfgmodule_register_new_element ( cmod, "membrowser_page", CFGENTYPE_UNSIGNED, 0, 0, 0xffffffff );
        cfgelement_set_handlers ( elm, (void*) &g_membrowser.page, (void*) &g_membrowser.page );

        elm = cfgmodule_register_new_element ( cmod, "membrowser_selected_addr", CFGENTYPE_TEXT, "0x0000" );
        cfgelement_set_propagate_cb ( elm, debugger_membrowser_selected_addr_propagatecfg_cb, NULL );
        cfgelement_set_save_cb ( elm, debugger_membrowser_selected_addr_save_cb, NULL );
     */
#ifndef NOT_HAVE_DEBUGGER_UI
    elm = cfgmodule_register_new_element ( cmod, "membrowser_pzik_addressing", CFGENTYPE_KEYWORD, MEMBROWSER_PEZIK_ADDRESSING_BE,
                                           MEMBROWSER_PEZIK_ADDRESSING_BE, "B_ENDIAN",
                                           MEMBROWSER_PEZIK_ADDRESSING_LE, "L_ENDIAN",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.pezik_addressing, (void*) &g_membrowser.pezik_addressing );

    elm = cfgmodule_register_new_element ( cmod, "membrowser_vram_plane", CFGENTYPE_UNSIGNED, 0, 0, 3 );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.vram_plane, (void*) &g_membrowser.vram_plane );

    elm = cfgmodule_register_new_element ( cmod, "membrowser_pezik_e8_bank", CFGENTYPE_UNSIGNED, 0, 0, 0x07 );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.pezik_bank[1], (void*) &g_membrowser.pezik_bank[1] );

    elm = cfgmodule_register_new_element ( cmod, "membrowser_pezik_68_bank", CFGENTYPE_UNSIGNED, 0, 0, 0x07 );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.pezik_bank[0], (void*) &g_membrowser.pezik_bank[0] );

    elm = cfgmodule_register_new_element ( cmod, "membrowser_mr1z18_bank", CFGENTYPE_UNSIGNED, 0, 0, 0xff );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.mr1z18_bank, (void*) &g_membrowser.mr1z18_bank );

    elm = cfgmodule_register_new_element ( cmod, "membrowser_pehu_bank", CFGENTYPE_UNSIGNED, 0, 0, 0x3f );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.memext_pehu_bank, (void*) &g_membrowser.memext_pehu_bank );

    elm = cfgmodule_register_new_element ( cmod, "membrowser_memext_luftner_bank", CFGENTYPE_UNSIGNED, 0, 0, 0xff );
    cfgelement_set_handlers ( elm, (void*) &g_membrowser.memext_luftner_bank, (void*) &g_membrowser.memext_luftner_bank );
#endif

    /* Memory Heatmap GUI okno persistence (color mode, threshold, zoom,
     * atd.). Registrujeme do stejného DEBUGGER cfg modulu jako ostatní
     * debug klíče. SENTINEL: platí pro všechny známé architektury. */
    mhmap_window_register_persistence ( cmod );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );

    /* UI persistence registrace volající funkce z ui-imgui/ - skip jen v
     * headless test buildu, kde ui-imgui/ není linkován (jinak undefined
     * reference). Historický #ifndef NOT_HAVE_DEBUGGER_UI guard sem
     * nesedí (je vždy defined, vynechal by i production ImGui build). */
#ifndef MZTEST_HEADLESS
    /* I/O Ports panel persistence - samostatná sekce [IO_PORTS_PANEL]
     * (per UX spec a Michalovo rozhodnutí). Klíče: collapse_<chip>,
     * history_capacity, history_auto_follow, tracking_active. */
    {
        CFGMOD *iomod = cfgroot_register_new_module ( g_cfgmain,
                                                       "IO_PORTS_PANEL" );
        if ( iomod ) {
            io_window_register_persistence ( iomod );
            cfgmodule_parse ( iomod );
            cfgmodule_propagate ( iomod );
        }
    }

    /* Memory Map okno persistence - samostatná sekce [MEMMAP_WINDOW].
     * Klíče: compact_mode (kompaktní vs normální zobrazení banking sloupce). */
    {
        CFGMOD *mmod = cfgroot_register_new_module ( g_cfgmain,
                                                      "MEMMAP_WINDOW" );
        if ( mmod ) {
            memmap_window_register_persistence ( mmod );
            cfgmodule_parse ( mmod );
            cfgmodule_propagate ( mmod );
        }
    }

    /* V3 multi-view: 5 instancí Memory Browser oken (main + #2..#5).
     * Per-instance sekce [MEMBROWSER_WINDOW_MAIN..5] + legacy fallback
     * sekce [MEMBROWSER_WINDOW] pro upgrade z V0/V1/V2 INI (jednorázová
     * migrace na MAIN slot, pokud MAIN sekce v INI chybí).
     *
     * Registrace + parse + propagate VŠECH sekcí proběhne uvnitř
     * membrowser_window_register_persistence_all - neopakovat zde.
     *
     * V0-leftovers F4: CLI option --memory-browser propagate v
     * membrowser_window_apply_persisted volaném později z
     * sdlapp_imgui_video.cpp (až po vytvoření g_gui). */
    membrowser_window_register_persistence_all ( g_cfgmain );

    /* Events okno persistence - samostatná sekce [EVENT_VIEWER_WINDOW].
     * Klíče: window_open (bool, start with okno otevřené?),
     * filter_expression (text, poslední aktivní filter). Emu sekce
     * [EVENT_LOG] zůstává netknutá (mode/capacity/categories_mask). */
    {
        CFGMOD *emod = cfgroot_register_new_module ( g_cfgmain,
                                                      "EVENT_VIEWER_WINDOW" );
        if ( emod ) {
            event_viewer_window_register_persistence ( emod );
            cfgmodule_parse ( emod );
            cfgmodule_propagate ( emod );
        }
    }

    /* Vlna 2 Commit 15: Saved filter presets - sekce [EVENT_LOG_FILTERS].
     * User-defined library filter expression bindings (name + expr).
     * Klíče: preset_00_name .. preset_31_name + preset_00_expr ..
     * preset_31_expr (fixní 32 slotů, prázdné name = slot nepoužitý). */
    {
        CFGMOD *fmod = cfgroot_register_new_module ( g_cfgmain,
                                                      "EVENT_LOG_FILTERS" );
        if ( fmod ) {
            event_viewer_window_register_filters_persistence ( fmod );
            cfgmodule_parse ( fmod );
            cfgmodule_propagate ( fmod );
            event_viewer_window_apply_filters_persisted ( );
        }
    }

    /* V1.7 C.3.5: BP list okno per-tab persistence - sekce [BREAKPOINTS_PANEL].
     * Klíče: active_tab (-1..5), tab_<typ>_filter (text), tab_<typ>_enabled_only
     * (bool) pro každý z 6 per-typ tabů (exec/mem/io/irq/events/misc).
     * Po startu propagace nastaví g_bpt_ui.restore_active_tab_once = true,
     * první render BP okna force-selectne uložený tab. */
    {
        CFGMOD *bmod = cfgroot_register_new_module ( g_cfgmain,
                                                      "BREAKPOINTS_PANEL" );
        if ( bmod ) {
            bpt_window_register_persistence ( bmod );
            cfgmodule_parse ( bmod );
            cfgmodule_propagate ( bmod );
        }
    }

    /* F7 disassembler-window-v1: Disassembler V1 okno persistence - sekce
     * [DASM_WINDOW] s rozsahem From/To, options (sym_db/CDL/dialect/ORG/
     * bytes/uppercase) a poslední save path. */
    {
        CFGMOD *dmod = cfgroot_register_new_module ( g_cfgmain,
                                                      "DASM_WINDOW" );
        if ( dmod ) {
            dasm_window_register_persistence ( dmod );
            cfgmodule_parse ( dmod );
            cfgmodule_propagate ( dmod );
            dasm_window_apply_persisted ( );
        }
    }

    /* V6: Stack Monitor UI panel persistence - sekce [STACK_PANEL] s
     * UI preferencemi (V10: sp_lines_above slider, show_regions_window,
     * show_history_window, deprecated lock_sp_center). Definice regionů
     * jsou v emu vrstve (stack_regions_cfg_init -> sekce [STACK_REGIONS]),
     * recording flag v sekci [STACK_HISTORY]. UI panel pouze UI preference. */
    dbg_stack_panel_cfg_init ( );

    /* Callstack (mutant callstack Fáze 2C) UI panel persistence - sekce
     * [CALLSTACK_PANEL] s filter preferencemi (hide_irq, hide_synth).
     * Aktivace subsystému je v samostatné sekci [CALLSTACK] (= jádro
     * callstack.c, viz callstack_init na začátku debugger_init). */
    dbg_callstack_cfg_init ( );
#endif

    /* CLI options override pro CDL nastavení (po propagaci .ini hodnot).
     * Při invalidní hodnotě vypsat error a pokračovat (ignorovat), aby
     * uživatel viděl chybu ale emulátor se spustil s default. */
    const char *opt_mode = sdlapp_option_value ( "--cdl-mode" );
    if ( opt_mode )
    {
        if ( strcmp ( opt_mode, "off" ) == 0 )
        {
            g_debugger.mhmap_mode = DEBUGGER_MHMAP_MODE_OFF;
        }
        else if ( strcmp ( opt_mode, "window" ) == 0 )
        {
            g_debugger.mhmap_mode = DEBUGGER_MHMAP_MODE_WITH_WINDOW;
        }
        else if ( strcmp ( opt_mode, "always" ) == 0 )
        {
            g_debugger.mhmap_mode = DEBUGGER_MHMAP_MODE_ALWAYS;
        }
        else
        {
            fprintf ( stderr, "Invalid --cdl-mode value: %s (expected off|window|always)\n", opt_mode );
        };
    };

    const char *opt_dir = sdlapp_option_value ( "--cdl-dir" );
    if ( opt_dir )
    {
        /* Realokovat cdl_export_dir přes malloc/strdup-style (cfgmodule používá
         * malloc, takže free starého a vlastní alokace nového stejně). */
        char *new_dir = (char *) malloc ( strlen ( opt_dir ) + 1 );
        if ( new_dir )
        {
            strcpy ( new_dir, opt_dir );
            free ( g_debugger.cdl_export_dir );
            g_debugger.cdl_export_dir = new_dir;
        };
    };

    const char *opt_name = sdlapp_option_value ( "--cdl-name" );
    if ( opt_name )
    {
        char *new_name = (char *) malloc ( strlen ( opt_name ) + 1 );
        if ( new_name )
        {
            strcpy ( new_name, opt_name );
            free ( g_debugger.cdl_export_name );
            g_debugger.cdl_export_name = new_name;
        };
    };

    const char *opt_save = sdlapp_option_value ( "--cdl-save-on-exit" );
    if ( opt_save )
    {
        if ( strcmp ( opt_save, "on" ) == 0 )
        {
            g_debugger.cdl_export_on_exit = 1;
        }
        else if ( strcmp ( opt_save, "off" ) == 0 )
        {
            g_debugger.cdl_export_on_exit = 0;
        }
        else
        {
            fprintf ( stderr, "Invalid --cdl-save-on-exit value: %s (expected on|off)\n", opt_save );
        };
    };

    /* trace-suite subsystémy - init + CLI override + initial activation.
     * Aktivace bude triggernuta v mzarch_platform_fn_debugger_state_changed
     * volaném níže (recompute_active je tam). */
    cputrack_init ( );
    cputrack_apply_cli_options ( );

    /* Callstack subsystém (mutant callstack-experimental Fáze 1B). Init
     * registruje [CALLSTACK] sekci s `active` (bool). CLI --callstack
     * vynutí enable nad INI hodnotou. Aktivace zaregistruje Z80 CALL/RET
     * hooky - kontingent na cpu instanci, která už v tomto bodu existuje
     * (mzarch_platform_init proběhl). */
    callstack_init ( );
    callstack_apply_cli_options ( );

    /* Profiler subsystém (mutant profiler-experimental Fáze F1+F5). Init
     * registruje [PROFILER] sekci s klíči `active` (bool) a `show_window`
     * (bool, UI vrstva). Při active=1 vynutí callstack_set_active(true)
     * a zaregistruje listener. CLI --profiler vynutí enable nad INI
     * hodnotou (analogicky --callstack). */
    profiler_init ( );
    profiler_apply_cli_options ( );

    iorqlog_init ( );
    iorqlog_apply_cli_options ( );
    intlog_init ( );
    intlog_apply_cli_options ( );
    hwlog_init ( );
    hwlog_apply_cli_options ( );
    marklog_init ( );
    marklog_apply_cli_options ( );

    /* Event Viewer (mutant event-viewer Vlna 1 Commit 1) - in-memory ring
     * pro Events okno. Volá se po marklog_init() aby cfg propagate proběhl
     * dříve než alokace ringu (capacity se čte z cfg). */
    eventlog_init ( 0 );

    /* Vlna 4 Commit 29 - CLI replay: pokud byl předán --eventlog-replay
     * PATH, naimportuj soubor zpět do ringu po inicializaci. Selhání jen
     * logujeme (= chybný / chybějící soubor neblokuje start emu). */
    {
        const char *replay_path = sdlapp_option_value ( "--eventlog-replay" );
        if ( replay_path != NULL && replay_path[0] != '\0' ) {
            if ( eventlog_import_from_file ( replay_path ) != 0 ) {
                fprintf ( stderr, "[debugger] --eventlog-replay '%s' failed, continuing without replay\n",
                          replay_path );
            }
        }
    }

    /* I/O Ports panel activity tracker (V1.5 faze 3.3 / 1.5). */
    io_activity_init ( );

    /* I/O Ports panel history ring (V1.5 faze 3.3 / 1.6). */
    io_history_init ( IO_HISTORY_DEFAULT_CAPACITY );

    /* Shorthand --all-traces-mode / --all-traces-dir aplikujeme PO
     * per-subsystém options. Per-subsystém option má precedenci - shorthand
     * vyplní pouze ty subsystémy, kde uživatel per-subsys option nepředal.
     * Detekce přes sdlapp_option_present (ne hodnotu - hodnota může být
     * zfáknutá z .ini, my chceme jen CLI override). */
    {
        const char *all_mode = sdlapp_option_value ( "--all-traces-mode" );
        if ( all_mode ) {
            en_TLOG_MODE m = TLOG_MODE_OFF;
            int valid = 1;
            if ( g_ascii_strcasecmp ( all_mode, "off" ) == 0 ) m = TLOG_MODE_OFF;
            else if ( g_ascii_strcasecmp ( all_mode, "window" ) == 0 ) m = TLOG_MODE_WITH_WINDOW;
            else if ( g_ascii_strcasecmp ( all_mode, "always" ) == 0 ) m = TLOG_MODE_ALWAYS;
            else {
                fprintf ( stderr, "Invalid --all-traces-mode value: %s "
                                  "(expected off|window|always)\n", all_mode );
                valid = 0;
            }
            if ( valid ) {
                if ( !sdlapp_option_present ( "--cputrack-mode" ) ) g_cputrack_config.mode = m;
                if ( !sdlapp_option_present ( "--iorqlog-mode" ) )  g_iorqlog_config.mode  = m;
                if ( !sdlapp_option_present ( "--intlog-mode" ) )   g_intlog_config.mode   = m;
                if ( !sdlapp_option_present ( "--hwlog-mode" ) )    g_hwlog_config.mode    = m;
                if ( !sdlapp_option_present ( "--marklog-mode" ) )  g_marklog_config.mode  = m;
            }
        }

        const char *all_dir = sdlapp_option_value ( "--all-traces-dir" );
        if ( all_dir ) {
            if ( !sdlapp_option_present ( "--cputrack-dir" ) ) {
                g_free ( g_cputrack_config.dir );
                g_cputrack_config.dir = g_strdup ( all_dir );
            }
            if ( !sdlapp_option_present ( "--iorqlog-dir" ) ) {
                g_free ( g_iorqlog_config.dir );
                g_iorqlog_config.dir = g_strdup ( all_dir );
            }
            if ( !sdlapp_option_present ( "--intlog-dir" ) ) {
                g_free ( g_intlog_config.dir );
                g_intlog_config.dir = g_strdup ( all_dir );
            }
            if ( !sdlapp_option_present ( "--hwlog-dir" ) ) {
                g_free ( g_hwlog_config.dir );
                g_hwlog_config.dir = g_strdup ( all_dir );
            }
            if ( !sdlapp_option_present ( "--marklog-dir" ) ) {
                g_free ( g_marklog_config.dir );
                g_marklog_config.dir = g_strdup ( all_dir );
            }
        }
    }

    /* Po načtení .ini hodnot a CLI override musíme triggernout swap CPU
     * callbacků - jinak by Z80 zůstalo na vanilla callbackech (memory_read_cb,
     * ne _with_logging_cb) až do prvního UI eventu. To by způsobilo, že
     * recording v režimu --cdl-mode=always bez otevřeného debug okna
     * vůbec neproběhne (default callbacks neaktivují MH/Tracelog hooky). */
    mzarch_platform_fn_debugger_state_changed ( TEST_DEBUGGER_ACTIVE );
}


void debugger_show_main_window ( void ) {
    /* g_debugger.active musí být nastavený PŘED voláním swap callbacků,
     * protože swap funkce čte tento flag přes TEST_DEBUGGER_NEED_DEBUG_CALLBACKS
     * makro pro rozhodnutí mezi rychlou a pomalou cestou. */
    g_debugger.active = 1;
    mzarch_platform_fn_debugger_state_changed(true);
    ui_debugger_show_main_window ( );
    debugger_step_call ( 0 );
}


void debugger_hide_main_window ( void ) {
    /* Stejné pořadí: nejdřív aktualizovat flag, pak provést swap. */
    g_debugger.active = 0;
    mzarch_platform_fn_debugger_state_changed(false);
    ui_debugger_hide_main_window ( );
    debugger_step_call ( 0 );
}


void debugger_show_hide_main_window ( void ) {
    if ( !TEST_DEBUGGER_ACTIVE ) {
        debugger_show_main_window ( );
    } else {
        debugger_hide_main_window ( );
    };
}


/* Timeout pro synchronní doručení RECOMPUTE na emu vlákno. Stejná hodnota
 * jako DBG_UI_DEFAULT_TIMEOUT_MS (dbgapi_helpers.h) - UI hlavičku sem
 * netaháme (vrstvení: emulator core nesmí záviset na ui-imgui). */
#define DEBUGGER_WINDOW_RECOMPUTE_TIMEOUT_MS 200


/* Společné jádro *_request variant: nastaví g_debugger.active (atomický
 * int zápis) a swap CPU callbacků deleguje na EMU vlákno přes
 * DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE (vzor trace-suite). Přímé volání
 * mzarch_platform_fn_debugger_state_changed() z UI vlákna by souběžně
 * s běžící instrukční smyčkou přepínalo memory/port callbacky a výběr
 * instrukční smyčky = data race. */
static void debugger_window_state_apply_request ( int active ) {
    g_debugger.active = active;
    if ( !dbgapi_ui_submit_cmd_sync ( &g_dbgapi_cmdrq_queue,
                                      DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE,
                                      NULL, NULL,
                                      DEBUGGER_WINDOW_RECOMPUTE_TIMEOUT_MS ) ) {
        /* Emu vlákno request nestihlo/nepřevzalo (startup, shutdown,
         * plná fronta). Okno se přesto zobrazí/skryje - callbacky srovná
         * nejbližší další recompute (další toggle, trace/BP změna). */
        fprintf ( stderr, "%s(): debugger state recompute not delivered to emu thread!\n", __func__ );
    };
}


void debugger_show_main_window_request ( void ) {
    debugger_window_state_apply_request ( 1 );
    ui_debugger_show_main_window ( );
    debugger_step_call ( 0 );
}


void debugger_hide_main_window_request ( void ) {
    debugger_window_state_apply_request ( 0 );
    ui_debugger_hide_main_window ( );
    debugger_step_call ( 0 );
}


void debugger_show_hide_main_window_request ( void ) {
    if ( !TEST_DEBUGGER_ACTIVE ) {
        debugger_show_main_window_request ( );
    } else {
        debugger_hide_main_window_request ( );
    };
}


void debugger_update_all ( void ) {
    if ( !TEST_DEBUGGER_ACTIVE ) return;
}


void debugger_animation ( void ) {
    if ( !TEST_DEBUGGER_ACTIVE ) return;
}


uint8_t debugger_dasm_read_cb ( uint16_t addr, void *user_data ) {
    (void)user_data;
    g_debugger.memop_call = 1;
    uint8_t retval = memory_read_byte ( addr );
    g_debugger.memop_call = 0;
    return retval;
}


uint8_t debugger_dasm_history_read_cb ( uint16_t addr, void *user_data ) {
    uint8_t *position = user_data;
    uint8_t retval = g_debugger_history.row[*position].byte[addr - g_debugger_history.row[*position].addr];
    return retval;
}


uint8_t debugger_dasm_pure_ram_read_cb ( uint16_t addr, void *user_data ) {
    (void)user_data;
    return memory_pure_ram_read_byte ( addr );
}


void debugger_memory_write_byte ( uint16_t addr, uint8_t value ) {
    g_debugger.memop_call = 1;
    g_debugger.memop_vram_touched = 0;
    memory_write_byte ( addr, value );
    g_debugger.memop_call = 0;

    /* Pokud zápis v aktuálním banking/mode kontextu dopadl do VRAM/CGRAM
     * (= vramctrl_*_memop_write_byte nastavily memop_vram_touched), a uživatel
     * má zapnuté "Auto refresh on edit", vyvoláme force screen update.
     * Bez toho by se změna obrazu projevila až při dalším frame nebo step. */
    if ( g_debugger.memop_vram_touched ) {
        g_debugger.memop_vram_touched = 0;
        debugger_screen_refresh_if_enabled();
    }
}


void debugger_screen_refresh_if_enabled ( void ) {
    if ( g_debugger.screen_refresh_on_edit ) {
        debugger_forced_screen_update();
    }
}


void debugger_mmap_mount ( unsigned value ) {
    g_memory.map |= value;
}


void debugger_mmap_umount ( unsigned value ) {
    g_memory.map &= ( ~value ) & 0x0f;
}


void debugger_change_z80_flagbit ( unsigned flagbit, unsigned value ) {

    if ( !EMULATOR_TEST_PAUSED ) {
        /* Toggle button neni potreba nastavovat zpet, protoze po pauze dojde k update */
        return;
    };

    uint16_t af_value = z80_get_reg ( g_mzarch_main.cpu, Z80_REG_AF );

    if ( value ) {
        af_value |= 1 << flagbit;
    } else {
        af_value &= ~( 1 << flagbit );
    };

    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_AF, af_value );
}


void debugger_change_z80_register ( z80_reg_t reg, uint16_t value ) {

    if ( !EMULATOR_TEST_PAUSED ) {
        return;
    };

    if ( reg == Z80_REG_IR ) {
        /* Specialni handling: UI posila R jako IR, ale chceme nastavit jen dolni bajt (R) */
        g_mzarch_main.cpu->r = (uint8_t)(value & 0x7F) | (g_mzarch_main.cpu->r & 0x80);
    } else {
        z80_set_reg ( g_mzarch_main.cpu, reg, value );
    };
}


void debugger_change_dmd ( uint8_t value ) {

    if ( !EMULATOR_TEST_PAUSED ) {
        /* Hodnotu comboboxu neni potreba nastavovat zpet, protoze po pauze dojde k update */
        return;
    };

    gdg_write_byte ( 0xce, value );
}


void debugger_change_gdg_reg_border ( uint8_t value ) {

    if ( !EMULATOR_TEST_PAUSED ) {
        /* Hodnotu comboboxu neni potreba nastavovat zpet, protoze po pauze dojde k update */
        return;
    };

    gdg_write_byte ( 0x06cf, value );
}


void debugger_change_gdg_reg_palgrp ( uint8_t value ) {

    if ( !EMULATOR_TEST_PAUSED ) {
        /* Hodnotu comboboxu neni potreba nastavovat zpet, protoze po pauze dojde k update */
        return;
    };

    gdg_write_byte ( 0xf0, value | 0x40 );
}


void debugger_change_gdg_reg_pal ( uint8_t pal, uint8_t value ) {

    if ( !EMULATOR_TEST_PAUSED ) {
        /* Hodnotu comboboxu neni potreba nastavovat zpet, protoze po pauze dojde k update */
        return;
    };

    gdg_write_byte ( 0xf0, value | ( pal << 4 ) );
}


void debugger_change_gdg_rfr ( uint8_t value ) {

    if ( !EMULATOR_TEST_PAUSED ) {
        /* Hodnotu comboboxu neni potreba nastavovat zpet, protoze po pauze dojde k update */
        return;
    };

    gdg_write_byte ( 0xcd, value );
}


void debugger_change_gdg_wfr ( uint8_t value ) {

    if ( !EMULATOR_TEST_PAUSED ) {
        /* Hodnotu comboboxu neni potreba nastavovat zpet, protoze po pauze dojde k update */
        return;
    };

    gdg_write_byte ( 0xcc, value );
}


static uint16_t debugger_a2hex ( char c ) {

    if ( c >= '0' && c <= '9' ) {
        return ( c - '0' );
    };

    if ( c >= 'a' && c <= 'f' ) {
        return ( c - 'a' + 0x0a );
    };

    if ( c >= 'A' && c <= 'F' ) {
        return ( c - 'A' + 0x0a );
    };

    return 0;
}


uint32_t debuger_hextext_to_uint32 ( const char *txt ) {

    unsigned length;
    unsigned i;
    uint32_t value = 0;

    length = strlen ( txt );

    if ( length == 0 ) {
        return 0;
    };

    i = 0;
    while ( length ) {
        value += debugger_a2hex ( txt [ length - 1 ] ) << i;
        length--;
        i += 4;
    };

    return value;
}


void debugger_forced_screen_update ( void ) {
    /* Vlastní logika přesunuta do core mzarch_forced_full_screen_refresh(),
     * aby byla dostupná i v buildu bez debuggeru (snapshot load ji volá).
     * Tato funkce zůstává jako tenký wrapper pro stávající debugger call
     * sites (Ctrl+R, screen_refresh_at_step, screen_refresh_on_edit). */
    mzarch_forced_full_screen_refresh ( );
}

#endif // MZ800EMU_CFG_DEBUGGER_ENABLED
