/*
 * test_wd279x.c - unit testy WD279x chip vrstvy.
 *
 * Pokrývá wd279x_decode_command_type() a wd279x_command_type_name(). Tyto
 * funkce jsou čisté lookup (žádný chip state), tedy testovatelné bez
 * mztest_init() emu inicializace.
 *
 * Dále pokrývá WRITE TRACK (formátování stopy) nad místní instancí chipu
 * a mechaniky s paměťovým DSK obrazem: velikost sektoru podle kódu N
 * z ID pole (128 << N) a délku dat sektoru v Sector Info.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>
#include <stdlib.h>

#include "hw-generic/fdc/wd279x.h"
#include "hw-generic/fdc/wd279x_internal.h"
#include "hw-generic/fdc/fdc.h"
#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"

/** Chip pro testy WRITE TRACK (mimo g_fdc, aby test neměnil stav emulátoru). */
static st_WD279X s_chip;
/** Mechaniky napojené na s_chip; používá se jen mechanika 0. */
static st_FDDrive s_drives[FDC_NUM_DRIVES];

/** @brief Vynuluje místní chip a mechaniky před každým testem. */
void setUp ( void ) {
    memset ( &s_chip, 0, sizeof ( s_chip ) );
    memset ( s_drives, 0, sizeof ( s_drives ) );
}

/** @brief Zavře paměťový obraz mechaniky 0, pokud ho test otevřel. */
void tearDown ( void ) {
    if ( s_drives[0].handler_valid ) {
        generic_driver_close ( &s_drives[0].handler );
        s_drives[0].handler_valid = 0;
    }
}


/* ================================================================
 * Decoder top-nibble pokrytí - každý opcode rozsah mapovaný na enum.
 * ================================================================ */

void test_wd279x_decode_command_type_type1 ( void )
{
    /* Type I commands: Restore / Seek / Step (3 varianty - každá s/bez
     * update bit 4). */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_RESTORE,
                             wd279x_decode_command_type ( 0x00 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_RESTORE,
                             wd279x_decode_command_type ( 0x0F ) );

    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_SEEK,
                             wd279x_decode_command_type ( 0x10 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_SEEK,
                             wd279x_decode_command_type ( 0x1F ) );

    /* Step bez update: top nibble 2 nebo 3, bit 4 (= 0x10) = 0 */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_STEP,
                             wd279x_decode_command_type ( 0x20 ) );
    /* Step s update: bit 4 = 1 */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_STEP_UPDATE,
                             wd279x_decode_command_type ( 0x30 ) );

    /* Step In - bez/s update */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_STEP_IN,
                             wd279x_decode_command_type ( 0x40 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_STEP_IN_UPDATE,
                             wd279x_decode_command_type ( 0x50 ) );

    /* Step Out - bez/s update */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_STEP_OUT,
                             wd279x_decode_command_type ( 0x60 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_STEP_OUT_UPDATE,
                             wd279x_decode_command_type ( 0x70 ) );
}


void test_wd279x_decode_command_type_type2_3 ( void )
{
    /* Read Sector single (0x80) i multi (0x90) -> stejný typ. */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_READ_SECTOR,
                             wd279x_decode_command_type ( 0x80 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_READ_SECTOR,
                             wd279x_decode_command_type ( 0x90 ) );

    /* Write Sector single (0xA0) i multi (0xB0). */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_WRITE_SECTOR,
                             wd279x_decode_command_type ( 0xA0 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_WRITE_SECTOR,
                             wd279x_decode_command_type ( 0xB0 ) );

    /* Read Address (0xC0..0xCF). */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_READ_ADDRESS,
                             wd279x_decode_command_type ( 0xC0 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_READ_ADDRESS,
                             wd279x_decode_command_type ( 0xCF ) );

    /* Read Track (0xE0..0xEF). */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_READ_TRACK,
                             wd279x_decode_command_type ( 0xE0 ) );

    /* Write Track (0xF0..0xFF). */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_WRITE_TRACK,
                             wd279x_decode_command_type ( 0xF0 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_WRITE_TRACK,
                             wd279x_decode_command_type ( 0xFF ) );
}


void test_wd279x_decode_command_type_force_interrupt ( void )
{
    /* Force Interrupt 0xD0..0xDF - všechno mapuje na FORCE_INTERRUPT. */
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_FORCE_INTERRUPT,
                             wd279x_decode_command_type ( 0xD0 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_FORCE_INTERRUPT,
                             wd279x_decode_command_type ( 0xD8 ) );
    TEST_ASSERT_EQUAL_INT ( WD279X_CMD_FORCE_INTERRUPT,
                             wd279x_decode_command_type ( 0xDF ) );
}


