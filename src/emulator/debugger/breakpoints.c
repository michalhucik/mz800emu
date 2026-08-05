/*
 * File:   breakpoints.c
 *
 * Modul breakpoints — vyšší vrstva nad bptmap.
 * Vlastní kompletní data breakpointů a skupin, řídí jejich životní cyklus,
 * ukládá konfiguraci do hlavního INI.
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

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdlib.h>

#include "breakpoints.h"
#include "bp_expr.h"
#include "bp_action.h"
#include "bp_vars.h"
#include "bptmap.h"
#include "cfgmain.h"
#include "main.h"   /* g_sdlapp */
#include "libs/sdlapp/sdlapp_paths.h"

/* V1.5.A8.5 - IRQ_SIG enforce potřebuje MZARCH_INTERRUPT_* a (volitelně)
 * g_pioz80 pro sub-detekci PIOZ80 portu A / B. */
#include "mzarch/interrupt.h"
#include "mzarch/mzhal.h"
#include "hw-generic/pioz80/pioz80.h"

/* V1.5 fáze 2.4 - Cycle/Frame/Scanline ctx zdroje z GDG.
 * Per-arch include (mz800_gdg.h a mz1500_gdg.h definují identický
 * pattern g_gdg + total_elapsed.screens / ticks + beam_row). */
#include "hw-generic/gdg/gdg_state.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>


st_BREAKPOINTS g_breakpoints;


/* ========================================================================= */
/*  Interní (static) funkce                                                  */
/* ========================================================================= */


/* 0019 vrstva 3 - cfg callbacky byte backstopu (definice u API dole). */
static void breakpoints_propagatecfg_byte_limit ( void *e, void *data );
static void breakpoints_savecfg_byte_limit ( void *e, void *data );

/* 0019 vrstva 2 - cfg callbacky globálního default rate-limit intervalu. */
static void breakpoints_propagatecfg_fwd_default_interval ( void *e, void *data );
static void breakpoints_savecfg_fwd_default_interval ( void *e, void *data );


/*
 * Přidělí nové unikátní ID (od 1 výše). Vrátí -1 při přetečení.
 */
static int breakpoints_alloc_id ( void ) {
    if ( g_breakpoints.next_id < 1 ) {
        fprintf ( stderr, "BREAKPOINTS ERROR: ID counter overflow!\n" );
        return -1;
    };
    return g_breakpoints.next_id++;
}


/*
 * Najde index skupiny v GArray podle group_id. Vrátí index nebo -1.
 */
static int breakpoints_find_group_index ( int group_id ) {
    unsigned i;
    for ( i = 0; i < g_breakpoints.groups->len; i++ ) {
        st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, i );
        if ( grp->id == group_id ) return (int)i;
    };
    return -1;
}


/*
 * Mapuje en_BPT_TYPE na en_BPTMAP_TYPE_IDX (= per-typ index v bptmap V2).
 * PC_EXEC nemá per-typ tabulku (= legacy O(1) bpmap[]) - vrátí -1.
 * HW_EVENT mapuje na BPTMAP_IDX_HW_EVENT (D.3).
 * SP_THRESHOLD mapuje na BPTMAP_IDX_SP_THRESHOLD (D.4).
 */
static int breakpoints_bpt_type_to_idx ( en_BPT_TYPE type ) {
    switch ( type ) {
        case BPT_TYPE_MEM_R:        return BPTMAP_IDX_MEM_R;
        case BPT_TYPE_MEM_W:        return BPTMAP_IDX_MEM_W;
        case BPT_TYPE_IORQ_R:       return BPTMAP_IDX_IORQ_R;
        case BPT_TYPE_IORQ_W:       return BPTMAP_IDX_IORQ_W;
        case BPT_TYPE_IRQ:          return BPTMAP_IDX_IRQ;
        case BPT_TYPE_GLOBAL:       return BPTMAP_IDX_GLOBAL;
        case BPT_TYPE_HW_EVENT:     return BPTMAP_IDX_HW_EVENT;
        case BPT_TYPE_SP_THRESHOLD: return BPTMAP_IDX_SP_THRESHOLD;
        case BPT_TYPE_IRQ_SIG:      return BPTMAP_IDX_IRQ_SIG;
        default:                    return -1;
    };
}


/*
 * Přepočítá g_bp_event_active[] na základě aktuální množiny BP s typem
 * HW_EVENT a effective enabled. Volá se z sync_bptmap a po set_event_name.
 *
 * Algoritmus: clear all → projdi BP s type=HW_EVENT effective enabled
 * a parsed_event != NONE, set bit. Stejný princip jako per_type_active[].
 */
static void breakpoints_recompute_event_active ( void ) {
    bp_event_clear_all_active ( );
    unsigned i;
    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( bpt->type != BPT_TYPE_HW_EVENT ) continue;
        if ( bpt->parsed_event <= BP_EVENT_NONE || bpt->parsed_event >= BP_EVENT_COUNT ) continue;
        if ( !bpt->enabled ) continue;
        /* group enabled check provede caller přes sync_bptmap, ale pro
         * standalone volání (= set_event_name na effective enabled BP)
         * checkujeme zde také. */
        extern bool breakpoints_is_effectively_enabled ( int );
        if ( !breakpoints_is_effectively_enabled ( bpt->id ) ) continue;
        bp_event_set_active ( bpt->parsed_event, true );
    };
}


/*
 * Najde index breakpointu v GArray podle bpt_id. Vrátí index nebo -1.
 */
static int breakpoints_find_bpt_index ( int bpt_id ) {
    unsigned i;
    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( bpt->id == bpt_id ) return (int)i;
    };
    return -1;
}


/**
 * @brief Maximální hloubka rekurze pri prohledávání parent řetězce
 *        v breakpoints_is_group_enabled.
 *
 * Belt-and-suspenders ochrana: load-time validace
 * (breakpoints_validate_group_parents) by měla cykly zachytit, ale
 * runtime guard chrání případ, kdy by setter selhal nebo kdyby kód
 * obešel validaci. Praktické hierarchie skupin mají jen jednotky úrovní
 * - 32 je s rezervou.
 */
#define BREAKPOINTS_GROUP_DEPTH_MAX  32


/*
 * Rekurzivně zkontroluje, zda je skupina (a všechny její rodičovské skupiny) povolena.
 * Vrátí true pokud je celý řetězec enabled.
 *
 * @param group_id  ID skupiny k testování (-1 = root, vždy true).
 * @param depth     Aktuální hloubka rekurze - volat externě s 0.
 * @return true pokud je skupina i všichni rodičové enabled, jinak false.
 *         Pri překročení BREAKPOINTS_GROUP_DEPTH_MAX vrací true (= bezpečný
 *         fallback shodný s neexistující skupinou) a vypisuje warning.
 */
static bool breakpoints_is_group_enabled_depth ( int group_id, int depth ) {
    if ( group_id == -1 ) return true; /* root je vždy enabled */

    if ( depth >= BREAKPOINTS_GROUP_DEPTH_MAX ) {
        fprintf ( stderr,
                  "BREAKPOINTS WARNING: group parent chain depth limit (%d) "
                  "exceeded at group_id=%d - cycle suspected, treating as enabled\n",
                  BREAKPOINTS_GROUP_DEPTH_MAX, group_id );
        return true;
    };

    int idx = breakpoints_find_group_index ( group_id );
    if ( idx < 0 ) return true; /* neexistující skupina — považujeme za root */

    st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, (unsigned)idx );
    if ( !grp->enabled ) return false;

    return breakpoints_is_group_enabled_depth ( grp->parent, depth + 1 );
}


/*
 * Wrapper bez depth parametru - veřejné rozhraní.
 */
static bool breakpoints_is_group_enabled ( int group_id ) {
    return breakpoints_is_group_enabled_depth ( group_id, 0 );
}


/*
 * Uvolní heap stringy v breakpointu (name + smart BP V1: event_name, expr, action).
 */
static void breakpoints_free_bpt_strings ( st_BPT *bpt ) {
    if ( bpt->name ) {
        g_free ( bpt->name );
        bpt->name = NULL;
    };
    if ( bpt->event_name ) {
        g_free ( bpt->event_name );
        bpt->event_name = NULL;
    };
    if ( bpt->expr ) {
        g_free ( bpt->expr );
        bpt->expr = NULL;
    };
    if ( bpt->action ) {
        g_free ( bpt->action );
        bpt->action = NULL;
    };
    if ( bpt->parsed_expr ) {
        bp_expr_free ( bpt->parsed_expr );
        bpt->parsed_expr = NULL;
    };
    if ( bpt->parsed_action ) {
        bp_action_free ( bpt->parsed_action );
        bpt->parsed_action = NULL;
    };
}


/*
 * Uvolní heap stringy ve skupině.
 */
static void breakpoints_free_group_strings ( st_BPTGROUP *grp ) {
    if ( grp->name ) {
        g_free ( grp->name );
        grp->name = NULL;
    };
}


/* ========================================================================= */
/*  Životní cyklus                                                           */
/* ========================================================================= */


void breakpoints_init ( void ) {

    memset ( &g_breakpoints, 0x00, sizeof ( g_breakpoints ) );
    g_breakpoints.next_id = 1;

    g_breakpoints.groups = g_array_new ( FALSE, TRUE, sizeof ( st_BPTGROUP ) );
    g_breakpoints.breakpoints = g_array_new ( FALSE, TRUE, sizeof ( st_BPT ) );

    /* User $vars storage (D.1.3). */
    bp_vars_init ( );

    /* HW event vocabulary infra (D.3) - clear bitmap aktivních eventů. */
    bp_event_init ( );

    /* Registrace konfigurace do hlavního INI */
    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "BREAKPOINTS" );
    CFGELM *elm;

    /* Nový JSON formát .bpt (post fáze D.1). Default filename je
     * per-architektura ("mz800-breakpoints.bpt" / "mz1500-breakpoints.bpt"),
     * aby šlo přepínat mezi mz800emu a mz1500emu se separátní BP sadou
     * v jednom config dir.
     *
     * Klíč přejmenován z "default_file" (= INI éra A'/C) na "bpt_file"
     * - pokud user má v cfgmain.ini starý "default_file=breakpoints.ini"
     * záznam, cfg lib ho ignoruje a použije nový default. Stará hodnota
     * zůstane v INI jako mrtvý klíč (= žádná migrace v V1). */
    static char default_bpt_file[48];
    g_snprintf ( default_bpt_file, sizeof ( default_bpt_file ),
                 "%s-breakpoints.bpt", g_mzhal.arch_name );
#define DEFAULT_BPT_FILE default_bpt_file
    elm = cfgmodule_register_new_element ( cmod, "bpt_file", CFGENTYPE_TEXT, DEFAULT_BPT_FILE );
    cfgelement_bind ( elm, (void*) &g_breakpoints.default_file );
#undef DEFAULT_BPT_FILE

    elm = cfgmodule_register_new_element ( cmod, "auto_save", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void*) &g_breakpoints.auto_save );

    elm = cfgmodule_register_new_element ( cmod, "auto_load", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void*) &g_breakpoints.auto_load );

    elm = cfgmodule_register_new_element ( cmod, "groups_first", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void*) &g_breakpoints.groups_first );

    /* 0019 vrstva 3: práh kumulativního byte backstopu pro FWD akce v MB.
     * Default 256 MB (= BP_ACTION_FWD_DEFAULT_BYTE_LIMIT). 0 = backstop vypnut.
     * Hodnotu propaguje breakpoints_propagatecfg_byte_limit (MB -> bajty), zpět
     * ji ukládá breakpoints_savecfg_byte_limit (bajty -> MB). */
    elm = cfgmodule_register_new_element ( cmod, "fwd_byte_limit_mb",
                                           CFGENTYPE_UNSIGNED,
                                           (unsigned) ( BP_ACTION_FWD_DEFAULT_BYTE_LIMIT / ( 1024 * 1024 ) ),
                                           0, 0xFFFFFFFFu );
    cfgelement_set_propagate_cb ( elm, breakpoints_propagatecfg_byte_limit, NULL );
    cfgelement_set_save_cb ( elm, breakpoints_savecfg_byte_limit, NULL );

    /* 0019 vrstva 2: globální default min. odstup těžkých FWD akcí v ms pro
     * BP bez vlastního per-BP override (fwd_min_interval_ms == 0). Default
     * BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS (= bezpečný default i bez
     * konfigurace). Propaguje breakpoints_propagatecfg_fwd_default_interval,
     * zpět ji ukládá breakpoints_savecfg_fwd_default_interval. */
    elm = cfgmodule_register_new_element ( cmod, "fwd_default_min_interval_ms",
                                           CFGENTYPE_UNSIGNED,
                                           (unsigned) BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS,
                                           0, 0xFFFFFFFFu );
    cfgelement_set_propagate_cb ( elm, breakpoints_propagatecfg_fwd_default_interval, NULL );
    cfgelement_set_save_cb ( elm, breakpoints_savecfg_fwd_default_interval, NULL );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );

    /* 0019 vrstva 3: reset byte akumulátoru při (re-)init = reset emulátoru. */
    breakpoints_fwd_reset_byte_accounting ( );

    /* 0019 vrstva 3 (flush-side): zaregistruj guard do trace vrstvy, aby se byte
     * backstop vyhodnotil i na flush cestě (inkrementální chunk swapy mezi dvěma
     * trace_save fire), ne jen na hraně BP fire. Bez něj cputrack/trace flood bez
     * periodické trace_save akce roste neohraničený (V19B disk-flood vektor). */
    bp_action_install_disk_flush_guard ( );

    /* Automatické načtení breakpointů ze souboru */
    if ( g_breakpoints.auto_load ) {
        breakpoints_load_from_file ( );
    };

    /* V1.5.B: cfg sekce [BP_VARS] + auto-load standalone .vars souboru.
     * Volá se PO breakpoints_load_from_file - tj. .vars soubor případně
     * přepíše vars načtené z .bpt (= explicit per-arch separace).
     * Pokud user nechce per-arch standalone vars, nastaví auto_load=0
     * v BP_VARS sekci. */
    bp_vars_cfg_init ( );
}


void breakpoints_exit ( void ) {

    /* Automatické uložení breakpointů do souboru */
    if ( g_breakpoints.auto_save ) {
        breakpoints_save_to_file ( );
    };

    /* V1.5.B: auto-save standalone .vars souboru (nezávisle na .bpt).
     * Vars jsou v .bpt vždy součástí (= sdílené persist), separate
     * .vars je pro export/share/per-arch izolaci. */
    bp_vars_cfg_auto_save ( );

    /* Uvolnění paměti breakpointů */
    unsigned i;
    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        breakpoints_free_bpt_strings ( bpt );
    };
    g_array_free ( g_breakpoints.breakpoints, TRUE );
    g_breakpoints.breakpoints = NULL;

    /* Uvolnění paměti skupin */
    for ( i = 0; i < g_breakpoints.groups->len; i++ ) {
        st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, i );
        breakpoints_free_group_strings ( grp );
    };
    g_array_free ( g_breakpoints.groups, TRUE );
    g_breakpoints.groups = NULL;

    /* User $vars storage (D.1.3) - po posledním save je free safe. */
    bp_vars_destroy ( );
}


/* ========================================================================= */
/*  Validace po načtení                                                      */
/* ========================================================================= */


/*
 * Vrátí true pokud string je NULL, prázdný, nebo obsahuje jen mezery.
 */
static bool breakpoints_is_name_empty ( const char *name ) {
    if ( !name ) return true;
    while ( *name ) {
        if ( *name != ' ' ) return false;
        name++;
    };
    return true;
}


/*
 * Zkontroluje parent reference skupin:
 * - mrtvé reference (rodič neexistuje) → rozpojení na root + warning
 * - cyklické reference → rozpojení na root + warning
 *
 * Cycle detection používá pevný depth limit BREAKPOINTS_GROUP_DEPTH_MAX
 * (= 32) - je s rezervou pro reálné stromy a chrání i případ self-loop
 * (= grp->parent == grp->id), kde dynamický limit groups->len neuspěl
 * by sice (cyklus se uzavírá přes 1 hop), ale fixní limit ukončí scan
 * deterministicky.
 *
 * Warningy jdou na stderr (= konzistence s ostatními BP runtime warningy).
 */
static void breakpoints_validate_group_parents ( void ) {
    const unsigned max_depth = BREAKPOINTS_GROUP_DEPTH_MAX;
    unsigned i;

    for ( i = 0; i < g_breakpoints.groups->len; i++ ) {
        st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, i );
        if ( grp->parent < 0 ) continue; /* root — OK */

        /* Zkontrolovat, zda přímý rodič vůbec existuje */
        if ( breakpoints_find_group_index ( grp->parent ) < 0 ) {
            fprintf ( stderr,
                      "BREAKPOINTS WARNING: group '%s' (id=%d) references "
                      "non-existent parent %d - unparenting\n",
                      grp->name ? grp->name : "", grp->id, grp->parent );
            grp->parent = -1;
            continue;
        };

        /* Projít chain a detekovat cyklus.
         * Cyklus rozeznáme dvěma způsoby:
         * 1. Návrat na current group ID (= self-loop nebo uzavřený kruh)
         * 2. Překročení depth limitu (= belt-and-suspenders) */
        int current = grp->parent;
        unsigned depth = 0;
        bool cycle = false;

        while ( current >= 0 && depth < max_depth ) {
            if ( current == grp->id ) {
                cycle = true;
                break;
            };
            int idx = breakpoints_find_group_index ( current );
            if ( idx < 0 ) break;
            st_BPTGROUP *parent_grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, (unsigned)idx );
            current = parent_grp->parent;
            depth++;
        };

        if ( cycle || depth >= max_depth ) {
            fprintf ( stderr,
                      "BREAKPOINTS WARNING: cyclic parent chain in group '%s' "
                      "(id=%d) - unparenting (depth=%u, %s)\n",
                      grp->name ? grp->name : "", grp->id, depth,
                      cycle ? "self-revisit" : "depth limit" );
            grp->parent = -1;
        };
    };

    /* Totéž pro eventy — zkontrolovat mrtvé parent reference */
    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( bpt->parent < 0 ) continue;

        if ( breakpoints_find_group_index ( bpt->parent ) < 0 ) {
            fprintf ( stderr,
                      "BREAKPOINTS WARNING: event '%s' (id=%d) references "
                      "non-existent group %d - unparenting\n",
                     bpt->name ? bpt->name : "", bpt->id, bpt->parent );
            bpt->parent = -1;
        };
    };
}


/*
 * Zkontroluje, zda na stejné adrese nejsou dva efektivně povolené eventy.
 * Pokud najde duplikát, zakáže pozdější event a vypíše warning.
 */
static void breakpoints_validate_event_addresses ( void ) {
    unsigned i, j;

    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( !bpt->enabled ) continue;
        if ( !breakpoints_is_group_enabled ( bpt->parent ) ) continue;

        /* Zkontrolovat proti všem předchozím efektivně povoleným eventům */
        for ( j = 0; j < i; j++ ) {
            st_BPT *prev = &g_array_index ( g_breakpoints.breakpoints, st_BPT, j );
            if ( !prev->enabled ) continue;
            if ( !breakpoints_is_group_enabled ( prev->parent ) ) continue;

            if ( bpt->addr == prev->addr ) {
                printf ( "BREAKPOINTS WARNING: duplicate enabled event at address 0x%04x "
                         "(id=%d '%s' conflicts with id=%d '%s') — disabling\n",
                         bpt->addr, bpt->id, bpt->name ? bpt->name : "",
                         prev->id, prev->name ? prev->name : "" );
                bpt->enabled = false;
                break;
            };
        };
    };
}


/* ========================================================================= */
/*  Persistence — JSON save / load (smart BP V1, fáze D.1)                   */
/* ========================================================================= */

/*
 * Verze .bpt JSON formátu. Při změně schématu (přidaná/odebraná pole)
 * inkrementovat - load by měl tolerovat starší/novější verze v rámci
 * rozumu, neznámá pole ignorovat, chybějící pole brát jako default.
 */
#define BREAKPOINTS_JSON_FORMAT_VERSION  1


/*
 * Sestaví JSON pro skupinu a přidá ji do builderu (jako prvek pole).
 */
static void breakpoints_json_emit_group ( JsonBuilder *b, const st_BPTGROUP *grp ) {
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "id" );
    json_builder_add_int_value ( b, grp->id );

    json_builder_set_member_name ( b, "parent_id" );
    json_builder_add_int_value ( b, grp->parent );

    json_builder_set_member_name ( b, "name" );
    json_builder_add_string_value ( b, grp->name ? grp->name : "" );

    json_builder_set_member_name ( b, "enabled" );
    json_builder_add_boolean_value ( b, grp->enabled );

    json_builder_set_member_name ( b, "order" );
    json_builder_add_double_value ( b, (double) grp->order );

    json_builder_set_member_name ( b, "color_bg" );
    json_builder_add_int_value ( b, (gint64) grp->bg_rgb );

    json_builder_set_member_name ( b, "color_fg" );
    json_builder_add_int_value ( b, (gint64) grp->fg_rgb );

    json_builder_end_object ( b );
}


