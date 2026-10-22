/*
 * test_cputrack.c - unit testy pro trace-suite CPU tracking log
 *
 * Testuje:
 *  - cputrack_init / cputrack_start / cputrack_stop lifecycle
 *  - emit eventu - 12-byte layout v2 (PC + len + bytes + wait_clk + t_std)
 *  - HALT/self-loop collapse (1 event + akumulované wait_clk)
 *  - chunk + meta.json soubory vytvořené
 *  - initial RAM dump v hlavičce
 *  - subsys_header obsahuje initial_regs
 *  - formát v3 (32 B s registry): stav po instrukci, sbalená smyčka se
 *    stavem po poslední iteraci, přijetí přerušení, synchronizační záznamy
 *    (start, návrat do filtru PC, zápis mimo instrukci), příznak
 *    neznámého stavu před
 *
 * Test simuluje "vykonané instrukce" přímým voláním
 * cputrack_on_instruction_complete s nastaveným cpu->pc, aby šla
 * otestovat collapse logika i běžný emit bez nutnosti spouštět
 * mzarch_main_emulator_run.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "debugger/trace/cputrack.h"
#include "debugger/trace/tlog_common.h"
#include "mzarch/mzarch.h"
#include "hw-generic/memory/memory.h"
#include "libs/cpu-z80/z80.h"
#include "json_test_helpers.h"
#include "debugger/dbgapi_cmdrq.h"
#include "debugger/replay_dbg.h"


/* ================================================================
 * setUp / tearDown
 * ================================================================ */

static char *s_test_dir = NULL;


static int rmtree ( const char *path )
{
    if ( !path || !g_file_test ( path, G_FILE_TEST_EXISTS ) ) return 0;
    if ( g_file_test ( path, G_FILE_TEST_IS_DIR ) && !g_file_test ( path, G_FILE_TEST_IS_SYMLINK ) ) {
        GError *err = NULL;
        GDir *d = g_dir_open ( path, 0, &err );
        if ( !d ) {
            if ( err ) g_error_free ( err );
            return -1;
        }
        const gchar *name;
        while ( ( name = g_dir_read_name ( d ) ) != NULL ) {
            char *child = g_build_filename ( path, name, NULL );
            rmtree ( child );
            g_free ( child );
        }
        g_dir_close ( d );
        return g_rmdir ( path );
    }
    return g_remove ( path );
}


void setUp ( void )
{
    GError *err = NULL;
    s_test_dir = g_dir_make_tmp ( "mztest-cputrack-XXXXXX", &err );
    if ( !s_test_dir ) {
        if ( err ) g_error_free ( err );
        TEST_FAIL_MESSAGE ( "Nepodařilo se vytvořit tmpdir" );
        return;
    }

    /* Reset config a aktivace state */
    g_free ( g_cputrack_config.dir );
    g_free ( g_cputrack_config.name );
    g_cputrack_config.dir = g_strdup ( s_test_dir );
    g_cputrack_config.name = g_strdup ( "cput" );
    g_cputrack_config.chunk_mb = 1;
    g_cputrack_config.max_total_mb = 0;
    g_cputrack_config.save_on_exit = 1;
    g_cputrack_config.mode = TLOG_MODE_OFF;
    g_cputrack_config.regs = 0;
    g_cputrack_config.pc_range_lo = 0x0000;
    g_cputrack_config.pc_range_hi = 0xFFFF;
    g_cputrack_active = 0;
}


void tearDown ( void )
{
    /* Pokud je writer otevřený, zavřít ho. */
    if ( g_cputrack_active ) {
        cputrack_stop ( );
        g_cputrack_active = 0;
    }

    if ( s_test_dir ) {
        rmtree ( s_test_dir );
        g_free ( s_test_dir );
        s_test_dir = NULL;
    }
}


/* ================================================================
 * Pomocné helpery
 * ================================================================ */

static uint8_t *read_whole_file ( const char *dir, const char *filename, size_t *out_size )
{
    char *path = g_build_filename ( dir, filename, NULL );
    FILE *fp = g_fopen ( path, "rb" );
    g_free ( path );
    if ( !fp ) {
        if ( out_size ) *out_size = 0;
        return NULL;
    }
    fseek ( fp, 0, SEEK_END );
    long size = ftell ( fp );
    fseek ( fp, 0, SEEK_SET );
    if ( size < 0 ) {
        fclose ( fp );
        if ( out_size ) *out_size = 0;
        return NULL;
    }
    uint8_t *buf = (uint8_t *) g_malloc ( (size_t) size + 1 );
    size_t n = fread ( buf, 1, (size_t) size, fp );
    buf[ n ] = 0;
    fclose ( fp );
    if ( out_size ) *out_size = n;
    return buf;
}


/* Parsování 12 B eventu. */
typedef struct {
    uint16_t pc;
    uint8_t  insn_len;
    uint8_t  insn_bytes[ 4 ];
    uint32_t wait_clk;
    uint8_t  t_std;
} parsed_event_t;


static void parse_event ( const uint8_t *buf, parsed_event_t *e )
{
    e->pc = (uint16_t) buf[ 0 ] | ( (uint16_t) buf[ 1 ] << 8 );
    e->insn_len = buf[ 2 ];
    e->insn_bytes[ 0 ] = buf[ 3 ];
    e->insn_bytes[ 1 ] = buf[ 4 ];
    e->insn_bytes[ 2 ] = buf[ 5 ];
    e->insn_bytes[ 3 ] = buf[ 6 ];
    e->wait_clk = (uint32_t) buf[ 7 ]
                | ( (uint32_t) buf[ 8 ] << 8 )
                | ( (uint32_t) buf[ 9 ] << 16 )
                | ( (uint32_t) buf[ 10 ] << 24 );
    e->t_std = buf[ 11 ];
}