/* ================================================================
 * Lidsky čitelné jméno - každá enum hodnota má unikátní string.
 * ================================================================ */

void test_wd279x_command_type_name_known ( void )
{
    TEST_ASSERT_EQUAL_STRING ( "Restore",
                                wd279x_command_type_name ( WD279X_CMD_RESTORE ) );
    TEST_ASSERT_EQUAL_STRING ( "Seek",
                                wd279x_command_type_name ( WD279X_CMD_SEEK ) );
    TEST_ASSERT_EQUAL_STRING ( "Step",
                                wd279x_command_type_name ( WD279X_CMD_STEP ) );
    TEST_ASSERT_EQUAL_STRING ( "Step+U",
                                wd279x_command_type_name ( WD279X_CMD_STEP_UPDATE ) );
    TEST_ASSERT_EQUAL_STRING ( "Step In",
                                wd279x_command_type_name ( WD279X_CMD_STEP_IN ) );
    TEST_ASSERT_EQUAL_STRING ( "Step In+U",
                                wd279x_command_type_name ( WD279X_CMD_STEP_IN_UPDATE ) );
    TEST_ASSERT_EQUAL_STRING ( "Step Out",
                                wd279x_command_type_name ( WD279X_CMD_STEP_OUT ) );
    TEST_ASSERT_EQUAL_STRING ( "Step Out+U",
                                wd279x_command_type_name ( WD279X_CMD_STEP_OUT_UPDATE ) );
    TEST_ASSERT_EQUAL_STRING ( "Read Sector",
                                wd279x_command_type_name ( WD279X_CMD_READ_SECTOR ) );
    TEST_ASSERT_EQUAL_STRING ( "Write Sector",
                                wd279x_command_type_name ( WD279X_CMD_WRITE_SECTOR ) );
    TEST_ASSERT_EQUAL_STRING ( "Read Address",
                                wd279x_command_type_name ( WD279X_CMD_READ_ADDRESS ) );
    TEST_ASSERT_EQUAL_STRING ( "Read Track",
                                wd279x_command_type_name ( WD279X_CMD_READ_TRACK ) );
    TEST_ASSERT_EQUAL_STRING ( "Write Track",
                                wd279x_command_type_name ( WD279X_CMD_WRITE_TRACK ) );
    TEST_ASSERT_EQUAL_STRING ( "Force INT",
                                wd279x_command_type_name ( WD279X_CMD_FORCE_INTERRUPT ) );
}


void test_wd279x_command_type_name_unknown ( void )
{
    /* Explicit UNKNOWN enum hodnota. */
    TEST_ASSERT_EQUAL_STRING ( "Unknown",
                                wd279x_command_type_name ( WD279X_CMD_UNKNOWN ) );
    /* Hodnota mimo enum range -> fallback default branch. */
    TEST_ASSERT_EQUAL_STRING ( "Unknown",
                                wd279x_command_type_name (
                                    (en_WD279X_COMMAND_TYPE) 0x55 ) );
}


/* ================================================================
 * WRITE TRACK - velikost sektoru podle kódu N a délka dat v Sector Info.
 * ================================================================ */

/**
 * @brief Připraví místní chip s mechanikou 0 nad paměťovým DSK obrazem.
 *
 * Obraz má jednu stopu 16 x 256 B; WRITE TRACK ho přeformátuje. Mechanika
 * je připravená k zápisu (mounted, handler_valid, geometry_valid).
 *
 * @return 1 = připraveno, 0 = chyba. Obraz zavírá tearDown().
 */
static int wt_prepare_drive ( void ) {
    st_FDDrive *drv = &s_drives[0];

    if ( NULL == generic_driver_open_memory ( &drv->handler, &g_memory_driver_realloc, 1024 ) ) return 0;
    drv->handler_valid = 1;

    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( desc == NULL ) return 0;
    memset ( desc, 0, desc_size );
    desc->tracks = 1;
    desc->sides = 1;
    desc->count_rules = 1;
    dsk_tools_assign_description ( desc, 0, 0, 16, DSK_SECTOR_SIZE_256, DSK_SEC_ORDER_NORMAL, NULL, 0xFF );
    int ret = dsk_tools_create_image ( &drv->handler, desc );
    free ( desc );
    if ( ret != EXIT_SUCCESS ) return 0;

    if ( EXIT_SUCCESS != dsk_get_geometry ( &drv->handler, &drv->geometry ) ) return 0;
    drv->geometry_valid = 1;
    drv->mounted = 1;
    drv->readonly = 0;

    s_chip.drives = s_drives;
    s_chip.MOTOR = 0x00; /* mechanika 0 */
    s_chip.SIDE = 0x00;
    s_chip.regTRACK = 0;
    return 1;
}


