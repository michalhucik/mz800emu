/**
 * @file   test_generic_driver.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Unit testy knihovny generic_driver (generický I/O driver).
 *
 * Testuje: jádrové API (setup, register, open/close, read/write, truncate,
 *          prepare, direct_read), chybové stavy, readonly ochrana,
 *          memory leak prevence, error messages.
 *
 * Test vyžaduje emulátorový kontext (memory_driver_init, file_driver_init).
 * Linkuje se s TEST_ALL_OBJS + Unity.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
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
 */

#include "mztest.h"

#include <string.h>
#include <unistd.h>

#include "libs/generic_driver/generic_driver.h"
#include "generic_driver/memory_driver.h"
#include "generic_driver/file_driver.h"


/* ========================================================================
 * setUp / tearDown — volány před/po každém testu
 * ======================================================================== */

/** @brief Inicializace před každým testem */
void setUp ( void ) {
}


/** @brief Úklid po každém testu */
void tearDown ( void ) {
}


/* ========================================================================
 * Pomocné funkce
 * ======================================================================== */

/**
 * @brief Vytvoří dočasný soubor se zadaným obsahem a vrátí jeho cestu.
 *
 * Volající je zodpovědný za smazání souboru (remove).
 * Soubor se vytváří v aktuálním adresáři (MinGW na Windows nepřekládá /tmp).
 *
 * @param data ukazatel na data k zápisu
 * @param size velikost dat v bajtech
 * @return cesta k vytvořenému souboru (statický buffer)
 */
static char tmp_filepath[256];
static int tmp_counter = 0;

static char* create_tmp_file ( const uint8_t *data, size_t size ) {
    snprintf ( tmp_filepath, sizeof ( tmp_filepath ), "test_gd_%d_%d.bin", (int) getpid (), tmp_counter++ );
    FILE *f = fopen ( tmp_filepath, "wb" );
    if ( f ) {
        fwrite ( data, 1, size, f );
        fclose ( f );
    }
    return tmp_filepath;
}


/* ========================================================================
 * Testy — registrace a setup
 * ======================================================================== */

/** @brief Testuje registraci paměťového handleru */
void test_register_handler_memory ( void ) {
    st_HANDLER h;
    memset ( &h, 0xFF, sizeof ( h ) );

    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );

    TEST_ASSERT_EQUAL_INT ( HANDLER_TYPE_MEMORY, h.type );
    TEST_ASSERT_EQUAL_INT ( HANDLER_STATUS_NOT_READY, h.status );
    TEST_ASSERT_EQUAL_INT ( HANDLER_ERROR_NONE, h.err );
    TEST_ASSERT_NULL ( h.driver );
}


/** @brief Testuje registraci souborového handleru */
void test_register_handler_file ( void ) {
    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_FILE );

    TEST_ASSERT_EQUAL_INT ( HANDLER_TYPE_FILE, h.type );
    TEST_ASSERT_EQUAL_INT ( HANDLER_STATUS_NOT_READY, h.status );
}


/** @brief Testuje inicializaci driveru s NULL callbacky */
void test_setup_driver ( void ) {
    st_DRIVER d;
    memset ( &d, 0xFF, sizeof ( d ) );

    generic_driver_setup ( &d, NULL, NULL, NULL, NULL, NULL, NULL );

    TEST_ASSERT_NULL ( d.open_cb );
    TEST_ASSERT_NULL ( d.close_cb );
    TEST_ASSERT_NULL ( d.read_cb );
    TEST_ASSERT_NULL ( d.write_cb );
    TEST_ASSERT_NULL ( d.prepare_cb );
    TEST_ASSERT_NULL ( d.truncate_cb );
    TEST_ASSERT_EQUAL_INT ( GENERIC_DRIVER_ERROR_NONE, d.err );
}


/* ========================================================================
 * Testy — paměťový handler: otevření, čtení, zápis, uzavření
 * ======================================================================== */

