/*
 * test_history_regs.c - registry před instrukcí v historii debuggeru (cpuhist)
 *
 * Testuje, že logovací čtecí callback při M1 prvního bajtu instrukce
 * zachytí do řádku g_debugger_history stav registrů PŘED instrukcí:
 *  - LD HL,nn: řádek nese HL před instrukcí, další řádek HL = nn,
 *  - R před zvýšením za M1 této instrukce (jádro přičte R až po čtení),
 *  - instrukce s prefixem DD (LD IX,nn): IX před, jeden řádek na instrukci,
 *  - IFF1/IFF2/IM v příznacích,
 *  - bez aktivního cpuhist se řádky nemění.
 *
 * Krokuje skutečné jádro (z80_step) nad RAM MZ-800 s logovacími callbacky,
 * stejně jako test_bp_integration.c.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>

#include "debugger/debugger.h"
#include "mzarch/mzarch.h"
#include "hw-generic/memory/memory.h"
#include "mzarch/mz800/memory/mz800_memory.h"
#include "libs/cpu-z80/z80.h"


void setUp ( void )
{
    g_memory.map = 0;                       /* RAM i na 0000h-1FFFh */
    z80_set_mread ( g_mzarch_main.cpu, memory_read_with_logging_cb, NULL );
    z80_set_mwrite ( g_mzarch_main.cpu, memory_write_with_logging_cb, NULL );
#ifdef MZ800EMU_CFG_RAM_FASTPATH
    /* Logovací cesta = bez RAM fast-path (v emulátoru ho vypne přepnutí
     * callbacků při aktivním cpuhist). */
    g_mzarch_main.cpu->ram_fp_enabled = false;
#endif
    memset ( &g_debugger_history, 0, sizeof ( g_debugger_history ) );
    g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_ALWAYS;
    z80_t *cpu = g_mzarch_main.cpu;
    cpu->halted = false;
    cpu->iff1 = 1;
    cpu->iff2 = 1;
    cpu->im = 1;
    cpu->sp = 0x7FF0;
}


void tearDown ( void )
{
    g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_WITH_WINDOW_OR_BP;
}


/** @brief Vložit bajty do paměti (aktuální mapování, kudy čte CPU) a nastavit PC. */
static void inject ( uint16_t addr, const uint8_t *b, size_t n )
{
    for ( size_t i = 0; i < n; i++ ) debugger_memory_write_byte ( (uint16_t) ( addr + i ), b[ i ] );
    g_mzarch_main.cpu->pc = addr;
}


/** @brief Jeden krok jádra jako hlavní smyčka (instruction_addr = PC před krokem). */
static void step ( void )
{
    g_mzarch_main.instruction_addr = g_mzarch_main.cpu->pc;
    z80_step ( g_mzarch_main.cpu );
}


/** @brief Nejnovější řádek historie. */
static const st_DEBUGGER_HISTORY_ROW *last_row ( void )
{
    return &g_debugger_history.row[ debugger_history_position ( g_debugger_history.position ) ];
}


/** @brief Řádek nese stav před instrukcí (HL, R, příznaky). */
void test_history_row_has_regs_before_instruction ( void )
{
    static const uint8_t prog[] = { 0x21, 0x34, 0x12,   /* LD HL,1234h */
                                    0x23 };             /* INC HL */
    inject ( 0x1000, prog, sizeof ( prog ) );
    z80_t *cpu = g_mzarch_main.cpu;
    cpu->hl.w = 0xAAAA;
    cpu->r = 0x10;

    step ( );
    const st_DEBUGGER_HISTORY_ROW *r1 = last_row ( );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1000, r1->addr );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0xAAAA, r1->regs.hl, "HL před LD HL,nn" );
    TEST_ASSERT_EQUAL_HEX8_MESSAGE ( 0x10, r1->regs.r, "R před M1 této instrukce" );
    TEST_ASSERT_EQUAL_HEX8 ( DEBUGGER_HISTORY_F_IFF1 | DEBUGGER_HISTORY_F_IFF2 | ( 1u << DEBUGGER_HISTORY_F_IM_SHIFT ),
                             r1->regs.flags );
    TEST_ASSERT_EQUAL_HEX16 ( 0x7FF0, r1->regs.sp );

    step ( );
    const st_DEBUGGER_HISTORY_ROW *r2 = last_row ( );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1003, r2->addr );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x1234, r2->regs.hl, "HL před INC HL = výsledek LD" );
    TEST_ASSERT_EQUAL_HEX8 ( 0x11, r2->regs.r );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1235, cpu->hl.w );
}


/** @brief Instrukce s prefixem DD: jeden řádek, IX před instrukcí. */
void test_history_prefixed_instruction_one_row ( void )
{
    static const uint8_t prog[] = { 0xDD, 0x21, 0x78, 0x56,   /* LD IX,5678h */
                                    0x00 };                   /* NOP */
    inject ( 0x1100, prog, sizeof ( prog ) );
    g_mzarch_main.cpu->ix.w = 0x0101;
    unsigned pos0 = g_debugger_history.position;
    step ( );
    TEST_ASSERT_EQUAL_UINT_MESSAGE ( pos0 + 1, g_debugger_history.position, "jeden řádek na instrukci s prefixem" );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1100, last_row ( )->addr );
    TEST_ASSERT_EQUAL_HEX16 ( 0x0101, last_row ( )->regs.ix );
    step ( );
    TEST_ASSERT_EQUAL_HEX16 ( 0x5678, last_row ( )->regs.ix );
}


/** @brief Bez aktivního cpuhist se historie nemění. */
void test_history_inactive_keeps_rows ( void )
{
    static const uint8_t prog[] = { 0x00 };
    inject ( 0x1200, prog, sizeof ( prog ) );
    g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_OFF;
    unsigned pos0 = g_debugger_history.position;
    step ( );
    TEST_ASSERT_EQUAL_UINT ( pos0, g_debugger_history.position );
}


int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );
    RUN_TEST ( test_history_row_has_regs_before_instruction );
    RUN_TEST ( test_history_prefixed_instruction_one_row );
    RUN_TEST ( test_history_inactive_keeps_rows );
    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