/* Activate cputrack ručně - obejdeme TLOG_MODE_OFF default. */
static int activate_cputrack ( void )
{
    int rc = cputrack_start ( );
    if ( rc == 0 ) g_cputrack_active = 1;
    return rc;
}


/* ================================================================
 * Lifecycle
 * ================================================================ */

void test_cputrack_start_stop_basic ( void )
{
    TEST_ASSERT_EQUAL_INT_MESSAGE ( 0, activate_cputrack ( ),
                                    "cputrack_start musí uspět" );
    cputrack_stop ( );
    g_cputrack_active = 0;

    /* meta.json a initial dumpy musí existovat. */
    char *meta_path = g_build_filename ( s_test_dir, "cput.json", NULL );
    TEST_ASSERT_TRUE_MESSAGE ( g_file_test ( meta_path, G_FILE_TEST_IS_REGULAR ),
                               "cput.json musí být zapsán" );
    g_free ( meta_path );

    char *ram_path = g_build_filename ( s_test_dir, "cput_initial_ram.bin", NULL );
    TEST_ASSERT_TRUE_MESSAGE ( g_file_test ( ram_path, G_FILE_TEST_IS_REGULAR ),
                               "cput_initial_ram.bin musí být zapsán" );
    g_free ( ram_path );
}


/**
 * @brief Přepočet aktivity podle režimu přepne bit cputrack v masce háčků
 *        po instrukci (brána hlavní smyčky čte jen masku).
 */
void test_cputrack_recompute_sets_post_insn_mask ( void )
{
    g_cputrack_config.mode = TLOG_MODE_ALWAYS;
    cputrack_recompute_active ( 0 );
    TEST_ASSERT_EQUAL_INT ( 1, g_cputrack_active );
    TEST_ASSERT_TRUE ( g_mzarch_post_insn_hooks & MZARCH_POST_INSN_CPUTRACK );

    g_cputrack_config.mode = TLOG_MODE_OFF;
    cputrack_recompute_active ( 0 );
    TEST_ASSERT_EQUAL_INT ( 0, g_cputrack_active );
    TEST_ASSERT_FALSE ( g_mzarch_post_insn_hooks & MZARCH_POST_INSN_CPUTRACK );
}


void test_cputrack_start_idempotent ( void )
{
    TEST_ASSERT_EQUAL_INT ( 0, activate_cputrack ( ) );
    /* Druhé volání nesmí spadnout (vrací 0 = už aktivní). */
    TEST_ASSERT_EQUAL_INT ( 0, cputrack_start ( ) );
    cputrack_stop ( );
    g_cputrack_active = 0;
}


void test_cputrack_stop_idempotent ( void )
{
    TEST_ASSERT_EQUAL_INT ( 0, activate_cputrack ( ) );
    cputrack_stop ( );
    g_cputrack_active = 0;
    /* Druhé stop = no-op. */
    cputrack_stop ( );
}


/* ================================================================
 * Initial RAM dump v hlavičce
 * ================================================================ */

void test_cputrack_initial_ram_dump_size ( void )
{
    activate_cputrack ( );
    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput_initial_ram.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( MEMORY_SIZE_RAM, fsize,
                                       "RAM dump musí mít MEMORY_SIZE_RAM B" );
    g_free ( buf );
}


void test_cputrack_initial_vram_dump_exists ( void )
{
    activate_cputrack ( );
    cputrack_stop ( );
    g_cputrack_active = 0;

    char *p = g_build_filename ( s_test_dir, "cput_initial_vram.bin", NULL );
    TEST_ASSERT_TRUE_MESSAGE ( g_file_test ( p, G_FILE_TEST_IS_REGULAR ),
                               "VRAM dump musí existovat" );
    g_free ( p );
}


/* ================================================================
 * Header obsahuje initial_regs
 * ================================================================ */

void test_cputrack_meta_contains_initial_regs ( void )
{
    activate_cputrack ( );
    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.json", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    const char *s = (const char *) buf;

    TEST_ASSERT_NOT_NULL_MESSAGE ( strstr ( s, "\"initial_regs\"" ),
                                   "meta musí obsahovat initial_regs" );
    /* Klíčové registry. Pretty print json-glib formátuje "klíč" : value
     * (mezera před i po :), proto json_key_present místo strstr. */
    TEST_ASSERT_TRUE ( json_key_present ( s, "AF" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "BC" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "DE" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "HL" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IX" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IY" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "SP" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "PC" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "AF_alt" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "BC_alt" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "DE_alt" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "HL_alt" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "I" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "R" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IM" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IFF1" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IFF2" ) );

    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "event_record_size", "12" ),
                                   "event_record_size musí být 12" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "event_record_version", "2" ),
                                   "event_record_version musí být 2 (bajt 11 = t_std)" );

    g_free ( buf );
}


/* ================================================================
 * Per-event 12 B layout
 * ================================================================ */

