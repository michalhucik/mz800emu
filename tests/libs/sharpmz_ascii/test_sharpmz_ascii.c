/**
 * @file   test_sharpmz_ascii.c
 * @brief  Unit test knihovny sharpmz_ascii: verze, tabulka KOI8-CS a Sharp MZ ASCII (EU).
 *
 * - Verze knihovny musí být 2.2.1.
 * - sharpmz_to_utf8 ( c, SHARPMZ_CHARSET_KOI8CS ) pro všech 256 bajtů se
 *   porovnává se souborem expected_koi8cs.txt (řádek "<idx> <utf8 hex>").
 *   Soubor je regresní snímek knihovny sharpmz_ascii 2.2.0 (vygenerován volbou
 *   --dump novou knihovnou), test tedy hlídá jen stabilitu. Shodu se starou
 *   knihovnou sharpmz_koi8cs ověřil jednorázově diff všech 256 bajtů při
 *   migraci; z repozitáře ji nelze zopakovat. Tabulka koi8cs_table.h se
 *   přesunula beze změny řádku, data glyfů jsou tedy shodná a rozdíl by mohl
 *   být jen v obalující logice.
 * - Vybrané známé hodnoty Sharp MZ ASCII (SHARPMZ_CHARSET_EU).
 *
 * Volba `--dump <soubor>` očekávaný soubor znovu vygeneruje z aktuální
 * knihovny (použít jen při vědomé změně tabulky).
 */

#include "mztest.h"

#include <stdio.h>
#include <string.h>

#include "libs/sharpmz_ascii/sharpmz_ascii.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "."
#endif

/** Cesta k očekávaným datům. */
#define EXPECTED_FILE TEST_DATA_DIR "/expected_koi8cs.txt"

/** Zapíše řetězec jako hex bajty (bez oddělovačů). */
static void put_hex ( FILE *f, const char *s ) {
    for ( ; *s; s++ ) fprintf ( f, "%02X", ( unsigned ) ( unsigned char ) *s );
}

/**
 * @brief Zapíše očekávanou tabulku pro bajty 0..255 do souboru.
 * @param path Cílový soubor.
 * @return 0 při úspěchu, 1 při chybě otevření.
 */
static int dump_table ( const char *path ) {
    FILE *f = fopen ( path, "wb" );
    if ( !f ) return 1;
    for ( int c = 0; c < 256; c++ ) {
        fprintf ( f, "%d ", c );
        put_hex ( f, sharpmz_to_utf8 ( ( uint8_t ) c, SHARPMZ_CHARSET_KOI8CS ) );
        fputc ( 10, f );
    }
    fclose ( f );
    return 0;
}

/** Zapíše řetězec do bufferu jako hex (buf musí mít >= 2*strlen+1 bajtů). */
static void to_hex ( char *buf, const char *s ) {
    for ( ; *s; s++, buf += 2 ) sprintf ( buf, "%02X", ( unsigned ) ( unsigned char ) *s );
    *buf = '\0';
}

void setUp ( void ) {}
void tearDown ( void ) {}

static void test_version ( void ) {
    TEST_ASSERT_NOT_NULL ( sharpmz_ascii_version () );
    TEST_ASSERT_EQUAL_INT ( 0, strcmp ( sharpmz_ascii_version (), "2.2.1" ) );
}

static void test_koi8cs_table ( void ) {
    FILE *f = fopen ( EXPECTED_FILE, "rb" );
    TEST_ASSERT_NOT_NULL_MESSAGE ( f, "chybí " EXPECTED_FILE );

    char line[256];
    for ( int c = 0; c < 256; c++ ) {
        char msg[64];
        char got_k[32];
        char exp[80];
        snprintf ( msg, sizeof ( msg ), "bajt %d", c );
        TEST_ASSERT_NOT_NULL_MESSAGE ( fgets ( line, sizeof ( line ), f ), msg );
        to_hex ( got_k, sharpmz_to_utf8 ( ( uint8_t ) c, SHARPMZ_CHARSET_KOI8CS ) );
        snprintf ( exp, sizeof ( exp ), "%d %s", c, got_k );
        strcat ( exp, "\n" );
        TEST_ASSERT_EQUAL_STRING_MESSAGE ( line, exp, msg );
    }
    TEST_ASSERT_NULL ( fgets ( line, sizeof ( line ), f ) );
    fclose ( f );
}

/** Známé hodnoty Sharp MZ ASCII (EU): 0x41 = A atd. */
static void test_eu_known_values ( void ) {
    TEST_ASSERT_EQUAL_STRING ( "A", sharpmz_to_utf8 ( 0x41, SHARPMZ_CHARSET_EU ) );
    TEST_ASSERT_EQUAL_STRING ( "Z", sharpmz_to_utf8 ( 0x5A, SHARPMZ_CHARSET_EU ) );
    TEST_ASSERT_EQUAL_STRING ( "0", sharpmz_to_utf8 ( 0x30, SHARPMZ_CHARSET_EU ) );
    TEST_ASSERT_EQUAL_STRING ( "9", sharpmz_to_utf8 ( 0x39, SHARPMZ_CHARSET_EU ) );
    TEST_ASSERT_EQUAL_STRING ( " ", sharpmz_to_utf8 ( 0x20, SHARPMZ_CHARSET_EU ) );
}

int main ( int argc, char *argv[] ) {
    for ( int i = 1; i + 1 < argc; i++ ) {
        if ( strcmp ( argv[i], "--dump" ) == 0 ) return dump_table ( argv[i + 1] );
    }
    UNITY_BEGIN ();
    RUN_TEST ( test_version );
    RUN_TEST ( test_koi8cs_table );
    RUN_TEST ( test_eu_known_values );
    return UNITY_END ();
}
