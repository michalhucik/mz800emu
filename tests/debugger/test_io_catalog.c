/*
 * test_io_catalog.c - unit testy pro statický I/O port katalog (D.7 / V1.5).
 *
 * Testuje:
 *  - Naming konvence (= "<chip> - <function> (<dir>)" formát) per UX
 *    spec ux-io-ports.md.
 *  - Multi-entry: 0xCE má 2 entries (DMD W + Status R), 0xF0 má 2 entries
 *    (Palette W + JOY0 R).
 *  - Žádný entry nemá zastaralé "GDG status" jako 0xCF (= chyba V1).
 *  - available_for_mzarch je vždy non-zero pro každý entry.
 *  - Sentinel zakončení.
 *
 * Naming validace je manuální parser (žádný regex.h - POSIX nepřítomný
 * v MSYS2 build), kontrola:
 *   1. Ne-prázdný název
 *   2. Buď "<chip> - <function> (<dir>)" nebo "<simple> (<dir>)" (= JOY0)
 *   3. <dir> ∈ {"R", "W", "R/W"}
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>
#include <stdio.h>

#include "debugger/io_catalog.h"
#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_hwscroll.h"
#endif


void setUp ( void ) {}
void tearDown ( void ) {}


/* ================================================================
 * Naming validace
 * ================================================================ */

/**
 * Najde poslední výskyt znaku v řetězci.
 *
 * @param s    Vstupní řetězec.
 * @param ch   Hledaný znak.
 * @return Pointer na poslední výskyt, nebo NULL.
 */
static const char *find_last ( const char *s, char ch )
{
    const char *last = NULL;
    while ( *s ) {
        if ( *s == ch ) last = s;
        s++;
    }
    return last;
}


/**
 * Validace formátu name fieldu io_catalog entry.
 *
 * Akceptované formáty (UX spec ux-io-ports.md):
 *   "<prefix> (<dir>)"
 * kde <dir> ∈ {"R", "W", "R/W"} a <prefix> je libovolný neprázdný text.
 *
 * V praxi <prefix> má dvě varianty:
 *   - "<chip> - <function>"  (např. "GDG - DMD", "8255 PPI - Port A")
 *   - "<simple>"             (např. "JOY0", "JOY1")
 *
 * @param name  Vstupní řetězec.
 * @param err   Pokud non-NULL, zapíše chybovou zprávu při neúspěchu.
 * @return 1 = OK, 0 = invalid.
 */
static int name_format_valid ( const char *name, char *err, size_t err_size )
{
    if ( !name || !*name ) {
        if ( err ) snprintf ( err, err_size, "empty name" );
        return 0;
    }

    /* Najdi poslední "(...)" - direction tag musí být na konci */
    const char *open = find_last ( name, '(' );
    if ( !open ) {
        if ( err ) snprintf ( err, err_size, "no '(' in name='%s'", name );
        return 0;
    }
    const char *close = strchr ( open, ')' );
    if ( !close ) {
        if ( err ) snprintf ( err, err_size, "no closing ')' in name='%s'", name );
        return 0;
    }
    /* Po ')' musí být konec */
    if ( *( close + 1 ) != '\0' ) {
        if ( err ) snprintf ( err, err_size, "trailing chars after ')' in '%s'", name );
        return 0;
    }

    /* Direction string mezi ( a ) */
    size_t dlen = (size_t)( close - open - 1 );
    char dir[ 8 ];
    if ( dlen >= sizeof( dir ) ) {
        if ( err ) snprintf ( err, err_size, "direction too long in '%s'", name );
        return 0;
    }
    memcpy ( dir, open + 1, dlen );
    dir[ dlen ] = '\0';

    if ( strcmp ( dir, "R" ) != 0
         && strcmp ( dir, "W" ) != 0
         && strcmp ( dir, "R/W" ) != 0 ) {
        if ( err ) snprintf ( err, err_size, "bad direction '%s' in '%s'", dir, name );
        return 0;
    }

    /* Před '(' musí být alespoň jeden non-space znak + jeden space */
    if ( open == name ) {
        if ( err ) snprintf ( err, err_size, "missing prefix in '%s'", name );
        return 0;
    }
    if ( *( open - 1 ) != ' ' ) {
        if ( err ) snprintf ( err, err_size, "no space before '(' in '%s'", name );
        return 0;
    }

    return 1;
}