void test_cputrack_event_layout_basic ( void )
{
    activate_cputrack ( );

    /* Simulujeme jednu instrukci na PC=0x1234. Aby NEdošlo k collapse,
     * nastavíme cpu->pc na jinou hodnotu (= "PC po stepu se posunul"). */
    g_mzarch_main.cpu->pc = 0x1237;
    cputrack_on_instruction_complete ( 0x1234, 4, 0 );

    /* Druhá instrukce na PC=0x1237 - taky PC se posune. */
    g_mzarch_main.cpu->pc = 0x123A;
    cputrack_on_instruction_complete ( 0x1237, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    /* Načíst chunk soubor. Po close se 24 B (2 events) flushnou. */
    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL_MESSAGE ( buf, "cput.000.bin musí existovat" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 24, fsize,
                                       "Po 2 events musí mít chunk 24 B" );

    parsed_event_t e0, e1;
    parse_event ( buf, &e0 );
    parse_event ( buf + 12, &e1 );

    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x1234, e0.pc, "PC eventu 0" );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 0, e0.wait_clk, "wait_clk = 0 pro běžnou instr" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 4, e0.t_std, "bajt 11 (v2) = standardní T-stavy instrukce" );
    TEST_ASSERT_TRUE_MESSAGE ( e0.insn_len >= 1 && e0.insn_len <= 4,
                               "insn_len v rozsahu 1-4" );

    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x1237, e1.pc, "PC eventu 1" );

    g_free ( buf );
}


void test_cputrack_event_with_wait ( void )
{
    activate_cputrack ( );

    /* Instr s explicit wait_extra. */
    g_mzarch_main.cpu->pc = 0x2003;
    cputrack_on_instruction_complete ( 0x2000, 4, 7 );

    /* Druhá instr - posune PC, aby se předchozí emitnula. */
    g_mzarch_main.cpu->pc = 0x2006;
    cputrack_on_instruction_complete ( 0x2003, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t ( 24, fsize );

    parsed_event_t e0;
    parse_event ( buf, &e0 );
    TEST_ASSERT_EQUAL_HEX16 ( 0x2000, e0.pc );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 7, e0.wait_clk,
                                       "wait_extra=7 musí být v eventu" );

    g_free ( buf );
}


void test_cputrack_event_wait_saturates_at_max ( void )
{
    /* Per michal.txt: "Pro WAIT bych dal uint 32b a pokud jej překročíme,
     * tak bude nastaven na full stav" => 0xFFFFFFFF sentinel. Dosažitelné
     * v praxi přes long HALT smyčku s mid-instr WAIT, nebo přímým passu
     * 0xFFFFFFFF do hooku pro non-collapse instr. */
    activate_cputrack ( );

    /* Instr s wait_extra = 0xFFFFFFFF (= explicit sentinel od caller). */
    g_mzarch_main.cpu->pc = 0x3003;
    cputrack_on_instruction_complete ( 0x3000, 4, 0xFFFFFFFFu );

    /* Posun PC aby se 0x3000 emitnulo. */
    g_mzarch_main.cpu->pc = 0x3006;
    cputrack_on_instruction_complete ( 0x3003, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );

    parsed_event_t e0;
    parse_event ( buf, &e0 );
    TEST_ASSERT_EQUAL_HEX16 ( 0x3000, e0.pc );
    TEST_ASSERT_EQUAL_HEX32_MESSAGE ( 0xFFFFFFFFu, e0.wait_clk,
                                      "wait_clk = 0xFFFFFFFF (saturated full state)" );

    g_free ( buf );
}


void test_cputrack_collapse_wait_extra_accumulates ( void )
{
    /* Self-loop s nenulovym wait_extra v každé iteraci - akumulace
     * by měla zahrnout jak insn_tstates, tak wait_extra. */
    activate_cputrack ( );

    /* Bootstrap - nějaká instr před smyčkou. */
    g_mzarch_main.cpu->pc = 0x6000;
    cputrack_on_instruction_complete ( 0x5FFD, 4, 0 );

    /* 5 iterací self-loop, každá insn_tstates=4, wait_extra=10 => total
     * pending_wait_clk = 5*(4+10) = 70. */
    for ( int i = 0; i < 5; i++ ) {
        g_mzarch_main.cpu->pc = 0x6000;
        cputrack_on_instruction_complete ( 0x6000, 4, 10 );
    }
    /* Exit smyčky. */
    g_mzarch_main.cpu->pc = 0x7000;
    cputrack_on_instruction_complete ( 0x6000, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t ( 36, fsize );

    parsed_event_t e1;
    parse_event ( buf + 12, &e1 );
    TEST_ASSERT_EQUAL_HEX16 ( 0x6000, e1.pc );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 5 * (4 + 10), e1.wait_clk,
        "Collapsed event akumuluje insn_tstates + wait_extra per iteraci" );

    g_free ( buf );
}


/* ================================================================
 * HALT / self-loop collapse
 * ================================================================ */

/* v2: přijetí přerušení = záznam s insn_length 0 (pc obsluhy, t_std =
 * T-stavy přijetí); sbalená smyčka HALT, kterou přerušení ukončilo, se
 * zapíše před něj. */