/** @brief Testuje otevření a zavření paměťového handleru */
void test_memory_open_close ( void ) {
    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );

    st_HANDLER *result = generic_driver_open_memory ( &h, &g_memory_driver_static, 256 );

    TEST_ASSERT_NOT_NULL ( result );
    TEST_ASSERT_EQUAL_PTR ( &h, result );
    TEST_ASSERT_EQUAL_INT ( HANDLER_STATUS_READY, h.status );
    TEST_ASSERT_EQUAL_INT ( HANDLER_TYPE_MEMORY, h.type );
    TEST_ASSERT_NOT_NULL ( h.spec.memspec.ptr );
    TEST_ASSERT_EQUAL_UINT32 ( 256, h.spec.memspec.size );

    int ret = generic_driver_close ( &h );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_INT ( HANDLER_STATUS_NOT_READY, h.status );
    TEST_ASSERT_NULL ( h.spec.memspec.ptr );
}


/** @brief Testuje otevření paměťového handleru s NULL handlerem (dynamická alokace) */
void test_memory_open_null_handler ( void ) {
    /* Pokud handler == NULL, alokuje se dynamicky */
    st_HANDLER *h = generic_driver_open_memory ( NULL, &g_memory_driver_static, 64 );

    TEST_ASSERT_NOT_NULL ( h );
    TEST_ASSERT_EQUAL_INT ( HANDLER_STATUS_READY, h->status );
    TEST_ASSERT_EQUAL_UINT32 ( 64, h->spec.memspec.size );

    generic_driver_close ( h );
    free ( h );
}


/** @brief Testuje zápis a zpětné čtení dat v paměťovém handleru */
void test_memory_write_read ( void ) {
    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 256 );

    uint8_t write_data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    int ret = generic_driver_write ( &h, 10, write_data, sizeof ( write_data ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );

    uint8_t read_data[4] = { 0 };
    ret = generic_driver_read ( &h, 10, read_data, sizeof ( read_data ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );

    TEST_ASSERT_EQUAL_UINT8_ARRAY ( write_data, read_data, sizeof ( write_data ) );

    generic_driver_close ( &h );
}


/** @brief Testuje zápis a čtení na hranicích bufferu (začátek a konec) */
void test_memory_write_at_boundaries ( void ) {
    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 16 );

    /* Zápis na začátek */
    uint8_t data = 0xAA;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, generic_driver_write ( &h, 0, &data, 1 ) );

    /* Zápis na konec */
    data = 0xBB;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, generic_driver_write ( &h, 15, &data, 1 ) );

    /* Čtení zpět */
    uint8_t result;
    generic_driver_read ( &h, 0, &result, 1 );
    TEST_ASSERT_EQUAL_HEX8 ( 0xAA, result );

    generic_driver_read ( &h, 15, &result, 1 );
    TEST_ASSERT_EQUAL_HEX8 ( 0xBB, result );

    generic_driver_close ( &h );
}


/** @brief Testuje čtení za koncem paměťového bufferu (musí selhat) */
void test_memory_read_beyond_size ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 16 );

    uint8_t buf[4];
    /* Offset + size > handler size — musí selhat */
    int ret = generic_driver_read ( &h, 14, buf, 4 );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );

    /* Offset za koncem — musí selhat */
    ret = generic_driver_read ( &h, 20, buf, 1 );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );

    generic_driver_close ( &h );
}


/** @brief Testuje zápis za koncem paměťového bufferu (musí selhat) */
void test_memory_write_beyond_size ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 16 );

    uint8_t buf[4] = { 0 };
    /* Offset + size > handler size — musí selhat */
    int ret = generic_driver_write ( &h, 14, buf, 4 );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );

    generic_driver_close ( &h );
}


/* ========================================================================
 * Testy — paměťový handler s realokací
 * ======================================================================== */

