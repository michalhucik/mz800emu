/**
 * @file   intlog.c
 * @brief  Implementace Interrupt Log subsystému trace-suite.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "intlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "libs/cfgfile/cfgmodule.h"
#include "emulator/cfgmain.h"
#include "emulator/debugger/trace/tlog_common.h"
#include "emulator/debugger/trace/reclife.h"
#include "emulator/debugger/trace/eventlog.h"
#include "emulator/mzarch/mzarch.h"
#include "emulator/mzarch/mzhal.h"
#include "hw-generic/pioz80/pioz80.h"

st_INTLOG_CONFIG g_intlog_config;
int g_intlog_active = 0;

static st_TLOG_WRITER s_writer;
static int s_writer_open = 0;

static uint8_t s_prev_interrupt = 0;
/* Předchozí masked_input pro PA / PB - per-bit detekce hran. */
static uint8_t s_prev_pioz80_pa_masked = 0;
static uint8_t s_prev_pioz80_pb_masked = 0;
/* Předchozí PIO_STATE bitmaska (READY+ARMED) pro detekci change. */
static uint32_t s_prev_pio_state_bits = 0;

/**
 * @brief Mapování @ref en_INTLOG_EVENT_CLASS na @ref en_EVENTLOG_CATEGORY.
 *
 * Helper pro fan-out emit do in-memory eventlog ringu. Pro
 * @c INTLOG_EVENT_PIO_STATE se vrací @c EVENTLOG_CAT_COUNT (PIO state
 * není samostatná Event Viewer kategorie - PIOZ80 detail se vidí přes
 * hwlog PIOZ80 cestu).
 */
static inline uint8_t intlog_class_to_eventlog_cat ( en_INTLOG_EVENT_CLASS cls )
{
    switch ( cls ) {
        case INTLOG_EVENT_PIN_EDGE:      return EVENTLOG_CAT_CPU_PIN_EDGE;
        case INTLOG_EVENT_CPU_INT_STATE: return EVENTLOG_CAT_CPU_INT;
        case INTLOG_EVENT_IRQ_ACK_IM2:   return EVENTLOG_CAT_IRQ_ACK_IM2;
        case INTLOG_EVENT_PIO_STATE:
        default:                         return (uint8_t) EVENTLOG_CAT_COUNT;
    }
}


/**
 * @brief Společný emit per-event do writeru + fan-out do eventlog ringu.
 *
 * Side effects:
 *  - Pokud @c s_writer_open, zapíše 24 B do tlog disk chunku.
 *  - Pokud @ref TEST_TRACE_EVENTLOG_ACTIVE a kategorie je v
 *    @ref g_eventlog_active_mask, zapíše paralelně 24 B do in-memory
 *    eventlog ringu. PC se dotahuje z @c g_mzarch_main.cpu->pc; pro
 *    PIN_EDGE eventy je PC ignorovatelné z UI hlediska (zdroj signálu
 *    je externí HW edge), ale konzistentně zapisujeme aktuální CPU PC
 *    (= "kdy" emit proběhl v běhu programu).
 *
 *  - PIN_EDGE encoded payload     = (uint32_t)((pin << 8) | edge)
 *                                   + horní 16 bitů = source_chip (Sub byte
 *                                   v eventlog je redundantně source_chip).
 *  - CPU_INT_STATE encoded        = state_bits (přímo, 32 b bitmask).
 *  - IRQ_ACK_IM2 encoded          = state_bits (LE 16 b vector + 16 b ISR
 *                                   addr, identické s intlog stávajícím
 *                                   layout).
 */