void test_cputrack_interrupt_accept_record ( void )
{
    activate_cputrack ( );

    g_mzarch_main.cpu->pc = 0x2001;
    cputrack_on_instruction_complete ( 0x2000, 4, 0 );
    /* HALT na 2001h se opakuje (sbalí se) */
    for ( int i = 0; i < 10; i++ ) {
        g_mzarch_main.cpu->pc = 0x2001;
        cputrack_on_instruction_complete ( 0x2001, 4, 0 );
    }
    /* přijetí IM 2, obsluha na 3434h */
    cputrack_on_interrupt_accept ( 0x3434, 19 );
    g_mzarch_main.cpu->pc = 0x3435;
    cputrack_on_instruction_complete ( 0x3434, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 48, fsize, "instr + sbalený HALT + přijetí + instr = 4 záznamy" );
    parsed_event_t e1, e2, e3;
    parse_event ( buf + 12, &e1 );
    parse_event ( buf + 24, &e2 );
    parse_event ( buf + 36, &e3 );
    TEST_ASSERT_EQUAL_HEX16 ( 0x2001, e1.pc );
    TEST_ASSERT_EQUAL_UINT32 ( 10 * 4, e1.wait_clk );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x3434, e2.pc, "přijetí: pc = adresa obsluhy" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 0, e2.insn_len, "přijetí: insn_length 0" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 19, e2.t_std, "přijetí: t_std = T-stavy přijetí" );
    TEST_ASSERT_EQUAL_UINT32 ( 0, e2.wait_clk );
    TEST_ASSERT_EQUAL_HEX16 ( 0x3434, e3.pc );
    TEST_ASSERT_EQUAL_UINT8 ( 4, e3.t_std );
    g_free ( buf );
}


void test_cputrack_self_loop_collapses_to_single_event ( void )
{
    activate_cputrack ( );

    /* Předchozí instr - posuneme stav a vytvoříme s_prev_pc_valid. */
    g_mzarch_main.cpu->pc = 0x3000;
    cputrack_on_instruction_complete ( 0x2FFD, 4, 0 );

    /* Self-loop: JR $-2 na 0x3000. PC po stepu zůstane = 0x3000.
     * 100 iterací musí dát 0 emitů (vše se akumuluje). */
    for ( int i = 0; i < 100; i++ ) {
        g_mzarch_main.cpu->pc = 0x3000;
        cputrack_on_instruction_complete ( 0x3000, 12, 0 );
    }

    /* Exit smyčky - PC se konečně posune. To emituje collapse event. */
    g_mzarch_main.cpu->pc = 0x4000;
    cputrack_on_instruction_complete ( 0x3000, 12, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );

    /* Očekáváme 3 events:
     *   1. instr na 0x2FFD (před smyčkou)
     *   2. collapsed event na 0x3000 s wait_clk = 100*12 = 1200
     *   3. exit instr (= ta která změnila PC) - to je opět 0x3000
     *      (sama sebe nahradila volání s new_pc=0x4000 != pc=0x3000)
     * Pozor: implementace emit pro "exit" instrukci taky emit s wait=0,
     * ale je to stejná instr co byla v collapse. To je per design.
     */
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 36, fsize,
        "Self-loop collapse: 1 pre + 1 collapsed + 1 exit = 3 events = 36 B" );

    parsed_event_t e0, e1, e2;
    parse_event ( buf, &e0 );
    parse_event ( buf + 12, &e1 );
    parse_event ( buf + 24, &e2 );

    TEST_ASSERT_EQUAL_HEX16 ( 0x2FFD, e0.pc );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x3000, e1.pc,
                                      "Collapsed event musí mít PC smyčky" );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 100 * 12, e1.wait_clk,
                                       "Collapsed event akumuluje 100*12 T-states" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 4, e0.t_std, "běžný záznam nese standardní T-stavy" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 0, e1.t_std,
                                      "sbalený záznam má t_std 0 (čas je ve wait_clk)" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 12, e2.t_std, "exit instrukce nese standardní T-stavy" );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x3000, e2.pc,
                                      "Exit instr emit s pc=collapse pc" );

    g_free ( buf );
}


void test_cputrack_no_collapse_when_pc_changes ( void )
{
    activate_cputrack ( );

    /* Sekvence růžných PC - žádný collapse. */
    uint16_t pcs[] = { 0x1000, 0x1003, 0x1006, 0x1009, 0x100C };
    /* Bootstrap - první instr nemá s_prev_pc_valid */
    g_mzarch_main.cpu->pc = pcs[ 0 ];
    cputrack_on_instruction_complete ( 0xFFFF, 4, 0 );  /* sentinel */

    for ( unsigned i = 0; i < G_N_ELEMENTS ( pcs ); i++ ) {
        uint16_t next = ( i + 1 < G_N_ELEMENTS ( pcs ) ) ? pcs[ i + 1 ] : 0x2000;
        g_mzarch_main.cpu->pc = next;
        cputrack_on_instruction_complete ( pcs[ i ], 4, 0 );
    }

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    /* sentinel + 5 = 6 eventů = 72 B. */
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 72, fsize,
                                       "Bez collapse 6 events = 72 B" );
    g_free ( buf );
}