/**
 * Test naming konvence pro každý entry v g_io_ports[].
 */
void test_io_catalog_naming_format_all_entries ( void )
{
    char err[ 128 ];
    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        const st_IO_PORT_DESC *p = &g_io_ports[ i ];
        TEST_ASSERT_NOT_NULL_MESSAGE ( p->name, "name musí být non-NULL" );

        if ( !name_format_valid ( p->name, err, sizeof( err ) ) ) {
            char msg[ 256 ];
            snprintf ( msg, sizeof( msg ),
                       "Naming format failed at idx=%zu addr=0x%04X: %s",
                       i, p->addr, err );
            TEST_FAIL_MESSAGE ( msg );
        }
    }
}


/**
 * Test: direction tag v name musí odpovídat direction enum field.
 */
void test_io_catalog_direction_tag_matches_field ( void )
{
    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        const st_IO_PORT_DESC *p = &g_io_ports[ i ];
        const char *open = find_last ( p->name, '(' );
        TEST_ASSERT_NOT_NULL ( open );
        const char *close = strchr ( open, ')' );
        TEST_ASSERT_NOT_NULL ( close );

        char dir[ 8 ];
        size_t dlen = (size_t)( close - open - 1 );
        memcpy ( dir, open + 1, dlen );
        dir[ dlen ] = '\0';

        const char *expected = NULL;
        switch ( p->direction ) {
            case IO_PORT_DIR_R:  expected = "R";   break;
            case IO_PORT_DIR_W:  expected = "W";   break;
            case IO_PORT_DIR_RW: expected = "R/W"; break;
        }
        TEST_ASSERT_NOT_NULL ( expected );

        char msg[ 128 ];
        snprintf ( msg, sizeof( msg ),
                   "addr=0x%04X name='%s': dir tag '%s' != enum '%s'",
                   p->addr, p->name, dir, expected );
        TEST_ASSERT_EQUAL_STRING_MESSAGE ( expected, dir, msg );
    }
}


/* ================================================================
 * Multi-entry pro IN/OUT split
 * ================================================================ */

/**
 * Vrátí počet entries pro daný addr s daným direction.
 */
static size_t count_entries ( uint16_t addr, en_IO_PORT_DIR dir )
{
    size_t n = 0;
    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        if ( g_io_ports[ i ].addr == addr
             && g_io_ports[ i ].direction == dir ) n++;
    }
    return n;
}


#if (MZARCH == 800) || (MZARCH == 1500)

/**
 * 0xCE musí mít 2 entries: DMD (W) + Status (R).
 */
void test_io_catalog_0xCE_multi_entry ( void )
{
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 1, count_entries ( 0x00CE, IO_PORT_DIR_W ),
                                       "0xCE musí mít 1 entry s direction=W (DMD)" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 1, count_entries ( 0x00CE, IO_PORT_DIR_R ),
                                       "0xCE musí mít 1 entry s direction=R (Status)" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 0, count_entries ( 0x00CE, IO_PORT_DIR_RW ),
                                       "0xCE NESMÍ mít kombinovaný R/W entry (V1.5 split)" );
}


#if MZARCH == 800
/**
 * Dekodér DMD (0xCE W) popisuje všech 16 hodnot podle báze (mz-800.txt,
 * hw/09a): bit 0 = obraz A/B, bit 1 = HICOLOR, bit 2 = 640, bit 3 =
 * MZ-700 (bity 0-2 pak bez účinku); 03h a 07h nedokumentované. Bity 4-7
 * GDG ignoruje.
 */