static void emit_event ( en_INTLOG_EVENT_CLASS cls,
                         en_INTLOG_SOURCE_CHIP src, uint8_t pin,
                         en_INTLOG_EDGE edge, uint32_t state_bits )
{
    /* Cesta 1: existující tlog disk write. */
    if ( s_writer_open ) {
        uint64_t pxclk_total = tlog_common_get_pxclk_total ( );
        uint32_t screens = tlog_common_get_screens_total ( );
        uint32_t pxclk_in = tlog_common_get_pxclk_in_screen ( );

        uint8_t buf[ 24 ];
        for ( int i = 0; i < 8; i++ ) buf[ i ] = (uint8_t)( pxclk_total >> ( i * 8 ) );
        for ( int i = 0; i < 4; i++ ) buf[ 8 + i ] = (uint8_t)( screens >> ( i * 8 ) );
        for ( int i = 0; i < 4; i++ ) buf[ 12 + i ] = (uint8_t)( pxclk_in >> ( i * 8 ) );
        buf[ 16 ] = (uint8_t) cls;
        buf[ 17 ] = (uint8_t) src;
        buf[ 18 ] = pin;
        buf[ 19 ] = (uint8_t) edge;
        for ( int i = 0; i < 4; i++ ) buf[ 20 + i ] = (uint8_t)( state_bits >> ( i * 8 ) );

        tlog_writer_append ( &s_writer, buf, sizeof ( buf ) );
    }

    /* Cesta 2: paralelní fan-out do in-memory eventlog ringu. */
    if ( TEST_TRACE_EVENTLOG_ACTIVE ) {
        uint8_t cat = intlog_class_to_eventlog_cat ( cls );
        if ( cat < (uint8_t) EVENTLOG_CAT_COUNT
             && ( g_eventlog_active_mask & ( 1ULL << cat ) ) ) {
            uint32_t pl;
            uint8_t sub;
            switch ( cls ) {
                case INTLOG_EVENT_PIN_EDGE:
                    /* Subtype = source_chip (CTC2, PIOZ80, FDC, PIOZ80@PAx/PBx)
                     * - UI dekódér rozezná zdroj. Payload low B = pin, B+1 = edge. */
                    sub = (uint8_t) src;
                    pl = (uint32_t) pin | ( (uint32_t) edge << 8 );
                    break;
                case INTLOG_EVENT_CPU_INT_STATE:
                    /* Subtype = 0 (state record), payload = bitmask. */
                    sub = 0;
                    pl = state_bits;
                    break;
                case INTLOG_EVENT_IRQ_ACK_IM2:
                    /* Subtype = source_chip, payload = encoded vector|isr. */
                    sub = (uint8_t) src;
                    pl = state_bits;
                    break;
                default:
                    sub = 0;
                    pl = 0;
                    break;
            }
            uint16_t pc = (uint16_t) g_mzarch_main.cpu->pc;
            eventlog_record ( cat, sub, pc, pl );
        }
    }
}


void intlog_record_pin_edge ( en_INTLOG_SOURCE_CHIP src, uint8_t pin,
                              en_INTLOG_EDGE edge )
{
    emit_event ( INTLOG_EVENT_PIN_EDGE, src, pin, edge, 0 );
}


void intlog_record_cpu_int_state ( uint32_t state_bits )
{
    emit_event ( INTLOG_EVENT_CPU_INT_STATE, INTLOG_CHIP_CPU, 0,
                 INTLOG_EDGE_NONE, state_bits );
}


void intlog_record_pio_state ( uint32_t state_bits )
{
    emit_event ( INTLOG_EVENT_PIO_STATE, INTLOG_CHIP_PIOZ80, 0,
                 INTLOG_EDGE_NONE, state_bits );
}


void intlog_record_irq_ack_im2 ( en_INTLOG_SOURCE_CHIP src,
                                 uint16_t vector_table_addr,
                                 uint16_t isr_addr )
{
    /* Encode 32 bit payload do reused state_bitmask pole:
     *  low 16 bit = vector_table_addr (entry adresa v IM 2 tabulce)
     *  high 16 bit = isr_addr (16-bit ISR adresa, nahrávaná do PC) */
    uint32_t encoded = (uint32_t) vector_table_addr
                     | ( (uint32_t) isr_addr << 16 );
    emit_event ( INTLOG_EVENT_IRQ_ACK_IM2, src, 0, INTLOG_EDGE_NONE, encoded );
}


