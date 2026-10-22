/**
 * @file   coverage_hook.c
 * @brief  Napojení pokrytí kódu na emulátor - implementace (viz coverage_hook.h).
 *
 * @par Licence: GPLv3
 */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "coverage_hook.h"

#include <glib.h>

#include "debugger/debugger.h"
#include "debugger/trace/ramlog.h"
#include "debugger/trace/ramlog_hook.h"
#include "hw-generic/memory/memory.h"
#include "mzarch/mzarch_platform_functions.h"
#include "coverage_export.h"
#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/sdlapp/sdlapp_options.h"

#include <stdio.h>


/* coverage_space_name() (coverage.c) pojmenovává prostory v pořadí en_RAMLOG_SPACE. */
G_STATIC_ASSERT ( RAMLOG_SPACE_RAM == 0 && RAMLOG_SPACE_VRAM == 1 && RAMLOG_SPACE_PCG == 2
                  && RAMLOG_SPACE_MEMEXT == 3 && RAMLOG_SPACE_ROM == 4 );


int g_coverage_need_fetch = 0;
uint16_t g_coverage_pending_flags = 0;


/** @brief Kontext čtení bajtů kroku. */
typedef struct st_CH_READ {
    uint16_t pc;   /**< Adresa začátku kroku. */
    bool fallback; /**< Některý bajt nebyl v záznamu fetch. */
} st_CH_READ;


/**
 * @brief Bajt kroku z paměti po kroku (úroveň branch).
 * @param ctx   st_CH_READ.
 * @param index Pozice bajtu v kroku.
 * @return Bajt z aktuálního mapování, bez vedlejších efektů.
 */
static uint8_t ch_read_mem ( void *ctx, unsigned index )
{
    const st_CH_READ *c = (const st_CH_READ *) ctx;
    return memory_read_byte_no_se ( (uint16_t) ( c->pc + index ) );
}


/**
 * @brief Bajt kroku ze záznamu fetch (úroveň detail), jinak z paměti.
 * @param ctx   st_CH_READ; při čtení mimo záznam nastaví `fallback`.
 * @param index Pozice bajtu v kroku.
 * @return Bajt, jak ho CPU načetlo, nebo z paměti po kroku.
 */
static uint8_t ch_read_fetch ( void *ctx, unsigned index )
{
    st_CH_READ *c = (st_CH_READ *) ctx;
    if ( g_debugger_insn_fetch.addr == c->pc && index < g_debugger_insn_fetch.count ) {
        return g_debugger_insn_fetch.byte[ index ];
    };
    c->fallback = true;
    return memory_read_byte_no_se ( (uint16_t) ( c->pc + index ) );
}


/**
 * @brief Fyzická buňka bajtu na CPU adrese v aktuálním mapování.
 * @param addr  CPU adresa.
 * @param flags Příznaky kroku; bez fyzické buňky dostanou COVERAGE_F_CELL_BUS.
 * @return Buňka (COVERAGE_CELL).
 */
static uint32_t ch_cell ( uint16_t addr, uint16_t *flags )
{
    st_RAMLOG_TARGET t[ RAMLOG_TARGETS_MAX ];
    bool mmio = false;
    unsigned n = ramlog_platform_targets ( addr, t, &mmio );
    if ( mmio || n == 0 || t[ 0 ].has_plane ) {
        *flags |= COVERAGE_F_CELL_BUS;
        return COVERAGE_CELL ( COVERAGE_SPACE_BUS, addr );
    };
    if ( t[ 0 ].space == RAMLOG_SPACE_RAM || t[ 0 ].space == RAMLOG_SPACE_MEMEXT ) {
        /* ramlog bere stránku pro zápis; kód se čte stránkou pro čtení
         * (s MemExt se mohou lišit). */
        const uint8_t *p = g_memory.memram_read[ addr >> 12 ] + ( addr & 0x0FFFu );
        uint8_t space = 0;
        uint32_t offset = 0;
        if ( !ramlog_platform_ram_cell ( p, &space, &offset ) ) {
            *flags |= COVERAGE_F_CELL_BUS;
            return COVERAGE_CELL ( COVERAGE_SPACE_BUS, addr );
        };
        return COVERAGE_CELL ( space, offset );
    };
    return COVERAGE_CELL ( t[ 0 ].space, t[ 0 ].offset );
}


