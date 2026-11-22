/**
 * @file test_mz800_vramwait.c
 * @brief Testy modelu WAIT u přístupu CPU do VRAM přes GDG (mz800_vramwait.h).
 *
 * Očekávané hodnoty jsou převzaté ze simulace netlistu GDG (C model
 * `c-whid-0` projektu gdg-mz800, časování sběrnice Z80 podle UM0080):
 * sonda měřila WAIT pro začátek přístupu na každé pozici řádku v DMD 00h,
 * 04h a 08h. Každý případ uvádí pozici T1r v CLK0 od sestupné hrany nHSYN
 * a počet TW naměřený v simulaci. Model se se simulací shoduje ve všech
 * 129 504 měřeních; tady je jen výběr (zatemnění a viditelná část,
 * samostatné přístupy a dvojice s mezerou 0 a 4 T).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "mzarch/mz800/gdg/mz800_vramwait.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_video.h"

#if MZARCH != 800
#error "test_mz800_vramwait je jen pro MZARCH=800"
#endif

/** @brief Začátek časové osy testu (celý počet řádků, aby pozice = čas mod řádek). */
#define BASE ( (uint64_t) 100u * VRAMWAIT_LINE_CLK0 )

/** @brief Režim 320x200 (DMD 00h): rozvrh MZ-800, dva zápisové sloty. */
#define M320 VRAMWAIT_GRID_MZ800, 1
/** @brief Režim 640x200 (DMD 04h): rozvrh MZ-800, jeden zápisový slot. */
#define M640 VRAMWAIT_GRID_MZ800, 0
/** @brief Režim MZ-700 (DMD 08h): rozvrh MZ-700, jeden zápisový slot. */
#define M700 VRAMWAIT_GRID_MZ700, 0


void setUp ( void )
{
}

void tearDown ( void )
{
}


/** @brief Přístup s prázdným stavem; vrátí TW. */
static unsigned single ( char kind, unsigned pos, en_VRAMWAIT_GRID grid, int two )
{
    uint64_t wrel = 0;
    if ( kind == 'R' ) return vramwait_read ( wrel, BASE + pos, grid );
    return vramwait_write ( &wrel, BASE + pos, grid, two );
}


/** @brief Dvojice přístupů s prázdným stavem; vrátí TW druhého. */
static unsigned pair ( char k1, unsigned p1, char k2, unsigned p2, en_VRAMWAIT_GRID grid, int two )
{
    uint64_t wrel = 0;
    if ( k1 == 'R' ) ( void ) vramwait_read ( wrel, BASE + p1, grid );
    else ( void ) vramwait_write ( &wrel, BASE + p1, grid, two );
    if ( k2 == 'R' ) return vramwait_read ( wrel, BASE + p2, grid );
    return vramwait_write ( &wrel, BASE + p2, grid, two );
}


/**
 * @brief Čtení v zatemnění: pila s periodou 8 CLK0 (CPU slot každých 8 CLK0).
 */