/** @brief Testuje paměťový handler s realokačním driverem (automatické zvětšení bufferu) */
void test_memory_realloc_driver ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_realloc, 16 );

    /* Zápis za konec — realloc driver automaticky zvětší buffer */
    uint8_t data[] = { 0x11, 0x22, 0x33 };
    int ret = generic_driver_write ( &h, 32, data, sizeof ( data ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_TRUE ( h.spec.memspec.size >= 35 );

    /* Zpětné čtení */
    uint8_t readback[3] = { 0 };
    ret = generic_driver_read ( &h, 32, readback, sizeof ( readback ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( data, readback, sizeof ( data ) );

    generic_driver_close ( &h );
}


/* ========================================================================
 * Testy — readonly ochrana
 * ======================================================================== */

/** @brief Testuje nastavení a zrušení readonly ochrany a její vliv na zápis/čtení */
void test_readonly_status ( void ) {
    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 64 );

    /* Nastavit readonly */
    generic_driver_set_handler_readonly_status ( &h, 1 );
    TEST_ASSERT_TRUE ( h.status & HANDLER_STATUS_READ_ONLY );

    /* Zápis by měl selhat */
    uint8_t data = 0x42;
    int ret = generic_driver_write ( &h, 0, &data, 1 );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );

    /* Čtení by mělo projít */
    uint8_t read;
    ret = generic_driver_read ( &h, 0, &read, 1 );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );

    /* Zrušit readonly */
    generic_driver_set_handler_readonly_status ( &h, 0 );
    TEST_ASSERT_FALSE ( h.status & HANDLER_STATUS_READ_ONLY );

    /* Zápis by měl projít */
    ret = generic_driver_write ( &h, 0, &data, 1 );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );

    generic_driver_close ( &h );
}


/* ========================================================================
 * Testy — prepare a direct_read
 * ======================================================================== */

/** @brief Testuje přímý přístup k paměti přes prepare a direct_read */
void test_prepare_memory_direct_access ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 256 );

    /* Zapsat referenční data */
    uint8_t ref[] = { 0xCA, 0xFE, 0xBA, 0xBE };
    generic_driver_write ( &h, 100, ref, sizeof ( ref ) );

    /* Direct read — pro paměťový handler by buffer měl ukazovat přímo do interních dat */
    uint8_t work_buffer[4];
    uint8_t *result = NULL;
    int ret = generic_driver_direct_read ( &h, 100, (void **) &result, work_buffer, sizeof ( work_buffer ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_NOT_NULL ( result );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( ref, result, sizeof ( ref ) );

    /* Pro paměťový handler by result měl ukazovat přímo do memspec->ptr, ne do work_buffer */
    TEST_ASSERT_EQUAL_PTR ( &h.spec.memspec.ptr[100], result );

    generic_driver_close ( &h );
}


/** @brief Testuje, že prepare bez prepare_cb nastaví buffer na work_buffer */
void test_prepare_sets_work_buffer_when_no_prepare_cb ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* Driver bez prepare_cb — prepare by měl nastavit *buffer na work_buffer */
    st_DRIVER d;
    generic_driver_setup ( &d, NULL, NULL, NULL, NULL, NULL, NULL );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    h.driver = &d;
    h.status = HANDLER_STATUS_READY;
    h.spec.memspec.ptr = (uint8_t *) malloc ( 64 );
    h.spec.memspec.size = 64;

    uint8_t work_buffer[16];
    void *buffer = NULL;
    int ret = generic_driver_prepare ( &h, 0, &buffer, work_buffer, sizeof ( work_buffer ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_PTR ( work_buffer, buffer );

    free ( h.spec.memspec.ptr );
}


/* ========================================================================
 * Testy — truncate
 * ======================================================================== */

/** @brief Testuje truncate paměťového handleru (zmenšení i zvětšení) */
void test_memory_truncate ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 256 );

    /* Zmenšit */
    int ret = generic_driver_truncate ( &h, 64 );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_UINT32 ( 64, h.spec.memspec.size );

    /* Zvětšit */
    ret = generic_driver_truncate ( &h, 512 );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_UINT32 ( 512, h.spec.memspec.size );

    generic_driver_close ( &h );
}