/*
 * Sestaví JSON pro breakpoint a přidá ho do builderu (jako prvek pole).
 *
 * Schéma per BP záznam (pořadí pro čitelnost):
 *   id, parent_id, type, addr, addr_end, addr_match_mode, addr_mask,
 *   zone, bank_id, bank_id_end, bank_match_mode, bank_id_mask,
 *   port, port_end, port_match_mode, port_mask, port_mode,
 *   event_name, sp_threshold, sp_upper, sp_mode,
 *   im2_vector_enabled, im2_vector_addr, im2_isr_enabled, im2_isr_addr,
 *   expr, action, hit_count, skip_count,
 *   edge_triggered, auto_name, name, enabled, color_bg, color_fg, hits,
 *   fwd_min_interval_ms, fwd_max_fires
 *
 * Stringy expr/action/event_name s NULL hodnotou jsou serializovány
 * jako JSON null (= explicit absence).
 *
 * V1.5.E nové klíče (addr_match_mode, addr_mask, port_*, bank_*,
 * sp_*) - viz BC fallback v breakpoints_json_load_bpt.
 *
 * V1.5.A8 nové klíče (im2_vector_enabled, im2_vector_addr,
 * im2_isr_enabled, im2_isr_addr) - chybí-li, defaultně false / 0
 * (= legacy IRQ chování bez filtru).
 *
 * V1.5.A8.5 nové klíče:
 *   im0_enabled / im1_enabled / im2_enabled (= IM mode discriminator,
 *     BC fallback all-true pro pre-A8.5 IRQ BP).
 *   im0_rst_mask (= IM 0 RST opcode bitmask, BC fallback 0 = match-all).
 *   irq_sig_sources (JSON array stable string names pro BPT_TYPE_IRQ_SIG,
 *     BC fallback prázdné = mask 0 = invalid).
 */
/**
 * @brief V1.D.2.C - en_DBGAPI_CMD_ORIGIN -> stable persist token.
 *
 * Stable tokeny "user" / "mcp" / "test" / "internal" napříč verzemi
 * schématu - loader je tolerantní k unknown (= default USER).
 */
static const char *_bp_origin_to_str ( en_DBGAPI_CMD_ORIGIN o ) {
    switch ( o ) {
        case DBGAPI_CMD_ORIGIN_USER:     return "user";
        case DBGAPI_CMD_ORIGIN_MCP:      return "mcp";
        case DBGAPI_CMD_ORIGIN_TEST:     return "test";
        case DBGAPI_CMD_ORIGIN_INTERNAL: return "internal";
    }
    return "user";
}


/**
 * @brief V1.D.2.C - parse stable persist token -> en_DBGAPI_CMD_ORIGIN.
 *
 * Tolerantní k missing / unknown tokenům: fallback na USER (= GUI default).
 */
static en_DBGAPI_CMD_ORIGIN _bp_origin_from_str ( const char *s ) {
    if ( !s || !s[0] ) return DBGAPI_CMD_ORIGIN_USER;
    if ( strcmp ( s, "mcp" )      == 0 ) return DBGAPI_CMD_ORIGIN_MCP;
    if ( strcmp ( s, "test" )     == 0 ) return DBGAPI_CMD_ORIGIN_TEST;
    if ( strcmp ( s, "internal" ) == 0 ) return DBGAPI_CMD_ORIGIN_INTERNAL;
    return DBGAPI_CMD_ORIGIN_USER;
}


static void breakpoints_json_emit_bpt ( JsonBuilder *b, const st_BPT *bpt ) {
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "id" );
    json_builder_add_int_value ( b, bpt->id );

    json_builder_set_member_name ( b, "parent_id" );
    json_builder_add_int_value ( b, bpt->parent );

    json_builder_set_member_name ( b, "type" );
    json_builder_add_string_value ( b, bpt_type_to_string ( bpt->type ) );

    json_builder_set_member_name ( b, "addr" );
    json_builder_add_int_value ( b, bpt->addr );

    json_builder_set_member_name ( b, "addr_end" );
    json_builder_add_int_value ( b, bpt->addr_end );

    /* V1.5.E - addr match mode + mask */
    json_builder_set_member_name ( b, "addr_match_mode" );
    json_builder_add_string_value ( b, bp_match_mode_to_string ( bpt->addr_match_mode ) );

    json_builder_set_member_name ( b, "addr_mask" );
    json_builder_add_int_value ( b, bpt->addr_mask );

    json_builder_set_member_name ( b, "zone" );
    json_builder_add_string_value ( b, bp_zone_to_string ( bpt->zone ) );

    json_builder_set_member_name ( b, "bank_id" );
    json_builder_add_int_value ( b, bpt->bank_id );

    /* V1.5.E - bank match mode + range end + mask */
    json_builder_set_member_name ( b, "bank_id_end" );
    json_builder_add_int_value ( b, bpt->bank_id_end );

    json_builder_set_member_name ( b, "bank_match_mode" );
    json_builder_add_string_value ( b, bp_match_mode_to_string ( bpt->bank_match_mode ) );

    json_builder_set_member_name ( b, "bank_id_mask" );
    json_builder_add_int_value ( b, bpt->bank_id_mask );

    /* Feature D: interpretace addr pro MMEXT_BANK (cpu_view / bank_offset). */
    json_builder_set_member_name ( b, "bp_addr_space" );
    json_builder_add_string_value ( b, bp_addr_space_to_string ( bpt->bp_addr_space ) );

    json_builder_set_member_name ( b, "port" );
    json_builder_add_int_value ( b, bpt->port );

    /* V1.5.E - port match mode + range end + mask */
    json_builder_set_member_name ( b, "port_end" );
    json_builder_add_int_value ( b, bpt->port_end );

    json_builder_set_member_name ( b, "port_match_mode" );
    json_builder_add_string_value ( b, bp_match_mode_to_string ( bpt->port_match_mode ) );

    json_builder_set_member_name ( b, "port_mask" );
    json_builder_add_int_value ( b, bpt->port_mask );

    /* V1.5.A7 - IORQ port addressing šířka (8BIT/16BIT). */
    json_builder_set_member_name ( b, "port_mode" );
    json_builder_add_string_value ( b, bp_port_mode_to_string ( bpt->port_mode ) );

    json_builder_set_member_name ( b, "event_name" );
    if ( bpt->event_name ) {
        json_builder_add_string_value ( b, bpt->event_name );
    } else {
        json_builder_add_null_value ( b );
    };

    /* V1.5 HWE - trigger condition (low/high/rising/falling/changed). */
    json_builder_set_member_name ( b, "event_trigger" );
    json_builder_add_string_value ( b, bp_event_trigger_to_string ( bpt->event_trigger ) );

    json_builder_set_member_name ( b, "sp_threshold" );
    json_builder_add_int_value ( b, bpt->sp_threshold );

    /* V1.5.E - SP mode + upper bound (WINDOW) */
    json_builder_set_member_name ( b, "sp_upper" );
    json_builder_add_int_value ( b, bpt->sp_upper );

    json_builder_set_member_name ( b, "sp_mode" );
    json_builder_add_string_value ( b, bp_sp_mode_to_string ( bpt->sp_mode ) );

    /* V1.5.A8.5 - IRQ IM mode discriminator (BPT_TYPE_IRQ). */
    json_builder_set_member_name ( b, "im0_enabled" );
    json_builder_add_boolean_value ( b, bpt->im0_enabled );

    json_builder_set_member_name ( b, "im1_enabled" );
    json_builder_add_boolean_value ( b, bpt->im1_enabled );

    json_builder_set_member_name ( b, "im2_enabled" );
    json_builder_add_boolean_value ( b, bpt->im2_enabled );

    json_builder_set_member_name ( b, "im0_rst_mask" );
    json_builder_add_int_value ( b, bpt->im0_rst_mask );

    /* V1.5.A8 - IRQ vector + ISR filter (BPT_TYPE_IRQ, IM 2 sub-filter).
     * V1.6+ TODO 4.4: pridany match_mode / addr_end / mask pro RANGE/MASK
     * (BC: missing klice -> SINGLE / 0 / 0xFFFF). */
    json_builder_set_member_name ( b, "im2_vector_enabled" );
    json_builder_add_boolean_value ( b, bpt->im2_vector_enabled );

    json_builder_set_member_name ( b, "im2_vector_addr" );
    json_builder_add_int_value ( b, bpt->im2_vector_addr );

    json_builder_set_member_name ( b, "im2_vector_match_mode" );
    json_builder_add_string_value ( b, bp_match_mode_to_string ( bpt->im2_vector_match_mode ) );

    json_builder_set_member_name ( b, "im2_vector_addr_end" );
    json_builder_add_int_value ( b, bpt->im2_vector_addr_end );

    json_builder_set_member_name ( b, "im2_vector_mask" );
    json_builder_add_int_value ( b, bpt->im2_vector_mask );

    json_builder_set_member_name ( b, "im2_isr_enabled" );
    json_builder_add_boolean_value ( b, bpt->im2_isr_enabled );

    json_builder_set_member_name ( b, "im2_isr_addr" );
    json_builder_add_int_value ( b, bpt->im2_isr_addr );

    json_builder_set_member_name ( b, "im2_isr_match_mode" );
    json_builder_add_string_value ( b, bp_match_mode_to_string ( bpt->im2_isr_match_mode ) );

    json_builder_set_member_name ( b, "im2_isr_addr_end" );
    json_builder_add_int_value ( b, bpt->im2_isr_addr_end );

    json_builder_set_member_name ( b, "im2_isr_mask" );
    json_builder_add_int_value ( b, bpt->im2_isr_mask );

    /* V1.5.A8.5 - IRQ_SIG source mask (BPT_TYPE_IRQ_SIG). Persistujeme
     * jako JSON array stable string names (= odolnější vůči případnému
     * reorderu enum bitů; bity samy zůstávají stable, ale string array
     * je čitelnější v .bpt souboru). */
    json_builder_set_member_name ( b, "irq_sig_sources" );
    json_builder_begin_array ( b );
    {
        unsigned k;
        const en_BP_IRQ_SIG_SOURCE all_bits[] = {
            BP_IRQ_SIG_PIOZ80_PORT_A,
            BP_IRQ_SIG_PIOZ80_PORT_B,
            BP_IRQ_SIG_CTC2,
            BP_IRQ_SIG_FDC,
            BP_IRQ_SIG_OTHER,
        };
        for ( k = 0; k < G_N_ELEMENTS ( all_bits ); k++ ) {
            if ( bpt->irq_sig_source_mask & all_bits[k] ) {
                const char *nm = bp_irq_sig_source_to_string ( all_bits[k] );
                if ( nm ) json_builder_add_string_value ( b, nm );
            };
        };
    };
    json_builder_end_array ( b );

    json_builder_set_member_name ( b, "expr" );
    if ( bpt->expr ) {
        json_builder_add_string_value ( b, bpt->expr );
    } else {
        json_builder_add_null_value ( b );
    };

    json_builder_set_member_name ( b, "action" );
    if ( bpt->action ) {
        json_builder_add_string_value ( b, bpt->action );
    } else {
        json_builder_add_null_value ( b );
    };

    json_builder_set_member_name ( b, "hit_count" );
    json_builder_add_int_value ( b, (gint64) bpt->hit_count );

    json_builder_set_member_name ( b, "skip_count" );
    json_builder_add_int_value ( b, (gint64) bpt->skip_count );

    json_builder_set_member_name ( b, "edge_triggered" );
    json_builder_add_boolean_value ( b, bpt->edge_triggered );

    json_builder_set_member_name ( b, "auto_name" );
    json_builder_add_boolean_value ( b, bpt->auto_name );

    json_builder_set_member_name ( b, "name" );
    json_builder_add_string_value ( b, bpt->name ? bpt->name : "" );

    json_builder_set_member_name ( b, "enabled" );
    json_builder_add_boolean_value ( b, bpt->enabled );

    json_builder_set_member_name ( b, "color_bg" );
    json_builder_add_int_value ( b, (gint64) bpt->bg_rgb );

    json_builder_set_member_name ( b, "color_fg" );
    json_builder_add_int_value ( b, (gint64) bpt->fg_rgb );

    json_builder_set_member_name ( b, "hits" );
    json_builder_add_int_value ( b, (gint64) bpt->hits );

    /* V1.D.2.C - persist cmd_origin pro audit trail. Stable token
     * "user"/"mcp"/"test"/"internal"; load tolerantní k missing klíči
     * (default USER) a unknown tokenům (= forward compat). */
    json_builder_set_member_name ( b, "origin" );
    json_builder_add_string_value ( b, _bp_origin_to_str ( bpt->cmd_origin ) );

    /* 0019 vrstva 2 - per-BP rate-limit override (těžké FWD akce). Load
     * tolerantní k missing klíči (default 0 = global/built-in default,
     * resp. 0 = neomezeno) = BC se staršími .bpt soubory. Runtime stav
     * (fwd_last_fire_us, fwd_fire_count) se NEpersistuje (resetuje se při
     * enable / init). */
    json_builder_set_member_name ( b, "fwd_min_interval_ms" );
    json_builder_add_int_value ( b, (gint64) bpt->fwd_min_interval_ms );

    json_builder_set_member_name ( b, "fwd_max_fires" );
    json_builder_add_int_value ( b, (gint64) bpt->fwd_max_fires );

    json_builder_end_object ( b );
}


void breakpoints_save_to_filepath ( const char *filepath ) {
    if ( !filepath || !filepath[0] ) return;

    printf ( "BREAKPOINTS: save_to_filepath('%s')\n", filepath );

    JsonBuilder *b = json_builder_new ( );
    json_builder_begin_object ( b );

    /* Schema version tag (V1.6+ TODO 4.7).
     *
     * Řetězcová verze schématu na začátku JSON root. Liší se od číselného
     * "version" - schema_version je readable label pro forward compatibility
     * a diagnostiku (např. "V1.5", "V1.6", "V2.0-pre"). Číselný "version"
     * je zachován pro BC s loadery V1.0 - V1.5 pre-4.7. */
    json_builder_set_member_name ( b, "schema_version" );
    json_builder_add_string_value ( b, BREAKPOINTS_SCHEMA_VERSION_CURRENT );

    json_builder_set_member_name ( b, "version" );
    json_builder_add_int_value ( b, BREAKPOINTS_JSON_FORMAT_VERSION );

    /* Skupiny */
    json_builder_set_member_name ( b, "groups" );
    json_builder_begin_array ( b );
    {
        unsigned i;
        for ( i = 0; i < g_breakpoints.groups->len; i++ ) {
            const st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, i );
            breakpoints_json_emit_group ( b, grp );
        };
    }
    json_builder_end_array ( b );

    /* Breakpointy */
    json_builder_set_member_name ( b, "breakpoints" );
    json_builder_begin_array ( b );
    {
        unsigned i;
        for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
            const st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
            breakpoints_json_emit_bpt ( b, bpt );
        };
    }
    json_builder_end_array ( b );

    /* User vars (D.1.3, V1.5.B comment + persist_value).
     *
     * Schéma per-záznam (V1.5.B):
     *   {"name": "...", "value": N, "comment": "...", "persist_value": bool}
     *
     * Pravidla:
     * - "comment" se emituje jen pokud je non-NULL a non-empty
     *   (= úspora místa, BC s V1 .bpt soubory bez comment).
     * - "persist_value" se emituje vždy (= explicitní flag, default true
     *   při missing v load).
     * - Pokud persist_value == false, "value" se neemituje (= storage
     *   per spec V1.5.B PLAN). Load takový záznam vytvoří s value=0.
     */
    json_builder_set_member_name ( b, "vars" );
    json_builder_begin_array ( b );
    {
        size_t n = bp_vars_count ( );
        size_t i;
        for ( i = 0; i < n; i++ ) {
            const bp_var_t *v = bp_vars_get_by_index ( i );
            if ( !v || !v->name ) continue;
            json_builder_begin_object ( b );
            json_builder_set_member_name ( b, "name" );
            json_builder_add_string_value ( b, v->name );
            if ( v->persist_value ) {
                json_builder_set_member_name ( b, "value" );
                json_builder_add_int_value ( b, (gint64) v->value );
            };
            if ( v->comment && v->comment[0] ) {
                json_builder_set_member_name ( b, "comment" );
                json_builder_add_string_value ( b, v->comment );
            };
            json_builder_set_member_name ( b, "persist_value" );
            json_builder_add_boolean_value ( b, v->persist_value );
            json_builder_end_object ( b );
        };
    }
    json_builder_end_array ( b );

    json_builder_end_object ( b );

    JsonGenerator *gen = json_generator_new ( );
    /* json_builder_get_root() vrací JsonNode vlastněný builderem - NEsmí
     * se volat json_node_free(), uvolní se s g_object_unref(builder).
     * Generator si interně bere referenci přes json_generator_set_root. */
    JsonNode *root = json_builder_get_root ( b );
    json_generator_set_root ( gen, root );
    json_generator_set_pretty ( gen, TRUE );
    json_generator_set_indent ( gen, 2 );

    /* Místo json_generator_to_file() (na Windows má některé verze problém
     * s UTF-8 cestami) generujeme do paměti a zapisujeme binárně. */
    gsize len = 0;
    gchar *out = json_generator_to_data ( gen, &len );
    FILE *fp = g_fopen ( filepath, "wb" );
    if ( !fp ) {
        fprintf ( stderr, "BREAKPOINTS: cannot open '%s' for writing\n", filepath );
    } else {
        size_t wrote = fwrite ( out, 1, len, fp );
        if ( wrote != len ) {
            fprintf ( stderr, "BREAKPOINTS: short write to '%s' (%zu of %zu)\n",
                      filepath, wrote, (size_t) len );
        };
        fclose ( fp );
    };
    g_free ( out );
    g_object_unref ( gen );
    g_object_unref ( b );
}


void breakpoints_save_to_file ( void ) {
    /* default_file je z cfg (default "breakpoints.bpt", JSON formát od
     * fáze D.1) - relativní cesty resolveme proti cfg_dir, absolutní
     * pass through. */
    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, g_breakpoints.default_file );
    breakpoints_save_to_filepath ( resolved );
    g_free ( resolved );
}


/*
 * Načte string member z JsonObject; pokud chybí nebo je null, vrátí default_val.
 * Vrácený pointer odkazuje do parserovy paměti (= valid jen po dobu života parser).
 */
static const char* breakpoints_json_get_string_or ( JsonObject *obj, const char *key, const char *default_val ) {
    if ( !json_object_has_member ( obj, key ) ) return default_val;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return default_val;
    if ( json_node_get_node_type ( node ) != JSON_NODE_VALUE ) return default_val;
    return json_node_get_string ( node );
}


/*
 * Načte integer member z JsonObject; pokud chybí, vrátí default_val.
 */
static gint64 breakpoints_json_get_int_or ( JsonObject *obj, const char *key, gint64 default_val ) {
    if ( !json_object_has_member ( obj, key ) ) return default_val;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return default_val;
    return json_object_get_int_member ( obj, key );
}


/*
 * Načte boolean member z JsonObject; pokud chybí, vrátí default_val.
 */
static gboolean breakpoints_json_get_bool_or ( JsonObject *obj, const char *key, gboolean default_val ) {
    if ( !json_object_has_member ( obj, key ) ) return default_val;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return default_val;
    return json_object_get_boolean_member ( obj, key );
}


/*
 * Načte double member z JsonObject; pokud chybí, vrátí default_val.
 */
static double breakpoints_json_get_double_or ( JsonObject *obj, const char *key, double default_val ) {
    if ( !json_object_has_member ( obj, key ) ) return default_val;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return default_val;
    return json_object_get_double_member ( obj, key );
}


/*
 * Parsuje JSON objekt skupiny a appenduje do g_breakpoints.groups.
 * Vrátí ID načtené skupiny (nebo -1 při chybě).
 */