void test_cputrack_collapse_state_reset ( void )
{
    activate_cputrack ( );

    /* Vytvoř collapse state. */
    g_mzarch_main.cpu->pc = 0x5000;
    cputrack_on_instruction_complete ( 0x4FFD, 4, 0 );
    g_mzarch_main.cpu->pc = 0x5000;
    cputrack_on_instruction_complete ( 0x5000, 12, 0 );
    g_mzarch_main.cpu->pc = 0x5000;
    cputrack_on_instruction_complete ( 0x5000, 12, 0 );

    /* Reset state. Po něm další instrukce se chová jako "první". */
    cputrack_reset_collapse_state ( );

    /* PC == 0x5000 znova (po resetu = bootstrap). */
    g_mzarch_main.cpu->pc = 0x5000;
    cputrack_on_instruction_complete ( 0x5000, 12, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    /* Očekávat: 1 instr před smyčkou (= 0x4FFD) + accumulated collapsed
     * event po stop_writeru (s_pending > 0 → emit při stop).
     * Test ověřuje hlavně že reset_collapse_state nepadne. */
    char *p = g_build_filename ( s_test_dir, "cput.000.bin", NULL );
    TEST_ASSERT_TRUE_MESSAGE ( g_file_test ( p, G_FILE_TEST_IS_REGULAR ),
                               "chunk soubor musí existovat" );
    g_free ( p );
}


/* ================================================================
 * Formát v3 (registry, synchronizační záznamy)
 * ================================================================ */

/** @brief Rozparsovaný 32B záznam v3. */
typedef struct {
    parsed_event_t e;     /* bajty 0-11 */
    uint16_t af, bc, de, hl, ix, iy, sp, wz;
    uint8_t  i, r, flags, kind;
    uint16_t alt[ 4 ];    /* jen synchronizační záznam: AF' BC' DE' HL' */
} parsed_v3_t;


static uint16_t rd16 ( const uint8_t *p )
{
    return (uint16_t) ( p[ 0 ] | ( p[ 1 ] << 8 ) );
}


static void parse_v3 ( const uint8_t *b, parsed_v3_t *o )
{
    parse_event ( b, &o->e );
    o->af = rd16 ( b + 12 );
    o->bc = rd16 ( b + 14 );
    o->de = rd16 ( b + 16 );
    o->hl = rd16 ( b + 18 );
    o->ix = rd16 ( b + 20 );
    o->iy = rd16 ( b + 22 );
    o->sp = rd16 ( b + 24 );
    o->wz = rd16 ( b + 26 );
    o->i = b[ 28 ];
    o->r = b[ 29 ];
    o->flags = b[ 30 ];
    o->kind = b[ 31 ];
    for ( unsigned k = 0; k < 4; k++ ) o->alt[ k ] = rd16 ( b + 3 + 2 * k );
}


/** @brief Nastavit známý stav CPU (registry, alternativní sada, IM 2, IFF). */
static void set_cpu_state ( void )
{
    z80_t *c = g_mzarch_main.cpu;
    c->af.w = 0x1144; c->bc.w = 0x2233; c->de.w = 0x3322; c->hl.w = 0x4411;
    c->ix.w = 0x5500; c->iy.w = 0x6600; c->sp = 0x7FF0; c->wz.w = 0x0BCD;
    c->af2.w = 0xA1A2; c->bc2.w = 0xB1B2; c->de2.w = 0xC1C2; c->hl2.w = 0xD1D2;
    c->i = 0x3F; c->r = 0x85; c->im = 2; c->iff1 = 1; c->iff2 = 1; c->halted = false;
    c->pc = 0x1000;
}


/** @brief Spustit segment v3 a vrátit počet záznamů po stopu přes read_v3_all. */
static int activate_cputrack_regs ( void )
{
    g_cputrack_config.regs = 1;
    return activate_cputrack ( );
}


/** @brief Zastavit a načíst všechny záznamy v3 (volající uvolní). */
static uint8_t *stop_and_read ( size_t *n_rec )
{
    cputrack_stop ( );
    g_cputrack_active = 0;
    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL_MESSAGE ( buf, "cput.000.bin musí existovat" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 0, fsize % 32, "záznamy v3 mají 32 B" );
    *n_rec = fsize / 32;
    return buf;
}


/**
 * @brief Segment v3: manifest 32 B / verze 3, první záznam = synchronizace
 *        START s počátečním stavem (včetně alternativní sady).
 */
void test_cputrack_v3_start_sync_and_meta ( void )
{
    set_cpu_state ( );
    TEST_ASSERT_EQUAL_INT ( 0, activate_cputrack_regs ( ) );
    TEST_ASSERT_EQUAL_INT ( 1, cputrack_segment_has_regs ( ) );
    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t ( 1, n );
    parsed_v3_t s;
    parse_v3 ( buf, &s );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_INSN_LEN_SYNC, s.e.insn_len );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_SYNC | ( CPUTRACK_SYNC_START << CPUTRACK_SYNC_REASON_SHIFT ), s.kind );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1000, s.e.pc );
    TEST_ASSERT_EQUAL_UINT8 ( 0, s.e.t_std );
    TEST_ASSERT_EQUAL_HEX16 ( 0x4411, s.hl );
    TEST_ASSERT_EQUAL_HEX16 ( 0x0BCD, s.wz );
    TEST_ASSERT_EQUAL_HEX16 ( 0xA1A2, s.alt[ 0 ] );
    TEST_ASSERT_EQUAL_HEX16 ( 0xD1D2, s.alt[ 3 ] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x3F, s.i );
    TEST_ASSERT_EQUAL_HEX8 ( 0x85, s.r );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_REGS_F_IFF1 | CPUTRACK_REGS_F_IFF2 | ( 2u << CPUTRACK_REGS_F_IM_SHIFT ), s.flags );
    g_free ( buf );

    size_t msize = 0;
    uint8_t *meta = read_whole_file ( s_test_dir, "cput.json", &msize );
    TEST_ASSERT_NOT_NULL ( meta );
    TEST_ASSERT_TRUE ( json_kv_present ( (const char *) meta, "event_record_size", "32" ) );
    TEST_ASSERT_TRUE ( json_kv_present ( (const char *) meta, "event_record_version", "3" ) );
    g_free ( meta );

    st_CPUTRACK_REGS ir;
    TEST_ASSERT_EQUAL_INT ( 1, cputrack_get_initial_regs ( &ir, NULL ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x4411, ir.hl );
}