/**
 * @brief Pošle chipu proud bajtů WRITE TRACK pro stopu 0 a danou stranu.
 *
 * Proud: IAM (FCh), pro každý sektor IDAM (FEh), track, side, ID sektoru,
 * kód N, CRC (F7h), mezera, DAM (FBh), data sektoru (128 << N bajtů
 * výplně), CRC (F7h) a mezera; nakonec mezera delší než časový limit
 * fáze 4, která spustí uložení stopy do obrazu.
 *
 * @param side    Strana (0 nebo 1).
 * @param sectors Počet sektorů (ID 1..sectors).
 * @param n       Kód velikosti sektoru N (0..3).
 * @param fill    Bajt výplně dat sektoru.
 */
static void wt_format_track0 ( uint8_t side, uint8_t sectors, uint8_t n, uint8_t fill ) {
    s_chip.SIDE = side;
    do_write_track_setup ( &s_chip, 0xF4 );
    do_write_track_byte ( &s_chip, 0xFC );

    uint16_t sec_bytes = (uint16_t) ( 128u << n );
    uint8_t sec;
    uint16_t i;
    for ( sec = 1; sec <= sectors; sec++ ) {
        do_write_track_byte ( &s_chip, 0xFE );
        do_write_track_byte ( &s_chip, 0 );      /* track */
        do_write_track_byte ( &s_chip, side );   /* side */
        do_write_track_byte ( &s_chip, sec );    /* ID sektoru */
        do_write_track_byte ( &s_chip, n );      /* kód velikosti N */
        do_write_track_byte ( &s_chip, 0xF7 );   /* CRC */
        for ( i = 0; i < 22; i++ ) do_write_track_byte ( &s_chip, 0x4E );
        do_write_track_byte ( &s_chip, 0xFB );
        for ( i = 0; i < sec_bytes; i++ ) do_write_track_byte ( &s_chip, fill );
        do_write_track_byte ( &s_chip, 0xF7 );   /* CRC */
        for ( i = 0; i < 24; i++ ) do_write_track_byte ( &s_chip, 0x4E );
    }
    for ( i = 0; i < 300; i++ ) do_write_track_byte ( &s_chip, 0x4E );
}


/**
 * @brief Naformátuje obě strany stopy 0 a ověří hlavičky i data v obrazu.
 *
 * Formátování zapisuje do hlavičky obrazu dvě strany, proto se formátují
 * obě; obraz s jedinou stranou by diagnostika právem hlásila jako neúplný.
 *
 * Kontroluje: konec příkazu (fáze 5), u obou stran počet sektorů a kód N
 * v hlavičce stopy, u každého sektoru kód N a délku dat 128 << N, velikost
 * stopy v hlavičce obrazu, velikost obrazu, čitelnost posledního sektoru
 * přes libs/dsk a čistou diagnostiku obrazu.
 *
 * @param sectors Počet sektorů.
 * @param n       Kód velikosti sektoru N (0..3).
 */
static void wt_check_format ( uint8_t sectors, uint8_t n ) {
    TEST_ASSERT_TRUE ( wt_prepare_drive ( ) );
    st_HANDLER *h = &s_drives[0].handler;
    uint16_t sec_bytes = (uint16_t) ( 128u << n );
    uint8_t tsize = dsk_encode_track_size ( sectors, (en_DSK_SECTOR_SIZE) n );

    uint8_t side;
    for ( side = 0; side < 2; side++ ) {
        wt_format_track0 ( side, sectors, n, 0xE5 );
        TEST_ASSERT_EQUAL_UINT8 ( 5, s_chip.write_track_stage );
    }

    st_DSK_SHORT_IMAGE_INFO iinfo;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_image_info ( h, &iinfo ) );
    TEST_ASSERT_EQUAL_UINT8 ( 1, iinfo.tracks );
    TEST_ASSERT_EQUAL_UINT8 ( 2, iinfo.sides );

    for ( side = 0; side < 2; side++ ) {
        st_DSK_TRACK_HEADER_INFO info;
        TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_read_track_header_info ( h, side, &info ) );
        TEST_ASSERT_EQUAL_UINT8 ( 0, info.track );
        TEST_ASSERT_EQUAL_UINT8 ( side, info.side );
        TEST_ASSERT_EQUAL_UINT8 ( sectors, info.sectors );
        TEST_ASSERT_EQUAL_UINT8 ( n, info.ssize );
        uint8_t i;
        for ( i = 0; i < sectors; i++ ) {
            TEST_ASSERT_EQUAL_UINT8 ( i + 1, info.sinfo[i].sector );
            TEST_ASSERT_EQUAL_UINT8 ( n, info.sinfo[i].ssize );
            TEST_ASSERT_EQUAL_UINT16 ( sec_bytes, dsk_sector_info_get_data_length ( &info.sinfo[i] ) );
        }

        /* Velikost stopy v hlavičce obrazu odpovídá skutečné geometrii
         * (hlavička stopy + data, zaokrouhleno nahoru na 256 B). */
        TEST_ASSERT_EQUAL_UINT8 ( tsize, iinfo.tsize[side] );

        /* Poslední sektor je čitelný celý a obsahuje výplň. */
        uint8_t data[1024];
        memset ( data, 0, sizeof ( data ) );
        TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_sector ( h, side, sectors, data ) );
        uint16_t b;
        for ( b = 0; b < sec_bytes; b++ ) {
            TEST_ASSERT_EQUAL_HEX8 ( 0xE5, data[b] );
        }
    }

    TEST_ASSERT_EQUAL_UINT32 ( 0x100u + 2u * (uint32_t) tsize * 0x100u, (uint32_t) h->spec.memspec.size );

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( h );
    TEST_ASSERT_NOT_NULL ( diag );
    uint16_t flags = diag->image_flags;
    uint8_t count_tracks = diag->count_tracks;
    uint16_t track_flags = 0;
    uint8_t t;
    for ( t = 0; t < count_tracks; t++ ) track_flags |= diag->tracks[t].flags;
    dsk_tools_destroy_diag_result ( diag );
    TEST_ASSERT_EQUAL_UINT8 ( 2, count_tracks );
    TEST_ASSERT_EQUAL_HEX16 ( DSK_DIAG_TRACK_OK, track_flags );
    TEST_ASSERT_EQUAL_HEX16 ( DSK_DIAG_IMAGE_OK, flags );
}