/** @brief Testuje, že truncate na readonly handleru selže */
void test_memory_truncate_readonly_fails ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 256 );

    generic_driver_set_handler_readonly_status ( &h, 1 );

    int ret = generic_driver_truncate ( &h, 64 );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );

    generic_driver_set_handler_readonly_status ( &h, 0 );
    generic_driver_close ( &h );
}


/* ========================================================================
 * Testy — updated flag
 * ======================================================================== */

/** @brief Testuje nastavení updated flagu po zápisu do paměťového handleru */
void test_memory_updated_flag ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 64 );

    /* Po otevření by updated mělo být 0 */
    TEST_ASSERT_EQUAL_INT ( 0, h.spec.memspec.updated );

    /* Po zápisu by updated mělo být 1 */
    uint8_t data = 0x42;
    generic_driver_write ( &h, 0, &data, 1 );
    TEST_ASSERT_EQUAL_INT ( 1, h.spec.memspec.updated );

    generic_driver_close ( &h );
}


/* ========================================================================
 * Testy — chybové zprávy
 * ======================================================================== */

/** @brief Testuje chybovou zprávu při stavu bez chyby */
void test_error_message_no_error ( void ) {
    st_HANDLER h;
    st_DRIVER d;
    h.err = HANDLER_ERROR_NONE;
    d.err = GENERIC_DRIVER_ERROR_NONE;

    const char *msg = generic_driver_error_message ( &h, &d );
    TEST_ASSERT_EQUAL_STRING ( "no error", msg );
}


/** @brief Testuje chybové zprávy při chybách driveru (read, fopen) */
void test_error_message_driver_error ( void ) {
    st_HANDLER h;
    st_DRIVER d;
    h.err = HANDLER_ERROR_NONE;

    d.err = GENERIC_DRIVER_ERROR_READ;
    const char *msg = generic_driver_error_message ( &h, &d );
    TEST_ASSERT_EQUAL_STRING ( "read error", msg );

    d.err = GENERIC_DRIVER_ERROR_FOPEN;
    msg = generic_driver_error_message ( &h, &d );
    TEST_ASSERT_EQUAL_STRING ( "fopen error", msg );
}


/** @brief Testuje chybové zprávy při chybách handleru (not ready, write protected) */
void test_error_message_handler_error ( void ) {
    st_HANDLER h;
    st_DRIVER d;
    d.err = GENERIC_DRIVER_ERROR_NONE;

    h.err = HANDLER_ERROR_NOT_READY;
    const char *msg = generic_driver_error_message ( &h, &d );
    TEST_ASSERT_EQUAL_STRING ( "handler not ready", msg );

    h.err = HANDLER_ERROR_WRITE_PROTECTED;
    msg = generic_driver_error_message ( &h, &d );
    TEST_ASSERT_EQUAL_STRING ( "handler is write protected", msg );
}


/** @brief Testuje chybovou zprávu při NULL handleru a driveru */
void test_error_message_null_driver ( void ) {
    const char *msg = generic_driver_error_message ( NULL, NULL );
    TEST_ASSERT_EQUAL_STRING ( "unknown error", msg );
}


/** @brief Testuje chybovou zprávu pro neznámý typ chyby */
void test_error_message_unknown ( void ) {
    st_HANDLER h;
    st_DRIVER d;
    d.err = GENERIC_DRIVER_ERROR_UNKNOWN;
    const char *msg = generic_driver_error_message ( &h, &d );
    TEST_ASSERT_EQUAL_STRING ( "unknown error", msg );
}


/* ========================================================================
 * Testy — chybové stavy (robustnost)
 * ======================================================================== */