/**
 * @brief Pomocná funkce - per-bit detekce hran v 8-bit masce.
 *
 * Pro každý změněný bit emit @c intlog_record_pin_edge s @p src + offset
 * pin pos a edge dle nové úrovně.
 */
static void emit_pin_edges_for_mask ( en_INTLOG_SOURCE_CHIP src_base,
                                      uint8_t cur, uint8_t prev )
{
    uint8_t diff = cur ^ prev;
    if ( diff == 0 ) return;
    for ( uint8_t pin = 0; pin < 8; pin++ ) {
        uint8_t bit = (uint8_t)( 1u << pin );
        if ( diff & bit ) {
            intlog_record_pin_edge ( (en_INTLOG_SOURCE_CHIP)( src_base + pin ),
                                     pin,
                                     ( cur & bit ) ? INTLOG_EDGE_RISING : INTLOG_EDGE_FALLING );
        }
    }
}


void intlog_poll_interrupt_state ( uint8_t current_interrupt )
{
    if ( !s_writer_open ) return;
    uint8_t diff = current_interrupt ^ s_prev_interrupt;

    /* Per-bit detect rising/falling globálních INT pinů. Bit 0 = CTC2,
     * bit 1 = PIOZ80 (souhrn), bit 2 = FDC. */
    if ( diff & 0x01 ) {
        intlog_record_pin_edge ( INTLOG_CHIP_CTC2, 0,
                                 ( current_interrupt & 0x01 ) ? INTLOG_EDGE_RISING : INTLOG_EDGE_FALLING );
    }
    if ( diff & 0x02 ) {
        intlog_record_pin_edge ( INTLOG_CHIP_PIOZ80, 0,
                                 ( current_interrupt & 0x02 ) ? INTLOG_EDGE_RISING : INTLOG_EDGE_FALLING );
    }
    if ( diff & 0x04 ) {
        intlog_record_pin_edge ( INTLOG_CHIP_FDC, 0,
                                 ( current_interrupt & 0x04 ) ? INTLOG_EDGE_RISING : INTLOG_EDGE_FALLING );
    }
    s_prev_interrupt = current_interrupt;

    if (g_mzhal.have_pioz80) { /* runtime capability (mzhal krok 8) */
    /* Per-pin granularita PIOZ80@P[A,B][0..7]. masked_input je interní stav
     * po io_mask + invert (iclvl) + filter (icmask) - reflektuje ty piny,
     * které "by triggerovaly INT" v aktuální konfiguraci. Diff vs předchozí
     * snapshot dá hrany. */
    uint8_t pa = g_pioz80.port[ PIOZ80_PORT_A ].masked_input;
    uint8_t pb = g_pioz80.port[ PIOZ80_PORT_B ].masked_input;
    emit_pin_edges_for_mask ( INTLOG_CHIP_PIOZ80_PA0, pa, s_prev_pioz80_pa_masked );
    emit_pin_edges_for_mask ( INTLOG_CHIP_PIOZ80_PB0, pb, s_prev_pioz80_pb_masked );
    s_prev_pioz80_pa_masked = pa;
    s_prev_pioz80_pb_masked = pb;

    /* PIO_STATE bitmaska:
     *  - PIO_READY  - g_pioz80.interrupt v PENDING/RECEIVED stavu (= IRQ
     *                 visí na sběrnici nebo se odbavuje)
     *  - PIO_ARMED  - libovolný port má icena = ENABLED (= IRQ povolen) */
    uint32_t state = 0;
    if ( g_pioz80.interrupt & PIOZ80_INTERRUPT_INT_BIT ) {
        state |= INTLOG_STATE_BIT_PIO_READY;
    }
    if ( g_pioz80.port[ PIOZ80_PORT_A ].icena == PIOZ80_ICENA_ENABLED
         || g_pioz80.port[ PIOZ80_PORT_B ].icena == PIOZ80_ICENA_ENABLED ) {
        state |= INTLOG_STATE_BIT_PIO_ARMED;
    }
    if ( state != s_prev_pio_state_bits ) {
        intlog_record_pio_state ( state );
        s_prev_pio_state_bits = state;
    }
    }
}