/**
 * @brief Buňka jedné 4 KiB stránky CPU v mapování platném při výpočtu.
 *
 * @invariant Je-li @c valid, pak pro každou adresu stránky mimo okno
 *            E000-E008 platí ch_cell(addr) == base + (addr & 0x0FFF)
 *            a příznaky @c flags, dokud se nezmění @c map_key ani @c rd.
 */
typedef struct st_CH_PAGE {
    const uint8_t *rd; /**< g_memory.memram_read[stránka] při výpočtu. */
    uint32_t map_key;  /**< ramlog_platform_map_key() při výpočtu. */
    uint32_t base;     /**< Buňka prvního bajtu stránky. */
    uint16_t flags;    /**< COVERAGE_F_CELL_BUS, nebo 0. */
    bool valid;        /**< Záznam platí. */
} st_CH_PAGE;

/** Cache buněk po stránkách (úroveň detail), jen emu vlákno. */
static st_CH_PAGE s_ch_page[ 16 ];


/**
 * @brief Fyzická buňka bajtu na CPU adrese přes cache po stránkách.
 *
 * Výsledek je stejný jako ch_cell(), ta se ale volá jen při první adrese
 * stránky po změně mapování (klíč ramlog_platform_map_key() nebo ukazatel
 * memram_read stránky). Buňka je v rámci stránky lineární ve všech
 * prostorech (RAM, MemExt, ROM, VRAM, CG-RAM, PCG, sběrnice); výjimkou je
 * okno portů E000-E008, které jde vždy přes ch_cell().
 *
 * @param addr  CPU adresa.
 * @param flags Příznaky kroku; bez fyzické buňky dostanou COVERAGE_F_CELL_BUS.
 * @return Buňka (COVERAGE_CELL).
 */
static uint32_t ch_cell_cached ( uint16_t addr, uint16_t *flags )
{
    if ( addr >= 0xE000u && addr <= 0xE008u ) {
        return ch_cell ( addr, flags );
    };
    st_CH_PAGE *pg = &s_ch_page[ addr >> 12 ];
    const uint8_t *rd = g_memory.memram_read[ addr >> 12 ];
    uint32_t key = ramlog_platform_map_key ( );
    if ( !pg->valid || pg->map_key != key || pg->rd != rd ) {
        /* Poslední bajt stránky: nikdy neleží v okně E000-E008. */
        uint16_t f = 0;
        uint32_t last = ch_cell ( (uint16_t) ( addr | 0x0FFFu ), &f );
        pg->base = last - 0x0FFFu;
        pg->flags = f;
        pg->rd = rd;
        pg->map_key = key;
        pg->valid = true;
    };
    *flags |= pg->flags;
    return pg->base + ( addr & 0x0FFFu );
}


/** @brief Přepočítá příznak logovacích callbacků, masku háčků a callbacky CPU. */
static void ch_apply ( void )
{
    st_COVERAGE_STATUS st;
    coverage_get_status ( &st );
    g_coverage_need_fetch = ( st.active && st.level == COVERAGE_LEVEL_DETAIL ) ? 1 : 0;
    g_coverage_pending_flags = 0;
    mzarch_post_insn_hooks_recompute ( );
    mzarch_platform_fn_debugger_state_changed ( TEST_DEBUGGER_ACTIVE );
}


bool coverage_hook_start ( const st_COVERAGE_CONFIG *cfg, bool reset )
{
    bool ok = coverage_start ( cfg, reset );
    ch_apply ( );
    return ok;
}