/** @brief Testuje otevření paměťového handleru s NULL driverem (musí selhat) */
void test_open_memory_null_driver ( void ) {
    st_HANDLER h;
    st_HANDLER *result = generic_driver_open_memory ( &h, NULL, 64 );
    TEST_ASSERT_NULL ( result );
}


/** @brief Testuje zavření handleru bez přiřazeného driveru (musí selhat) */
void test_close_null_driver ( void ) {
    st_HANDLER h;
    memset ( &h, 0, sizeof ( h ) );
    h.driver = NULL;

    int ret = generic_driver_close ( &h );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );
}


/** @brief Testuje souborový bootstrap na paměťovém handleru (musí selhat) */
void test_bootstrap_wrong_type ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 64 );

    /* Zkusit souborový bootstrap na paměťovém handleru */
    int ret = generic_driver_file_operation_internal_bootstrap ( &h );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );

    generic_driver_close ( &h );
}


/** @brief Testuje paměťový bootstrap na nepřipraveném handleru (musí selhat) */
void test_bootstrap_not_ready ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    st_DRIVER d;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    h.driver = &d;
    /* Status = NOT_READY, ptr = NULL */

    int ret = generic_driver_memory_operation_internal_bootstrap ( &h );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );
    TEST_ASSERT_EQUAL_INT ( HANDLER_ERROR_NOT_READY, h.err );
}


/* ========================================================================
 * Testy — souborový handler
 * ======================================================================== */

/** @brief Testuje otevření, čtení a zavření souborového handleru */
void test_file_open_read_close ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* Vytvořit dočasný soubor */
    uint8_t content[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    char *path = create_tmp_file ( content, sizeof ( content ) );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_FILE );

    st_HANDLER *result = generic_driver_open_file ( &h, &g_file_driver, path, FILE_DRIVER_OPMODE_RO );
    TEST_ASSERT_NOT_NULL_MESSAGE ( result, "Nelze otevřít dočasný soubor" );
    TEST_ASSERT_TRUE ( h.status & HANDLER_STATUS_READY );
    TEST_ASSERT_TRUE ( h.status & HANDLER_STATUS_READ_ONLY );

    /* Čtení */
    uint8_t buf[4] = { 0 };
    int ret = generic_driver_read ( &h, 2, buf, 4 );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_HEX8 ( 0x03, buf[0] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x04, buf[1] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x05, buf[2] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x06, buf[3] );

    generic_driver_close ( &h );
    TEST_ASSERT_EQUAL_INT ( HANDLER_STATUS_NOT_READY, h.status );

    remove ( path );
}


/** @brief Testuje zápis a zpětné čtení v souborovém handleru (RW režim) */
void test_file_write_read ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    uint8_t content[] = { 0x00, 0x00, 0x00, 0x00 };
    char *path = create_tmp_file ( content, sizeof ( content ) );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_FILE );
    st_HANDLER *result = generic_driver_open_file ( &h, &g_file_driver, path, FILE_DRIVER_OPMODE_RW );
    TEST_ASSERT_NOT_NULL ( result );
    TEST_ASSERT_FALSE ( h.status & HANDLER_STATUS_READ_ONLY );

    /* Zápis */
    uint8_t write_data[] = { 0xAB, 0xCD };
    int ret = generic_driver_write ( &h, 1, write_data, sizeof ( write_data ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );

    /* Zpětné čtení */
    uint8_t read_data[2] = { 0 };
    ret = generic_driver_read ( &h, 1, read_data, sizeof ( read_data ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( write_data, read_data, sizeof ( write_data ) );

    generic_driver_close ( &h );
    remove ( path );
}


/** @brief Testuje, že zápis do readonly souborového handleru selže */
void test_file_readonly_write_fails ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    uint8_t content[] = { 0x00 };
    char *path = create_tmp_file ( content, sizeof ( content ) );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_FILE );
    generic_driver_open_file ( &h, &g_file_driver, path, FILE_DRIVER_OPMODE_RO );

    uint8_t data = 0x42;
    int ret = generic_driver_write ( &h, 0, &data, 1 );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );

    generic_driver_close ( &h );
    remove ( path );
}