/* ===========================================================================
 *  Lifecycle
 * =========================================================================== */

static char *build_subsys_header_json ( void )
{
    JsonBuilder *b = json_builder_new ( );
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "start_pxclk" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_total ( ) );

    json_builder_set_member_name ( b, "start_pxclk_in_screen" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_in_screen ( ) );

    json_builder_set_member_name ( b, "start_screens" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_screens_total ( ) );

    json_builder_set_member_name ( b, "start_cpuclk" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_cpuclk_total ( ) );

    json_builder_set_member_name ( b, "event_record_size" );
    json_builder_add_int_value ( b, 24 );

    json_builder_set_member_name ( b, "initial_interrupt_bus" );
    json_builder_add_int_value ( b, s_prev_interrupt );

    if (g_mzhal.have_pioz80) { /* runtime capability (mzhal krok 8) */
    /* PIOZ80 baseline (snapshot interních registrů) - umožní externí
     * re-simulaci interrupt subsystému start od přesného stavu. */
    json_builder_set_member_name ( b, "initial_pioz80" );
    json_builder_begin_object ( b );

#define ADD_PIO_INT( name_str, value_expr ) \
    do { json_builder_set_member_name ( b, name_str ); \
         json_builder_add_int_value ( b, (gint64) ( value_expr ) ); } while (0)

    ADD_PIO_INT ( "pa_masked_input", g_pioz80.port[ PIOZ80_PORT_A ].masked_input );
    ADD_PIO_INT ( "pb_masked_input", g_pioz80.port[ PIOZ80_PORT_B ].masked_input );
    ADD_PIO_INT ( "pa_icmask",       g_pioz80.port[ PIOZ80_PORT_A ].icmask );
    ADD_PIO_INT ( "pb_icmask",       g_pioz80.port[ PIOZ80_PORT_B ].icmask );
    ADD_PIO_INT ( "pa_io_mask",      g_pioz80.port[ PIOZ80_PORT_A ].io_mask );
    ADD_PIO_INT ( "pb_io_mask",      g_pioz80.port[ PIOZ80_PORT_B ].io_mask );
    ADD_PIO_INT ( "pa_icena",        (unsigned) g_pioz80.port[ PIOZ80_PORT_A ].icena );
    ADD_PIO_INT ( "pb_icena",        (unsigned) g_pioz80.port[ PIOZ80_PORT_B ].icena );
    ADD_PIO_INT ( "interrupt",       (unsigned) g_pioz80.interrupt );

#undef ADD_PIO_INT
    json_builder_end_object ( b );
    }

    json_builder_end_object ( b );

    JsonGenerator *gen = json_generator_new ( );
    JsonNode *root = json_builder_get_root ( b );
    json_generator_set_root ( gen, root );
    json_generator_set_pretty ( gen, TRUE );
    json_generator_set_indent ( gen, 2 );

    gsize len = 0;
    gchar *out = json_generator_to_data ( gen, &len );

    g_object_unref ( gen );
    g_object_unref ( b );

    return out;
}