static int breakpoints_json_load_group ( JsonObject *obj ) {
    st_BPTGROUP grp;
    memset ( &grp, 0x00, sizeof ( grp ) );

    grp.id = (int) breakpoints_json_get_int_or ( obj, "id", -1 );
    if ( grp.id < 0 ) {
        fprintf ( stderr, "BREAKPOINTS: group object missing valid 'id', skipping\n" );
        return -1;
    };
    grp.parent = (int) breakpoints_json_get_int_or ( obj, "parent_id", -1 );
    grp.enabled = breakpoints_json_get_bool_or ( obj, "enabled", TRUE ) ? true : false;
    grp.order = (float) breakpoints_json_get_double_or ( obj, "order", 0.0 );
    grp.bg_rgb = (uint32_t) breakpoints_json_get_int_or ( obj, "color_bg", BREAKPOINTS_DEFAULT_BG_RGB );
    grp.fg_rgb = (uint32_t) breakpoints_json_get_int_or ( obj, "color_fg", BREAKPOINTS_DEFAULT_FG_RGB );

    const char *name = breakpoints_json_get_string_or ( obj, "name", NULL );
    if ( !name || !name[0] || breakpoints_is_name_empty ( name ) ) {
        char auto_name[32];
        snprintf ( auto_name, sizeof ( auto_name ), "Group %d", grp.id );
        grp.name = g_strdup ( auto_name );
    } else {
        grp.name = g_strdup ( name );
    };

    g_array_append_val ( g_breakpoints.groups, grp );
    return grp.id;
}


/*
 * Parsuje JSON objekt breakpointu a appenduje do g_breakpoints.breakpoints.
 * Vrátí ID načteného BP (nebo -1 při chybě).
 */
static int breakpoints_json_load_bpt ( JsonObject *obj ) {
    st_BPT bpt;
    memset ( &bpt, 0x00, sizeof ( bpt ) );

    bpt.id = (int) breakpoints_json_get_int_or ( obj, "id", -1 );
    if ( bpt.id < 0 ) {
        fprintf ( stderr, "BREAKPOINTS: breakpoint object missing valid 'id', skipping\n" );
        return -1;
    };
    bpt.parent = (int) breakpoints_json_get_int_or ( obj, "parent_id", -1 );
    bpt.enabled = breakpoints_json_get_bool_or ( obj, "enabled", TRUE ) ? true : false;
    bpt.auto_name = breakpoints_json_get_bool_or ( obj, "auto_name", TRUE ) ? true : false;
    bpt.addr = (uint16_t) breakpoints_json_get_int_or ( obj, "addr", 0 );
    bpt.addr_end = (uint16_t) breakpoints_json_get_int_or ( obj, "addr_end", bpt.addr );
    bpt.bg_rgb = (uint32_t) breakpoints_json_get_int_or ( obj, "color_bg", BREAKPOINTS_DEFAULT_BG_RGB );
    bpt.fg_rgb = (uint32_t) breakpoints_json_get_int_or ( obj, "color_fg", BREAKPOINTS_DEFAULT_FG_RGB );
    bpt.hits = (uint64_t) breakpoints_json_get_int_or ( obj, "hits", 0 );

    /* Smart BP fields - default PC_EXEC + CPU_VIEW při chybějících klíčích */
    const char *type_str = breakpoints_json_get_string_or ( obj, "type", "PC_EXEC" );
    if ( !bpt_type_from_string ( type_str, &bpt.type ) ) {
        fprintf ( stderr, "BREAKPOINTS: unknown type '%s' for id=%d, falling back to PC_EXEC\n",
                  type_str, bpt.id );
        bpt.type = BPT_TYPE_PC_EXEC;
    };
    const char *zone_str = breakpoints_json_get_string_or ( obj, "zone", "CPU_VIEW" );
    if ( !bp_zone_from_string ( zone_str, &bpt.zone ) ) {
        fprintf ( stderr, "BREAKPOINTS: unknown zone '%s' for id=%d, falling back to CPU_VIEW\n",
                  zone_str, bpt.id );
        bpt.zone = BP_ZONE_CPU_VIEW;
    };
    bpt.bank_id = (uint8_t) breakpoints_json_get_int_or ( obj, "bank_id", 0 );
    bpt.port = (uint16_t) breakpoints_json_get_int_or ( obj, "port", 0 );
    bpt.sp_threshold = (uint16_t) breakpoints_json_get_int_or ( obj, "sp_threshold", 0 );
    bpt.hit_count = (uint32_t) breakpoints_json_get_int_or ( obj, "hit_count", 0 );
    bpt.skip_count = (uint32_t) breakpoints_json_get_int_or ( obj, "skip_count", 0 );
    bpt.edge_triggered = breakpoints_json_get_bool_or ( obj, "edge_triggered", FALSE ) ? true : false;

    /* 0019 vrstva 2 - per-BP rate-limit override. Chybí-li klíč (starší .bpt),
     * default 0 = global/built-in default (min_interval) resp. neomezeno
     * (max_fires). */
    bpt.fwd_min_interval_ms = (uint32_t) breakpoints_json_get_int_or ( obj, "fwd_min_interval_ms", 0 );
    bpt.fwd_max_fires = (uint32_t) breakpoints_json_get_int_or ( obj, "fwd_max_fires", 0 );

    /* V1.5.E match mode fields - BC fallback pro V1 .bpt soubory.
     * Chybí-li klíč, použije se SINGLE + max-mask (= V1 sémantika). */
    bpt.addr_match_mode = BP_MATCH_SINGLE;
    {
        const char *amm_str = breakpoints_json_get_string_or ( obj, "addr_match_mode", NULL );
        if ( amm_str ) {
            en_BP_MATCH_MODE m;
            if ( bp_match_mode_from_string ( amm_str, &m ) ) {
                bpt.addr_match_mode = m;
            } else {
                fprintf ( stderr, "BREAKPOINTS: unknown addr_match_mode '%s' for id=%d, using SINGLE\n",
                          amm_str, bpt.id );
            };
        };
    };
    bpt.addr_mask = (uint16_t) breakpoints_json_get_int_or ( obj, "addr_mask", 0xFFFF );

    bpt.port_match_mode = BP_MATCH_SINGLE;
    {
        const char *pmm_str = breakpoints_json_get_string_or ( obj, "port_match_mode", NULL );
        if ( pmm_str ) {
            en_BP_MATCH_MODE m;
            if ( bp_match_mode_from_string ( pmm_str, &m ) ) {
                bpt.port_match_mode = m;
            } else {
                fprintf ( stderr, "BREAKPOINTS: unknown port_match_mode '%s' for id=%d, using SINGLE\n",
                          pmm_str, bpt.id );
            };
        };
    };
    bpt.port_end = (uint16_t) breakpoints_json_get_int_or ( obj, "port_end", 0 );
    bpt.port_mask = (uint16_t) breakpoints_json_get_int_or ( obj, "port_mask", 0xFFFF );

    /* V1.5.A7 - port_mode (8BIT/16BIT) s BC fallback na 8BIT pro V1
     * .bpt soubory bez tohoto klíče (= legacy chování zachováno). */
    bpt.port_mode = BP_PORT_8BIT;
    {
        const char *pm_str = breakpoints_json_get_string_or ( obj, "port_mode", NULL );
        if ( pm_str ) {
            en_BP_PORT_MODE m;
            if ( bp_port_mode_from_string ( pm_str, &m ) ) {
                bpt.port_mode = m;
            } else {
                fprintf ( stderr, "BREAKPOINTS: unknown port_mode '%s' for id=%d, using 8BIT\n",
                          pm_str, bpt.id );
            };
        };
    };

    bpt.bp_addr_space = BP_ADDR_SPACE_CPU_VIEW;
    bpt.bank_match_mode = BP_MATCH_SINGLE;
    {
        const char *bmm_str = breakpoints_json_get_string_or ( obj, "bank_match_mode", NULL );
        if ( bmm_str ) {
            en_BP_MATCH_MODE m;
            if ( bp_match_mode_from_string ( bmm_str, &m ) ) {
                bpt.bank_match_mode = m;
            } else {
                fprintf ( stderr, "BREAKPOINTS: unknown bank_match_mode '%s' for id=%d, using SINGLE\n",
                          bmm_str, bpt.id );
            };
        };
    };
    bpt.bank_id_end = (uint8_t) breakpoints_json_get_int_or ( obj, "bank_id_end", 0 );
    bpt.bank_id_mask = (uint8_t) breakpoints_json_get_int_or ( obj, "bank_id_mask", 0xFF );
    /* Feature D: bp_addr_space (default CPU_VIEW pro starší .bpt bez pole). */
    {
        const char *as_str = breakpoints_json_get_string_or ( obj, "bp_addr_space", NULL );
        en_BP_ADDR_SPACE as;
        if ( as_str && bp_addr_space_from_string ( as_str, &as ) ) {
            bpt.bp_addr_space = as;
        };
    }

    bpt.sp_mode = BP_SP_SINGLE;
    {
        const char *spm_str = breakpoints_json_get_string_or ( obj, "sp_mode", NULL );
        if ( spm_str ) {
            en_BP_SP_MODE m;
            if ( bp_sp_mode_from_string ( spm_str, &m ) ) {
                bpt.sp_mode = m;
            } else {
                fprintf ( stderr, "BREAKPOINTS: unknown sp_mode '%s' for id=%d, using SINGLE\n",
                          spm_str, bpt.id );
            };
        };
    };
    bpt.sp_upper = (uint16_t) breakpoints_json_get_int_or ( obj, "sp_upper", 0 );

    /* V1.5.A8 - IRQ vector / ISR filter s BC fallback na disabled / 0
     * pro V1 / V1.5.E .bpt soubory bez těchto klíčů (= legacy chování).
     *
     * V1.6+ TODO 4.4: nove fieldy match_mode / addr_end / mask. BC pro
     * pre-4.4 .bpt soubory: missing -> SINGLE / 0 / 0xFFFF (= zachova
     * V1.5 SINGLE-only chovani). */
    bpt.im2_vector_enabled = breakpoints_json_get_bool_or ( obj, "im2_vector_enabled", FALSE ) ? true : false;
    bpt.im2_vector_addr = (uint16_t) breakpoints_json_get_int_or ( obj, "im2_vector_addr", 0 );
    bpt.im2_vector_match_mode = BP_MATCH_SINGLE;
    {
        const char *vmm_str = breakpoints_json_get_string_or ( obj, "im2_vector_match_mode", NULL );
        if ( vmm_str ) {
            en_BP_MATCH_MODE m;
            if ( bp_match_mode_from_string ( vmm_str, &m ) ) {
                bpt.im2_vector_match_mode = m;
            } else {
                fprintf ( stderr, "BREAKPOINTS: unknown im2_vector_match_mode '%s' for id=%d, using SINGLE\n",
                          vmm_str, bpt.id );
            };
        };
    };
    bpt.im2_vector_addr_end = (uint16_t) breakpoints_json_get_int_or ( obj, "im2_vector_addr_end", 0 );
    bpt.im2_vector_mask = (uint16_t) breakpoints_json_get_int_or ( obj, "im2_vector_mask", 0xFFFF );

    bpt.im2_isr_enabled = breakpoints_json_get_bool_or ( obj, "im2_isr_enabled", FALSE ) ? true : false;
    bpt.im2_isr_addr = (uint16_t) breakpoints_json_get_int_or ( obj, "im2_isr_addr", 0 );
    bpt.im2_isr_match_mode = BP_MATCH_SINGLE;
    {
        const char *imm_str = breakpoints_json_get_string_or ( obj, "im2_isr_match_mode", NULL );
        if ( imm_str ) {
            en_BP_MATCH_MODE m;
            if ( bp_match_mode_from_string ( imm_str, &m ) ) {
                bpt.im2_isr_match_mode = m;
            } else {
                fprintf ( stderr, "BREAKPOINTS: unknown im2_isr_match_mode '%s' for id=%d, using SINGLE\n",
                          imm_str, bpt.id );
            };
        };
    };
    bpt.im2_isr_addr_end = (uint16_t) breakpoints_json_get_int_or ( obj, "im2_isr_addr_end", 0 );
    bpt.im2_isr_mask = (uint16_t) breakpoints_json_get_int_or ( obj, "im2_isr_mask", 0xFFFF );

    /* V1.5.A8.5 - IRQ IM mode discriminator s BC fallback all-true (=
     * V1 / V1.5.E / A7 / A8 IRQ BP loadne v legacy fire-on-every IRQ módu). */
    bpt.im0_enabled = breakpoints_json_get_bool_or ( obj, "im0_enabled", TRUE ) ? true : false;
    bpt.im1_enabled = breakpoints_json_get_bool_or ( obj, "im1_enabled", TRUE ) ? true : false;
    bpt.im2_enabled = breakpoints_json_get_bool_or ( obj, "im2_enabled", TRUE ) ? true : false;
    bpt.im0_rst_mask = (uint8_t) breakpoints_json_get_int_or ( obj, "im0_rst_mask", 0 );

    /* V1.5.A8.5 - IRQ_SIG source mask s BC fallback 0 (= invalid; nový
     * BPT_TYPE_IRQ_SIG bez 'irq_sig_sources' klíče se neaktivuje a UI
     * validation chytne missing source mask). */
    bpt.irq_sig_source_mask = 0;
    if ( json_object_has_member ( obj, "irq_sig_sources" ) ) {
        JsonNode *sn = json_object_get_member ( obj, "irq_sig_sources" );
        if ( sn && json_node_get_node_type ( sn ) == JSON_NODE_ARRAY ) {
            JsonArray *sarr = json_node_get_array ( sn );
            guint sn_len = json_array_get_length ( sarr );
            guint k;
            for ( k = 0; k < sn_len; k++ ) {
                JsonNode *en = json_array_get_element ( sarr, k );
                if ( !en || json_node_get_node_type ( en ) != JSON_NODE_VALUE ) continue;
                const gchar *src_str = json_node_get_string ( en );
                if ( !src_str ) continue;
                en_BP_IRQ_SIG_SOURCE src;
                if ( bp_irq_sig_source_from_string ( src_str, &src ) ) {
                    bpt.irq_sig_source_mask |= (uint8_t) src;
                } else {
                    fprintf ( stderr, "BREAKPOINTS: unknown irq_sig_source '%s' for id=%d, ignoring\n",
                              src_str, bpt.id );
                };
            };
        };
    };

    const char *event_name = breakpoints_json_get_string_or ( obj, "event_name", NULL );
    bpt.event_name = event_name ? g_strdup ( event_name ) : NULL;
    /* D.3 - parse event_name do cache. Persist neukládá parsed_event /
     * event_param samostatně (= odvozeno z event_name); ale po loadu
     * cache musí být v souladu, jinak HW_EVENT BP nikdy nezafire. */
    bpt.parsed_event = BP_EVENT_NONE;
    bpt.event_param = 0;
    if ( bpt.event_name && bpt.event_name[0] != '\0' ) {
        en_BP_EVENT ev = BP_EVENT_NONE;
        int32_t pp = 0;
        if ( bp_event_from_string ( bpt.event_name, &ev, &pp ) ) {
            bpt.parsed_event = ev;
            bpt.event_param = pp;
        } else {
            fprintf ( stderr, "BREAKPOINTS: unknown event_name in loaded BP id=%d: '%s'\n",
                      bpt.id, bpt.event_name );
        };
    };

    /* V1.5 HWE - trigger condition s BC fallback RISING (= legacy
     * "fire on event happened" pre-HWE chování). */
    bpt.event_trigger = BP_EVT_TRIG_RISING;
    {
        const char *trig_str = breakpoints_json_get_string_or ( obj, "event_trigger", NULL );
        if ( trig_str ) {
            en_BP_EVENT_TRIGGER t;
            if ( bp_event_trigger_from_string ( trig_str, &t ) ) {
                bpt.event_trigger = t;
            } else {
                fprintf ( stderr, "BREAKPOINTS: unknown event_trigger '%s' for id=%d, using rising\n",
                          trig_str, bpt.id );
            };
        };
    };

    const char *expr = breakpoints_json_get_string_or ( obj, "expr", NULL );
    bpt.expr = expr ? g_strdup ( expr ) : NULL;
    /* Parse cached AST (D.1.2). Selhání je tolerováno - eval helper
     * pak fire-uje conservatively. */
    bpt.parsed_expr = NULL;
    if ( bpt.expr && bpt.expr[0] != '\0' ) {
        char errbuf[160];
        bpt.parsed_expr = bp_expr_parse ( bpt.expr, errbuf, sizeof ( errbuf ) );
        if ( !bpt.parsed_expr ) {
            fprintf ( stderr, "BREAKPOINTS: invalid expr in loaded BP id=%d: %s\n",
                      bpt.id, errbuf );
        };
    };

    const char *action = breakpoints_json_get_string_or ( obj, "action", NULL );
    bpt.action = action ? g_strdup ( action ) : NULL;
    /* Parse cached action AST (D.1.3). Prázdná action nebo selhání =
     * NULL (= stop sémantika v breakpoints_run_action). */
    bpt.parsed_action = NULL;
    if ( bpt.action && bpt.action[0] != '\0' ) {
        char act_err[160];
        bpt.parsed_action = bp_action_parse ( bpt.action, act_err, sizeof ( act_err ) );
        if ( !bpt.parsed_action && act_err[0] != '\0' ) {
            fprintf ( stderr, "BREAKPOINTS: invalid action in loaded BP id=%d: %s\n",
                      bpt.id, act_err );
        };
    };

    const char *name = breakpoints_json_get_string_or ( obj, "name", NULL );
    if ( !name || !name[0] || breakpoints_is_name_empty ( name ) ) {
        char auto_name[20];
        snprintf ( auto_name, sizeof ( auto_name ), "Addr: 0x%04X", bpt.addr );
        bpt.name = g_strdup ( auto_name );
    } else {
        bpt.name = g_strdup ( name );
    };

    /* V1.D.2.C - load cmd_origin tolerantně (= chybějící klíč defaultuje
     * na USER, neznámé tokeny taky na USER). */
    const char *origin_str = breakpoints_json_get_string_or ( obj, "origin", NULL );
    bpt.cmd_origin = _bp_origin_from_str ( origin_str );

    g_array_append_val ( g_breakpoints.breakpoints, bpt );
    return bpt.id;
}