void test_read_blanking_sawtooth ( void )
{
    TEST_ASSERT_EQUAL_UINT ( 2u, single ( 'R', 3, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 4u, single ( 'R', 4, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 3u, single ( 'R', 5, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 3u, single ( 'R', 9, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 2u, single ( 'R', 10, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 2u, single ( 'R', 11, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 4u, single ( 'R', 12, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 2u, single ( 'R', 315, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 3u, single ( 'R', 997, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 4u, single ( 'R', 1004, M320 ) );
}


/**
 * @brief Čtení ve viditelné části MZ-800: pila s periodou 16 CLK0, 2-5 TW.
 */
void test_read_visible_sawtooth ( void )
{
    TEST_ASSERT_EQUAL_UINT ( 5u, single ( 'R', 316, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 4u, single ( 'R', 320, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 3u, single ( 'R', 325, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 2u, single ( 'R', 331, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 5u, single ( 'R', 444, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 4u, single ( 'R', 401, M640 ) );
}


/**
 * @brief Čtení v režimu MZ-700: v zatemnění 2-4 TW, ve viditelné části
 *        až do jejího konce.
 */
void test_read_mz700 ( void )
{
    TEST_ASSERT_EQUAL_UINT ( 4u, single ( 'R', 4, M700 ) );
    TEST_ASSERT_EQUAL_UINT ( 2u, single ( 'R', 307, M700 ) );
    TEST_ASSERT_EQUAL_UINT ( 132u, single ( 'R', 308, M700 ) );
    TEST_ASSERT_EQUAL_UINT ( 113u, single ( 'R', 401, M700 ) );
}


/**
 * @brief Samostatný zápis jde přes buffer a nečeká v žádném režimu.
 */
void test_single_write_no_wait ( void )
{
    TEST_ASSERT_EQUAL_UINT ( 0u, single ( 'W', 2, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 0u, single ( 'W', 400, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 0u, single ( 'W', 400, M640 ) );
    TEST_ASSERT_EQUAL_UINT ( 0u, single ( 'W', 400, M700 ) );
}


/**
 * @brief Dvojice W+W: druhý zápis čeká na zápis prvního do DRAM.
 */
void test_write_write ( void )
{
    TEST_ASSERT_EQUAL_UINT ( 2u, pair ( 'W', 3, 'W', 18, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 4u, pair ( 'W', 401, 'W', 416, M320 ) );
    /* uvolnění WRITE FF 9 CLK0 po slotu (měřeno na MZ-800); s 8 CLK0 by bylo 0 */
    TEST_ASSERT_EQUAL_UINT ( 1u, pair ( 'W', 400, 'W', 435, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 1u, pair ( 'W', 3, 'W', 18, M640 ) );
    TEST_ASSERT_EQUAL_UINT ( 1u, pair ( 'W', 401, 'W', 416, M640 ) );
    TEST_ASSERT_EQUAL_UINT ( 1u, pair ( 'W', 3, 'W', 18, M700 ) );
    TEST_ASSERT_EQUAL_UINT ( 110u, pair ( 'W', 401, 'W', 416, M700 ) );
    TEST_ASSERT_EQUAL_UINT ( 106u, pair ( 'W', 400, 'W', 435, M700 ) );
}


/**
 * @brief Dvojice W+R: čtení čeká na zápis do DRAM a pak na svůj CPU slot.
 */
void test_write_read ( void )
{
    TEST_ASSERT_EQUAL_UINT ( 7u, pair ( 'W', 5, 'R', 20, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 7u, pair ( 'W', 402, 'R', 417, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 3u, pair ( 'W', 3, 'R', 38, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 5u, pair ( 'W', 5, 'R', 20, M640 ) );
    TEST_ASSERT_EQUAL_UINT ( 4u, pair ( 'W', 402, 'R', 417, M640 ) );
    TEST_ASSERT_EQUAL_UINT ( 113u, pair ( 'W', 402, 'R', 417, M700 ) );
    TEST_ASSERT_EQUAL_UINT ( 109u, pair ( 'W', 401, 'R', 436, M700 ) );
}


/**
 * @brief Dvojice R+R a R+W: čtení stav zápisu nemění, zápis po čtení nečeká.
 */
void test_read_then_access ( void )
{
    TEST_ASSERT_EQUAL_UINT ( 3u, pair ( 'R', 1, 'R', 31, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 4u, pair ( 'R', 400, 'R', 435, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 2u, pair ( 'R', 4, 'R', 59, M640 ) );
    TEST_ASSERT_EQUAL_UINT ( 4u, pair ( 'R', 400, 'R', 980, M700 ) );
    TEST_ASSERT_EQUAL_UINT ( 0u, pair ( 'R', 4, 'W', 39, M320 ) );
    TEST_ASSERT_EQUAL_UINT ( 0u, pair ( 'R', 401, 'W', 981, M700 ) );
}


/**
 * @brief Rozvrh CPU slotů na hranicích úseků řádku a přes konec řádku.
 */
void test_next_cpu_slot_boundaries ( void )
{
    TEST_ASSERT_EQUAL_UINT64 ( BASE + 320u, vramwait_next_cpu_slot ( BASE + 320u, VRAMWAIT_GRID_MZ800 ) );
    TEST_ASSERT_EQUAL_UINT64 ( BASE + 336u, vramwait_next_cpu_slot ( BASE + 321u, VRAMWAIT_GRID_MZ800 ) );
    TEST_ASSERT_EQUAL_UINT64 ( BASE + 352u, vramwait_next_cpu_slot ( BASE + 337u, VRAMWAIT_GRID_MZ800 ) );
    TEST_ASSERT_EQUAL_UINT64 ( BASE + 960u, vramwait_next_cpu_slot ( BASE + 945u, VRAMWAIT_GRID_MZ800 ) );
    TEST_ASSERT_EQUAL_UINT64 ( BASE + 968u, vramwait_next_cpu_slot ( BASE + 961u, VRAMWAIT_GRID_MZ800 ) );
    TEST_ASSERT_EQUAL_UINT64 ( BASE + VRAMWAIT_LINE_CLK0, vramwait_next_cpu_slot ( BASE + 1129u, VRAMWAIT_GRID_MZ800 ) );
    TEST_ASSERT_EQUAL_UINT64 ( BASE + 312u, vramwait_next_cpu_slot ( BASE + 312u, VRAMWAIT_GRID_MZ700 ) );
    TEST_ASSERT_EQUAL_UINT64 ( BASE + 960u, vramwait_next_cpu_slot ( BASE + 313u, VRAMWAIT_GRID_MZ700 ) );
}


/**
 * @brief Kotva časové osy: sestupná hrana nHSYN (sloupec
 *        VIDEO_REAL_HSYNC_START_COLUMN) je pozice 0 modelu; posun 186 CLK0
 *        je určen měřením na MZ-800. Délka řádku shodná.
 */
void test_time_axis_anchor_measured ( void )
{
    TEST_ASSERT_EQUAL_UINT ( 186u, GDG_VRAMWAIT_TICKS_TO_LINE_OFFSET );
    TEST_ASSERT_EQUAL_UINT ( 0u, ( VIDEO_REAL_HSYNC_START_COLUMN + GDG_VRAMWAIT_TICKS_TO_LINE_OFFSET ) % VRAMWAIT_LINE_CLK0 );
    TEST_ASSERT_EQUAL_UINT ( VRAMWAIT_LINE_CLK0, VIDEO_SCREEN_WIDTH );
}


int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );
    RUN_TEST ( test_read_blanking_sawtooth );
    RUN_TEST ( test_read_visible_sawtooth );
    RUN_TEST ( test_read_mz700 );
    RUN_TEST ( test_single_write_no_wait );
    RUN_TEST ( test_write_write );
    RUN_TEST ( test_write_read );
    RUN_TEST ( test_read_then_access );
    RUN_TEST ( test_next_cpu_slot_boundaries );
    RUN_TEST ( test_time_axis_anchor_measured );
    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