/** @brief Záznam instrukce nese stav po ní; stav před = předchozí záznam. */
void test_cputrack_v3_state_after_instruction ( void )
{
    set_cpu_state ( );
    activate_cputrack_regs ( );
    /* LD HL,0C000h na 1000h: po kroku HL = C000h, PC = 1003h. */
    g_mzarch_main.cpu->hl.w = 0xC000;
    g_mzarch_main.cpu->pc = 0x1003;
    cputrack_on_instruction_complete ( 0x1000, 10, 0 );
    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t ( 2, n );
    parsed_v3_t s, i;
    parse_v3 ( buf, &s );
    parse_v3 ( buf + 32, &i );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x4411, s.hl, "stav před = synchronizace" );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1000, i.e.pc );
    TEST_ASSERT_EQUAL_UINT8 ( 10, i.e.t_std );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0xC000, i.hl, "stav po instrukci" );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_INSN, i.kind );
    g_free ( buf );
}


/** @brief Sbalená smyčka (LDIR) nese stav po poslední iteraci. */
void test_cputrack_v3_collapse_regs_after_last_iteration ( void )
{
    set_cpu_state ( );
    activate_cputrack_regs ( );
    g_mzarch_main.cpu->pc = 0x2000;
    cputrack_on_instruction_complete ( 0x1FFD, 10, 0 );
    /* LDIR na 2000h: 3 iterace s PC na sobě (HL, DE rostou, BC klesá). */
    for ( int k = 1; k <= 3; k++ ) {
        g_mzarch_main.cpu->hl.w = (uint16_t) ( 0x8000 + k );
        g_mzarch_main.cpu->bc.w = (uint16_t) ( 4 - k );
        g_mzarch_main.cpu->pc = 0x2000;
        cputrack_on_instruction_complete ( 0x2000, 21, 0 );
    }
    /* Poslední iterace (BC = 0) propadne. */
    g_mzarch_main.cpu->hl.w = 0x8004;
    g_mzarch_main.cpu->bc.w = 0;
    g_mzarch_main.cpu->pc = 0x2002;
    cputrack_on_instruction_complete ( 0x2000, 16, 0 );
    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t ( 4, n );
    parsed_v3_t loop, last;
    parse_v3 ( buf + 2 * 32, &loop );
    parse_v3 ( buf + 3 * 32, &last );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_LOOP, loop.kind );
    TEST_ASSERT_EQUAL_UINT32 ( 3 * 21, loop.e.wait_clk );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x8003, loop.hl, "sbalený záznam: HL po poslední sbalené iteraci" );
    TEST_ASSERT_EQUAL_HEX16 ( 0x0001, loop.bc );
    TEST_ASSERT_EQUAL_HEX16 ( 0x8004, last.hl );
    TEST_ASSERT_EQUAL_HEX16 ( 0x0000, last.bc );
    g_free ( buf );
}


/** @brief Přijetí přerušení: stav po přijetí (SP o 2 níž, PC = obsluha). */
void test_cputrack_v3_interrupt_accept ( void )
{
    set_cpu_state ( );
    activate_cputrack_regs ( );
    g_mzarch_main.cpu->pc = 0x1001;
    cputrack_on_instruction_complete ( 0x1000, 4, 0 );
    g_mzarch_main.cpu->sp = 0x7FEE;
    g_mzarch_main.cpu->iff1 = 0;
    g_mzarch_main.cpu->iff2 = 0;
    g_mzarch_main.cpu->pc = 0x0038;
    cputrack_on_interrupt_accept ( 0x0038, 13 );
    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t ( 3, n );
    parsed_v3_t a;
    parse_v3 ( buf + 2 * 32, &a );
    TEST_ASSERT_EQUAL_UINT8 ( 0, a.e.insn_len );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_INT, a.kind );
    TEST_ASSERT_EQUAL_HEX16 ( 0x0038, a.e.pc );
    TEST_ASSERT_EQUAL_UINT8 ( 13, a.e.t_std );
    TEST_ASSERT_EQUAL_HEX16 ( 0x7FEE, a.sp );
    TEST_ASSERT_EQUAL_HEX8 ( 2u << CPUTRACK_REGS_F_IM_SHIFT, a.flags );
    g_free ( buf );
}


/**
 * @brief Zápis registru mimo instrukci: synchronizační záznam WRITE;
 *        kontrola bez změny nezapíše nic.
 */
void test_cputrack_v3_sync_after_register_write ( void )
{
    set_cpu_state ( );
    activate_cputrack_regs ( );
    g_mzarch_main.cpu->pc = 0x1001;
    cputrack_on_instruction_complete ( 0x1000, 4, 0 );
    cputrack_sync_check ( );                 /* beze změny: nic */
    g_mzarch_main.cpu->de2.w = 0x9999;       /* zápis z debuggeru (alternativní sada) */
    cputrack_sync_check ( );
    cputrack_sync_check ( );                 /* podruhé už beze změny */
    g_mzarch_main.cpu->pc = 0x5000;          /* zápis PC */
    cputrack_sync_check ( );
    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t ( 4, n );
    parsed_v3_t w1, w2;
    parse_v3 ( buf + 2 * 32, &w1 );
    parse_v3 ( buf + 3 * 32, &w2 );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_SYNC | ( CPUTRACK_SYNC_WRITE << CPUTRACK_SYNC_REASON_SHIFT ), w1.kind );
    TEST_ASSERT_EQUAL_HEX16 ( 0x9999, w1.alt[ 2 ] );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1001, w1.e.pc );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x5000, w2.e.pc, "synchronizace nese PC příští instrukce" );
    g_free ( buf );
}