void breakpoints_load_from_filepath ( const char *filepath ) {
    if ( !filepath || !filepath[0] ) return;

    printf ( "BREAKPOINTS: load_from_filepath('%s')\n", filepath );

    /* Vyčistit aktuální data */
    breakpoints_clear_all ( );

    JsonParser *parser = json_parser_new ( );
    GError *err = NULL;
    if ( !json_parser_load_from_file ( parser, filepath, &err ) ) {
        /* Soubor neexistuje nebo nelze přečíst - tichá no-op (default
         * prázdný stav). */
        if ( err ) {
            /* Logovat až na úrovni "diag" - pro neexistující soubor je
             * to běžný stav (= první spuštění bez .bpt). */
            g_error_free ( err );
        };
        g_object_unref ( parser );
        /* Zajistit platný next_id i po prázdném loadu */
        if ( g_breakpoints.next_id < 1 ) g_breakpoints.next_id = 1;
        return;
    };

    JsonNode *root = json_parser_get_root ( parser );
    if ( !root || json_node_get_node_type ( root ) != JSON_NODE_OBJECT ) {
        fprintf ( stderr, "BREAKPOINTS: root JSON is not an object\n" );
        g_object_unref ( parser );
        if ( g_breakpoints.next_id < 1 ) g_breakpoints.next_id = 1;
        return;
    };

    JsonObject *root_obj = json_node_get_object ( root );

    /* Volitelná verze - V1 ji jen logujeme. */
    gint64 version = breakpoints_json_get_int_or ( root_obj, "version", -1 );
    if ( version >= 0 && version != BREAKPOINTS_JSON_FORMAT_VERSION ) {
        printf ( "BREAKPOINTS: file format version=%lld (current=%d), proceeding best-effort\n",
                 (long long) version, BREAKPOINTS_JSON_FORMAT_VERSION );
    };

    /* Schema version tag (V1.6+ TODO 4.7).
     *
     * Loader pravidla:
     * - Chybějící klíč → fallback "V1.0-pre-versioning", info log
     *   (= soubor zapsaný před zavedením versioning).
     * - Hodnota === current → silent (= běžný stav).
     * - Jiná hodnota → warning na stderr, ale parse pokračuje
     *   (= forward-compat: V1.5 emu best-effort načte V1.6 file).
     */
    {
        const char *schema_ver = breakpoints_json_get_string_or ( root_obj, "schema_version", NULL );
        if ( !schema_ver ) {
            printf ( "BREAKPOINTS: schema_version missing - assuming '%s' (BC fallback)\n",
                     BREAKPOINTS_SCHEMA_VERSION_FALLBACK );
        } else if ( strcmp ( schema_ver, BREAKPOINTS_SCHEMA_VERSION_CURRENT ) != 0 ) {
            fprintf ( stderr, "BREAKPOINTS WARNING: schema_version='%s' (current='%s') - "
                              "proceeding best-effort\n",
                      schema_ver, BREAKPOINTS_SCHEMA_VERSION_CURRENT );
        };
    };

    int max_id = -1;

    /* === Skupiny === */
    if ( json_object_has_member ( root_obj, "groups" ) ) {
        JsonNode *gn = json_object_get_member ( root_obj, "groups" );
        if ( gn && json_node_get_node_type ( gn ) == JSON_NODE_ARRAY ) {
            JsonArray *garr = json_node_get_array ( gn );
            guint n = json_array_get_length ( garr );
            guint i;
            for ( i = 0; i < n; i++ ) {
                JsonNode *en = json_array_get_element ( garr, i );
                if ( !en || json_node_get_node_type ( en ) != JSON_NODE_OBJECT ) continue;
                int gid = breakpoints_json_load_group ( json_node_get_object ( en ) );
                if ( gid > max_id ) max_id = gid;
            };
        };
    };

    /* === User vars (D.1.3, V1.5.B comment + persist_value) ===
     *
     * BC pravidla:
     * - Missing "comment" → NULL (V1 .bpt soubory).
     * - Missing "persist_value" → true (V1 .bpt + V1.5.B s běžnou var).
     * - persist_value=false + missing "value" → load value=0 (per spec).
     * - persist_value=false + present "value" → load value=0 + warning
     *   na stderr (= soubor je nekonzistentní, ale value se má resetovat).
     */
    bp_vars_clear_storage ( );
    if ( json_object_has_member ( root_obj, "vars" ) ) {
        JsonNode *vn = json_object_get_member ( root_obj, "vars" );
        if ( vn && json_node_get_node_type ( vn ) == JSON_NODE_ARRAY ) {
            JsonArray *varr = json_node_get_array ( vn );
            guint n = json_array_get_length ( varr );
            guint i;
            for ( i = 0; i < n; i++ ) {
                JsonNode *en = json_array_get_element ( varr, i );
                if ( !en || json_node_get_node_type ( en ) != JSON_NODE_OBJECT ) continue;
                JsonObject *vo = json_node_get_object ( en );
                const char *vname = breakpoints_json_get_string_or ( vo, "name", NULL );
                if ( !vname || !vname[0] ) continue;
                bool persist_value = breakpoints_json_get_bool_or ( vo, "persist_value", TRUE ) ? true : false;
                int32_t value = 0;
                if ( persist_value ) {
                    value = (int32_t) breakpoints_json_get_int_or ( vo, "value", 0 );
                } else if ( json_object_has_member ( vo, "value" ) ) {
                    fprintf ( stderr, "BREAKPOINTS WARNING: var '%s' has persist_value=false "
                                      "but 'value' key present - resetting to 0\n", vname );
                };
                const char *comment = breakpoints_json_get_string_or ( vo, "comment", NULL );
                bp_var_set_full ( vname, value, comment, persist_value );
            };
        };
    };

    /* === Breakpointy === */
    if ( json_object_has_member ( root_obj, "breakpoints" ) ) {
        JsonNode *bn = json_object_get_member ( root_obj, "breakpoints" );
        if ( bn && json_node_get_node_type ( bn ) == JSON_NODE_ARRAY ) {
            JsonArray *barr = json_node_get_array ( bn );
            guint n = json_array_get_length ( barr );
            guint i;
            for ( i = 0; i < n; i++ ) {
                JsonNode *en = json_array_get_element ( barr, i );
                if ( !en || json_node_get_node_type ( en ) != JSON_NODE_OBJECT ) continue;
                int bid = breakpoints_json_load_bpt ( json_node_get_object ( en ) );
                if ( bid > max_id ) max_id = bid;
            };
        };
    };

    g_object_unref ( parser );

    /* Nastavit next_id za nejvyšší použité ID (minimálně 1) */
    g_breakpoints.next_id = max_id + 1;
    if ( g_breakpoints.next_id < 1 ) g_breakpoints.next_id = 1;

    /* Validace načtených dat */
    breakpoints_validate_group_parents ( );
    breakpoints_validate_event_addresses ( );

    /* Synchronizovat bptmap s načtenými daty */
    breakpoints_sync_bptmap ( );
    g_breakpoints.version++;
}


void breakpoints_load_from_file ( void ) {
    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, g_breakpoints.default_file );
    breakpoints_load_from_filepath ( resolved );
    g_free ( resolved );
}


/* ========================================================================= */
/*  CRUD skupiny                                                             */
/* ========================================================================= */


int breakpoints_group_add ( const char *name, int parent ) {
    int new_id = breakpoints_alloc_id ( );
    if ( new_id < 0 ) return -1;

    st_BPTGROUP grp;
    memset ( &grp, 0x00, sizeof ( grp ) );

    grp.id = new_id;
    grp.parent = parent;
    grp.order = 0.0f;
    grp.enabled = true;
    grp.name = g_strdup ( name ? name : "" );
    grp.bg_rgb = BREAKPOINTS_DEFAULT_BG_RGB;
    grp.fg_rgb = BREAKPOINTS_DEFAULT_FG_RGB;

    g_array_append_val ( g_breakpoints.groups, grp );
    g_breakpoints.version++;
    return grp.id;
}


bool breakpoints_group_remove ( int group_id ) {
    int idx = breakpoints_find_group_index ( group_id );
    if ( idx < 0 ) return false;

    /* Přesunout potomky (skupiny) do root */
    unsigned i;
    for ( i = 0; i < g_breakpoints.groups->len; i++ ) {
        st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, i );
        if ( grp->parent == group_id ) {
            grp->parent = -1;
        };
    };

    /* Přesunout potomky (breakpointy) do root */
    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( bpt->parent == group_id ) {
            bpt->parent = -1;
        };
    };

    /* Uvolnit stringy a odstranit z pole */
    st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, (unsigned)idx );
    breakpoints_free_group_strings ( grp );
    g_array_remove_index ( g_breakpoints.groups, (unsigned)idx );

    /* Resynchronizovat bptmap — stav enabled skupin se mohl změnit */
    breakpoints_sync_bptmap ( );

    g_breakpoints.version++;
    return true;
}


st_BPTGROUP* breakpoints_group_find_by_id ( int group_id ) {
    int idx = breakpoints_find_group_index ( group_id );
    if ( idx < 0 ) return NULL;
    return &g_array_index ( g_breakpoints.groups, st_BPTGROUP, (unsigned)idx );
}


bool breakpoints_group_set_enabled ( int group_id, bool enabled ) {
    st_BPTGROUP *grp = breakpoints_group_find_by_id ( group_id );
    if ( !grp ) return false;
    grp->enabled = enabled;

    /* Resynchronizovat bptmap — stav enabled se projeví na všech BPT ve skupině */
    breakpoints_sync_bptmap ( );
    g_breakpoints.version++;
    return true;
}


bool breakpoints_group_set_name ( int group_id, const char *name ) {
    st_BPTGROUP *grp = breakpoints_group_find_by_id ( group_id );
    if ( !grp ) return false;
    g_free ( grp->name );
    grp->name = g_strdup ( name ? name : "" );
    g_breakpoints.version++;
    return true;
}


bool breakpoints_group_set_colors ( int group_id, uint32_t bg_rgb, uint32_t fg_rgb ) {
    st_BPTGROUP *grp = breakpoints_group_find_by_id ( group_id );
    if ( !grp ) return false;
    grp->bg_rgb = bg_rgb;
    grp->fg_rgb = fg_rgb;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_group_set_order ( int group_id, float order ) {
    st_BPTGROUP *grp = breakpoints_group_find_by_id ( group_id );
    if ( !grp ) return false;
    grp->order = order;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_group_set_parent ( int group_id, int new_parent ) {
    st_BPTGROUP *grp = breakpoints_group_find_by_id ( group_id );
    if ( !grp ) return false;

    /* Nelze nastavit sám sebe jako rodiče */
    if ( new_parent == group_id ) return false;

    /* Zkontrolovat, zda nový rodič existuje (pokud není root) */
    if ( new_parent >= 0 && breakpoints_find_group_index ( new_parent ) < 0 ) return false;

    /* Detekce cyklu — projít řetězec rodičů od new_parent nahoru.
     * Pokud narazíme na group_id, vznikl by cyklus. */
    if ( new_parent >= 0 ) {
        int current = new_parent;
        unsigned max_depth = g_breakpoints.groups->len;
        unsigned depth = 0;

        while ( current >= 0 && depth < max_depth ) {
            if ( current == group_id ) return false; /* cyklus! */
            int idx = breakpoints_find_group_index ( current );
            if ( idx < 0 ) break;
            st_BPTGROUP *parent_grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, (unsigned)idx );
            current = parent_grp->parent;
            depth++;
        };
    };

    grp->parent = new_parent;

    /* Resynchronizovat bptmap — stav enabled skupin se mohl změnit */
    breakpoints_sync_bptmap ( );
    g_breakpoints.version++;
    return true;
}


unsigned breakpoints_group_count ( void ) {
    return g_breakpoints.groups->len;
}


/* ========================================================================= */
/*  CRUD breakpointy                                                         */
/* ========================================================================= */


int breakpoints_add ( uint16_t addr, const char *name, int parent ) {
    int new_id = breakpoints_alloc_id ( );
    if ( new_id < 0 ) return -1;

    st_BPT bpt;
    memset ( &bpt, 0x00, sizeof ( bpt ) );

    bpt.id = new_id;
    bpt.parent = parent;
    bpt.enabled = true;
    bpt.auto_name = true;
    bpt.addr = addr;
    bpt.name = g_strdup ( name ? name : "" );
    bpt.bg_rgb = BREAKPOINTS_DEFAULT_BG_RGB;
    bpt.fg_rgb = BREAKPOINTS_DEFAULT_FG_RGB;
    bpt.hits = 0;

    /* Smart BP V1 defaults (= legacy PC execution na CPU view) */
    bpt.type = BPT_TYPE_PC_EXEC;
    bpt.addr_end = addr;          /* prázdný range = single address */
    bpt.zone = BP_ZONE_CPU_VIEW;
    /* bank_id, port, sp_threshold, hit_count, skip_count, edge_triggered = 0/false (memset) */
    /* event_name, expr, action = NULL (memset) */

    /* V1.5.E match mode defaults - SINGLE + max-mask zachová V1 sémantiku. */
    bpt.addr_match_mode = BP_MATCH_SINGLE;
    bpt.addr_mask = 0xFFFF;
    bpt.port_match_mode = BP_MATCH_SINGLE;
    bpt.port_end = 0;
    bpt.port_mask = 0xFFFF;
    bpt.bank_match_mode = BP_MATCH_SINGLE;
    bpt.bank_id_end = 0;
    bpt.bank_id_mask = 0xFF;
    bpt.bp_addr_space = BP_ADDR_SPACE_CPU_VIEW;
    bpt.sp_mode = BP_SP_SINGLE;
    bpt.sp_upper = 0;

    /* V1.5.A7 - IORQ port mode default 8BIT (= V1 chování). */
    bpt.port_mode = BP_PORT_8BIT;

    /* V1.5.A8 - IRQ vector / ISR filtry vypnuté (= legacy fire-every-IRQ). */
    bpt.im2_vector_enabled = false;
    bpt.im2_vector_addr = 0;
    bpt.im2_isr_enabled = false;
    bpt.im2_isr_addr = 0;

    /* V1.6+ TODO 4.4: IM2 vector / ISR Match modes default SINGLE +
     * max-mask zachová V1.5 sémantiku (= exact match). */
    bpt.im2_vector_match_mode = BP_MATCH_SINGLE;
    bpt.im2_vector_addr_end = 0;
    bpt.im2_vector_mask = 0xFFFF;
    bpt.im2_isr_match_mode = BP_MATCH_SINGLE;
    bpt.im2_isr_addr_end = 0;
    bpt.im2_isr_mask = 0xFFFF;

    /* V1.5.A8.5 - IM mode discriminator: all 3 enabled (= legacy
     * fire-on-every-IRQ-dispatch chování po A8). RST mask 0 = match-all. */
    bpt.im0_enabled = true;
    bpt.im1_enabled = true;
    bpt.im2_enabled = true;
    bpt.im0_rst_mask = 0;

    /* V1.5.A8.5 - IRQ_SIG source mask 0 (= invalid pro nový IRQ_SIG BP;
     * UI validation requires aspoň 1 source). */
    bpt.irq_sig_source_mask = 0;

    /* V1.5 HWE - default trigger RISING (= legacy "fire on event"). */
    bpt.event_trigger = BP_EVT_TRIG_RISING;

    /* Registrace v bptmap — pokud na adrese existuje jiný BPT, vrátí jeho ID (> 0) */
    if ( breakpoints_is_group_enabled ( bpt.parent ) ) {
        int conflict = bptmap_event_add ( addr, bpt.id );
        if ( conflict > 0 ) {
            /* Na adrese již existuje jiný breakpoint */
            g_free ( bpt.name );
            g_breakpoints.next_id--;
            return -1;
        };
    };

    g_array_append_val ( g_breakpoints.breakpoints, bpt );
    g_breakpoints.version++;
    return bpt.id;
}


int breakpoints_add_auto ( uint16_t addr, const char *name, int parent ) {
    int new_id = breakpoints_alloc_id ( );
    if ( new_id < 0 ) return -1;

    st_BPT bpt;
    memset ( &bpt, 0x00, sizeof ( bpt ) );

    bpt.id = new_id;
    bpt.parent = parent;
    bpt.enabled = true;
    bpt.auto_name = true;
    bpt.addr = addr;
    bpt.name = g_strdup ( name ? name : "" );
    bpt.bg_rgb = BREAKPOINTS_DEFAULT_BG_RGB;
    bpt.fg_rgb = BREAKPOINTS_DEFAULT_FG_RGB;
    bpt.hits = 0;

    /* Smart BP V1 defaults (viz breakpoints_add). */
    bpt.type = BPT_TYPE_PC_EXEC;
    bpt.addr_end = addr;
    bpt.zone = BP_ZONE_CPU_VIEW;

    /* V1.5.E match mode defaults - SINGLE + max-mask zachová V1 sémantiku. */
    bpt.addr_match_mode = BP_MATCH_SINGLE;
    bpt.addr_mask = 0xFFFF;
    bpt.port_match_mode = BP_MATCH_SINGLE;
    bpt.port_end = 0;
    bpt.port_mask = 0xFFFF;
    bpt.bank_match_mode = BP_MATCH_SINGLE;
    bpt.bank_id_end = 0;
    bpt.bank_id_mask = 0xFF;
    bpt.bp_addr_space = BP_ADDR_SPACE_CPU_VIEW;
    bpt.sp_mode = BP_SP_SINGLE;
    bpt.sp_upper = 0;

    /* V1.5.A7 - IORQ port mode default 8BIT (= V1 chování). */
    bpt.port_mode = BP_PORT_8BIT;

    /* V1.5.A8 - IRQ vector / ISR filtry vypnuté (= legacy fire-every-IRQ). */
    bpt.im2_vector_enabled = false;
    bpt.im2_vector_addr = 0;
    bpt.im2_isr_enabled = false;
    bpt.im2_isr_addr = 0;

    /* V1.6+ TODO 4.4: IM2 Match modes - default SINGLE + max-mask. */
    bpt.im2_vector_match_mode = BP_MATCH_SINGLE;
    bpt.im2_vector_addr_end = 0;
    bpt.im2_vector_mask = 0xFFFF;
    bpt.im2_isr_match_mode = BP_MATCH_SINGLE;
    bpt.im2_isr_addr_end = 0;
    bpt.im2_isr_mask = 0xFFFF;

    /* V1.5.A8.5 - viz breakpoints_add. */
    bpt.im0_enabled = true;
    bpt.im1_enabled = true;
    bpt.im2_enabled = true;
    bpt.im0_rst_mask = 0;
    bpt.irq_sig_source_mask = 0;

    /* V1.5 HWE - default trigger RISING (= legacy "fire on event"). */
    bpt.event_trigger = BP_EVT_TRIG_RISING;

    /* Registrace v bptmap — pokud na adrese existuje jiný BPT, auto-disable */
    if ( breakpoints_is_group_enabled ( bpt.parent ) ) {
        int conflict = bptmap_event_add ( addr, bpt.id );
        if ( conflict > 0 ) {
            /* Na adrese již existuje jiný breakpoint — přidáme, ale disabled */
            bpt.enabled = false;
        };
    };

    g_array_append_val ( g_breakpoints.breakpoints, bpt );
    g_breakpoints.version++;
    return bpt.id;
}


bool breakpoints_remove ( int bpt_id ) {
    int idx = breakpoints_find_bpt_index ( bpt_id );
    if ( idx < 0 ) return false;

    st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, (unsigned)idx );

    /* Odregistrovat z bptmap (legacy PC bpmap[] + per-typ listy V2). */
    bptmap_event_clear ( bpt->addr, bpt->id );
    bptmap_unregister_all ( bpt->id );

    /* Uvolnit stringy a odstranit z pole */
    breakpoints_free_bpt_strings ( bpt );
    g_array_remove_index ( g_breakpoints.breakpoints, (unsigned)idx );

    g_breakpoints.version++;
    return true;
}


st_BPT* breakpoints_find_by_id ( int bpt_id ) {
    int idx = breakpoints_find_bpt_index ( bpt_id );
    if ( idx < 0 ) return NULL;
    return &g_array_index ( g_breakpoints.breakpoints, st_BPT, (unsigned)idx );
}


st_BPT* breakpoints_find_by_addr ( uint16_t addr ) {
    unsigned i;
    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( bpt->addr == addr ) return bpt;
    };
    return NULL;
}


int breakpoints_find_all_by_addr ( uint16_t addr, GArray *out_ids ) {
    if ( !out_ids ) return 0;

    unsigned entry_len = out_ids->len;

    for ( unsigned i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );

        bool matches = false;
        switch ( bpt->type ) {
            case BPT_TYPE_PC_EXEC:
            case BPT_TYPE_MEM_R:
            case BPT_TYPE_MEM_W:
                /* SINGLE / RANGE / MASK přes sdílený bp_match16 helper.
                 * Pro SINGLE jen bpt->addr == addr; RANGE inclusive
                 * bpt->addr..addr_end; MASK (x & mask) == (addr & mask). */
                matches = bp_match16 ( bpt->addr_match_mode, addr,
                                        bpt->addr, bpt->addr_end,
                                        bpt->addr_mask );
                break;

            case BPT_TYPE_GLOBAL:
                /* GLOBAL nemá adresu - condition může referencovat cokoliv,
                 * proto se na hover libovolné addr zobrazuje (= relevantní
                 * pro debug context na řádku). */
                matches = true;
                break;

            case BPT_TYPE_IORQ_R:
            case BPT_TYPE_IORQ_W:
            case BPT_TYPE_IRQ:
            case BPT_TYPE_IRQ_SIG:
            case BPT_TYPE_HW_EVENT:
            case BPT_TYPE_SP_THRESHOLD:
            case BPT_TYPE_COUNT:
            default:
                /* Tyto typy se neváží na CPU adresu - nezahrnuto. */
                break;
        };

        if ( matches ) {
            g_array_append_val ( out_ids, bpt->id );
        };
    };

    return (int) ( out_ids->len - entry_len );
}