void coverage_hook_stop ( void )
{
    coverage_stop ( );
    ch_apply ( );
}


void coverage_hook_on_instruction ( const st_MZARCH_INSN_INFO *insn )
{
    const z80_t *cpu = g_mzarch_main.cpu;
    st_CH_READ ctx = { insn->pc, false };
    st_COVERAGE_STEP s = {
        .pc = insn->pc,
        .next_pc = insn->next_pc,
        .f = cpu->af.l,
        .b = cpu->bc.h,
        .halted = cpu->halted,
        .flags = g_coverage_pending_flags,
        .read_ctx = &ctx,
    };
    g_coverage_pending_flags = 0;
    if ( g_coverage_need_fetch ) {
        s.read = ch_read_fetch;
        s.fallback = &ctx.fallback;
        s.cell = ch_cell_cached ( insn->pc, &s.flags );
    } else {
        s.read = ch_read_mem;
    };
    coverage_on_step ( &s );
}


void coverage_hook_note_cmt_trap ( void )
{
    if ( g_coverage_active ) {
        g_coverage_pending_flags |= COVERAGE_F_CMT_TRAP;
    };
}





/* ---------------- Konfigurace (INI [COVERAGE], CLI) ---------------- */

st_COVERAGE_INI g_coverage_ini = {
    COVERAGE_CFG_MODE_OFF, COVERAGE_KEY_OPCODE, COVERAGE_DEFAULT_CAPACITY, 0, NULL
};

/** Konfigurace už sběr spustila (nebo se o to pokusila); spouští se jen jednou. */
static bool s_cfg_done = false;


void coverage_cfg_init ( void )
{
    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "COVERAGE" );
    CFGELM *elm;

    elm = cfgmodule_register_new_element ( cmod, "mode", CFGENTYPE_KEYWORD, COVERAGE_CFG_MODE_OFF,
                                           COVERAGE_CFG_MODE_OFF,    "OFF",
                                           COVERAGE_CFG_MODE_BRANCH, "BRANCH",
                                           COVERAGE_CFG_MODE_DETAIL, "DETAIL",
                                           -1 );
    cfgelement_set_handlers ( elm, (void *) &g_coverage_ini.mode, (void *) &g_coverage_ini.mode );

    elm = cfgmodule_register_new_element ( cmod, "key", CFGENTYPE_KEYWORD, COVERAGE_KEY_OPCODE,
                                           COVERAGE_KEY_OPCODE,     "OPCODE",
                                           COVERAGE_KEY_FULL_BYTES, "FULL_BYTES",
                                           -1 );
    cfgelement_set_handlers ( elm, (void *) &g_coverage_ini.key, (void *) &g_coverage_ini.key );

    elm = cfgmodule_register_new_element ( cmod, "capacity", CFGENTYPE_UNSIGNED, COVERAGE_DEFAULT_CAPACITY,
                                           1, COVERAGE_MAX_CAPACITY );
    cfgelement_set_handlers ( elm, (void *) &g_coverage_ini.capacity, (void *) &g_coverage_ini.capacity );

    elm = cfgmodule_register_new_element ( cmod, "reset_on_discontinuity", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void *) &g_coverage_ini.reset_on_discontinuity,
                              (void *) &g_coverage_ini.reset_on_discontinuity );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