/**
 * @brief Pauza v HALT: kontrola beze změny sbalenou smyčku nepřeruší,
 *        se změnou ji zapíše a za ni synchronizaci.
 */
void test_cputrack_v3_sync_with_pending_loop ( void )
{
    set_cpu_state ( );
    activate_cputrack_regs ( );
    g_mzarch_main.cpu->pc = 0x3000;
    cputrack_on_instruction_complete ( 0x2FFF, 4, 0 );
    g_mzarch_main.cpu->halted = true;
    for ( int k = 0; k < 5; k++ ) {
        g_mzarch_main.cpu->r = (uint8_t) ( 0x10 + k );
        g_mzarch_main.cpu->pc = 0x3000;
        cputrack_on_instruction_complete ( 0x3000, 4, 0 );
    }
    cputrack_sync_check ( );                 /* pauza v HALT beze změny */
    g_mzarch_main.cpu->hl.w = 0x0F0F;        /* zápis z debuggeru v pauze */
    cputrack_sync_check ( );
    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 4, n, "start, instrukce, sbalený HALT, synchronizace" );
    parsed_v3_t loop, s;
    parse_v3 ( buf + 2 * 32, &loop );
    parse_v3 ( buf + 3 * 32, &s );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_LOOP, loop.kind );
    TEST_ASSERT_EQUAL_HEX8 ( 0x14, loop.r );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_REGS_F_HALTED, loop.flags & CPUTRACK_REGS_F_HALTED );
    TEST_ASSERT_EQUAL_HEX16 ( 0x4411, loop.hl );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_SYNC | ( CPUTRACK_SYNC_WRITE << CPUTRACK_SYNC_REASON_SHIFT ), s.kind );
    TEST_ASSERT_EQUAL_HEX16 ( 0x0F0F, s.hl );
    g_free ( buf );
}


/**
 * @brief Filtr PC: instrukce mimo rozsah se nezapíše, návrat do rozsahu
 *        zapíše synchronizaci PC_RANGE a první instrukce v rozsahu má
 *        stav před známý.
 */
void test_cputrack_v3_pc_filter_return_sync ( void )
{
    set_cpu_state ( );
    g_cputrack_config.pc_range_lo = 0x2000;
    g_cputrack_config.pc_range_hi = 0x2FFF;
    activate_cputrack_regs ( );
    /* 1000h mimo rozsah, pokračuje mimo rozsah: nic. */
    g_mzarch_main.cpu->pc = 0x1003;
    cputrack_hot_path_hook ( 0x1000, 10, 0 );
    /* Dokud řetěz drží filtr a PC je mimo rozsah, kontrola nic nezapíše. */
    cputrack_sync_check ( );
    /* CALL 2000h z 1003h: vede do rozsahu -> synchronizace. */
    g_mzarch_main.cpu->sp = 0x7FEE;
    g_mzarch_main.cpu->pc = 0x2000;
    cputrack_hot_path_hook ( 0x1003, 17, 0 );
    g_mzarch_main.cpu->hl.w = 0xABCD;
    g_mzarch_main.cpu->pc = 0x2003;
    cputrack_hot_path_hook ( 0x2000, 10, 0 );
    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t ( 3, n );
    parsed_v3_t s, i;
    parse_v3 ( buf + 32, &s );
    parse_v3 ( buf + 2 * 32, &i );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_SYNC | ( CPUTRACK_SYNC_PC_RANGE << CPUTRACK_SYNC_REASON_SHIFT ), s.kind );
    TEST_ASSERT_EQUAL_HEX16 ( 0x2000, s.e.pc );
    TEST_ASSERT_EQUAL_HEX16 ( 0x7FEE, s.sp );
    TEST_ASSERT_EQUAL_HEX16 ( 0x2000, i.e.pc );
    TEST_ASSERT_EQUAL_HEX8_MESSAGE ( CPUTRACK_KIND_INSN, i.kind, "stav před první instrukcí v rozsahu je známý" );
    TEST_ASSERT_EQUAL_HEX16 ( 0xABCD, i.hl );
    g_free ( buf );
}


/**
 * @brief Přijetí přerušení do rozsahu po instrukci mimo rozsah, která do
 *        rozsahu nevedla: záznam přijetí má CPUTRACK_FLAG_NO_BEFORE.
 */
void test_cputrack_v3_filtered_gap_marks_no_before ( void )
{
    set_cpu_state ( );
    g_cputrack_config.pc_range_lo = 0x0000;
    g_cputrack_config.pc_range_hi = 0x0FFF;
    g_mzarch_main.cpu->pc = 0x0100;
    activate_cputrack_regs ( );
    g_mzarch_main.cpu->pc = 0x1000;          /* JP 1000h: odchod z rozsahu */
    cputrack_hot_path_hook ( 0x0100, 10, 0 );
    g_mzarch_main.cpu->pc = 0x1001;
    cputrack_hot_path_hook ( 0x1000, 4, 0 ); /* mimo rozsah */
    g_mzarch_main.cpu->pc = 0x0038;
    cputrack_on_interrupt_accept ( 0x0038, 13 );
    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t ( 3, n );
    parsed_v3_t a;
    parse_v3 ( buf + 2 * 32, &a );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_INT | CPUTRACK_FLAG_NO_BEFORE, a.kind );
    g_free ( buf );
}