/** @brief WRITE TRACK se sektory 128 B (N=0), lichý počet sektorů */
void test_wd279x_write_track_128 ( void ) {
    wt_check_format ( 9, 0 );
}

/** @brief WRITE TRACK se sektory 256 B (N=1) */
void test_wd279x_write_track_256 ( void ) {
    wt_check_format ( 16, 1 );
}

/** @brief WRITE TRACK se sektory 512 B (N=2) */
void test_wd279x_write_track_512 ( void ) {
    wt_check_format ( 9, 2 );
}

/** @brief WRITE TRACK se sektory 1024 B (N=3) */
void test_wd279x_write_track_1024 ( void ) {
    wt_check_format ( 5, 3 );
}

/** @brief WRITE TRACK s kódem N mimo 0..3 stopu do obrazu neuloží */
void test_wd279x_write_track_invalid_size_code ( void ) {
    TEST_ASSERT_TRUE ( wt_prepare_drive ( ) );
    st_HANDLER *h = &s_drives[0].handler;

    do_write_track_setup ( &s_chip, 0xF4 );
    do_write_track_byte ( &s_chip, 0xFC );
    do_write_track_byte ( &s_chip, 0xFE );
    do_write_track_byte ( &s_chip, 0 );
    do_write_track_byte ( &s_chip, 0 );
    do_write_track_byte ( &s_chip, 1 );
    do_write_track_byte ( &s_chip, 4 );      /* neplatný kód N */
    do_write_track_byte ( &s_chip, 0xFB );
    uint16_t i;
    for ( i = 0; i < 1024 + 2; i++ ) do_write_track_byte ( &s_chip, 0xE5 );
    for ( i = 0; i < 300; i++ ) do_write_track_byte ( &s_chip, 0x4E );
    TEST_ASSERT_EQUAL_UINT8 ( 5, s_chip.write_track_stage );

    /* Zahájení formátu stopy 0 vynulovalo počet stop a velikosti stop
     * v hlavičce obrazu; odmítnutá stopa je nesmí změnit. */
    uint8_t hdr[2] = { 0xFF, 0xFF };
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_on_offset ( h, 0x30, &hdr[0], 1 ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_on_offset ( h, 0x34, &hdr[1], 1 ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0, hdr[0] );
    TEST_ASSERT_EQUAL_UINT8 ( 0, hdr[1] );
}


/* === MAIN === */

int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_wd279x_decode_command_type_type1 );
    RUN_TEST ( test_wd279x_decode_command_type_type2_3 );
    RUN_TEST ( test_wd279x_decode_command_type_force_interrupt );
    RUN_TEST ( test_wd279x_command_type_name_known );
    RUN_TEST ( test_wd279x_command_type_name_unknown );

    RUN_TEST ( test_wd279x_write_track_128 );
    RUN_TEST ( test_wd279x_write_track_256 );
    RUN_TEST ( test_wd279x_write_track_512 );
    RUN_TEST ( test_wd279x_write_track_1024 );
    RUN_TEST ( test_wd279x_write_track_invalid_size_code );

    int result = UNITY_END ( );
    mztest_teardown ( );
    return result;
}