/* ========================================================================
 * Testy — open_memory_from_file
 * ======================================================================== */

/** @brief Testuje načtení souboru do paměťového handleru */
void test_open_memory_from_file ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    uint8_t content[] = { 0x10, 0x20, 0x30, 0x40, 0x50 };
    char *path = create_tmp_file ( content, sizeof ( content ) );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    st_HANDLER *result = generic_driver_open_memory_from_file ( &h, &g_memory_driver_static, path );

    TEST_ASSERT_NOT_NULL_MESSAGE ( result, "Nelze otevřít soubor do paměti" );
    TEST_ASSERT_EQUAL_UINT32 ( sizeof ( content ), h.spec.memspec.size );

    /* Ověřit obsah */
    uint8_t buf[5] = { 0 };
    generic_driver_read ( &h, 0, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( content, buf, sizeof ( content ) );

    generic_driver_close ( &h );
    remove ( path );
}


/** @brief Testuje načtení neexistujícího souboru do paměti (musí selhat) */
void test_open_memory_from_nonexistent_file ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    st_HANDLER *result = generic_driver_open_memory_from_file ( &h, &g_memory_driver_static, "nonexistent_test_file_gd_12345.bin" );

    TEST_ASSERT_NULL ( result );
}


/* ========================================================================
 * Testy — save_memory
 * ======================================================================== */

/** @brief Testuje uložení paměťového handleru do souboru a zpětné ověření obsahu */
void test_save_memory ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 8 );

    uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE };
    generic_driver_write ( &h, 0, data, sizeof ( data ) );

    char save_path[256];
    snprintf ( save_path, sizeof ( save_path ), "test_gd_save_%d.bin", (int) getpid () );

    int ret = generic_driver_save_memory ( &h, save_path );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );

    /* Ověřit obsah souboru zpětným čtením */
    FILE *f = fopen ( save_path, "rb" );
    TEST_ASSERT_NOT_NULL ( f );
    uint8_t readback[8] = { 0 };
    size_t readback_bytes = fread ( readback, 1, sizeof ( readback ), f );
    fclose ( f );
    TEST_ASSERT_EQUAL_size_t ( sizeof ( readback ), readback_bytes );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( data, readback, sizeof ( data ) );

    generic_driver_close ( &h );
    remove ( save_path );
}


/* ========================================================================
 * Testy — ppread / ppwrite (dvoustupňové I/O)
 * ======================================================================== */