void test_io_catalog_0xCE_dmd_decode_all_values ( void )
{
    static const char *const c_expected[ 16 ] = {
        "320x200x4 A", "320x200x4 B", "320x200x16", "320x200x16 undoc",
        "640x200x2 A", "640x200x2 B", "640x200x4", "640x200x4 undoc",
        "MZ-700 mode", "MZ-700 mode (undoc)", "MZ-700 mode (undoc)",
        "MZ-700 mode (undoc)", "MZ-700 mode (undoc)", "MZ-700 mode (undoc)",
        "MZ-700 mode (undoc)", "MZ-700 mode (undoc)"
    };
    const st_IO_PORT_DESC *dmd = NULL;
    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        if ( g_io_ports[ i ].addr == 0x00CE && g_io_ports[ i ].direction == IO_PORT_DIR_W ) {
            dmd = &g_io_ports[ i ];
        }
    }
    TEST_ASSERT_NOT_NULL ( dmd );
    TEST_ASSERT_NOT_NULL ( dmd->decode );
    for ( unsigned v = 0; v < 16; v++ ) {
        TEST_ASSERT_EQUAL_STRING ( c_expected[ v ], dmd->decode ( (uint8_t) v ) );
        TEST_ASSERT_EQUAL_STRING ( c_expected[ v ], dmd->decode ( (uint8_t) ( v | 0xF0 ) ) );
    }
}
#endif


/**
 * 0xF0 musí mít 2 entries: Palette (W) + JOY0 (R).
 */
void test_io_catalog_0xF0_multi_entry ( void )
{
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 1, count_entries ( 0x00F0, IO_PORT_DIR_W ),
                                       "0xF0 musí mít 1 entry s direction=W (Palette)" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 1, count_entries ( 0x00F0, IO_PORT_DIR_R ),
                                       "0xF0 musí mít 1 entry s direction=R (JOY0)" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 0, count_entries ( 0x00F0, IO_PORT_DIR_RW ),
                                       "0xF0 NESMÍ mít kombinovaný R/W entry (V1.5 split)" );
}

#endif /* MZARCH == 800 || 1500 */


/* ================================================================
 * Žádný "GDG status" entry na 0xCF (= V1 chyba, V1.5 odstraněno)
 *
 * V1.5.A: Stará V1 katalog měl entry "GDG status (IN)" pro 0xCF, což
 * je chybné (= 0xCF je 16-bit CRTC family pro 7 sub-registry).
 * ================================================================ */

#if (MZARCH == 800) || (MZARCH == 1500)

void test_io_catalog_no_legacy_0xCF_status ( void )
{
    /* Iteruj přes všechny 0x00CF entries (8-bit notace) a ověř že
     * žádný neexistuje. 16-bit notace 0xCF01..0xCF07 jsou legitimní. */
    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        const st_IO_PORT_DESC *p = &g_io_ports[ i ];
        if ( p->addr == 0x00CF ) {
            char msg[ 128 ];
            snprintf ( msg, sizeof( msg ),
                       "Stara V1 entry 0x00CF '%s' musi byt smazana (V1.5 cleanup)",
                       p->name );
            TEST_FAIL_MESSAGE ( msg );
        }
    }
}

#endif /* MZARCH == 800 || 1500 */


/* ================================================================
 * 0xCF 16-bit CRTC family - 7 sub-registry SOF0..CKSW
 * ================================================================ */

#if MZARCH == 800

/**
 * Vrátí entry pro daný addr nebo NULL.
 */
static const st_IO_PORT_DESC *find_entry ( uint16_t addr )
{
    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        if ( g_io_ports[ i ].addr == addr ) return &g_io_ports[ i ];
    }
    return NULL;
}


void test_io_catalog_0xCF_subregisters_present ( void )
{
    static const struct {
        uint16_t addr;
        const char *expected_name;
    } expected[] = {
        { 0xCF01, "GDG - SOF0 (W)" },
        { 0xCF02, "GDG - SOF1 (W)" },
        { 0xCF03, "GDG - SW (W)" },
        { 0xCF04, "GDG - SSA (W)" },
        { 0xCF05, "GDG - SEA (W)" },
        { 0xCF06, "GDG - BCOL (W)" },
        { 0xCF07, "GDG - CKSW (W)" },
    };

    for ( size_t i = 0; i < sizeof( expected ) / sizeof( expected[ 0 ] ); i++ ) {
        const st_IO_PORT_DESC *p = find_entry ( expected[ i ].addr );
        char msg[ 128 ];
        snprintf ( msg, sizeof( msg ),
                   "0x%04X entry chybi v g_io_ports[]", expected[ i ].addr );
        TEST_ASSERT_NOT_NULL_MESSAGE ( p, msg );

        snprintf ( msg, sizeof( msg ),
                   "0x%04X name mismatch", expected[ i ].addr );
        TEST_ASSERT_EQUAL_STRING_MESSAGE ( expected[ i ].expected_name,
                                            p->name, msg );

        snprintf ( msg, sizeof( msg ),
                   "0x%04X direction musi byt W", expected[ i ].addr );
        TEST_ASSERT_EQUAL_INT_MESSAGE ( IO_PORT_DIR_W, p->direction, msg );
    }
}