/** @brief trace_reset zahodí sbalenou smyčku; kontrola pak obnoví řetěz. */
void test_cputrack_v3_reset_collapse_then_sync ( void )
{
    set_cpu_state ( );
    activate_cputrack_regs ( );
    g_mzarch_main.cpu->pc = 0x4000;
    cputrack_on_instruction_complete ( 0x3FFD, 10, 0 );
    for ( int k = 0; k < 3; k++ ) {
        g_mzarch_main.cpu->r = (uint8_t) ( 0x20 + k );
        g_mzarch_main.cpu->pc = 0x4000;
        cputrack_on_instruction_complete ( 0x4000, 4, 0 );
    }
    cputrack_reset_collapse_state ( );
    cputrack_sync_check ( );
    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t ( 3, n );
    parsed_v3_t s;
    parse_v3 ( buf + 2 * 32, &s );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_SYNC | ( CPUTRACK_SYNC_WRITE << CPUTRACK_SYNC_REASON_SHIFT ), s.kind );
    TEST_ASSERT_EQUAL_HEX8 ( 0x22, s.r );
    g_free ( buf );
}


/**
 * @brief Přehraná událost záznamu vstupů set_reg (replay_dbg_execute() mimo
 *        drain dbgapi) zapíše synchronizační záznam WRITE.
 */
void test_cputrack_v3_replay_set_reg_syncs ( void )
{
    set_cpu_state ( );
    activate_cputrack_regs ( );
    g_mzarch_main.cpu->pc = 0x1001;
    cputrack_on_instruction_complete ( 0x1000, 4, 0 );

    st_DBGAPI_REG_PARAM rp;
    memset ( &rp, 0, sizeof ( rp ) );
    rp.reg_id = Z80_REG_HL;
    rp.value = 0x5A5A;
    char *args = replay_dbg_args_to_json ( DBGAPI_CMD_SET_REG, &rp );
    TEST_ASSERT_NOT_NULL ( args );
    char *err = NULL;
    TEST_ASSERT_TRUE_MESSAGE ( replay_dbg_execute ( "set_reg", args, &err ), err ? err : "" );
    g_free ( args );
    g_free ( err );

    size_t n = 0;
    uint8_t *buf = stop_and_read ( &n );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 3, n, "start, instrukce, synchronizace po přehraném set_reg" );
    parsed_v3_t s;
    parse_v3 ( buf + 2 * 32, &s );
    TEST_ASSERT_EQUAL_HEX8 ( CPUTRACK_KIND_SYNC | ( CPUTRACK_SYNC_WRITE << CPUTRACK_SYNC_REASON_SHIFT ), s.kind );
    TEST_ASSERT_EQUAL_HEX16 ( 0x5A5A, s.hl );
    g_free ( buf );
}


/** @brief Bez volby regs zůstává segment v2 (12 B) a kontrola nic nezapíše. */
void test_cputrack_v2_sync_check_noop ( void )
{
    set_cpu_state ( );
    activate_cputrack ( );
    TEST_ASSERT_EQUAL_INT ( 0, cputrack_segment_has_regs ( ) );
    g_mzarch_main.cpu->pc = 0x1001;
    cputrack_on_instruction_complete ( 0x1000, 4, 0 );
    g_mzarch_main.cpu->hl.w = 0x1111;
    cputrack_sync_check ( );
    cputrack_stop ( );
    g_cputrack_active = 0;
    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t ( 12, fsize );
    g_free ( buf );
}


/* ================================================================
 * MAIN
 * ================================================================ */

int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    /* Lifecycle */
    RUN_TEST ( test_cputrack_start_stop_basic );
    RUN_TEST ( test_cputrack_start_idempotent );
    RUN_TEST ( test_cputrack_recompute_sets_post_insn_mask );
    RUN_TEST ( test_cputrack_stop_idempotent );

    /* Initial dumps + meta */
    RUN_TEST ( test_cputrack_initial_ram_dump_size );
    RUN_TEST ( test_cputrack_initial_vram_dump_exists );
    RUN_TEST ( test_cputrack_meta_contains_initial_regs );

    /* Per-event layout */
    RUN_TEST ( test_cputrack_event_layout_basic );
    RUN_TEST ( test_cputrack_event_with_wait );
    RUN_TEST ( test_cputrack_event_wait_saturates_at_max );

    /* Collapse logic */
    RUN_TEST ( test_cputrack_self_loop_collapses_to_single_event );
    RUN_TEST ( test_cputrack_interrupt_accept_record );
    RUN_TEST ( test_cputrack_no_collapse_when_pc_changes );
    RUN_TEST ( test_cputrack_collapse_state_reset );
    RUN_TEST ( test_cputrack_collapse_wait_extra_accumulates );

    /* Formát v3 */
    RUN_TEST ( test_cputrack_v3_start_sync_and_meta );
    RUN_TEST ( test_cputrack_v3_state_after_instruction );
    RUN_TEST ( test_cputrack_v3_collapse_regs_after_last_iteration );
    RUN_TEST ( test_cputrack_v3_interrupt_accept );
    RUN_TEST ( test_cputrack_v3_sync_after_register_write );
    RUN_TEST ( test_cputrack_v3_sync_with_pending_loop );
    RUN_TEST ( test_cputrack_v3_pc_filter_return_sync );
    RUN_TEST ( test_cputrack_v3_filtered_gap_marks_no_before );
    RUN_TEST ( test_cputrack_v3_reset_collapse_then_sync );
    RUN_TEST ( test_cputrack_v3_replay_set_reg_syncs );
    RUN_TEST ( test_cputrack_v2_sync_check_noop );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