/** @brief Testuje dvoustupňové I/O (prepare + ppwrite / ppread) */
void test_ppread_ppwrite ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER h;
    generic_driver_register_handler ( &h, HANDLER_TYPE_MEMORY );
    generic_driver_open_memory ( &h, &g_memory_driver_static, 128 );

    /* Prepare + ppwrite */
    void *buffer = NULL;
    uint8_t work_buf[4];
    int ret = generic_driver_prepare ( &h, 50, &buffer, work_buf, sizeof ( work_buf ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_NOT_NULL ( buffer );

    uint8_t data[] = { 0xAA, 0xBB, 0xCC, 0xDD };
    ret = generic_driver_ppwrite ( &h, 50, data, sizeof ( data ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );

    /* Prepare + ppread */
    buffer = NULL;
    ret = generic_driver_prepare ( &h, 50, &buffer, work_buf, sizeof ( work_buf ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );

    uint8_t readback[4] = { 0 };
    ret = generic_driver_ppread ( &h, 50, readback, sizeof ( readback ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( data, readback, sizeof ( data ) );

    generic_driver_close ( &h );
}


/* ========================================================================
 * Testy — driver bez callbacku
 * ======================================================================== */

/** @brief Testuje ppread bez read callbacku (musí selhat) */
void test_ppread_without_read_cb ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_DRIVER d;
    generic_driver_setup ( &d, NULL, NULL, NULL, NULL, NULL, NULL );

    st_HANDLER h;
    memset ( &h, 0, sizeof ( h ) );
    h.driver = &d;

    uint8_t buf[4];
    int ret = generic_driver_ppread ( &h, 0, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );
    TEST_ASSERT_EQUAL_INT ( GENERIC_DRIVER_ERROR_CB_NOT_EXIST, d.err );
}


/** @brief Testuje ppwrite bez write callbacku (musí selhat) */
void test_ppwrite_without_write_cb ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_DRIVER d;
    generic_driver_setup ( &d, NULL, NULL, NULL, NULL, NULL, NULL );

    st_HANDLER h;
    memset ( &h, 0, sizeof ( h ) );
    h.driver = &d;

    uint8_t buf[4] = { 0 };
    int ret = generic_driver_ppwrite ( &h, 0, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );
    TEST_ASSERT_EQUAL_INT ( GENERIC_DRIVER_ERROR_CB_NOT_EXIST, d.err );
}


/* ========================================================================
 * main
 * ======================================================================== */

int main ( int argc, char *argv[] ) {

    mztest_parse_args ( argc, argv );
    mztest_init ();

    UNITY_BEGIN ();

    /* Registrace a setup */
    RUN_TEST ( test_register_handler_memory );
    RUN_TEST ( test_register_handler_file );
    RUN_TEST ( test_setup_driver );

    /* Paměťový handler: životní cyklus */
    RUN_TEST ( test_memory_open_close );
    RUN_TEST ( test_memory_open_null_handler );
    RUN_TEST ( test_memory_write_read );
    RUN_TEST ( test_memory_write_at_boundaries );
    RUN_TEST ( test_memory_read_beyond_size );
    RUN_TEST ( test_memory_write_beyond_size );

    /* Paměťový handler s realokací */
    RUN_TEST ( test_memory_realloc_driver );

    /* Readonly ochrana */
    RUN_TEST ( test_readonly_status );

    /* Prepare a direct_read */
    RUN_TEST ( test_prepare_memory_direct_access );
    RUN_TEST ( test_prepare_sets_work_buffer_when_no_prepare_cb );

    /* Truncate */
    RUN_TEST ( test_memory_truncate );
    RUN_TEST ( test_memory_truncate_readonly_fails );

    /* Updated flag */
    RUN_TEST ( test_memory_updated_flag );

    /* Error messages */
    RUN_TEST ( test_error_message_no_error );
    RUN_TEST ( test_error_message_driver_error );
    RUN_TEST ( test_error_message_handler_error );
    RUN_TEST ( test_error_message_null_driver );
    RUN_TEST ( test_error_message_unknown );

    /* Chybové stavy */
    RUN_TEST ( test_open_memory_null_driver );
    RUN_TEST ( test_close_null_driver );
    RUN_TEST ( test_bootstrap_wrong_type );
    RUN_TEST ( test_bootstrap_not_ready );

    /* Souborový handler */
    RUN_TEST ( test_file_open_read_close );
    RUN_TEST ( test_file_write_read );
    RUN_TEST ( test_file_readonly_write_fails );

    /* open_memory_from_file */
    RUN_TEST ( test_open_memory_from_file );
    RUN_TEST ( test_open_memory_from_nonexistent_file );

    /* save_memory */
    RUN_TEST ( test_save_memory );

    /* Dvoustupňové I/O */
    RUN_TEST ( test_ppread_ppwrite );

    /* Chybějící callbacky */
    RUN_TEST ( test_ppread_without_read_cb );
    RUN_TEST ( test_ppwrite_without_write_cb );

    int result = UNITY_END ();

    mztest_teardown ();
    return result;
}