/**
 * Zrcadla registrů scrollu vrací hodnotu tak, jak ji program zapsal do portu
 * (ne interní bajtovou adresu plane, kterou drží g_hwscroll).
 */
void test_io_catalog_0xCF_scroll_values_match_port_writes ( void )
{
    static const struct {
        uint16_t addr;
        uint8_t value;
    } writes[] = {
        { 0xCF01, 0xA5 },   /* SOF_LO */
        { 0xCF02, 0x02 },   /* SOF_HI (2 bity) */
        { 0xCF03, 0x7D },   /* SW */
        { 0xCF04, 0x05 },   /* SSA */
        { 0xCF05, 0x50 },   /* SEA */
    };

    for ( size_t i = 0; i < sizeof( writes ) / sizeof( writes[ 0 ] ); i++ ) {
        hwscroll_set_reg ( writes[ i ].addr & 0xFF, writes[ i ].value );
    }
    for ( size_t i = 0; i < sizeof( writes ) / sizeof( writes[ 0 ] ); i++ ) {
        const st_IO_PORT_DESC *p = find_entry ( writes[ i ].addr );
        char msg[ 128 ];
        snprintf ( msg, sizeof( msg ), "0x%04X zrcadlo != zapsana hodnota", writes[ i ].addr );
        TEST_ASSERT_NOT_NULL_MESSAGE ( p, msg );
        TEST_ASSERT_NOT_NULL_MESSAGE ( p->read_value, msg );
        TEST_ASSERT_EQUAL_HEX8_MESSAGE ( writes[ i ].value, p->read_value ( ), msg );
    }
    hwscroll_reset ( );
}

#endif /* MZARCH == 800 */


/* ================================================================
 * available_for_mzarch musí být non-zero
 * ================================================================ */

void test_io_catalog_arch_mask_set ( void )
{
    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        const st_IO_PORT_DESC *p = &g_io_ports[ i ];
        char msg[ 128 ];
        snprintf ( msg, sizeof( msg ),
                   "addr=0x%04X name='%s' has zero available_for_mzarch",
                   p->addr, p->name );
        TEST_ASSERT_TRUE_MESSAGE ( p->available_for_mzarch != 0, msg );
    }
}


/* ================================================================
 * Sentinel
 * ================================================================ */

void test_io_catalog_sentinel ( void )
{
    /* Po posledním validním entry musí být sentinel (addr=0xFFFF, name=NULL).
     * g_io_ports_count nezahrnuje sentinel - test že na pozici count
     * je sentinel hodnota. */
    const st_IO_PORT_DESC *sentinel = &g_io_ports[ g_io_ports_count ];
    TEST_ASSERT_EQUAL_HEX16 ( 0xFFFF, sentinel->addr );
    TEST_ASSERT_NULL ( sentinel->name );
}


/* ================================================================
 * Main
 * ================================================================ */

int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    /* Pozn.: io_catalog je čistá data tabulka - mztest_init není nutný,
     * ale voláme pro konzistenci s ostatními debugger testy. */
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_io_catalog_naming_format_all_entries );
    RUN_TEST ( test_io_catalog_direction_tag_matches_field );

#if (MZARCH == 800) || (MZARCH == 1500)
    RUN_TEST ( test_io_catalog_0xCE_multi_entry );
    RUN_TEST ( test_io_catalog_0xF0_multi_entry );
    RUN_TEST ( test_io_catalog_no_legacy_0xCF_status );
#endif
#if MZARCH == 800
    RUN_TEST ( test_io_catalog_0xCE_dmd_decode_all_values );
    RUN_TEST ( test_io_catalog_0xCF_subregisters_present );
    RUN_TEST ( test_io_catalog_0xCF_scroll_values_match_port_writes );
#endif

    RUN_TEST ( test_io_catalog_arch_mask_set );
    RUN_TEST ( test_io_catalog_sentinel );

    return UNITY_END ( );
}