int intlog_start ( void )
{
    if ( s_writer_open ) return 0;
    if ( !g_intlog_config.dir || !g_intlog_config.dir[0]
         || !g_intlog_config.name || !g_intlog_config.name[0] ) {
        return -1;
    }
    char *resolved_dir = sdlapp_paths_resolve_work ( g_sdlapp->paths, g_intlog_config.dir );
    uint64_t init_px = tlog_common_get_pxclk_total ( );
    uint64_t init_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t init_sc = tlog_common_get_screens_total ( );
    if ( tlog_writer_open ( &s_writer, "intlog", resolved_dir, g_intlog_config.name,
                            g_intlog_config.chunk_mb, g_intlog_config.max_total_mb,
                            init_px, init_cpu, init_sc ) != 0 ) {
        g_free ( resolved_dir );
        return -1;
    }
    s_writer_open = 1;
    s_prev_interrupt = 0;
    if (g_mzhal.have_pioz80) { /* runtime capability (mzhal krok 8) */
    /* Inicializace per-pin PIOZ80 baseline z aktuálního masked_input -
     * první poll po startu nevygeneruje "false edges" z dříve nastavených
     * stavů (= eventy logujeme jen pro skutečné změny po startu). */
    s_prev_pioz80_pa_masked = g_pioz80.port[ PIOZ80_PORT_A ].masked_input;
    s_prev_pioz80_pb_masked = g_pioz80.port[ PIOZ80_PORT_B ].masked_input;
    s_prev_pio_state_bits = 0;
    if ( g_pioz80.interrupt & PIOZ80_INTERRUPT_INT_BIT )
        s_prev_pio_state_bits |= INTLOG_STATE_BIT_PIO_READY;
    if ( g_pioz80.port[ PIOZ80_PORT_A ].icena == PIOZ80_ICENA_ENABLED
         || g_pioz80.port[ PIOZ80_PORT_B ].icena == PIOZ80_ICENA_ENABLED ) {
        s_prev_pio_state_bits |= INTLOG_STATE_BIT_PIO_ARMED;
    }
    }
    char *hdr = build_subsys_header_json ( );
    tlog_writer_update_meta ( &s_writer, hdr );
    g_free ( hdr );
    g_free ( resolved_dir );
    fprintf ( stderr, "[trace-suite] intlog: recording started\n" );
    return 0;
}


void intlog_stop ( void )
{
    if ( !s_writer_open ) return;
    uint64_t now_px = tlog_common_get_pxclk_total ( );
    uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t now_sc = tlog_common_get_screens_total ( );
    tlog_writer_close ( &s_writer, now_px, now_cpu, now_sc );
    s_writer_open = 0;
    fprintf ( stderr, "[trace-suite] intlog: recording stopped\n" );
}


void intlog_finalize ( void )
{
    if ( g_intlog_config.save_on_exit && s_writer_open ) intlog_stop ( );
    g_free ( g_intlog_config.dir );
    g_free ( g_intlog_config.name );
    g_intlog_config.dir = NULL;
    g_intlog_config.name = NULL;
}


int intlog_is_truncated ( void )
{
    /* Pokud writer není otevřený, recording neexistuje, tedy ani
     * "truncated" stav nedává smysl. */
    if ( !s_writer_open ) return 0;
    return tlog_writer_is_truncated ( &s_writer );
}


int intlog_save_segment ( const char *path )
{
    /* Uzavřít aktuální segment + volitelně přesměrovat na path. Vzor sdílený
     * s cputrack_save_segment - viz tam pro detaily semantiky. */
    int was_active = ( g_intlog_active != 0 );

    /* Prázdná cesta = "force save now": flush rozdělaného segmentu BEZ
     * uzavření/reopenu (segment pokračuje do téhož manifestu). Viz
     * cputrack_save_segment + mzdos 0022 pro detaily. */
    if ( !( path && path[ 0 ] ) ) {
        if ( s_writer_open ) {
            uint64_t now_px = tlog_common_get_pxclk_total ( );
            uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
            uint32_t now_sc = tlog_common_get_screens_total ( );
            tlog_writer_flush_chunk ( &s_writer, now_px, now_cpu, now_sc );
        }
        return 0;
    }

    if ( s_writer_open ) {
        intlog_stop ( );
    }
    reclife_redirect_path ( &g_intlog_config.dir, &g_intlog_config.name, path );
    if ( was_active ) {
        return intlog_start ( );
    }
    return 0;
}