void coverage_apply_cli_options ( void )
{
    const char *v;

    v = sdlapp_option_value ( "--coverage" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_coverage_ini.mode = COVERAGE_CFG_MODE_OFF;
        else if ( g_ascii_strcasecmp ( v, "branch" ) == 0 ) g_coverage_ini.mode = COVERAGE_CFG_MODE_BRANCH;
        else if ( g_ascii_strcasecmp ( v, "detail" ) == 0 ) g_coverage_ini.mode = COVERAGE_CFG_MODE_DETAIL;
        else fprintf ( stderr, "Invalid --coverage value: %s (expected off|branch|detail)\n", v );
    };
    v = sdlapp_option_value ( "--coverage-key" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "opcode" ) == 0 ) g_coverage_ini.key = COVERAGE_KEY_OPCODE;
        else if ( g_ascii_strcasecmp ( v, "full_bytes" ) == 0 ) g_coverage_ini.key = COVERAGE_KEY_FULL_BYTES;
        else fprintf ( stderr, "Invalid --coverage-key value: %s (expected opcode|full_bytes)\n", v );
    };
    v = sdlapp_option_value ( "--coverage-capacity" );
    if ( v ) {
        guint64 c = g_ascii_strtoull ( v, NULL, 0 );
        if ( c < 1 || c > COVERAGE_MAX_CAPACITY ) {
            fprintf ( stderr, "Invalid --coverage-capacity value: %s (expected 1..%u)\n", v, COVERAGE_MAX_CAPACITY );
        } else {
            g_coverage_ini.capacity = (unsigned) c;
        };
    };
    g_free ( g_coverage_ini.save_on_exit );
    g_coverage_ini.save_on_exit = mzarch_cli_option_value_utf8 ( "--coverage-save-on-exit" );
}


void coverage_recompute_active ( void )
{
    if ( s_cfg_done || g_coverage_ini.mode == COVERAGE_CFG_MODE_OFF ) return;
    s_cfg_done = true;
    st_COVERAGE_CONFIG cfg = {
        .level = ( g_coverage_ini.mode == COVERAGE_CFG_MODE_DETAIL ) ? COVERAGE_LEVEL_DETAIL : COVERAGE_LEVEL_BRANCH,
        .key = (en_COVERAGE_KEY) g_coverage_ini.key,
        .capacity = g_coverage_ini.capacity,
        .reset_on_discontinuity = g_coverage_ini.reset_on_discontinuity != 0,
    };
    if ( !coverage_start ( &cfg, true ) ) {
        fprintf ( stderr, "[COVERAGE] start from configuration failed (out of memory)\n" );
        return;
    };
    g_coverage_need_fetch = ( cfg.level == COVERAGE_LEVEL_DETAIL ) ? 1 : 0;
    g_coverage_pending_flags = 0;
    mzarch_post_insn_hooks_recompute ( );
}


void coverage_on_exit ( void )
{
    if ( !g_coverage_ini.save_on_exit || !g_coverage_ini.save_on_exit[ 0 ] ) return;
    /* Relativní cesta vůči aktuálnímu adresáři procesu, jako u ostatních
     * voleb příkazové řádky se souborem (--result-json, --record, ...);
     * --work-dir platí pro dialogy a výstupní adresáře debuggeru z INI. */
    char *path = g_canonicalize_filename ( g_coverage_ini.save_on_exit, NULL );
    st_COVERAGE_SNAPSHOT *s = g_new0 ( st_COVERAGE_SNAPSHOT, 1 );
    if ( !coverage_snapshot_take ( s, true ) ) {
        fprintf ( stderr, "[COVERAGE] not saved to '%s': out of memory\n", path );
    } else if ( !s->status.has_data ) {
        fprintf ( stderr, "[COVERAGE] not saved to '%s': no coverage data\n", path );
    } else {
        size_t rows = 0;
        char *err = NULL;
        bool csv = g_str_has_suffix ( path, ".csv" ) || g_str_has_suffix ( path, ".CSV" );
        if ( coverage_export_write ( s, path, csv, &rows, &err ) ) {
            fprintf ( stderr, "[COVERAGE] saved %zu rows to '%s'\n", rows, path );
        } else {
            fprintf ( stderr, "[COVERAGE] %s\n", err ? err : "save failed" );
            g_free ( err );
        };
    };
    coverage_snapshot_clear ( s );
    g_free ( s );
    g_free ( path );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