bool breakpoints_set_enabled ( int bpt_id, bool enabled ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;

    /* Pokud se stav nemění, nic nedělat (vyhnout se self-konfliktu v bptmap) */
    if ( bpt->enabled == enabled ) return true;

    /* Synchronizace s bptmap.
     * - PC_EXEC: legacy bpmap[] s O(1) lookup (a kolizní detekce per addr).
     * - non-PC (D.2): per-typ list, žádná addr kolize (více BP na stejné
     *   adrese je v pořádku - rozliší je condition).
     */
    int idx = breakpoints_bpt_type_to_idx ( bpt->type );

    if ( enabled && breakpoints_is_group_enabled ( bpt->parent ) ) {
        if ( bpt->type == BPT_TYPE_PC_EXEC ) {
            int conflict = bptmap_event_add ( bpt->addr, bpt->id );
            if ( conflict > 0 ) {
                /* Na adrese již existuje jiný povolený BPT — nelze povolit */
                printf ( "BREAKPOINTS WARNING: cannot enable event id=%d at address 0x%04x — conflict with id=%d\n",
                         bpt->id, bpt->addr, conflict );
                return false;
            };
        } else if ( idx >= 0 ) {
            bptmap_register ( bpt->id, (en_BPTMAP_TYPE_IDX) idx );
        };
    } else {
        if ( bpt->type == BPT_TYPE_PC_EXEC ) {
            bptmap_event_clear ( bpt->addr, bpt->id );
        } else if ( idx >= 0 ) {
            bptmap_unregister ( bpt->id, (en_BPTMAP_TYPE_IDX) idx );
        };
    };

    bpt->enabled = enabled;

    /* 0019 vrstva 2: při (re-)enable BP vynuluj runtime stav rate-limitu, aby
     * max_fires nezůstal trvale "vyčerpaný" napříč session (= disable_self po
     * dosažení stropu + pozdější ruční enable musí dát BP novou kvótu). */
    if ( enabled ) {
        bpt->fwd_last_fire_us = 0;
        bpt->fwd_fire_count   = 0;
    };

    /* D.3 - pokud je BP HW_EVENT, aktualizuj globální active bitmap. */
    if ( bpt->type == BPT_TYPE_HW_EVENT ) {
        breakpoints_recompute_event_active ( );
    };
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_name ( int bpt_id, const char *name ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    g_free ( bpt->name );
    bpt->name = g_strdup ( name ? name : "" );
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_auto_name ( int bpt_id, bool auto_name ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->auto_name = auto_name;

    /* Pokud se zapíná auto_name, ihned generovat jméno z adresy */
    if ( auto_name ) {
        char name[20];
        snprintf ( name, sizeof ( name ), "Addr: 0x%04X", bpt->addr );
        g_free ( bpt->name );
        bpt->name = g_strdup ( name );
    };

    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_colors ( int bpt_id, uint32_t bg_rgb, uint32_t fg_rgb ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->bg_rgb = bg_rgb;
    bpt->fg_rgb = fg_rgb;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_parent ( int bpt_id, int new_parent ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;

    /* Zkontrolovat, zda nový rodič existuje (pokud není root) */
    if ( new_parent >= 0 && breakpoints_find_group_index ( new_parent ) < 0 ) return false;

    bpt->parent = new_parent;

    /* Resynchronizovat bptmap — stav enabled skupin se mohl změnit */
    breakpoints_sync_bptmap ( );
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_addr ( int bpt_id, uint16_t new_addr ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;

    /* Pokud byl single-point BP (addr == addr_end), drž je v sync.
     * Pro range BP (addr_end > addr) ponechej addr_end nezměněné -
     * uživatel si range řídí explicitně. */
    bool single_point = ( bpt->addr_end == bpt->addr );
    bpt->addr = new_addr;
    if ( single_point ) {
        bpt->addr_end = new_addr;
    };

    /* Auto-update jména pokud je auto_name zapnuto */
    if ( bpt->auto_name ) {
        char auto_name_buf[20];
        snprintf ( auto_name_buf, sizeof ( auto_name_buf ), "Addr: 0x%04X", new_addr );
        g_free ( bpt->name );
        bpt->name = g_strdup ( auto_name_buf );
    };

    g_breakpoints.version++;

    /* Přeregistrace v bptmap podle AKTUÁLNÍHO typu BP. Dřív se tu ručně
     * volalo bptmap_event_add(new_addr) BEZ kontroly typu - to byl bug:
     * MEM_R/W (a jiné non-PC_EXEC) BP se tím dostaly do bpmap[] jako by byly
     * PC_EXEC, takže EXEC na jejich adrese je spustil přes enforce_pc_exec
     * (bpmap[] cesta). breakpoints_sync_bptmap() je type-aware: PC_EXEC ->
     * bpmap[], ostatní -> per-typ list. Tím se pollution odstraní. */
    breakpoints_sync_bptmap ( );
    return true;
}


void breakpoints_increment_hits ( int bpt_id ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return;
    bpt->hits++;
    g_breakpoints.version++;
}


void breakpoints_reset_hits ( int bpt_id ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return;
    bpt->hits = 0;
    g_breakpoints.version++;
}


unsigned breakpoints_count ( void ) {
    return g_breakpoints.breakpoints->len;
}


/* ========================================================================= */
/*  Smart BP setters (V1, fáze D.1)                                          */
/* ========================================================================= */


bool breakpoints_set_type ( int bpt_id, en_BPT_TYPE type ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( type < 0 || type >= BPT_TYPE_COUNT ) return false;

    /* Pokud se typ skutečně mění a BP je efektivně povolený, musíme
     * přesynchronizovat bptmap registrace - starý typ odregistrovat,
     * nový zaregistrovat. Pro robustnost přes všechny scénáře (= různé
     * indexy / addr kolize) použijeme full sync. */
    bool effective = bpt->enabled && breakpoints_is_group_enabled ( bpt->parent );
    bool needs_resync = effective && bpt->type != type;

    bpt->type = type;
    g_breakpoints.version++;

    if ( needs_resync ) breakpoints_sync_bptmap ( );

    return true;
}


bool breakpoints_set_zone ( int bpt_id, en_BP_ZONE zone ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( zone < 0 || zone >= BP_ZONE_COUNT ) return false;
    bpt->zone = zone;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_bank_id ( int bpt_id, uint8_t bank_id ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->bank_id = bank_id;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_addr_end ( int bpt_id, uint16_t addr_end ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->addr_end = addr_end;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_addr_range ( int bpt_id, uint16_t addr, uint16_t addr_end ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;

    /* addr propagujeme přes set_addr aby zachoval bptmap sync (= legacy
     * PC enforcement). Po něm doplníme addr_end (set_addr by ho srovnal
     * pokud byl single-point). */
    breakpoints_set_addr ( bpt_id, addr );
    bpt = breakpoints_find_by_id ( bpt_id ); /* defensive: GArray může re-allocovat */
    if ( !bpt ) return false;
    bpt->addr_end = addr_end;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_port ( int bpt_id, uint16_t port ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->port = port;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_event_name ( int bpt_id, const char *event_name ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    g_free ( bpt->event_name );
    bpt->event_name = event_name ? g_strdup ( event_name ) : NULL;

    /* D.3 cache - parse event_name do enum + parametru. Při invalid
     * stringu necháme parsed_event = NONE (= BP nikdy nezafire) a
     * zalogujeme warning. Tento BP zůstane visible v UI s původním
     * stringem, aby uživatel viděl, co zadal špatně. */
    bpt->parsed_event = BP_EVENT_NONE;
    bpt->event_param = 0;
    if ( bpt->event_name && bpt->event_name[0] != '\0' ) {
        en_BP_EVENT ev = BP_EVENT_NONE;
        int32_t pp = 0;
        if ( bp_event_from_string ( bpt->event_name, &ev, &pp ) ) {
            bpt->parsed_event = ev;
            bpt->event_param = pp;
        } else {
            fprintf ( stderr, "BREAKPOINTS WARN: unknown event_name for BP id=%d: '%s'\n",
                      bpt_id, bpt->event_name );
        };
    };

    /* Globální bitmap update - bezpodmínečně přepočítat (cheap, max 25 events). */
    breakpoints_recompute_event_active ( );

    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_event_trigger ( int bpt_id, en_BP_EVENT_TRIGGER trig ) {
    if ( trig < 0 || trig >= BP_EVT_TRIG_COUNT ) return false;
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->event_trigger = trig;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_sp_threshold ( int bpt_id, uint16_t threshold ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->sp_threshold = threshold;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_expr ( int bpt_id, const char *expr ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    g_free ( bpt->expr );
    /* Invalidace cache. */
    if ( bpt->parsed_expr ) {
        bp_expr_free ( bpt->parsed_expr );
        bpt->parsed_expr = NULL;
    };
    bpt->expr = expr ? g_strdup ( expr ) : NULL;

    /* Lazy parse není - parsujeme rovnou, ať máme rychlou eval cestu
     * a chybu detekujeme co nejdřív (= log při set). */
    if ( bpt->expr && bpt->expr[0] != '\0' ) {
        char errbuf[160];
        bpt->parsed_expr = bp_expr_parse ( bpt->expr, errbuf, sizeof ( errbuf ) );
        if ( !bpt->parsed_expr ) {
            fprintf ( stderr, "BREAKPOINTS WARN: invalid expr for BP id=%d: %s\n",
                      bpt_id, errbuf );
            /* Ponecháme bpt->expr (uložený text) pro UI editaci, ale
             * eval helper se v takovém případě chová jako "always true"
             * (= conservatively fire). */
        };
    };

    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_action ( int bpt_id, const char *action ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    g_free ( bpt->action );
    /* Invalidace cache. */
    if ( bpt->parsed_action ) {
        bp_action_free ( bpt->parsed_action );
        bpt->parsed_action = NULL;
    };
    bpt->action = action ? g_strdup ( action ) : NULL;

    /* Eager parse - chybu detekujeme co nejdřív. Prázdná akce
     * (NULL nebo whitespace-only) nechá parsed_action = NULL =
     * stop sémantika. */
    if ( bpt->action && bpt->action[0] != '\0' ) {
        char errbuf[160];
        bpt->parsed_action = bp_action_parse ( bpt->action, errbuf, sizeof ( errbuf ) );
        if ( !bpt->parsed_action ) {
            /* Parser vrátil NULL bez chyby (= jen prázdný source) je OK,
             * NULL s naplněným errbuf = syntax error. */
            if ( errbuf[0] != '\0' ) {
                fprintf ( stderr, "BREAKPOINTS WARN: invalid action for BP id=%d: %s\n",
                          bpt_id, errbuf );
            };
        };
    };

    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_hit_count ( int bpt_id, uint32_t hit_count ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->hit_count = hit_count;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_skip_count ( int bpt_id, uint32_t skip_count ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->skip_count = skip_count;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_edge_triggered ( int bpt_id, bool edge_triggered ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->edge_triggered = edge_triggered;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_fwd_min_interval_ms ( int bpt_id, uint32_t interval_ms ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->fwd_min_interval_ms = interval_ms;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_fwd_max_fires ( int bpt_id, uint32_t max_fires ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->fwd_max_fires = max_fires;
    g_breakpoints.version++;
    return true;
}


/* ========================================================================= */
/*  Match mode settery (V1.5.E)                                              */
/* ========================================================================= */


bool breakpoints_set_addr_match_mode ( int bpt_id, en_BP_MATCH_MODE mode ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( mode < 0 || mode >= BP_MATCH_COUNT ) return false;
    bpt->addr_match_mode = mode;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_addr_mask ( int bpt_id, uint16_t mask ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->addr_mask = mask;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_port_match_mode ( int bpt_id, en_BP_MATCH_MODE mode ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( mode < 0 || mode >= BP_MATCH_COUNT ) return false;
    bpt->port_match_mode = mode;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_port_end ( int bpt_id, uint16_t port_end ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->port_end = port_end;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_port_mask ( int bpt_id, uint16_t mask ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->port_mask = mask;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_port_mode ( int bpt_id, en_BP_PORT_MODE mode ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( mode < 0 || mode >= BP_PORT_MODE_COUNT ) return false;
    bpt->port_mode = mode;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_bp_addr_space ( int bpt_id, en_BP_ADDR_SPACE space ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( space < 0 || space >= BP_ADDR_SPACE_COUNT ) return false;
    bpt->bp_addr_space = space;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_bank_match_mode ( int bpt_id, en_BP_MATCH_MODE mode ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( mode < 0 || mode >= BP_MATCH_COUNT ) return false;
    bpt->bank_match_mode = mode;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_bank_id_end ( int bpt_id, uint8_t bank_id_end ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->bank_id_end = bank_id_end;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_bank_id_mask ( int bpt_id, uint8_t mask ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->bank_id_mask = mask;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_sp_mode ( int bpt_id, en_BP_SP_MODE mode ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( mode < 0 || mode >= BP_SP_MODE_COUNT ) return false;
    bpt->sp_mode = mode;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_sp_upper ( int bpt_id, uint16_t sp_upper ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->sp_upper = sp_upper;
    g_breakpoints.version++;
    return true;
}


/* ========================================================================= */
/*  IRQ vector / ISR filter settery (V1.5.A8)                                */
/* ========================================================================= */


bool breakpoints_set_im2_vector_filter ( int bpt_id, bool enabled, uint16_t addr ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->im2_vector_enabled = enabled;
    bpt->im2_vector_addr = addr;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_im2_isr_filter ( int bpt_id, bool enabled, uint16_t addr ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->im2_isr_enabled = enabled;
    bpt->im2_isr_addr = addr;
    g_breakpoints.version++;
    return true;
}


/* === V1.6+ TODO 4.4: IM2 vector / ISR Match modes settery ================= */


bool breakpoints_set_im2_vector_match_mode ( int bpt_id, en_BP_MATCH_MODE mode ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( mode < 0 || mode >= BP_MATCH_COUNT ) return false;
    bpt->im2_vector_match_mode = mode;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_im2_vector_addr_end ( int bpt_id, uint16_t addr_end ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->im2_vector_addr_end = addr_end;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_im2_vector_mask ( int bpt_id, uint16_t mask ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->im2_vector_mask = mask;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_im2_isr_match_mode ( int bpt_id, en_BP_MATCH_MODE mode ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( mode < 0 || mode >= BP_MATCH_COUNT ) return false;
    bpt->im2_isr_match_mode = mode;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_im2_isr_addr_end ( int bpt_id, uint16_t addr_end ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->im2_isr_addr_end = addr_end;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_im2_isr_mask ( int bpt_id, uint16_t mask ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->im2_isr_mask = mask;
    g_breakpoints.version++;
    return true;
}


/* ========================================================================= */
/*  IRQ IM mode + RST filter + IRQ_SIG settery (V1.5.A8.5)                   */
/* ========================================================================= */


bool breakpoints_set_im_enabled ( int bpt_id, uint8_t im, bool enabled ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    switch ( im ) {
        case 0: bpt->im0_enabled = enabled; break;
        case 1: bpt->im1_enabled = enabled; break;
        case 2: bpt->im2_enabled = enabled; break;
        default: return false;
    };
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_im0_rst_mask ( int bpt_id, uint8_t mask ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->im0_rst_mask = mask;
    g_breakpoints.version++;
    return true;
}


bool breakpoints_set_irq_sig_source_mask ( int bpt_id, uint8_t mask ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    bpt->irq_sig_source_mask = mask;
    g_breakpoints.version++;
    return true;
}


/*
 * Stabilní jména pro IRQ_SIG sources (= JSON persistence).
 * Pořadí odpovídá bit pozicím en_BP_IRQ_SIG_SOURCE.
 */
static const struct {
    en_BP_IRQ_SIG_SOURCE bit;
    const char *name;
} s_irq_sig_source_names[] = {
    { BP_IRQ_SIG_PIOZ80_PORT_A, "PIOZ80_A" },
    { BP_IRQ_SIG_PIOZ80_PORT_B, "PIOZ80_B" },
    { BP_IRQ_SIG_CTC2,          "CTC2" },
    { BP_IRQ_SIG_FDC,           "FDC" },
    { BP_IRQ_SIG_OTHER,         "OTHER" },
};


const char* bp_irq_sig_source_to_string ( en_BP_IRQ_SIG_SOURCE src ) {
    unsigned i;
    for ( i = 0; i < G_N_ELEMENTS ( s_irq_sig_source_names ); i++ ) {
        if ( s_irq_sig_source_names[i].bit == src ) {
            return s_irq_sig_source_names[i].name;
        };
    };
    return NULL;
}


bool bp_irq_sig_source_from_string ( const char *s, en_BP_IRQ_SIG_SOURCE *out ) {
    if ( !s || !out ) return false;
    unsigned i;
    for ( i = 0; i < G_N_ELEMENTS ( s_irq_sig_source_names ); i++ ) {
        if ( strcmp ( s, s_irq_sig_source_names[i].name ) == 0 ) {
            *out = s_irq_sig_source_names[i].bit;
            return true;
        };
    };
    return false;
}


/* ========================================================================= */
/*  Match helpery (V1.5.E)                                                   */
/* ========================================================================= */


bool bp_match16 ( en_BP_MATCH_MODE mode, uint16_t x,
                   uint16_t ref, uint16_t end, uint16_t mask ) {
    switch ( mode ) {
        case BP_MATCH_SINGLE: return x == ref;
        case BP_MATCH_RANGE:  return ( x >= ref ) && ( x <= end );
        case BP_MATCH_MASK:   return ( x & mask ) == ( ref & mask );
        default:              return false;
    };
}


bool bp_match8 ( en_BP_MATCH_MODE mode, uint8_t x,
                  uint8_t ref, uint8_t end, uint8_t mask ) {
    switch ( mode ) {
        case BP_MATCH_SINGLE: return x == ref;
        case BP_MATCH_RANGE:  return ( x >= ref ) && ( x <= end );
        case BP_MATCH_MASK:   return ( x & mask ) == ( ref & mask );
        default:              return false;
    };
}


/* ========================================================================= */
/*  String konverze pro typ a zónu                                           */
/* ========================================================================= */


/*
 * Tabulka mapping en_BPT_TYPE → string. Index = enum hodnota.
 */
static const char *s_bpt_type_names[ BPT_TYPE_COUNT ] = {
    [ BPT_TYPE_PC_EXEC ]      = "PC_EXEC",
    [ BPT_TYPE_MEM_R ]        = "MEM_R",
    [ BPT_TYPE_MEM_W ]        = "MEM_W",
    [ BPT_TYPE_IORQ_R ]       = "IORQ_R",
    [ BPT_TYPE_IORQ_W ]       = "IORQ_W",
    [ BPT_TYPE_IRQ ]          = "IRQ",
    [ BPT_TYPE_HW_EVENT ]     = "HW_EVENT",
    [ BPT_TYPE_SP_THRESHOLD ] = "SP_THRESHOLD",
    [ BPT_TYPE_GLOBAL ]       = "GLOBAL",
    [ BPT_TYPE_IRQ_SIG ]      = "IRQ_SIG",
};


/*
 * Tabulka mapping en_BP_ZONE → string. Index = enum hodnota.
 */
static const char *s_bp_zone_names[ BP_ZONE_COUNT ] = {
    [ BP_ZONE_CPU_VIEW ]  = "CPU_VIEW",
    [ BP_ZONE_ROM_LOWER ] = "ROM_LOWER",
    [ BP_ZONE_ROM_UPPER ] = "ROM_UPPER",
    [ BP_ZONE_RAM ]       = "RAM",
    [ BP_ZONE_VRAM_FB ]   = "VRAM_FB",
    [ BP_ZONE_PCG ]       = "PCG",
    [ BP_ZONE_MMEXT_BANK ] = "MMEXT_BANK",
};


const char* bpt_type_to_string ( en_BPT_TYPE type ) {
    if ( type < 0 || type >= BPT_TYPE_COUNT ) {
        return "PC_EXEC";
    };
    return s_bpt_type_names[ type ];
}


bool bpt_type_from_string ( const char *s, en_BPT_TYPE *out ) {
    if ( !s || !out ) return false;
    int i;
    for ( i = 0; i < BPT_TYPE_COUNT; i++ ) {
        if ( strcmp ( s, s_bpt_type_names[ i ] ) == 0 ) {
            *out = (en_BPT_TYPE) i;
            return true;
        };
    };
    return false;
}


const char* bp_zone_to_string ( en_BP_ZONE zone ) {
    if ( zone < 0 || zone >= BP_ZONE_COUNT ) {
        return "CPU_VIEW";
    };
    return s_bp_zone_names[ zone ];
}


bool bp_zone_from_string ( const char *s, en_BP_ZONE *out ) {
    if ( !s || !out ) return false;
    /* Legacy alias: PEHU_BANK -> MMEXT_BANK (rename V1.5).
     * Existing .bpt files mohou mít původní jméno - akceptujeme. */
    if ( strcmp ( s, "PEHU_BANK" ) == 0 ) {
        *out = BP_ZONE_MMEXT_BANK;
        return true;
    };
    /* Legacy alias: VRAM_RF -> VRAM_FB (rename V1.7+: RF/WF registry
     * jsou MZ-800-only koncept, "framebuffer" je platform-neutrální).
     * Existing .bpt files s "VRAM_RF" se načtou jako VRAM_FB. */
    if ( strcmp ( s, "VRAM_RF" ) == 0 ) {
        *out = BP_ZONE_VRAM_FB;
        return true;
    };
    int i;
    for ( i = 0; i < BP_ZONE_COUNT; i++ ) {
        if ( strcmp ( s, s_bp_zone_names[ i ] ) == 0 ) {
            *out = (en_BP_ZONE) i;
            return true;
        };
    };
    return false;
}


/*
 * Tabulka mapping en_BP_MATCH_MODE → string. Index = enum hodnota.
 */
static const char *s_bp_match_mode_names[ BP_MATCH_COUNT ] = {
    [ BP_MATCH_SINGLE ] = "SINGLE",
    [ BP_MATCH_RANGE ]  = "RANGE",
    [ BP_MATCH_MASK ]   = "MASK",
};


const char* bp_match_mode_to_string ( en_BP_MATCH_MODE mode ) {
    if ( mode < 0 || mode >= BP_MATCH_COUNT ) {
        return "SINGLE";
    };
    return s_bp_match_mode_names[ mode ];
}


bool bp_match_mode_from_string ( const char *s, en_BP_MATCH_MODE *out ) {
    if ( !s || !out ) return false;
    int i;
    for ( i = 0; i < BP_MATCH_COUNT; i++ ) {
        if ( strcmp ( s, s_bp_match_mode_names[ i ] ) == 0 ) {
            *out = (en_BP_MATCH_MODE) i;
            return true;
        };
    };
    return false;
}


/*
 * Tabulka mapping en_BP_ADDR_SPACE → string (feature D). Index = enum
 * hodnota. Stabilní klíče pro .bpt JSON serializaci.
 */
static const char *s_bp_addr_space_names[ BP_ADDR_SPACE_COUNT ] = {
    [ BP_ADDR_SPACE_CPU_VIEW ]    = "cpu_view",
    [ BP_ADDR_SPACE_BANK_OFFSET ] = "bank_offset",
};


const char* bp_addr_space_to_string ( en_BP_ADDR_SPACE space ) {
    if ( space < 0 || space >= BP_ADDR_SPACE_COUNT ) {
        return "cpu_view";
    };
    return s_bp_addr_space_names[ space ];
}


bool bp_addr_space_from_string ( const char *s, en_BP_ADDR_SPACE *out ) {
    if ( !s || !out ) return false;
    int i;
    for ( i = 0; i < BP_ADDR_SPACE_COUNT; i++ ) {
        if ( strcmp ( s, s_bp_addr_space_names[ i ] ) == 0 ) {
            *out = (en_BP_ADDR_SPACE) i;
            return true;
        };
    };
    return false;
}


/*
 * Tabulka mapping en_BP_SP_MODE → string. Index = enum hodnota.
 */
static const char *s_bp_sp_mode_names[ BP_SP_MODE_COUNT ] = {
    [ BP_SP_SINGLE ] = "SINGLE",
    [ BP_SP_WINDOW ] = "WINDOW",
};


const char* bp_sp_mode_to_string ( en_BP_SP_MODE mode ) {
    if ( mode < 0 || mode >= BP_SP_MODE_COUNT ) {
        return "SINGLE";
    };
    return s_bp_sp_mode_names[ mode ];
}


bool bp_sp_mode_from_string ( const char *s, en_BP_SP_MODE *out ) {
    if ( !s || !out ) return false;
    int i;
    for ( i = 0; i < BP_SP_MODE_COUNT; i++ ) {
        if ( strcmp ( s, s_bp_sp_mode_names[ i ] ) == 0 ) {
            *out = (en_BP_SP_MODE) i;
            return true;
        };
    };
    return false;
}


/*
 * Tabulka mapping en_BP_PORT_MODE → string (V1.5.A7). Index = enum hodnota.
 */
static const char *s_bp_port_mode_names[ BP_PORT_MODE_COUNT ] = {
    [ BP_PORT_8BIT ]  = "8BIT",
    [ BP_PORT_16BIT ] = "16BIT",
};


const char* bp_port_mode_to_string ( en_BP_PORT_MODE mode ) {
    if ( mode < 0 || mode >= BP_PORT_MODE_COUNT ) {
        return "8BIT";
    };
    return s_bp_port_mode_names[ mode ];
}


bool bp_port_mode_from_string ( const char *s, en_BP_PORT_MODE *out ) {
    if ( !s || !out ) return false;
    int i;
    for ( i = 0; i < BP_PORT_MODE_COUNT; i++ ) {
        if ( strcmp ( s, s_bp_port_mode_names[ i ] ) == 0 ) {
            *out = (en_BP_PORT_MODE) i;
            return true;
        };
    };
    return false;
}


/* ========================================================================= */
/*  Hromadné operace                                                         */
/* ========================================================================= */




void breakpoints_clear_all ( void ) {
    unsigned i;

    /* Uvolnit stringy breakpointů */
    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        breakpoints_free_bpt_strings ( bpt );
    };
    g_array_set_size ( g_breakpoints.breakpoints, 0 );

    /* Uvolnit stringy skupin */
    for ( i = 0; i < g_breakpoints.groups->len; i++ ) {
        st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, i );
        breakpoints_free_group_strings ( grp );
    };
    g_array_set_size ( g_breakpoints.groups, 0 );

    bptmap_clear_all ( );

    /* User $vars storage (D.1.3) také wipe - clear_all sémantika je
     * "vrať se do clean stavu". UI Clear All v fázi C nemělo vars,
     * ale po zavedení V1 je čisté smazat i vars. */
    bp_vars_clear_storage ( );

    /* D.3 - HW event active bitmap také reset (= žádné registrované BP). */
    bp_event_clear_all_active ( );

    g_breakpoints.version++;
}


bool breakpoints_is_effectively_enabled ( int bpt_id ) {
    st_BPT *bpt = breakpoints_find_by_id ( bpt_id );
    if ( !bpt ) return false;
    if ( !bpt->enabled ) return false;
    return breakpoints_is_group_enabled ( bpt->parent );
}


void breakpoints_sync_bptmap ( void ) {
    /* Uložit adresu temporary breakpointu */
    int saved_temporary = g_bptmap.temporary_bpt_addr;

    /* Vyčistit celou bptmap (bpmap[] + per-typ listy + active flagy). */
    bptmap_clear_all ( );

    /* Znovu zaregistrovat všechny efektivně povolené breakpointy.
     * - PC_EXEC SINGLE -> bptmap_event_add na bpt->addr (O(1) bpmap[])
     * - PC_EXEC RANGE  -> bptmap_event_add na vsechny adresy v <addr, addr_end>;
     *                     kolize s jinym BP na konkretni addr = warning, addr
     *                     se v ramci tohoto RANGE BP "preskoci" (BP nezafire
     *                     na te jedne adrese, ale ostatni v range OK).
     * - PC_EXEC MASK   -> [neimplementovano V1.5.E pro hot path PC_EXEC]
     *                     Fallback: bptmap_event_add jen na bpt->addr (= ref).
     *                     User dostane runtime warning. MASK pro non-PC typy
     *                     (MEM/IORQ/BANK) funguje plne, jen PC vyzaduje per-
     *                     instruction iteraci pres seznam BP, ktery V1.5.E
     *                     nezavadi.
     * - non-PC  -> bptmap_register (per-typ list, V1.5.E rozsireno o match
     *              modes uvnitr breakpoints_dispatch_addr_list)
     */
    unsigned i;
    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( !( bpt->enabled && breakpoints_is_group_enabled ( bpt->parent ) ) ) continue;

        if ( bpt->type == BPT_TYPE_PC_EXEC ) {
            if ( bpt->addr_match_mode == BP_MATCH_RANGE ) {
                /* Registrovat vsechny adresy v <addr, addr_end>. */
                uint16_t lo = bpt->addr;
                uint16_t hi = ( bpt->addr_end >= bpt->addr ) ? bpt->addr_end : bpt->addr;
                uint32_t a;
                for ( a = lo; a <= hi; a++ ) {
                    int conflict = bptmap_event_add ( (uint16_t) a, bpt->id );
                    if ( conflict > 0 && conflict != bpt->id ) {
                        printf ( "BREAKPOINTS WARNING: PC_EXEC RANGE BP id=%d '%s' "
                                 "addr 0x%04x kolize s id=%d - tato adresa preskocena\n",
                                 bpt->id, bpt->name ? bpt->name : "",
                                 (uint16_t) a, conflict );
                    };
                };
            } else if ( bpt->addr_match_mode == BP_MATCH_MASK ) {
                /* V1.6+ TODO 4.3: PC_EXEC MASK plny support pres per-type
                 * list BPTMAP_IDX_PC_EXEC_NONSINGLE.
                 *
                 * Hot path mzarch.c testuje:
                 *   1. g_bptmap.bpmap[pc] != NONE  -> SINGLE/RANGE O(1) cesta
                 *   2. g_bptmap.per_type_active[PC_EXEC_NONSINGLE] -> iter list
                 *
                 * MASK BP zaregistrujeme jen do per-type listu (NE do bpmap[]),
                 * protoze sparse mask nelze enumerate na all matching addr.
                 * Registrace do listu nezpusobi conflict s jinymi BP na bpmap[]
                 * (= MASK BP nezabira konkretni adresu, fire dle bp_match16).
                 */
                bptmap_register ( bpt->id, BPTMAP_IDX_PC_EXEC_NONSINGLE );
            } else {
                /* SINGLE - legacy O(1) cesta. */
                int conflict = bptmap_event_add ( bpt->addr, bpt->id );
                if ( conflict > 0 ) {
                    printf ( "BREAKPOINTS WARNING: duplicate event at address 0x%04x "
                             "(id=%d '%s' conflicts with id=%d) - disabling\n",
                             bpt->addr, bpt->id, bpt->name ? bpt->name : "", conflict );
                    bpt->enabled = false;
                };
            };
        } else {
            int idx = breakpoints_bpt_type_to_idx ( bpt->type );
            if ( idx >= 0 ) {
                bptmap_register ( bpt->id, (en_BPTMAP_TYPE_IDX) idx );
            };
        };
    };

    /* Obnovit temporary breakpoint */
    if ( saved_temporary >= 0 && saved_temporary <= 0xffff ) {
        bptmap_set_temporary_event ( (uint16_t)saved_temporary );
    };

    /* D.3 - synchronizovat globální event_active bitmap. Per-typ list
     * pro HW_EVENT jsme už zaregistrovali výše (přes type_to_idx), ale
     * bp_event_active[] je separátní rychlý fast-skip a musí být v
     * souladu. */
    breakpoints_recompute_event_active ( );

    /* Historie CPU je k dispozici i bez okna, pokud je aspoň jeden BP
     * enabled - přepočti fast-skip flag has_enabled_bp. */
    breakpoints_recompute_has_enabled ( );
}


void breakpoints_recompute_has_enabled ( void ) {
    bool any = false;
    unsigned i;
    for ( i = 0; i < g_breakpoints.breakpoints->len; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( bpt->enabled && breakpoints_is_group_enabled ( bpt->parent ) ) {
            any = true;
            break;
        };
    };
    g_bptmap.has_enabled_bp = any;
}


/* ========================================================================= */
/*  Condition evaluation (D.1.2)                                             */
/* ========================================================================= */


bool breakpoints_eval_condition ( const st_BPT *bpt,
                                   const struct bp_expr_ctx_s *ctx ) {
    if ( !bpt ) return false;
    /* No expr = vždy fire (= legacy unconditional BP). */
    if ( !bpt->expr || bpt->expr[0] == '\0' ) return true;
    /* Expr je uložený, ale parser selhal -> conservatively fire,
     * aby se chování nepřevracelo do "nikdy" při syntax chybě. */
    if ( !bpt->parsed_expr ) return true;
    if ( !ctx ) return true;

    int32_t v = bp_expr_eval ( bpt->parsed_expr, ctx );
    return v != 0;
}


/* ========================================================================= */
/*  Action execution (D.1.3)                                                 */
/* ========================================================================= */


bool breakpoints_run_action ( const st_BPT *bpt,
                               const struct bp_expr_ctx_s *ctx ) {
    if ( !bpt ) return false;
    /* Nedefinovaná action nebo prázdný řetězec → klasický stop. */
    if ( !bpt->action || bpt->action[0] == '\0' ) return false;
    /* Parser selhal - conservatively stop (= chyba je už zalogovaná). */
    if ( !bpt->parsed_action ) return false;
    if ( !ctx ) return false;

    return bp_action_execute ( bpt->parsed_action, ctx );
}


/* ========================================================================= */
/*  Enforcement (D.2)                                                        */
/* ========================================================================= */


#include "emulator.h"
#include "debugger.h"
#include "dbgapi_emu.h"
#include "dbgapi_msg.h"
#include "mzarch/mzarch.h"
#include "bp_zone.h"        /* D.5 - banking zone filter */
#include "bp_event.h"       /* Vlna 1 mutant event-viewer: g_bp_fire_reason */
#include "trace/eventlog.h" /* Vlna 1 mutant event-viewer: BP_FIRE fan-out */

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include <json-glib/json-glib.h>
#include "../mcp/event_bus.h"
#include "../mcp/trap_manager.h"
#endif


void breakpoints_drain_control_plane_at_bp_boundary ( void ) {
    /* Fronta neinicializovaná (= dbgapi_init nebyl volán) - není co
     * drainovat. V běžícím emulátoru je fronta inicializovaná při startu,
     * tato větev tedy chrání jen brzkou fázi / testy bez fronty. Bez ní
     * by dbgapi_emu_has_pending() dereferencoval NULL queue_mutex. */
    if ( !g_dbgapi_cmdrq_queue.queue_mutex ) return;

    /* HOT-PATH disciplína: tuto funkci voláme JEN když BP akce reálně
     * proběhla a vrací continue (= off-hot-path událost, ne per-instrukce).
     * Prázdná fronta = jeden atomic read + branch, pak okamžitý návrat. */
    if ( !dbgapi_emu_has_pending ( &g_dbgapi_cmdrq_queue ) ) return;

    /* Drain CELÉ fronty stejnými primitivy jako per-frame bod
     * (mzarch.c screen-done). dequeue/dispatch/complete jsou thread-safe
     * a určené pro emu vlákno, na kterém běžíme - re-použití je legální.
     *
     * Po drainu: pokud některý příkaz nastavil pauzu (PAUSE/STOP přes
     * emulator_pause(true)), enforce po návratu narazí na EMULATOR_TEST_PAUSED
     * v mzarch.c a vstoupí do paused-loop. Žádný další kód zde netřeba. */
    st_DBGAPI_CMDRQ *rq;
    while ( ( rq = dbgapi_emu_dequeue ( &g_dbgapi_cmdrq_queue ) ) != NULL ) {
        dbgapi_emu_dispatch ( rq );
        dbgapi_emu_complete ( rq );
    };
}


/* ========================================================================= */
/*  0019 vrstva 3 - kumulativní byte backstop pro forward akce               */
/* ========================================================================= */

/**
 * @brief Práh byte backstopu v bajtech (0 = vypnuto).
 *
 * Měkký práh: měněn z UI/MCP vlákna, čten z emu vlákna při FWD akci.
 * Bez locku - prostý uint64 read/write, race na čtení neohrozí korektnost
 * (jen by o jeden snapshot posunul okamžik auto-pauzy). Inicializován na
 * default v breakpoints_init.
 */
static uint64_t g_bp_action_byte_limit = BP_ACTION_FWD_DEFAULT_BYTE_LIMIT;

/**
 * @brief Akumulátor bajtů zapsaných těžkými FWD akcemi (snapshot/trace_save).
 *
 * Sčítá se přes všechny BP. Resetuje se v breakpoints_init (= reset
 * emulátoru) nebo ručně přes breakpoints_fwd_reset_byte_accounting.
 * Modifikován výhradně z emu vlákna v bp_action.c.
 */
static uint64_t g_bp_action_total_bytes = 0;

/**
 * @brief Globální default minimálního odstupu těžkých FWD akcí (0019 v2) v ms.
 *
 * Použije se jako fallback pro BP, který nemá vlastní per-BP override
 * (fwd_min_interval_ms == 0). Inicializován z INI klíče
 * [BREAKPOINTS] fwd_default_min_interval_ms (default
 * BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS). 0 = global default vypnut -> spadne
 * se na vestavěnou konstantu BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS (bezpečný
 * default zůstává i kdyby uživatel nastavil 0). Měkká hodnota: měněna z
 * UI/MCP vlákna, čtena z emu vlákna; bez locku (prostý uint32 read/write).
 */
static uint32_t g_bp_action_fwd_default_min_interval_ms =
    BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS;


void breakpoints_fwd_set_byte_limit ( uint64_t limit_bytes ) {
    g_bp_action_byte_limit = limit_bytes;
}


void breakpoints_fwd_set_default_min_interval_ms ( uint32_t interval_ms ) {
    g_bp_action_fwd_default_min_interval_ms = interval_ms;
}


uint32_t breakpoints_fwd_get_default_min_interval_ms ( void ) {
    return g_bp_action_fwd_default_min_interval_ms;
}


uint64_t breakpoints_fwd_get_byte_limit ( void ) {
    return g_bp_action_byte_limit;
}


uint64_t breakpoints_fwd_get_total_bytes ( void ) {
    return g_bp_action_total_bytes;
}


void breakpoints_fwd_reset_byte_accounting ( void ) {
    g_bp_action_total_bytes = 0;
    /* 0019 v3: srovnej i baseline trace_save disk accountingu, aby trace_save
     * fire po resetu countoval deltu od TEĎ (ne historický footprint). */
    bp_action_reset_trace_disk_baseline ( );
}


/**
 * @brief cfg propagate callback: INI fwd_byte_limit_mb (MB) -> práh v bajtech.
 *
 * Volá cfg vrstva při načtení/změně konfigurace. Převede uloženou hodnotu
 * v MB na bajty a nastaví backstop práh. 0 MB = backstop vypnut.
 *
 * @param e CFGELM element (typ unsigned).
 * @param data Nepoužito (NULL).
 */
static void breakpoints_propagatecfg_byte_limit ( void *e, void *data ) {
    (void) data;
    unsigned mb = cfgelement_get_unsigned_value ( (CFGELM *) e );
    breakpoints_fwd_set_byte_limit ( (uint64_t) mb * 1024u * 1024u );
}


/**
 * @brief cfg save callback: práh v bajtech -> INI fwd_byte_limit_mb (MB).
 *
 * Volá cfg vrstva před uložením konfigurace. Převede aktuální práh z bajtů
 * zpět na MB (zaokrouhleno dolů) a zapíše do elementu.
 *
 * @param e CFGELM element (typ unsigned).
 * @param data Nepoužito (NULL).
 */
static void breakpoints_savecfg_byte_limit ( void *e, void *data ) {
    (void) data;
    unsigned mb = (unsigned) ( g_bp_action_byte_limit / ( 1024u * 1024u ) );
    cfgelement_set_unsigned_value ( (CFGELM *) e, mb );
}


/**
 * @brief cfg propagate callback: INI fwd_default_min_interval_ms -> global default.
 *
 * Volá cfg vrstva při načtení/změně konfigurace. Nastaví globální default
 * rate-limit interval pro BP bez vlastního override (0019 v2).
 *
 * @param e CFGELM element (typ unsigned).
 * @param data Nepoužito (NULL).
 */
static void breakpoints_propagatecfg_fwd_default_interval ( void *e, void *data ) {
    (void) data;
    unsigned ms = cfgelement_get_unsigned_value ( (CFGELM *) e );
    breakpoints_fwd_set_default_min_interval_ms ( (uint32_t) ms );
}


/**
 * @brief cfg save callback: global default -> INI fwd_default_min_interval_ms.
 *
 * @param e CFGELM element (typ unsigned).
 * @param data Nepoužito (NULL).
 */
static void breakpoints_savecfg_fwd_default_interval ( void *e, void *data ) {
    (void) data;
    cfgelement_set_unsigned_value ( (CFGELM *) e,
                                    g_bp_action_fwd_default_min_interval_ms );
}


/**
 * @brief Přičte bajty FWD akce a vyhodnotí byte backstop (0019 v3).
 *
 * Interní helper volaný z bp_action.c po každém úspěšném zápisu těžké FWD
 * akce. Akumuluje bajty a po překročení prahu emulátor sám zapauzuje +
 * vyemituje warning a "paused" event s důvodem (BP id + N MB).
 *
 * @param bp_id ID breakpointu, jehož akce zápis provedla (= do detailu eventu).
 * @param bytes Počet právě zapsaných bajtů.
 *
 * Side effects: inkrement g_bp_action_total_bytes; při překročení prahu
 * emulator_pause(true) + stderr warning + (pod MCP) "paused" event. Auto-pauza
 * proběhne jen jednou na hranici překročení (akumulátor dál roste, ale
 * emulator_pause(true) je idempotentní).
 * Threading: jen z emu vlákna (mezi instrukcemi, safe-point).
 */
void breakpoints_fwd_account_bytes ( int bp_id, uint64_t bytes ) {
    if ( bytes == 0 ) return;

    uint64_t before = g_bp_action_total_bytes;
    g_bp_action_total_bytes += bytes;

    uint64_t limit = g_bp_action_byte_limit;
    if ( limit == 0 ) return;   /* backstop vypnut */

    /* Auto-pauza jen na hraně překročení (before < limit <= after), aby se
     * warning/event neopakoval s každým dalším snapshotem nad prahem. */
    if ( before < limit && g_bp_action_total_bytes >= limit ) {
        unsigned long long total_mb =
            (unsigned long long) ( g_bp_action_total_bytes / ( 1024 * 1024 ) );
        fprintf ( stderr,
                  "[BP-ACTION] forward-action byte backstop tripped: BP #%d - "
                  "auto-paused after %llu MB written\n",
                  bp_id, total_mb );

        /* Čistá auto-pauza přes stejný mechanismus jako DBGAPI PAUSE
         * (= konzistentní side-efekty: audio pause, MZEVENT). Jsme na emu
         * vlákně mezi instrukcemi (safe-point), takže je to bezpečné. */
        emulator_pause ( true );

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
        /* Informuj MCP klienty: "paused" event s důvodem saturace, ať uživatel
         * ví PROČ se emulátor zastavil (vč. BP id a objemu). */
        JsonObject *p_payload = json_object_new ( );
        json_object_set_string_member ( p_payload, "reason",
                                         "bp_action_saturation" );
        json_object_set_int_member ( p_payload, "bp_id", bp_id );
        json_object_set_int_member ( p_payload, "bytes",
                                     (gint64) g_bp_action_total_bytes );
        char detail[128];
        snprintf ( detail, sizeof ( detail ),
                   "BP #%d forward-action saturation: auto-paused after %llu MB",
                   bp_id, total_mb );
        json_object_set_string_member ( p_payload, "detail", detail );
        event_bus_emit ( "paused", p_payload );
#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
    };
}


/**
 * @brief Naplní bp_expr_ctx_t globálním emu state (Cycle/Frame/Scanline + cpu).
 *
 * Caller předá zero-init ctx; tato funkce přepíše jen pole nezávislá
 * na typu hooku. Pole závislá na typu (Address, Value, IsRead/Write/Exec/Port,
 * BankPC, BankAddr, self_id) plní caller sám.
 *
 * V1.5.A10: promote z static na public (= reuse v UI Test Eval).
 */
void breakpoints_fill_global_ctx ( bp_expr_ctx_t *ctx ) {
    ctx->cpu = g_mzarch_main.cpu;
    /* V1.5 fáze 2.4: zdroje z GDG (mz800 + mz1500 sdílí pattern).
     *
     * - Frame    = g_gdg.total_elapsed.screens (= kompletní snímky od resetu;
     *              inkrementuje se v gdg_on_screen_done_event).
     * - Scanline = g_gdg.beam_row (= aktuální raster row paprsku).
     * - Cycle    = gdg_get_total_ticks() = kumulativní GDG pixel ticks od
     *              resetu (16 pixel ticks per Z80 T-state na 3.5 MHz). NENI
     *              čistý Z80 T-state counter; pro per-frame timing relativní
     *              jednotky stačí. Plný T-state cycle counter = TODO V1.6+. */
    /* SENTINEL: g_gdg timing kontrakt plati pro vsechny zname
     * architektury (700/800/1500, garantovano mzhal.c) - pri pridani
     * nove architektury tuto cast PROVER. */
    ctx->Cycle    = (int64_t) gdg_get_total_ticks ( );
    ctx->Frame    = (int64_t) g_gdg.total_elapsed.screens;
    ctx->Scanline = (int32_t) g_gdg.beam_row;
#if 0 /* sentinel fallback pro neznamou arch */
    ctx->Cycle    = 0;
    ctx->Frame    = 0;
    ctx->Scanline = 0;
#endif
    /* BankPC/BankAddr: D.5 banking awareness.
     * BankPC = en_BP_ZONE aktuálně paged-in pro PC (= co CPU právě
     * vykonává). BankAddr defaultně stejně, naplní caller pro MEM
     * hooky podle ctx->Address (= memory addr, ne PC). */
    {
        uint16_t pc = ctx->cpu ? (uint16_t) ctx->cpu->pc : 0;
        en_BP_ZONE pc_zone = bp_zone_active_at_pc ( pc );
        ctx->BankPC = (uint8_t) pc_zone;
        ctx->BankAddr = (uint8_t) pc_zone;
    };
}


void breakpoints_enforce ( st_BPT *bpt, struct bp_expr_ctx_s *ctx ) {
    if ( !bpt || !ctx ) return;

    /* 1) Effective enable check (BP enabled + skupinová cesta enabled). */
    if ( !bpt->enabled ) return;
    if ( !breakpoints_is_group_enabled ( bpt->parent ) ) return;

    /* 1b) Banking zone filter (D.5).
     *
     * Pokud BP cílí non-default zónu (= ne CPU_VIEW), ověříme že daná
     * zóna pokrývá ctx->Address a je aktuálně paged-in. Pokud ne, BP
     * nesmí fire (= zone awareness pro mzdos overlay debug, ROM/VRAM
     * banking apod.).
     *
     * Aplikuje se jen pro mem-typy BP (PC_EXEC / MEM_R / MEM_W) -
     * pro ostatní typy (IORQ, HW_EVENT, IRQ, SP_THRESHOLD, GLOBAL)
     * je adresa irelevantní (= UI by neměla nabízet zone dropdown).
     * V1 ignorujeme zone field pro non-mem typy (V1.5 případně
     * zpřísní). */
    if ( bpt->zone != BP_ZONE_CPU_VIEW ) {
        bool zone_relevant =
            ( bpt->type == BPT_TYPE_PC_EXEC ) ||
            ( bpt->type == BPT_TYPE_MEM_R ) ||
            ( bpt->type == BPT_TYPE_MEM_W );
        if ( zone_relevant ) {
            /* V1.5.E - signature rozsirena o bank match mode/end/mask.
             * Pro non-MMEXT_BANK zony jsou tyto parametry ignorovany. */
            if ( !bp_zone_is_active_at ( bpt->zone, ctx->Address,
                                          bpt->bank_match_mode,
                                          bpt->bank_id,
                                          bpt->bank_id_end,
                                          bpt->bank_id_mask ) ) {
                return;
            };
        };
    };

    /* 2) Skip count: prvních N hitů ignorovat. Podmínka i akce se vůbec
     *    neevaluují, ani hits++ se neinkrementuje (= konzistentní s
     *    "vůbec to nezahit"). */
    if ( bpt->skip_count > 0 ) {
        bpt->skip_count--;
        return;
    };

    /* 3) Condition eval. Pokud false → no fire. */
    if ( !breakpoints_eval_condition ( bpt, ctx ) ) return;

    /* 4) Hits counter. Inkrementujeme PŘED hit_count testem aby pořadí
     *    1, 2, 3, ... odpovídalo "hits == hit_count" sémantice. */
    bpt->hits++;

    /* 5) Hit count: trigger pouze na N. hitu (po skip_count). 0 = každý. */
    if ( bpt->hit_count > 0 && bpt->hits != bpt->hit_count ) return;

    /* 5b) Vlna 1 mutant event-viewer: BP_FIRE fan-out do eventlog ringu.
     *
     * Vkládáme PO úspěšném gatingu (= podmínka splněna, hit_count match)
     * a PŘED vykonáním action skriptu. Důvod pořadí:
     *
     *  - Eventlog má reflektovat "BP právě teď fíruje" - z pohledu UI je
     *    zajímavé, že trigger nastal, bez ohledu na výsledek akce.
     *  - Action může mít side-effect (disable_self, poke, set), který by
     *    interagoval s ringem - emit PŘED akcí zajišťuje, že event
     *    v ringu reflektuje stav BP v okamžiku triggeru.
     *  - Klasifikace subtype z @c bpt->parsed_action je čistá funkce
     *    (= AST je read-only mimo executor).
     *
     * Payload layout (low to high):
     *   bits  0..15 : bpt->id
     *   bits 16..23 : g_bp_fire_reason (ambient en_BP_FIRE_REASON)
     *   bits 24..31 : 0 (rezerva)
     *
     * PC se odečítá z @c ctx->cpu (= aktuální Z80 PC v okamžiku fire),
     * ne z bpt->addr - umožňuje rozlišit fire z různých instrukcí pro
     * BP s wildcard addr_match_mode (RANGE/MASK). */
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_BP_FIRE ) ) ) {
        /* Mapping en_BP_ACTION_SUB -> en_EVENTLOG_BP_FIRE_SUB je 1:1
         * (= hodnoty enumů sjednocené záměrně, viz Doxygen u
         * en_EVENTLOG_BP_FIRE_SUB). Stačí cast. */
        uint8_t sub = (uint8_t) bp_action_classify_subtype ( bpt->parsed_action );
        uint32_t pl = ( (uint32_t) (uint16_t) bpt->id )
                      | ( (uint32_t) g_bp_fire_reason << 16 );
        uint16_t pc = ctx->cpu ? (uint16_t) ctx->cpu->pc : 0;
        eventlog_record ( EVENTLOG_CAT_BP_FIRE, sub, pc, pl );
    }

    /* 6) Action execute. self_id povinné pro disable_self funkci. */
    ctx->self_id = bpt->id;
    bool should_continue = false;
    if ( bpt->action && bpt->action[0] != '\0' && bpt->parsed_action ) {
        should_continue = breakpoints_run_action ( bpt, ctx );
    };

    /* 7) Stop nebo continue. */
    if ( !should_continue ) {
        g_emulator.pause_reason = EMU_PAUSE_REASON_BREAKPOINT;
        emulator_pause ( true );
        debugger_show_main_window ( );

        /* MSG do UI vlákna - heap alloc per dbgapi_msg.h kontraktu
         * (dispatcher přebírá ownership a uvolňuje). */
        st_DBGAPI_MSG_DATA *msg_data = g_new0 ( st_DBGAPI_MSG_DATA, 1 );
        msg_data->msg_type = DBGAPI_MSG_BREAKPOINT_HIT;
        msg_data->addr = bpt->addr;
        msg_data->id = bpt->id;
        dbgapi_emu_send_msg ( DBGAPI_MSG_BREAKPOINT_HIT, msg_data );

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
        /* V1.A.4: MCP EVENT subscribe pattern - emituj "breakpoint_hit"
         * + zaregistruj TRAP. Klient přes emu_event_poll dostane event
         * s trap_id a přes emu_trap_respond reaguje (continue / step /
         * abort). Pause je už nastaven výše (= emergentní TRAP semantika
         * bez blocking GCond). */
        int64_t trap_id = trap_manager_register ( );
        JsonObject *bp_payload = json_object_new ( );
        json_object_set_int_member ( bp_payload, "id", bpt->id );
        json_object_set_int_member ( bp_payload, "addr", bpt->addr );
        json_object_set_string_member ( bp_payload, "type",
                                         bpt_type_to_string ( bpt->type ) );
        json_object_set_int_member ( bp_payload, "hits", bpt->hits );
        json_object_set_int_member ( bp_payload, "trap_id", trap_id );
        event_bus_emit ( "breakpoint_hit", bp_payload );

        /* Současně emit "paused" event pro klienty, kteří poslouchají
         * obecný pause topic (= union BP/manual/fatal/quit). */
        JsonObject *p_payload = json_object_new ( );
        json_object_set_string_member ( p_payload, "reason", "breakpoint" );
        uint16_t pc_now = ctx->cpu ? (uint16_t) ctx->cpu->pc : bpt->addr;
        json_object_set_int_member ( p_payload, "pc", pc_now );
        event_bus_emit ( "paused", p_payload );
#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
    } else {
        /* 8) Vrstva 1 (0019) control-plane garance: akce proběhla a vrací
         * continue (= off-hot-path hranice). Obsloužíme pending řídicí
         * příkazy (PAUSE/STOP/...), aby control-plane zůstal responzivní
         * i na horké BP smyčce, kde se per-frame drain (mzarch.c) nemusí
         * stihnout. Pokud příkaz nastaví pauzu, enforce po návratu narazí
         * na EMULATOR_TEST_PAUSED v mzarch.c a zastaví. */
        breakpoints_drain_control_plane_at_bp_boundary ( );
    };
}


/* ========================================================================= */
/*  Per-typ enforcement hooky (D.2)                                          */
/* ========================================================================= */


/**
 * @brief V1.6+ TODO 4.3: enforce PC_EXEC MASK BP per-instruction.
 *
 * Iteruje per-type list `BPTMAP_IDX_PC_EXEC_NONSINGLE` (= PC_EXEC s
 * RANGE/MASK match mode). Volaci konvence stejna jako enforce_pc_exec.
 *
 * Note: V1.6+ aktualne registrujeme jen MASK do tohoto listu (RANGE
 * pouziva bpmap[] addr-by-addr cestu pro O(1) per-instruction lookup).
 *
 * Hot path overhead: 1 array load + branch v default OFF stavu (= zero
 * impact pokud zadny PC_EXEC MASK BP). Pri aktivnim MASK BP iterace
 * O(N) kde N = pocet MASK BPs (typicky <5 = trivial).
 *
 * @param pc Aktualni instruction address.
 */
void breakpoints_enforce_pc_exec_nonsingle ( uint16_t pc ) {
    GArray *list = g_bptmap.per_type_lists[ BPTMAP_IDX_PC_EXEC_NONSINGLE ];
    if ( !list || list->len == 0 ) return;

    /* Kopie ID listy - actions muzou mutovat (disable_self -> unregister). */
    int stack_buf[16];
    int *ids = stack_buf;
    int *heap = NULL;
    unsigned n = list->len;
    if ( n > 16 ) {
        heap = g_new0 ( int, n );
        ids = heap;
    };
    unsigned i;
    for ( i = 0; i < n; i++ ) ids[i] = g_array_index ( list, int, i );

    for ( i = 0; i < n; i++ ) {
        st_BPT *bpt = breakpoints_find_by_id ( ids[i] );
        if ( !bpt ) continue;
        if ( bpt->type != BPT_TYPE_PC_EXEC ) continue;
        /* SINGLE/RANGE jsou v bpmap[] cesta, sem nesmi spadnout. */
        if ( bpt->addr_match_mode == BP_MATCH_SINGLE ) continue;

        uint16_t end = ( bpt->addr_end >= bpt->addr ) ? bpt->addr_end : bpt->addr;
        if ( !bp_match16 ( bpt->addr_match_mode, pc,
                           bpt->addr, end, bpt->addr_mask ) ) continue;

        bp_expr_ctx_t ctx;
        bp_expr_ctx_zero ( &ctx );
        breakpoints_fill_global_ctx ( &ctx );
        ctx.Address = pc;

        breakpoints_enforce ( bpt, &ctx );

        if ( EMULATOR_TEST_PAUSED ) break;
    };

    if ( heap ) g_free ( heap );
}


void breakpoints_enforce_pc_exec ( uint16_t pc ) {
    int marker = g_bptmap.bpmap[pc];
    if ( marker == BREAKPOINT_TYPE_NONE ) return;

    /* Temporary BP (run-to-cursor / step-over): legacy chování -
     * pause + show debugger, žádná condition / action. */
    if ( marker == BREAKPOINT_TYPE_TEMPORARY ) {
        printf ( "DEBUGGER - activated temporary breakpoint on addr: 0x%04x\n", pc );
        emulator_pause ( true );
        debugger_show_main_window ( );
        return;
    };

    /* marker > 0 = bp_id existujícího BP. */
    st_BPT *bpt = breakpoints_find_by_id ( marker );
    if ( !bpt ) return;

    /* V1.5.E - extra match check pro konzistenci se semantikou
     * addr_match_mode. Pro SINGLE = no-op (legacy). Pro RANGE = redundantni
     * (sync_bptmap registroval vsechny adresy v range, takze pokud sem
     * dorazil, je v range), ale levne. Pro MASK = aplikuje pravidlo
     * (i kdyz sync_bptmap fallbacknul na SINGLE, MASK match by mel byt
     * (pc & mask) == (addr & mask) coz pro pc==addr triv. plati). */
    uint16_t end = ( bpt->addr_end >= bpt->addr ) ? bpt->addr_end : bpt->addr;
    if ( !bp_match16 ( bpt->addr_match_mode, pc,
                        bpt->addr, end, bpt->addr_mask ) ) return;

    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    breakpoints_fill_global_ctx ( &ctx );
    ctx.Address = pc;
    ctx.IsExec = true;

    breakpoints_enforce ( bpt, &ctx );
}


/**
 * @brief Vnitřní helper - dispatch na všechny BP daného per-typ idx,
 *        které matchují adresní rozsah/port.
 *
 * Pro MEM_R/W matchuje hit_addr proti bpt->addr_match_mode (SINGLE/RANGE/
 * MASK) přes bp_match16().
 * Pro IORQ_R/W matchuje hit_addr proti bpt->port_match_mode přes bp_match8
 * (8-bit mode, low byte) nebo bp_match16 (16-bit mode, full BC) - viz
 * bpt->port_mode (V1.5.A7).
 * Pro IRQ matchuje vždy (= IRQ BP nemá addr filter).
 */
static void breakpoints_dispatch_addr_list ( en_BPTMAP_TYPE_IDX idx,
                                              uint16_t hit_addr,
                                              uint8_t value,
                                              bool is_read,
                                              bool is_write,
                                              bool is_port ) {
    GArray *list = bptmap_get_list ( idx );
    if ( !list || list->len == 0 ) return;

    /* Iterace po kopii - actions můžou mutovat list (= disable_self
     * volá set_enabled -> unregister). Kopie do stack array do 32 BP,
     * jinak heap fallback. */
    int stack_buf[32];
    int *ids = stack_buf;
    int *heap = NULL;
    unsigned n = list->len;
    if ( n > 32 ) {
        heap = g_new0 ( int, n );
        ids = heap;
    };
    unsigned i;
    for ( i = 0; i < n; i++ ) ids[i] = g_array_index ( list, int, i );

    for ( i = 0; i < n; i++ ) {
        st_BPT *bpt = breakpoints_find_by_id ( ids[i] );
        if ( !bpt ) continue;

        /* Address/port match check (V1.5.E). */
        if ( idx == BPTMAP_IDX_MEM_R || idx == BPTMAP_IDX_MEM_W ) {
            /* RANGE legacy: pokud addr_end < addr (= invalid range, napr.
             * uzivatel nezadal end), pouzijeme addr jako horni bound -
             * stejne jako V1 hi = max(addr, addr_end). */
            uint16_t end = ( bpt->addr_end >= bpt->addr ) ? bpt->addr_end : bpt->addr;
            if ( bpt->zone == BP_ZONE_MMEXT_BANK
                 && bpt->bp_addr_space == BP_ADDR_SPACE_BANK_OFFSET ) {
                /* Feature D: match offset v PEHU bance misto CPU adresy.
                 * Kontrola banky (bank_id/bank_match_mode) zustava na
                 * bp_zone_is_active_at v breakpoints_enforce (ctx->Address =
                 * hit_addr). Pokud write nemiri do PEHU banky, offset = -1
                 * a BP se preskoci. */
                int32_t off = mmext_pehu_offset_from_addr ( hit_addr );
                if ( off < 0 ) continue;
                if ( !bp_match16 ( bpt->addr_match_mode, (uint16_t) off,
                                    bpt->addr, end, bpt->addr_mask ) ) continue;
            } else {
                if ( !bp_match16 ( bpt->addr_match_mode, hit_addr,
                                    bpt->addr, end, bpt->addr_mask ) ) continue;
            }
        } else if ( idx == BPTMAP_IDX_IORQ_R || idx == BPTMAP_IDX_IORQ_W ) {
            /* V1.5.A7 - port mode rozhoduje šířku porovnání:
             *   8BIT  = jen low byte (IN A,(n) / OUT (n),A pattern, B
             *           registr u IN r,(C) je "don't care").
             *   16BIT = plný BC (= IN r,(C) / OUT (C),r pattern, rozliší
             *           např. 0x42CE od 0x88CE).
             *
             * V 8BIT módu cropujeme všechny operandy na 8 bitů a používáme
             * bp_match8 - to je sémanticky čisté (= žádné spurious matches
             * z RANGE/MASK kdy upper byte port_end/port_mask zůstal 0xFF
             * po inicializaci nebo user change). */
            if ( bpt->port_mode == BP_PORT_8BIT ) {
                uint8_t hb = (uint8_t) ( hit_addr & 0xFFu );
                uint8_t pref = (uint8_t) ( bpt->port & 0xFFu );
                uint8_t pend = (uint8_t) ( bpt->port_end & 0xFFu );
                uint8_t pmask = (uint8_t) ( bpt->port_mask & 0xFFu );
                if ( !bp_match8 ( bpt->port_match_mode, hb, pref, pend, pmask ) ) continue;
            } else {
                /* BP_PORT_16BIT - plný 16-bit match přes BC. */
                if ( !bp_match16 ( bpt->port_match_mode, hit_addr,
                                    bpt->port, bpt->port_end, bpt->port_mask ) ) continue;
            };
        };
        /* IRQ: match vždy. */

        bp_expr_ctx_t ctx;
        bp_expr_ctx_zero ( &ctx );
        breakpoints_fill_global_ctx ( &ctx );
        ctx.Address = hit_addr;
        ctx.Value = value;
        ctx.IsRead = is_read;
        ctx.IsWrite = is_write;
        ctx.IsPort = is_port;

        breakpoints_enforce ( bpt, &ctx );

        /* Pokud action zastavila emulátor, zbytek listy nebudeme dál
         * iterovat - úmysl je zastavit teď. Další hit BP zafire při
         * dalším resume. */
        if ( EMULATOR_TEST_PAUSED ) break;
    };

    if ( heap ) g_free ( heap );
}


void breakpoints_enforce_mem_r ( uint16_t addr, uint8_t value ) {
    breakpoints_dispatch_addr_list ( BPTMAP_IDX_MEM_R, addr, value,
                                      true, false, false );
}


void breakpoints_enforce_mem_w ( uint16_t addr, uint8_t value ) {
    breakpoints_dispatch_addr_list ( BPTMAP_IDX_MEM_W, addr, value,
                                      false, true, false );
}


void breakpoints_enforce_iorq_r ( uint16_t port, uint8_t value ) {
    breakpoints_dispatch_addr_list ( BPTMAP_IDX_IORQ_R, port, value,
                                      true, false, true );
}


void breakpoints_enforce_iorq_w ( uint16_t port, uint8_t value ) {
    breakpoints_dispatch_addr_list ( BPTMAP_IDX_IORQ_W, port, value,
                                      false, true, true );
}


void breakpoints_enforce_irq ( uint8_t raised, uint8_t im_mode,
                                uint16_t vector_addr, uint16_t isr_addr,
                                uint8_t int_vector_byte ) {
    /* V1.5.A8 - vlastní iterační smyčka (nepoužívá dispatch_addr_list),
     * protože IRQ BP teď podporuje per-BP vector + ISR filter.
     *
     * V1.5.A8.5: přidán IM mode discriminator (im0/im1/im2_enabled) + IM 0
     * RST opcode filter (im0_rst_mask). Filter pořadí v iteraci:
     *   1. IM mode pass (= aspoň 1 odpovídající IM enabled)
     *   2. IM 0 RST mask (= filter podle int_vector_byte na bus)
     *   3. IM 2 vector / ISR filter (legacy A8)
     *
     * Volání je POST-dispatch (= z mzarch.c po z80_process_interrupt).
     * V IM 0/1 jsou vector_addr a isr_addr nedefinované (caller posílá
     * vector_addr=0, isr_addr=cpu->pc); BP s aktivním IM 2 vector/ISR
     * filtrem NEFIRUJE, BP bez filtru fire vždy. */

    GArray *list = bptmap_get_list ( BPTMAP_IDX_IRQ );
    if ( !list || list->len == 0 ) return;

    /* Kopie ID listy - actions můžou mutovat (disable_self -> unregister). */
    int stack_buf[32];
    int *ids = stack_buf;
    int *heap = NULL;
    unsigned n = list->len;
    if ( n > 32 ) {
        heap = g_new0 ( int, n );
        ids = heap;
    };
    unsigned i;
    for ( i = 0; i < n; i++ ) ids[i] = g_array_index ( list, int, i );

    for ( i = 0; i < n; i++ ) {
        st_BPT *bpt = breakpoints_find_by_id ( ids[i] );
        if ( !bpt ) continue;
        if ( bpt->type != BPT_TYPE_IRQ ) continue;

        /* V1.5.A8.5 - IM mode discriminator. Pokud BP nemá daný IM
         * enabled, BP nefire. Unknown IM (mimo 0/1/2) = no fire. */
        bool im_pass;
        switch ( im_mode ) {
            case 0:  im_pass = bpt->im0_enabled; break;
            case 1:  im_pass = bpt->im1_enabled; break;
            case 2:  im_pass = bpt->im2_enabled; break;
            default: im_pass = false; break;
        };
        if ( !im_pass ) continue;

        /* V1.5.A8.5 - IM 0 RST opcode filter. Aktivní jen pokud
         * im_mode == 0 a mask != 0 (= match-all by 0). int_vector_byte
         * je raw RST opcode na bus (např. 0xFF = RST 38h).
         *
         * Mapping bit pozice: bit i = RST (i*8) = opcode 0xC7 + i*8. */
        if ( im_mode == 0 && bpt->im0_rst_mask != 0 ) {
            int rst_bit = -1;
            switch ( int_vector_byte ) {
                case 0xC7: rst_bit = 0; break; /* RST 00 */
                case 0xCF: rst_bit = 1; break; /* RST 08 */
                case 0xD7: rst_bit = 2; break; /* RST 10 */
                case 0xDF: rst_bit = 3; break; /* RST 18 */
                case 0xE7: rst_bit = 4; break; /* RST 20 */
                case 0xEF: rst_bit = 5; break; /* RST 28 */
                case 0xF7: rst_bit = 6; break; /* RST 30 */
                case 0xFF: rst_bit = 7; break; /* RST 38 */
                default:   rst_bit = -1; break;
            };
            if ( rst_bit < 0 ) continue;
            if ( !( bpt->im0_rst_mask & (uint8_t)( 1 << rst_bit ) ) ) continue;
        };

        /* V1.5.A8 + V1.6+ 4.4 - IM 2 vector / ISR filter (sub-filter pod IM 2).
         *
         * V1.6+ 4.4 Match modes: SINGLE / RANGE / MASK pres bp_match16.
         * Vector page boundary 0xFFFE aplikovan na vector_addr i bpt->im2_vector_addr
         * pred match testem (= bit 0 ignorovan v SINGLE i RANGE i MASK semantics). */
        if ( bpt->im2_vector_enabled ) {
            /* Filter explicit vyžaduje IM 2. */
            if ( im_mode != 2 ) continue;
            uint16_t va = vector_addr & 0xFFFEu;
            uint16_t vref = bpt->im2_vector_addr & 0xFFFEu;
            uint16_t vend = bpt->im2_vector_addr_end & 0xFFFEu;
            if ( !bp_match16 ( bpt->im2_vector_match_mode, va,
                               vref, vend, bpt->im2_vector_mask ) ) continue;
        };
        if ( bpt->im2_isr_enabled ) {
            if ( im_mode != 2 ) continue;
            if ( !bp_match16 ( bpt->im2_isr_match_mode, isr_addr,
                               bpt->im2_isr_addr, bpt->im2_isr_addr_end,
                               bpt->im2_isr_mask ) ) continue;
        };

        bp_expr_ctx_t ctx;
        bp_expr_ctx_zero ( &ctx );
        breakpoints_fill_global_ctx ( &ctx );
        /* Ctx pro IRQ: Address = isr_addr (= kde CPU právě skočil),
         * Value = raised mask (= kdo tahl INT pin před dispatchem).
         * Pre-dispatch verzia plnila Address=0/Value=raised; nyní má
         * Address smysluplnou hodnotu pro condition i log akce. */
        ctx.Address = isr_addr;
        ctx.Value = raised;

        breakpoints_enforce ( bpt, &ctx );

        if ( EMULATOR_TEST_PAUSED ) break;
    };

    if ( heap ) g_free ( heap );
}


/* ========================================================================= */
/*  IRQ_SIG dispatch (V1.5.A8.5 - pre-dispatch peripheral signal)            */
/* ========================================================================= */


void breakpoints_enforce_irq_sig ( uint8_t raised_now, uint8_t raised_prev ) {
    /* Edge raise detect: bity 0 -> 1. Pro fire potřebujeme aspoň jeden
     * newly raised bit, jinak no-op (= žádný edge, žádný BP). */
    uint8_t newly_raised = (uint8_t) ( raised_now & ~raised_prev );
    if ( newly_raised == 0 ) return;

    GArray *list = bptmap_get_list ( BPTMAP_IDX_IRQ_SIG );
    if ( !list || list->len == 0 ) return;

    /* Spočítej active source bitmask z newly_raised bus bitů. PIOZ80
     * sub-detekce A vs B přes g_pioz80.interrupt_port_id (= aktuální
     * port který drží INT pin v daisy chain). */
    uint8_t active_sources = 0;
    if ( newly_raised & MZARCH_INTERRUPT_PIOZ80 ) {
        if ( g_mzhal.have_pioz80 && g_pioz80.interrupt_port_id == PIOZ80_PORT_A ) {
            active_sources |= BP_IRQ_SIG_PIOZ80_PORT_A;
        } else if ( g_mzhal.have_pioz80 && g_pioz80.interrupt_port_id == PIOZ80_PORT_B ) {
            active_sources |= BP_IRQ_SIG_PIOZ80_PORT_B;
        } else {
            /* Race-safe fallback (PIOZ80_PORT_NONE) nebo platforma bez
             * PIO-Z80 - evidujeme jako OTHER (mzhal krok 8). */
            active_sources |= BP_IRQ_SIG_OTHER;
        };
    };
    if ( newly_raised & MZARCH_INTERRUPT_CTC2 ) {
        active_sources |= BP_IRQ_SIG_CTC2;
    };
    if ( newly_raised & MZARCH_INTERRUPT_FDC ) {
        active_sources |= BP_IRQ_SIG_FDC;
    };
    /* Ostatní bity (= mimo známé MZARCH_INTERRUPT_*) = OTHER. */
    uint8_t known_mask = (uint8_t) (
        MZARCH_INTERRUPT_CTC2 | MZARCH_INTERRUPT_PIOZ80 | MZARCH_INTERRUPT_FDC );
    if ( newly_raised & ~known_mask ) {
        active_sources |= BP_IRQ_SIG_OTHER;
    };

    if ( active_sources == 0 ) return;

    /* Kopie ID listy - actions můžou mutovat. */
    int stack_buf[32];
    int *ids = stack_buf;
    int *heap = NULL;
    unsigned n = list->len;
    if ( n > 32 ) {
        heap = g_new0 ( int, n );
        ids = heap;
    };
    unsigned i;
    for ( i = 0; i < n; i++ ) ids[i] = g_array_index ( list, int, i );

    for ( i = 0; i < n; i++ ) {
        st_BPT *bpt = breakpoints_find_by_id ( ids[i] );
        if ( !bpt ) continue;
        if ( bpt->type != BPT_TYPE_IRQ_SIG ) continue;

        /* Match: BP fires pokud aspoň 1 vybraný source byl právě raised.
         * Mask 0 = invalid (UI validation), tady defenzivně skip. */
        if ( bpt->irq_sig_source_mask == 0 ) continue;
        if ( ( bpt->irq_sig_source_mask & active_sources ) == 0 ) continue;

        bp_expr_ctx_t ctx;
        bp_expr_ctx_zero ( &ctx );
        breakpoints_fill_global_ctx ( &ctx );
        /* Ctx pro IRQ_SIG: Address = newly_raised (= edge bus mask),
         * Value = active_sources (= matched source bits). Žádná smysluplná
         * "address"; field použit k logu / debug condition. */
        ctx.Address = newly_raised;
        ctx.Value = active_sources;

        breakpoints_enforce ( bpt, &ctx );

        if ( EMULATOR_TEST_PAUSED ) break;
    };

    if ( heap ) g_free ( heap );
}


/* ========================================================================= */
/*  HW event dispatch (D.3)                                                   */
/* ========================================================================= */


void breakpoints_enforce_hw_event ( en_BP_EVENT event, int32_t value ) {
    if ( event <= BP_EVENT_NONE || event >= BP_EVENT_COUNT ) return;

    en_BP_EVENT_KIND kind = bp_event_get_kind ( event );

    /* Pro signal eventy přečteme prev hodnotu PŘED iterací (= konstantní
     * snapshot pro všechny BP testované v tomto fire). State cache update
     * až po iteraci, aby všechny BP viděly stejný prev->curr přechod. */
    uint8_t prev_state = 0;
    uint8_t curr_state = 0;
    if ( kind == BP_EVT_KIND_SIGNAL ) {
        prev_state = g_bp_event_state [ event ];
        curr_state = (uint8_t) ( value & 1 );
    };

    GArray *list = bptmap_get_list ( BPTMAP_IDX_HW_EVENT );
    if ( !list || list->len == 0 ) {
        /* I když nemáme žádný BP, state cache musíme update aby budoucí
         * registrace nezpožďovala edge detection o jeden fire. */
        if ( kind == BP_EVT_KIND_SIGNAL ) g_bp_event_state [ event ] = curr_state;
        return;
    };

    /* Stejná kopie-pak-iterace jako addr list (= disable_self bezpečnost,
     * action může vyvolat set_enabled -> sync_bptmap -> mutace listu). */
    int stack_buf[32];
    int *ids = stack_buf;
    int *heap = NULL;
    unsigned n = list->len;
    if ( n > 32 ) {
        heap = g_new0 ( int, n );
        ids = heap;
    };
    unsigned i;
    for ( i = 0; i < n; i++ ) ids[i] = g_array_index ( list, int, i );

    for ( i = 0; i < n; i++ ) {
        st_BPT *bpt = breakpoints_find_by_id ( ids[i] );
        if ( !bpt ) continue;
        if ( bpt->type != BPT_TYPE_HW_EVENT ) continue;

        /* Match event identifier. */
        if ( bpt->parsed_event != event ) continue;

        /* Per-kind filter: */
        if ( kind == BP_EVT_KIND_SIGNAL ) {
            /* Trigger condition check podle prev/curr signal level. */
            bool fire = false;
            switch ( bpt->event_trigger ) {
                case BP_EVT_TRIG_LOW:
                    fire = ( curr_state == 0 );
                    break;
                case BP_EVT_TRIG_HIGH:
                    fire = ( curr_state == 1 );
                    break;
                case BP_EVT_TRIG_RISING:
                    fire = ( prev_state == 0 && curr_state == 1 );
                    break;
                case BP_EVT_TRIG_FALLING:
                    fire = ( prev_state == 1 && curr_state == 0 );
                    break;
                case BP_EVT_TRIG_CHANGED:
                    fire = ( prev_state != curr_state );
                    break;
                default:
                    /* Neznámá hodnota = bezpečný default RISING (= legacy). */
                    fire = ( prev_state == 0 && curr_state == 1 );
                    break;
            };
            if ( !fire ) continue;
        } else if ( kind == BP_EVT_KIND_POINT_PARAM ) {
            /* Parametrizovaný point (raster:N): match na konkrétní řádek. */
            if ( bpt->event_param != value ) continue;
        };
        /* CHANGE / POINT_NOPARAM: implicit "happened" - žádný extra check. */

        bp_expr_ctx_t ctx;
        bp_expr_ctx_zero ( &ctx );
        breakpoints_fill_global_ctx ( &ctx );
        /* Ctx pro condition expression:
         *   SIGNAL       - Address = 0, Value = curr_state (0/1)
         *   CHANGE       - Address = 0, Value = nová hodnota (mode/palette/border)
         *   POINT_PARAM  - Address = param (= raster row), Value = 0
         *   POINT_NOPARAM- Address = 0, Value = info data (IM/IFF new value)
         */
        if ( kind == BP_EVT_KIND_SIGNAL ) {
            ctx.Value = curr_state;
        } else if ( kind == BP_EVT_KIND_POINT_PARAM ) {
            ctx.Address = (uint16_t) ( value & 0xFFFF );
        } else {
            /* CHANGE i POINT_NOPARAM - value nese info hodnotu. */
            ctx.Value = (uint16_t) ( value & 0xFFFF );
        };

        breakpoints_enforce ( bpt, &ctx );

        if ( EMULATOR_TEST_PAUSED ) break;
    };

    if ( heap ) g_free ( heap );

    /* Update state cache PO iteraci (= další fire vidí curr jako prev). */
    if ( kind == BP_EVT_KIND_SIGNAL ) {
        g_bp_event_state [ event ] = curr_state;
    };
}


/* ========================================================================= */
/*  SP threshold dispatch (D.4)                                               */
/* ========================================================================= */


void breakpoints_enforce_sp_threshold ( uint16_t old_sp, uint16_t new_sp ) {
    GArray *list = bptmap_get_list ( BPTMAP_IDX_SP_THRESHOLD );
    if ( !list || list->len == 0 ) return;

    /* Stejná kopie-pak-iterace jako u ostatních dispatch funkcí
     * (= disable_self bezpečnost, action může vyvolat set_enabled
     * -> sync_bptmap -> mutace listu). */
    int stack_buf[32];
    int *ids = stack_buf;
    int *heap = NULL;
    unsigned n = list->len;
    if ( n > 32 ) {
        heap = g_new0 ( int, n );
        ids = heap;
    };
    unsigned i;
    for ( i = 0; i < n; i++ ) ids[i] = g_array_index ( list, int, i );

    for ( i = 0; i < n; i++ ) {
        st_BPT *bpt = breakpoints_find_by_id ( ids[i] );
        if ( !bpt ) continue;
        if ( bpt->type != BPT_TYPE_SP_THRESHOLD ) continue;

        /* V1.5.E - dva mody:
         *   SINGLE: Edge crossing dolů: prev >= threshold && curr < threshold.
         *           Tento test odfiltruje jak "už byl pod prahem" (žádný edge),
         *           tak "stoupá" (no-op). Speciální case old_sp == new_sp by
         *           neměl nastat (caller nevolá pokud SP nezměnil hodnotu),
         *           ale bezpečně se vyhodnotí jako no-edge.
         *   WINDOW: Edge transition outside [sp_threshold..sp_upper]: byl uvnitr
         *           a ted je venku. Edge-triggered = nezpamuje pri sustained
         *           outside (prevent startup spam pri pre-existujicim outside SP).
         */
        if ( bpt->sp_mode == BP_SP_SINGLE ) {
            if ( !( old_sp >= bpt->sp_threshold && new_sp < bpt->sp_threshold ) )
                continue;
        } else { /* BP_SP_WINDOW */
            uint16_t lo = bpt->sp_threshold;
            uint16_t hi = bpt->sp_upper;
            /* Pokud user zadal hi < lo (UI by mela validovat), prohodime
             * pro robustnost - jinak by bylo "vse outside" a BP by hned
             * spamoval. */
            if ( hi < lo ) { uint16_t t = lo; lo = hi; hi = t; };
            bool was_inside = ( old_sp >= lo && old_sp <= hi );
            bool is_outside = ( new_sp <  lo || new_sp >  hi );
            if ( !( was_inside && is_outside ) ) continue;
        };

        bp_expr_ctx_t ctx;
        bp_expr_ctx_zero ( &ctx );
        breakpoints_fill_global_ctx ( &ctx );
        /* V1 ctx: Address = new_sp (aktuální SP), Value = threshold
         * (= snadný přístup pro action skripty: log "depth=%d", base-Address). */
        ctx.Address = new_sp;
        ctx.Value = bpt->sp_threshold;

        breakpoints_enforce ( bpt, &ctx );

        if ( EMULATOR_TEST_PAUSED ) break;
    };

    if ( heap ) g_free ( heap );
}


/**
 * @brief Per-BP storage pro edge-triggered tracking (D.2).
 *
 * V1 implementace: GHashTable<bp_id, last_result>. Edge BP fires jen
 * když condition se změnila false -> true mezi dvěma hookovými cykly.
 * Storage je interní static, lazy alokace.
 */
static GHashTable *s_global_edge_state = NULL;


/**
 * @brief Pomůcka pro edge-triggered: vrátí předchozí výsledek (0/1)
 *        a uloží nový.
 *
 * @param bp_id ID breakpointu.
 * @param current Aktuální condition výsledek (true = condition fired).
 * @return Předchozí výsledek (false = první vyhodnocení nebo BP neexistuje).
 */
static bool breakpoints_edge_swap ( int bp_id, bool current ) {
    if ( !s_global_edge_state ) {
        s_global_edge_state = g_hash_table_new ( g_direct_hash, g_direct_equal );
    };
    gpointer key = GINT_TO_POINTER ( bp_id );
    bool prev = ( g_hash_table_lookup ( s_global_edge_state, key ) != NULL );
    if ( current ) {
        g_hash_table_insert ( s_global_edge_state, key, GINT_TO_POINTER ( 1 ) );
    } else {
        g_hash_table_remove ( s_global_edge_state, key );
    };
    return prev;
}


void breakpoints_enforce_global ( void ) {
    GArray *list = bptmap_get_list ( BPTMAP_IDX_GLOBAL );
    if ( !list || list->len == 0 ) return;

    /* Stejná kopie-pak-iterace jako addr list (= disable_self bezpečnost). */
    int stack_buf[32];
    int *ids = stack_buf;
    int *heap = NULL;
    unsigned n = list->len;
    if ( n > 32 ) {
        heap = g_new0 ( int, n );
        ids = heap;
    };
    unsigned i;
    for ( i = 0; i < n; i++ ) ids[i] = g_array_index ( list, int, i );

    for ( i = 0; i < n; i++ ) {
        st_BPT *bpt = breakpoints_find_by_id ( ids[i] );
        if ( !bpt ) continue;

        bp_expr_ctx_t ctx;
        bp_expr_ctx_zero ( &ctx );
        breakpoints_fill_global_ctx ( &ctx );
        /* Žádná Address/Value/IsRead/Write/Exec - GLOBAL je condition-only. */

        if ( bpt->edge_triggered ) {
            /* Edge sémantika: spočti condition manuálně, srovnej s prev,
             * fire jen na prev=false, current=true. Nechat enforce projít
             * jen v takovém případě.
             *
             * Pozn: Toto duplicitně vyhodnotí condition (jednou tady, znovu
             * v breakpoints_enforce). V1 = OK, optim do V1.5. */
            bool current = breakpoints_eval_condition ( bpt, &ctx );
            bool prev = breakpoints_edge_swap ( bpt->id, current );
            if ( !( current && !prev ) ) continue; /* žádný edge */
        };

        breakpoints_enforce ( bpt, &ctx );

        if ( EMULATOR_TEST_PAUSED ) break;
    };

    if ( heap ) g_free ( heap );
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