/* Sdílený lifecycle deskriptor (reclife) - viz cputrack.c pro detaily vzoru. */
static const st_RECLIFE_DESC s_intlog_reclife = {
    .subsys_name = "intlog",
    .mode_ptr    = (int *) &g_intlog_config.mode,
    .active_ptr  = &g_intlog_active,
    .fn_start    = intlog_start,
    .fn_stop     = intlog_stop,
    .fn_reset    = NULL,
    .fn_save     = intlog_save_segment,
};


void intlog_recompute_active ( int debugger_active )
{
    reclife_recompute_active ( &s_intlog_reclife, debugger_active );
}


void intlog_init ( void )
{
    memset ( &g_intlog_config, 0, sizeof ( g_intlog_config ) );
    g_intlog_config.mode = TLOG_MODE_OFF;
    g_intlog_config.chunk_mb = TLOG_DEFAULT_CHUNK_MB;
    g_intlog_config.max_total_mb = TLOG_DEFAULT_MAX_TOTAL_MB;
    g_intlog_config.save_on_exit = 1;

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "TRACE_INTLOG" );
    CFGELM *elm;
    elm = cfgmodule_register_new_element ( cmod, "mode", CFGENTYPE_KEYWORD, TLOG_MODE_OFF,
                                           TLOG_MODE_OFF,         "OFF",
                                           TLOG_MODE_WITH_WINDOW, "WITH_WINDOW",
                                           TLOG_MODE_ALWAYS,      "ALWAYS",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_intlog_config.mode, (void*) &g_intlog_config.mode );
    elm = cfgmodule_register_new_element ( cmod, "dir", CFGENTYPE_TEXT, "trace-suite" );
    cfgelement_bind ( elm, (void*) &g_intlog_config.dir );
    elm = cfgmodule_register_new_element ( cmod, "name", CFGENTYPE_TEXT, "intlog" );
    cfgelement_bind ( elm, (void*) &g_intlog_config.name );
    elm = cfgmodule_register_new_element ( cmod, "chunk_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_CHUNK_MB, 1, 4096 );
    cfgelement_set_handlers ( elm, (void*) &g_intlog_config.chunk_mb, (void*) &g_intlog_config.chunk_mb );
    elm = cfgmodule_register_new_element ( cmod, "max_total_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_MAX_TOTAL_MB, 0, 1048576 );
    cfgelement_set_handlers ( elm, (void*) &g_intlog_config.max_total_mb, (void*) &g_intlog_config.max_total_mb );
    elm = cfgmodule_register_new_element ( cmod, "save_on_exit", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_intlog_config.save_on_exit, (void*) &g_intlog_config.save_on_exit );

    /* Propagace .ini / default hodnot do g_intlog_config (jinak by dir/name
     * zůstaly NULL). Viz analogický komentář v cputrack_init(). */
    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


void intlog_apply_cli_options ( void )
{
    const char *v;
    v = sdlapp_option_value ( "--intlog-mode" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_intlog_config.mode = TLOG_MODE_OFF;
        else if ( g_ascii_strcasecmp ( v, "window" ) == 0 ) g_intlog_config.mode = TLOG_MODE_WITH_WINDOW;
        else if ( g_ascii_strcasecmp ( v, "always" ) == 0 ) g_intlog_config.mode = TLOG_MODE_ALWAYS;
    }
    v = sdlapp_option_value ( "--intlog-dir" );
    if ( v ) { g_free ( g_intlog_config.dir ); g_intlog_config.dir = g_strdup ( v ); }
    v = sdlapp_option_value ( "--intlog-name" );
    if ( v ) { g_free ( g_intlog_config.name ); g_intlog_config.name = g_strdup ( v ); }
    v = sdlapp_option_value ( "--intlog-chunk-mb" );
    if ( v ) g_intlog_config.chunk_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--intlog-max-total-mb" );
    if ( v ) g_intlog_config.max_total_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--intlog-save-on-exit" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "on" ) == 0 ) g_intlog_config.save_on_exit = 1;
        else if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_intlog_config.save_on_exit = 0;
    }
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
