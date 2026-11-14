/*
 * test_watch.c - testy Watch storage + parse + persist + typy (Phase A + B).
 *
 * Pokryva:
 *   - lifecycle init/shutdown/clear_storage + idempotence
 *   - CRUD add/remove/rename/move + count/get
 *   - watch_parse_address: hex 0xC080, IASM C080h, dec 49280, symbol lookup
 *   - type/fmt string roundtrip (vsechny typy + formaty)
 *   - type metadata (has_length, has_format, default_fmt, max_length, size)
 *   - format helpery: int (unsigned/signed/bit, hex/dec/bin/char),
 *     ascii (escape, mz translation), bytes (truncation + tooltip)
 *   - watch_read_int + u16le vs u16be ctece
 *   - persistence round-trip (Phase A + Phase B fieldy + BC s Phase A)
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include "debugger/watch.h"
#include "debugger/watch_cache.h"  /* Phase E */
#include "debugger/symbols/sym_db.h"
#include "main.h"  /* g_sdlapp */
#include "libs/sdlapp/sdlapp_paths.h"
#include "hw-generic/memory/memory.h"

#include <string.h>
#include <stdio.h>
#include <glib.h>


void setUp ( void ) {
    /* Per-test cisty stav. */
    watch_init ( );
    watch_clear_storage ( );
    /* Phase E: cache + snapshot cisty stav. */
    watch_cache_init ( );
    watch_cache_reset_all ( );
    watch_snapshot_clear ( );
    /* Sym DB ciste kvuli parse_addr_symbol testum. */
    sym_db_init ( );
    sym_db_clear ( );
}


void tearDown ( void ) {
    watch_cache_shutdown ( );
    watch_shutdown ( );
    /* sym_db nezaviram - dalsi test si znova clear v setUp. */
}


/* ========================================================================= */
/*  Lifecycle                                                                */
/* ========================================================================= */


/**
 * Init je idempotentni (= druhe volani nesmi ztratit data).
 */
void test_init_idempotent ( void ) {
    watch_add ( "first", 0xC000, -1 );
    watch_init ( );  /* uz bylo init v setUp */
    TEST_ASSERT_EQUAL_INT ( 1, (int) watch_count ( ) );
}


/**
 * Count + get + out-of-range.
 */
void test_count_get_basic ( void ) {
    TEST_ASSERT_EQUAL_INT ( 0, (int) watch_count ( ) );

    watch_add ( "a", 0x1000, -1 );
    watch_add ( "b", 0x2000, -1 );
    watch_add ( "c", 0x3000, -1 );

    TEST_ASSERT_EQUAL_INT ( 3, (int) watch_count ( ) );

    const st_WATCH_ROW *r0 = watch_get ( 0 );
    const st_WATCH_ROW *r1 = watch_get ( 1 );
    const st_WATCH_ROW *r2 = watch_get ( 2 );
    const st_WATCH_ROW *r3 = watch_get ( 3 );

    TEST_ASSERT_NOT_NULL ( r0 );
    TEST_ASSERT_NOT_NULL ( r1 );
    TEST_ASSERT_NOT_NULL ( r2 );
    TEST_ASSERT_NULL ( r3 );

    TEST_ASSERT_EQUAL_STRING ( "a", r0->name );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1000, r0->addr );
    TEST_ASSERT_EQUAL_INT ( -1, r0->bank );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_U8, r0->type );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_HEX, r0->fmt );
    TEST_ASSERT_NULL ( r0->expr_text );

    TEST_ASSERT_EQUAL_STRING ( "b", r1->name );
    TEST_ASSERT_EQUAL_HEX16 ( 0x2000, r1->addr );

    TEST_ASSERT_EQUAL_STRING ( "c", r2->name );
}


/* ========================================================================= */
/*  CRUD                                                                     */
/* ========================================================================= */


void test_add_remove ( void ) {
    int i0 = watch_add ( "a", 0x1000, -1 );
    int i1 = watch_add ( "b", 0x2000, -1 );
    int i2 = watch_add ( "c", 0x3000, -1 );

    TEST_ASSERT_EQUAL_INT ( 0, i0 );
    TEST_ASSERT_EQUAL_INT ( 1, i1 );
    TEST_ASSERT_EQUAL_INT ( 2, i2 );
    TEST_ASSERT_EQUAL_INT ( 3, (int) watch_count ( ) );

    watch_remove ( 1 );

    TEST_ASSERT_EQUAL_INT ( 2, (int) watch_count ( ) );
    TEST_ASSERT_EQUAL_STRING ( "a", watch_get ( 0 )->name );
    TEST_ASSERT_EQUAL_STRING ( "c", watch_get ( 1 )->name );
}


void test_remove_out_of_range ( void ) {
    watch_add ( "a", 0x1000, -1 );
    watch_remove ( -1 );
    watch_remove ( 99 );
    TEST_ASSERT_EQUAL_INT ( 1, (int) watch_count ( ) );
}


void test_rename ( void ) {
    watch_add ( NULL, 0xC000, -1 );
    TEST_ASSERT_NULL ( watch_get ( 0 )->name );

    watch_rename ( 0, "score" );
    TEST_ASSERT_EQUAL_STRING ( "score", watch_get ( 0 )->name );

    watch_rename ( 0, NULL );
    TEST_ASSERT_NULL ( watch_get ( 0 )->name );

    watch_rename ( 0, "again" );
    watch_rename ( 0, "" );
    TEST_ASSERT_NULL ( watch_get ( 0 )->name );
}


void test_add_name_truncate ( void ) {
    char longname[ WATCH_NAME_MAX + 32 ];
    memset ( longname, 'X', sizeof ( longname ) - 1 );
    longname[ sizeof ( longname ) - 1 ] = '\0';

    watch_add ( longname, 0x4000, -1 );

    const st_WATCH_ROW *r = watch_get ( 0 );
    TEST_ASSERT_NOT_NULL ( r );
    TEST_ASSERT_NOT_NULL ( r->name );
    TEST_ASSERT_EQUAL_INT ( WATCH_NAME_MAX, (int) strlen ( r->name ) );
}


void test_move ( void ) {
    watch_add ( "a", 0x1000, -1 );
    watch_add ( "b", 0x2000, -1 );
    watch_add ( "c", 0x3000, -1 );
    watch_add ( "d", 0x4000, -1 );

    watch_move ( 0, 2 );

    TEST_ASSERT_EQUAL_STRING ( "b", watch_get ( 0 )->name );
    TEST_ASSERT_EQUAL_STRING ( "c", watch_get ( 1 )->name );
    TEST_ASSERT_EQUAL_STRING ( "a", watch_get ( 2 )->name );
    TEST_ASSERT_EQUAL_STRING ( "d", watch_get ( 3 )->name );
}


void test_move_noop_same_index ( void ) {
    watch_add ( "a", 0x1000, -1 );
    watch_add ( "b", 0x2000, -1 );
    watch_move ( 1, 1 );
    TEST_ASSERT_EQUAL_STRING ( "a", watch_get ( 0 )->name );
    TEST_ASSERT_EQUAL_STRING ( "b", watch_get ( 1 )->name );
}


void test_add_anonymous_row ( void ) {
    int idx = watch_add ( NULL, 0xC100, -1 );
    TEST_ASSERT_EQUAL_INT ( 0, idx );
    const st_WATCH_ROW *r = watch_get ( 0 );
    TEST_ASSERT_NOT_NULL ( r );
    TEST_ASSERT_NULL ( r->name );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC100, r->addr );
}


/* ========================================================================= */
/*  Clear                                                                    */
/* ========================================================================= */


void test_clear_storage ( void ) {
    watch_add ( "a", 0x1000, -1 );
    watch_add ( "b", 0x2000, -1 );
    watch_clear_storage ( );
    TEST_ASSERT_EQUAL_INT ( 0, (int) watch_count ( ) );
}


/* ========================================================================= */
/*  Parse address                                                            */
/* ========================================================================= */


void test_parse_addr_hex_cstyle ( void ) {
    uint16_t a = 0;
    TEST_ASSERT_TRUE ( watch_parse_address ( "0xC080", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC080, a );

    TEST_ASSERT_TRUE ( watch_parse_address ( "0xc080", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC080, a );

    TEST_ASSERT_TRUE ( watch_parse_address ( "0X1234", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, a );

    TEST_ASSERT_TRUE ( watch_parse_address ( "0x0", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x0000, a );

    TEST_ASSERT_TRUE ( watch_parse_address ( "0xFFFF", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0xFFFF, a );
}


void test_parse_addr_hex_iasm_suffix ( void ) {
    uint16_t a = 0;
    TEST_ASSERT_TRUE ( watch_parse_address ( "0C080h", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC080, a );

    TEST_ASSERT_FALSE ( watch_parse_address ( "C080h", &a ) );

    TEST_ASSERT_TRUE ( watch_parse_address ( "1234H", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, a );

    TEST_ASSERT_TRUE ( watch_parse_address ( "  0FFh  ", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x00FF, a );
}


void test_parse_addr_decimal ( void ) {
    uint16_t a = 0;
    TEST_ASSERT_TRUE ( watch_parse_address ( "49280", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC080, a );

    TEST_ASSERT_TRUE ( watch_parse_address ( "0", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0, a );

    TEST_ASSERT_TRUE ( watch_parse_address ( "65535", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0xFFFF, a );

    TEST_ASSERT_TRUE ( watch_parse_address ( "   42  ", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 42, a );
}


void test_parse_addr_out_of_range ( void ) {
    uint16_t a = 0;
    TEST_ASSERT_FALSE ( watch_parse_address ( "0x10000", &a ) );
    TEST_ASSERT_FALSE ( watch_parse_address ( "65536", &a ) );
    TEST_ASSERT_FALSE ( watch_parse_address ( "0xFFFFFF", &a ) );
}


void test_parse_addr_invalid ( void ) {
    uint16_t a = 0;
    TEST_ASSERT_FALSE ( watch_parse_address ( "", &a ) );
    TEST_ASSERT_FALSE ( watch_parse_address ( NULL, &a ) );
    TEST_ASSERT_FALSE ( watch_parse_address ( "abc", &a ) );
    TEST_ASSERT_FALSE ( watch_parse_address ( "0xZZ", &a ) );
    TEST_ASSERT_FALSE ( watch_parse_address ( "12 34", &a ) );
    TEST_ASSERT_FALSE ( watch_parse_address ( "-1", &a ) );
}


void test_parse_addr_symbol ( void ) {
    int rc = sym_db_add_user_label ( 0xC080, "score", NULL );
    TEST_ASSERT_EQUAL_INT ( 0, rc );

    uint16_t a = 0;
    TEST_ASSERT_TRUE ( watch_parse_address ( "score", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC080, a );

    TEST_ASSERT_TRUE ( watch_parse_address ( "  score  ", &a ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC080, a );

    TEST_ASSERT_FALSE ( watch_parse_address ( "nonexistent_xyz", &a ) );
}


/* ========================================================================= */
/*  Type/fmt string mapping                                                  */
/* ========================================================================= */


/**
 * Type roundtrip - kazdy enum -> string -> enum identita.
 */
void test_watch_type_string_roundtrip ( void ) {
    en_WATCH_TYPE all[] = {
        WATCH_TYPE_U8, WATCH_TYPE_I8,
        WATCH_TYPE_U16LE, WATCH_TYPE_U16BE,
        WATCH_TYPE_I16LE, WATCH_TYPE_I16BE,
        WATCH_TYPE_U32LE, WATCH_TYPE_U32BE,
        WATCH_TYPE_I32LE, WATCH_TYPE_I32BE,
        WATCH_TYPE_BIT,
        WATCH_TYPE_ASCII, WATCH_TYPE_MZASCII, WATCH_TYPE_BYTES,
    };
    for ( size_t i = 0; i < sizeof ( all ) / sizeof ( all[0] ); i++ ) {
        const char *s = watch_type_to_str ( all[ i ] );
        TEST_ASSERT_NOT_NULL ( s );

        en_WATCH_TYPE rt = WATCH_TYPE_U8;
        TEST_ASSERT_TRUE ( watch_type_from_str ( s, &rt ) );
        TEST_ASSERT_EQUAL_INT ( all[ i ], rt );
    };

    /* Neznamy. */
    en_WATCH_TYPE rt = WATCH_TYPE_U8;
    TEST_ASSERT_FALSE ( watch_type_from_str ( "xxx", &rt ) );
    TEST_ASSERT_FALSE ( watch_type_from_str ( "", &rt ) );
    TEST_ASSERT_FALSE ( watch_type_from_str ( NULL, &rt ) );
}


void test_watch_fmt_string_roundtrip ( void ) {
    en_WATCH_FMT all[] = {
        WATCH_FMT_DEC, WATCH_FMT_HEX, WATCH_FMT_BIN, WATCH_FMT_CHAR,
    };
    for ( size_t i = 0; i < sizeof ( all ) / sizeof ( all[0] ); i++ ) {
        const char *s = watch_fmt_to_str ( all[ i ] );
        TEST_ASSERT_NOT_NULL ( s );
        en_WATCH_FMT rt = WATCH_FMT_DEC;
        TEST_ASSERT_TRUE ( watch_fmt_from_str ( s, &rt ) );
        TEST_ASSERT_EQUAL_INT ( all[ i ], rt );
    };
    en_WATCH_FMT rt = WATCH_FMT_DEC;
    TEST_ASSERT_FALSE ( watch_fmt_from_str ( "blob", &rt ) );
}


/* ========================================================================= */
/*  Type metadata                                                            */
/* ========================================================================= */


void test_type_metadata ( void ) {
    /* has_format */
    TEST_ASSERT_TRUE  ( watch_type_has_format ( WATCH_TYPE_U8 ) );
    TEST_ASSERT_TRUE  ( watch_type_has_format ( WATCH_TYPE_I8 ) );
    TEST_ASSERT_TRUE  ( watch_type_has_format ( WATCH_TYPE_U16LE ) );
    TEST_ASSERT_TRUE  ( watch_type_has_format ( WATCH_TYPE_U32LE ) );
    TEST_ASSERT_FALSE ( watch_type_has_format ( WATCH_TYPE_BIT ) );
    TEST_ASSERT_FALSE ( watch_type_has_format ( WATCH_TYPE_ASCII ) );
    TEST_ASSERT_FALSE ( watch_type_has_format ( WATCH_TYPE_MZASCII ) );
    TEST_ASSERT_FALSE ( watch_type_has_format ( WATCH_TYPE_BYTES ) );

    /* has_length */
    TEST_ASSERT_FALSE ( watch_type_has_length ( WATCH_TYPE_U8 ) );
    TEST_ASSERT_FALSE ( watch_type_has_length ( WATCH_TYPE_BIT ) );
    TEST_ASSERT_TRUE  ( watch_type_has_length ( WATCH_TYPE_ASCII ) );
    TEST_ASSERT_TRUE  ( watch_type_has_length ( WATCH_TYPE_MZASCII ) );
    TEST_ASSERT_TRUE  ( watch_type_has_length ( WATCH_TYPE_BYTES ) );

    /* default_fmt */
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_HEX, watch_type_default_fmt ( WATCH_TYPE_U8 ) );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_DEC, watch_type_default_fmt ( WATCH_TYPE_I8 ) );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_DEC, watch_type_default_fmt ( WATCH_TYPE_I16LE ) );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_HEX, watch_type_default_fmt ( WATCH_TYPE_U32LE ) );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_HEX, watch_type_default_fmt ( WATCH_TYPE_U32BE ) );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_DEC, watch_type_default_fmt ( WATCH_TYPE_I32LE ) );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_DEC, watch_type_default_fmt ( WATCH_TYPE_I32BE ) );

    /* has_format - 32b */
    TEST_ASSERT_TRUE  ( watch_type_has_format ( WATCH_TYPE_U32BE ) );
    TEST_ASSERT_TRUE  ( watch_type_has_format ( WATCH_TYPE_I32LE ) );
    TEST_ASSERT_TRUE  ( watch_type_has_format ( WATCH_TYPE_I32BE ) );

    /* size */
    TEST_ASSERT_EQUAL_INT ( 1, (int) watch_type_size ( WATCH_TYPE_U8, 0 ) );
    TEST_ASSERT_EQUAL_INT ( 1, (int) watch_type_size ( WATCH_TYPE_BIT, 0 ) );
    TEST_ASSERT_EQUAL_INT ( 2, (int) watch_type_size ( WATCH_TYPE_U16LE, 0 ) );
    TEST_ASSERT_EQUAL_INT ( 4, (int) watch_type_size ( WATCH_TYPE_U32LE, 0 ) );
    TEST_ASSERT_EQUAL_INT ( 4, (int) watch_type_size ( WATCH_TYPE_U32BE, 0 ) );
    TEST_ASSERT_EQUAL_INT ( 4, (int) watch_type_size ( WATCH_TYPE_I32LE, 0 ) );
    TEST_ASSERT_EQUAL_INT ( 4, (int) watch_type_size ( WATCH_TYPE_I32BE, 0 ) );
    TEST_ASSERT_EQUAL_INT ( 8, (int) watch_type_size ( WATCH_TYPE_ASCII, 8 ) );
    TEST_ASSERT_EQUAL_INT ( 64, (int) watch_type_size ( WATCH_TYPE_BYTES, 64 ) );

    /* max_length */
    TEST_ASSERT_EQUAL_INT ( WATCH_LENGTH_MAX_STRING,
                            watch_type_max_length ( WATCH_TYPE_ASCII ) );
    TEST_ASSERT_EQUAL_INT ( WATCH_LENGTH_MAX_STRING,
                            watch_type_max_length ( WATCH_TYPE_MZASCII ) );
    TEST_ASSERT_EQUAL_INT ( WATCH_LENGTH_MAX_BYTES,
                            watch_type_max_length ( WATCH_TYPE_BYTES ) );
    TEST_ASSERT_EQUAL_INT ( 0, watch_type_max_length ( WATCH_TYPE_U8 ) );
}


/**
 * watch_set_type aplikuje defaulty (fmt, length).
 */
void test_set_type_defaults ( void ) {
    int idx = watch_add ( "v", 0xC000, -1 );
    TEST_ASSERT_EQUAL_INT ( 0, idx );

    /* signed -> default DEC */
    watch_set_type ( idx, WATCH_TYPE_I8 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_I8, watch_get ( 0 )->type );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_DEC, watch_get ( 0 )->fmt );
    TEST_ASSERT_EQUAL_INT ( 0, watch_get ( 0 )->length );

    /* ascii -> default length = WATCH_LENGTH_MIN (1) */
    watch_set_type ( idx, WATCH_TYPE_ASCII );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_ASCII, watch_get ( 0 )->type );
    TEST_ASSERT_EQUAL_INT ( WATCH_LENGTH_MIN, watch_get ( 0 )->length );

    /* set_length s prilis velkou hodnotou se clampuje. */
    watch_set_length ( idx, 9999 );
    TEST_ASSERT_EQUAL_INT ( WATCH_LENGTH_MAX_STRING, watch_get ( 0 )->length );

    /* prepnuti na bit nuluje length. */
    watch_set_type ( idx, WATCH_TYPE_BIT );
    TEST_ASSERT_EQUAL_INT ( 0, watch_get ( 0 )->length );

    watch_set_bit_index ( idx, 5 );
    TEST_ASSERT_EQUAL_INT ( 5, watch_get ( 0 )->bit_index );
    watch_set_bit_index ( idx, 99 );
    TEST_ASSERT_EQUAL_INT ( 7, watch_get ( 0 )->bit_index );  /* clamp */
}


/* ========================================================================= */
/*  Format helpery - int                                                     */
/* ========================================================================= */


/**
 * Pomocna funkce - vytvori docasny radek a vola watch_format_int.
 *
 * watch_format_int neziva read - jen formatuje predanou value podle typu
 * a fmt v radku. Mohli bychom volat na stack-allocated st_WATCH_ROW, ale
 * cistsi je pristupit pres storage.
 */
static void fmt_int_helper ( en_WATCH_TYPE t, en_WATCH_FMT f, uint64_t v,
                              char *out, size_t sz ) {
    watch_clear_storage ( );
    int idx = watch_add ( NULL, 0, -1 );
    watch_set_type ( idx, t );
    watch_set_fmt ( idx, f );
    const st_WATCH_ROW *r = watch_get ( (size_t) idx );
    watch_format_int ( r, v, out, sz );
}


void test_watch_format_int_unsigned ( void ) {
    char buf[64];

    /* u8 hex */
    fmt_int_helper ( WATCH_TYPE_U8, WATCH_FMT_HEX, 0x1A, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "0x1A", buf );

    /* u8 dec */
    fmt_int_helper ( WATCH_TYPE_U8, WATCH_FMT_DEC, 0xFF, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "255", buf );

    /* u8 bin */
    fmt_int_helper ( WATCH_TYPE_U8, WATCH_FMT_BIN, 0xAA, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "0b10101010", buf );

    /* u8 char printable */
    fmt_int_helper ( WATCH_TYPE_U8, WATCH_FMT_CHAR, 'A', buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "'A'", buf );

    /* u8 char non-printable = hex fallback */
    fmt_int_helper ( WATCH_TYPE_U8, WATCH_FMT_CHAR, 0x01, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "0x01", buf );

    /* u16le hex */
    fmt_int_helper ( WATCH_TYPE_U16LE, WATCH_FMT_HEX, 0x1234, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "0x1234", buf );

    /* u32le hex */
    fmt_int_helper ( WATCH_TYPE_U32LE, WATCH_FMT_HEX, 0xDEADBEEFu, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "0xDEADBEEF", buf );

    /* u32le dec max */
    fmt_int_helper ( WATCH_TYPE_U32LE, WATCH_FMT_DEC, 0xFFFFFFFFu, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "4294967295", buf );
}


void test_watch_format_int_signed ( void ) {
    char buf[64];

    /* i8: 0xFF interpretovano signed = -1 */
    fmt_int_helper ( WATCH_TYPE_I8, WATCH_FMT_DEC, 0xFF, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "-1", buf );

    /* i8: 0x80 = -128 */
    fmt_int_helper ( WATCH_TYPE_I8, WATCH_FMT_DEC, 0x80, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "-128", buf );

    /* i8: 0x7F = 127 */
    fmt_int_helper ( WATCH_TYPE_I8, WATCH_FMT_DEC, 0x7F, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "127", buf );

    /* i8 hex (= zobrazi v hex bez znamenka, mask na 8b) */
    fmt_int_helper ( WATCH_TYPE_I8, WATCH_FMT_HEX, 0xFF, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "0xFF", buf );

    /* i16le edge */
    fmt_int_helper ( WATCH_TYPE_I16LE, WATCH_FMT_DEC, 0x8000, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "-32768", buf );

    fmt_int_helper ( WATCH_TYPE_I16LE, WATCH_FMT_DEC, 0x7FFF, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "32767", buf );

    fmt_int_helper ( WATCH_TYPE_I16LE, WATCH_FMT_DEC, 0xFFFF, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "-1", buf );
}


/**
 * watch_format_int pro WATCH_TYPE_BIT vraci "0" nebo "1" bez ohledu na fmt.
 */
void test_watch_format_bit ( void ) {
    char buf[16];

    watch_clear_storage ( );
    int idx = watch_add ( NULL, 0, -1 );
    watch_set_type ( idx, WATCH_TYPE_BIT );

    const st_WATCH_ROW *r = watch_get ( 0 );

    watch_format_int ( r, 0, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "0", buf );

    watch_format_int ( r, 1, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "1", buf );

    /* Hodnota >1 -> bere se jen bit 0. */
    watch_format_int ( r, 2, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "0", buf );
}


/* ========================================================================= */
/*  Format helpery - ascii / bytes                                           */
/* ========================================================================= */


void test_watch_format_ascii_basic ( void ) {
    char buf[128];

    /* Plain ASCII "Hello" - vsechny printable. */
    const uint8_t s1[] = { 'H', 'e', 'l', 'l', 'o' };
    watch_format_ascii ( s1, sizeof ( s1 ), buf, sizeof ( buf ), false );
    TEST_ASSERT_EQUAL_STRING ( "Hello", buf );

    /* Non-printable v plain ASCII -> escape \xNN */
    const uint8_t s2[] = { 'A', 0x01, 'B' };
    watch_format_ascii ( s2, sizeof ( s2 ), buf, sizeof ( buf ), false );
    TEST_ASSERT_EQUAL_STRING ( "A\\x01B", buf );

    /* Backslash escape. */
    const uint8_t s3[] = { '\\', 'X' };
    watch_format_ascii ( s3, sizeof ( s3 ), buf, sizeof ( buf ), false );
    TEST_ASSERT_EQUAL_STRING ( "\\\\X", buf );

    /* Prazdny vstup. */
    watch_format_ascii ( NULL, 0, buf, sizeof ( buf ), false );
    TEST_ASSERT_EQUAL_STRING ( "", buf );
}


/**
 * mzascii (sharp_translation=true) převádí bajty jako Sharp MZ ASCII (EU)
 * přes sharpmz_to_utf8 ( byte, SHARPMZ_CHARSET_EU ).
 */
void test_watch_format_ascii_mz ( void ) {
    char buf[128];

    /* Bajty 0x48 0x49 = Sharp MZ ASCII 'H','I' (starý 1.0.0 převod je chybně
     * zobrazoval jako 'XY' - tabulka byla posunutá). */
    const uint8_t s[] = { 0x48, 0x49 };
    watch_format_ascii ( s, sizeof ( s ), buf, sizeof ( buf ), true );
    TEST_ASSERT_EQUAL_STRING ( "HI", buf );

    /* Cifry: 0x38, 0x39 = '8', '9'. */
    const uint8_t mz_89[] = { 0x38, 0x39 };
    watch_format_ascii ( mz_89, sizeof ( mz_89 ), buf, sizeof ( buf ), true );
    TEST_ASSERT_EQUAL_STRING ( "89", buf );
}


void test_watch_format_bytes_short ( void ) {
    char buf[128];
    const uint8_t s[] = { 0x12, 0x34, 0x56, 0x78 };
    bool truncated = watch_format_bytes ( s, sizeof ( s ), buf,
                                            sizeof ( buf ), 16 );
    TEST_ASSERT_FALSE ( truncated );
    TEST_ASSERT_EQUAL_STRING ( "12 34 56 78", buf );
}


void test_watch_format_bytes_long ( void ) {
    char buf[256];
    uint8_t s[ 32 ];
    for ( size_t i = 0; i < 32; i++ ) s[ i ] = (uint8_t) ( i + 0x10 );

    bool truncated = watch_format_bytes ( s, sizeof ( s ), buf,
                                            sizeof ( buf ), 16 );
    TEST_ASSERT_TRUE ( truncated );
    /* Prvni 16 bajtu = "10 11 ... 1F", pak " ..." */
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "10 11 12" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "1F" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "..." ) );
    /* Bajt 0x20 by uz nemel byt videt. */
    TEST_ASSERT_NULL ( strstr ( buf, " 20" ) );
}


/**
 * max_inline=0 = bez limitu (omezeny jen velikosti bufferu).
 */
void test_watch_format_bytes_no_inline_limit ( void ) {
    char buf[256];
    const uint8_t s[] = { 0xAA, 0xBB, 0xCC };
    bool truncated = watch_format_bytes ( s, sizeof ( s ), buf,
                                            sizeof ( buf ), 0 );
    TEST_ASSERT_FALSE ( truncated );
    TEST_ASSERT_EQUAL_STRING ( "AA BB CC", buf );
}


/* ========================================================================= */
/*  watch_read_int - u16 byteorder                                           */
/* ========================================================================= */


/**
 * Napise do RAM 2 bajty a overi LE vs BE interpretaci.
 *
 * Pouzivame Z80 RAM oblast bezpecne mimo system (= horni RAM oblast).
 * Test framework inicializuje memory v mztest_init().
 */
void test_watch_u16le_vs_be ( void ) {
    uint16_t addr = 0xD000;
    memory_write_byte ( addr,     0x34 );
    memory_write_byte ( addr + 1, 0x12 );

    /* LE rozhodnuti: lo=0x34, hi=0x12 -> 0x1234 */
    int idx_le = watch_add ( NULL, addr, -1 );
    watch_set_type ( idx_le, WATCH_TYPE_U16LE );
    uint64_t vle = watch_read_int ( watch_get ( (size_t) idx_le ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, (uint16_t) vle );

    /* BE rozhodnuti: hi=0x34, lo=0x12 -> 0x3412 */
    int idx_be = watch_add ( NULL, addr, -1 );
    watch_set_type ( idx_be, WATCH_TYPE_U16BE );
    uint64_t vbe = watch_read_int ( watch_get ( (size_t) idx_be ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x3412, (uint16_t) vbe );
}


/**
 * Bit read: zapise 0xAA, cte bity 0..7.
 */
void test_watch_read_bit ( void ) {
    uint16_t addr = 0xD010;
    memory_write_byte ( addr, 0xAA );  /* 10101010 */

    /* 0xAA = b7..b0 = 1,0,1,0,1,0,1,0 */
    int expected[8] = { 0, 1, 0, 1, 0, 1, 0, 1 };

    for ( int b = 0; b < 8; b++ ) {
        watch_clear_storage ( );
        int idx = watch_add ( NULL, addr, -1 );
        watch_set_type ( idx, WATCH_TYPE_BIT );
        watch_set_bit_index ( idx, (uint8_t) b );
        uint64_t v = watch_read_int ( watch_get ( (size_t) idx ) );
        TEST_ASSERT_EQUAL_INT_MESSAGE ( expected[ b ], (int) v,
                                         "bit_index mismatch" );
    };
}


/**
 * 32-bit byteorder: LE vs BE čtení stejných 4 bajtů.
 *
 * Bajty 0x12, 0x34, 0x56, 0x78 v RAM:
 *   - LE čte jako 0x78563412 (lo byte první)
 *   - BE čte jako 0x12345678 (hi byte první)
 */
void test_watch_u32_byteorder ( void ) {
    uint16_t addr = 0xD020;
    memory_write_byte ( addr,     0x12 );
    memory_write_byte ( addr + 1, 0x34 );
    memory_write_byte ( addr + 2, 0x56 );
    memory_write_byte ( addr + 3, 0x78 );

    /* LE */
    int idx_le = watch_add ( NULL, addr, -1 );
    watch_set_type ( idx_le, WATCH_TYPE_U32LE );
    uint64_t vle = watch_read_int ( watch_get ( (size_t) idx_le ) );
    TEST_ASSERT_EQUAL_HEX32 ( 0x78563412u, (uint32_t) vle );

    /* BE */
    int idx_be = watch_add ( NULL, addr, -1 );
    watch_set_type ( idx_be, WATCH_TYPE_U32BE );
    uint64_t vbe = watch_read_int ( watch_get ( (size_t) idx_be ) );
    TEST_ASSERT_EQUAL_HEX32 ( 0x12345678u, (uint32_t) vbe );
}


/**
 * Signed 32-bit interpretation - 0xFFFFFFFF jako i32le = -1, format DEC.
 *
 * Pokrývá: i32le sign-extend ve watch_format_int + i32be alternativní
 * byteorder.
 */
void test_watch_i32_signed ( void ) {
    uint16_t addr = 0xD030;
    memory_write_byte ( addr,     0xFF );
    memory_write_byte ( addr + 1, 0xFF );
    memory_write_byte ( addr + 2, 0xFF );
    memory_write_byte ( addr + 3, 0xFF );

    int idx = watch_add ( NULL, addr, -1 );
    watch_set_type ( idx, WATCH_TYPE_I32LE );
    /* Default fmt = DEC (signed). */
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_DEC, watch_get ( (size_t) idx )->fmt );

    uint64_t v = watch_read_int ( watch_get ( (size_t) idx ) );
    char buf[32];
    watch_format_int ( watch_get ( (size_t) idx ), v, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "-1", buf );

    /* I32BE: nezáleží na byteorderu pro 0xFFFFFFFF, ale ověříme cestu. */
    int idx_be = watch_add ( NULL, addr, -1 );
    watch_set_type ( idx_be, WATCH_TYPE_I32BE );
    uint64_t vbe = watch_read_int ( watch_get ( (size_t) idx_be ) );
    watch_format_int ( watch_get ( (size_t) idx_be ), vbe, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "-1", buf );

    /* I32LE positive max: 0x7FFFFFFF = 2147483647. */
    memory_write_byte ( addr,     0xFF );
    memory_write_byte ( addr + 1, 0xFF );
    memory_write_byte ( addr + 2, 0xFF );
    memory_write_byte ( addr + 3, 0x7F );
    uint64_t v2 = watch_read_int ( watch_get ( (size_t) idx ) );
    watch_format_int ( watch_get ( (size_t) idx ), v2, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "2147483647", buf );
}


/* ========================================================================= */
/*  Persistence round-trip                                                   */
/* ========================================================================= */


/**
 * Save -> clear -> load -> identicky stav (Phase A scope: jen u8).
 */
void test_persist_roundtrip ( void ) {
    const char *tmpfile = "test_watch_roundtrip.watch";

    watch_add ( "score", 0xC080, -1 );
    watch_add ( NULL,    0xC100, -1 );

    TEST_ASSERT_TRUE ( watch_save_to_filepath ( tmpfile ) );

    watch_clear_storage ( );
    TEST_ASSERT_EQUAL_INT ( 0, (int) watch_count ( ) );

    TEST_ASSERT_TRUE ( watch_load_from_filepath ( tmpfile ) );

    TEST_ASSERT_EQUAL_INT ( 2, (int) watch_count ( ) );

    const st_WATCH_ROW *r0 = watch_get ( 0 );
    const st_WATCH_ROW *r1 = watch_get ( 1 );

    TEST_ASSERT_NOT_NULL ( r0 );
    TEST_ASSERT_EQUAL_STRING ( "score", r0->name );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC080, r0->addr );
    TEST_ASSERT_EQUAL_INT ( -1, r0->bank );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_U8, r0->type );

    TEST_ASSERT_NOT_NULL ( r1 );
    TEST_ASSERT_NULL ( r1->name );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC100, r1->addr );

    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, tmpfile );
    if ( resolved ) {
        remove ( resolved );
        g_free ( resolved );
    };
}


/**
 * Phase B round-trip - vsechny nove fieldy.
 */
void test_persist_roundtrip_types ( void ) {
    const char *tmpfile = "test_watch_roundtrip_types.watch";

    /* u16le s nondefault fmt (bin) */
    int i0 = watch_add ( "word", 0xC000, -1 );
    watch_set_type ( i0, WATCH_TYPE_U16LE );
    watch_set_fmt  ( i0, WATCH_FMT_BIN );

    /* i8 - default fmt = DEC */
    int i1 = watch_add ( "snum", 0xC010, -1 );
    watch_set_type ( i1, WATCH_TYPE_I8 );

    /* bit s indexem 5 */
    int i2 = watch_add ( "flag", 0xC020, -1 );
    watch_set_type ( i2, WATCH_TYPE_BIT );
    watch_set_bit_index ( i2, 5 );

    /* ascii N=12 */
    int i3 = watch_add ( "text", 0xC030, -1 );
    watch_set_type ( i3, WATCH_TYPE_ASCII );
    watch_set_length ( i3, 12 );

    /* mzascii N=8 */
    int i4 = watch_add ( "mztxt", 0xC040, -1 );
    watch_set_type ( i4, WATCH_TYPE_MZASCII );
    watch_set_length ( i4, 8 );

    /* bytes N=32 */
    int i5 = watch_add ( "blob", 0xC050, -1 );
    watch_set_type ( i5, WATCH_TYPE_BYTES );
    watch_set_length ( i5, 32 );

    /* u32be - default fmt HEX */
    int i6 = watch_add ( "dwbe", 0xC060, -1 );
    watch_set_type ( i6, WATCH_TYPE_U32BE );

    /* i32le - default fmt DEC */
    int i7 = watch_add ( "snum32", 0xC070, -1 );
    watch_set_type ( i7, WATCH_TYPE_I32LE );

    /* i32be - default fmt DEC */
    int i8 = watch_add ( "snum32be", 0xC080, -1 );
    watch_set_type ( i8, WATCH_TYPE_I32BE );

    TEST_ASSERT_TRUE ( watch_save_to_filepath ( tmpfile ) );

    watch_clear_storage ( );
    TEST_ASSERT_TRUE ( watch_load_from_filepath ( tmpfile ) );

    TEST_ASSERT_EQUAL_INT ( 9, (int) watch_count ( ) );

    const st_WATCH_ROW *r0 = watch_get ( 0 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_U16LE, r0->type );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_BIN,    r0->fmt );

    const st_WATCH_ROW *r1 = watch_get ( 1 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_I8, r1->type );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_DEC, r1->fmt );

    const st_WATCH_ROW *r2 = watch_get ( 2 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_BIT, r2->type );
    TEST_ASSERT_EQUAL_INT ( 5, r2->bit_index );

    const st_WATCH_ROW *r3 = watch_get ( 3 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_ASCII, r3->type );
    TEST_ASSERT_EQUAL_INT ( 12, r3->length );

    const st_WATCH_ROW *r4 = watch_get ( 4 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_MZASCII, r4->type );
    TEST_ASSERT_EQUAL_INT ( 8, r4->length );

    const st_WATCH_ROW *r5 = watch_get ( 5 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_BYTES, r5->type );
    TEST_ASSERT_EQUAL_INT ( 32, r5->length );

    const st_WATCH_ROW *r6 = watch_get ( 6 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_U32BE, r6->type );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_HEX,    r6->fmt );

    const st_WATCH_ROW *r7 = watch_get ( 7 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_I32LE, r7->type );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_DEC,    r7->fmt );

    const st_WATCH_ROW *r8 = watch_get ( 8 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_I32BE, r8->type );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_DEC,    r8->fmt );

    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, tmpfile );
    if ( resolved ) {
        remove ( resolved );
        g_free ( resolved );
    };
}


/**
 * BC s Phase A formatem - chybejici fmt/length/bit_index = defaulty.
 */
void test_persist_load_phase_a_format ( void ) {
    const char *tmpfile = "test_watch_phase_a_format.watch";

    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, tmpfile );
    TEST_ASSERT_NOT_NULL ( resolved );

    /* Phase A schema: jen name/addr/bank/type, nic vic. */
    FILE *f = fopen ( resolved, "w" );
    TEST_ASSERT_NOT_NULL ( f );
    fprintf ( f,
        "{\n"
        "  \"watch\": [\n"
        "    {\"name\": \"old\", \"addr\": 49280, \"bank\": -1, \"type\": \"u8\"}\n"
        "  ]\n"
        "}\n" );
    fclose ( f );

    TEST_ASSERT_TRUE ( watch_load_from_filepath ( tmpfile ) );
    TEST_ASSERT_EQUAL_INT ( 1, (int) watch_count ( ) );

    const st_WATCH_ROW *r = watch_get ( 0 );
    TEST_ASSERT_EQUAL_STRING ( "old", r->name );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_U8, r->type );
    TEST_ASSERT_EQUAL_INT ( WATCH_FMT_HEX, r->fmt );  /* default */
    TEST_ASSERT_EQUAL_INT ( 0, r->length );
    TEST_ASSERT_EQUAL_INT ( 0, r->bit_index );

    remove ( resolved );
    g_free ( resolved );
}


/**
 * Load neexistujiciho souboru = success + prazdny storage.
 */
void test_persist_load_missing_file ( void ) {
    watch_add ( "stay_wiped", 0xC000, -1 );

    TEST_ASSERT_TRUE ( watch_load_from_filepath ( "definitely_does_not_exist.watch" ) );

    TEST_ASSERT_EQUAL_INT ( 0, (int) watch_count ( ) );
}


/**
 * Forward-compat: soubor s neznamym typem = neznamy radek se skipne.
 */
void test_persist_unknown_type_skip ( void ) {
    const char *tmpfile = "test_watch_unknown_type.watch";

    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, tmpfile );
    TEST_ASSERT_NOT_NULL ( resolved );

    FILE *f = fopen ( resolved, "w" );
    TEST_ASSERT_NOT_NULL ( f );
    fprintf ( f,
        "{\n"
        "  \"watch\": [\n"
        "    {\"name\": \"ok\", \"addr\": 49280, \"bank\": -1, \"type\": \"u8\"},\n"
        "    {\"name\": \"future\", \"addr\": 49408, \"bank\": -1, \"type\": \"q88\"}\n"
        "  ]\n"
        "}\n" );
    fclose ( f );

    TEST_ASSERT_TRUE ( watch_load_from_filepath ( tmpfile ) );

    /* "u8" radek prosel, "q88" (neznamy) skipnut. */
    TEST_ASSERT_EQUAL_INT ( 1, (int) watch_count ( ) );
    TEST_ASSERT_EQUAL_STRING ( "ok", watch_get ( 0 )->name );

    remove ( resolved );
    g_free ( resolved );
}


/**
 * V1.E.6.B - cmd_origin field round-trip přes save/load.
 *
 * Vytvoří watch řádky, nastaví různé cmd_origin, save, wipe, load -
 * origin fields se musí obnovit z JSON `origin` klíče.
 */
void test_persist_origin_roundtrip ( void ) {
    const char *tmpfile = "test_watch_origin_rt.watch";

    int i0 = watch_add ( "u_user",     0xC080, -1 );
    int i1 = watch_add ( "m_mcp",      0xC100, -1 );
    int i2 = watch_add ( "n_internal", 0xC200, -1 );
    TEST_ASSERT_TRUE ( i0 >= 0 );
    TEST_ASSERT_TRUE ( i1 >= 0 );
    TEST_ASSERT_TRUE ( i2 >= 0 );

    watch_set_cmd_origin ( i0, DBGAPI_CMD_ORIGIN_USER );
    watch_set_cmd_origin ( i1, DBGAPI_CMD_ORIGIN_MCP );
    watch_set_cmd_origin ( i2, DBGAPI_CMD_ORIGIN_INTERNAL );

    TEST_ASSERT_TRUE ( watch_save_to_filepath ( tmpfile ) );

    watch_clear_storage ( );
    TEST_ASSERT_EQUAL_INT ( 0, (int) watch_count ( ) );

    TEST_ASSERT_TRUE ( watch_load_from_filepath ( tmpfile ) );
    TEST_ASSERT_EQUAL_INT ( 3, (int) watch_count ( ) );

    const st_WATCH_ROW *r0 = watch_get ( 0 );
    const st_WATCH_ROW *r1 = watch_get ( 1 );
    const st_WATCH_ROW *r2 = watch_get ( 2 );
    TEST_ASSERT_NOT_NULL ( r0 );
    TEST_ASSERT_NOT_NULL ( r1 );
    TEST_ASSERT_NOT_NULL ( r2 );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_ORIGIN_USER,     r0->cmd_origin );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_ORIGIN_MCP,      r1->cmd_origin );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_ORIGIN_INTERNAL, r2->cmd_origin );

    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, tmpfile );
    if ( resolved ) {
        remove ( resolved );
        g_free ( resolved );
    };
}


/**
 * V1.E.6.B - backward compat: starý .watch soubor bez "origin" klíče
 * se načte tolerantně s defaultem USER.
 */
void test_persist_origin_bc_missing ( void ) {
    const char *tmpfile = "test_watch_origin_bc.watch";

    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, tmpfile );
    TEST_ASSERT_NOT_NULL ( resolved );

    /* Soubor BEZ origin klíče = starý formát před V1.E.6.B. */
    FILE *f = fopen ( resolved, "w" );
    TEST_ASSERT_NOT_NULL ( f );
    fprintf ( f,
        "{\n"
        "  \"watch\": [\n"
        "    {\"name\": \"legacy\", \"addr\": 49280, \"bank\": -1, \"type\": \"u8\"}\n"
        "  ]\n"
        "}\n" );
    fclose ( f );

    TEST_ASSERT_TRUE ( watch_load_from_filepath ( tmpfile ) );
    TEST_ASSERT_EQUAL_INT ( 1, (int) watch_count ( ) );
    const st_WATCH_ROW *r0 = watch_get ( 0 );
    TEST_ASSERT_NOT_NULL ( r0 );
    /* Missing origin -> default USER. */
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_ORIGIN_USER, r0->cmd_origin );

    remove ( resolved );
    g_free ( resolved );
}


/**
 * V1.E.6.B - tolerantní load: neznámý origin token = USER fallback.
 */
void test_persist_origin_unknown_token ( void ) {
    const char *tmpfile = "test_watch_origin_unk.watch";

    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, tmpfile );
    TEST_ASSERT_NOT_NULL ( resolved );

    FILE *f = fopen ( resolved, "w" );
    TEST_ASSERT_NOT_NULL ( f );
    fprintf ( f,
        "{\n"
        "  \"watch\": [\n"
        "    {\"name\": \"fut\", \"addr\": 49280, \"bank\": -1, \"type\": \"u8\","
        " \"origin\": \"future_role\"}\n"
        "  ]\n"
        "}\n" );
    fclose ( f );

    TEST_ASSERT_TRUE ( watch_load_from_filepath ( tmpfile ) );
    TEST_ASSERT_EQUAL_INT ( 1, (int) watch_count ( ) );
    const st_WATCH_ROW *r0 = watch_get ( 0 );
    TEST_ASSERT_NOT_NULL ( r0 );
    /* Unknown token -> USER fallback (= forward compat). */
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_ORIGIN_USER, r0->cmd_origin );

    remove ( resolved );
    g_free ( resolved );
}


/* ========================================================================= */
/*  Phase C - L2 Expression                                                  */
/* ========================================================================= */


/**
 * watch_add_expr parse validnich vyrazu = expr_text alokovany,
 * expr_ast != NULL, expr_error == NULL.
 */
void test_expr_parse_valid ( void ) {
    /* deref */
    int i0 = watch_add_expr ( "deref_HL", "[HL]", WATCH_MODE_EXPR_DEREF,
                                WATCH_TYPE_U8 );
    TEST_ASSERT_TRUE ( i0 >= 0 );
    const st_WATCH_ROW *r0 = watch_get ( (size_t) i0 );
    TEST_ASSERT_NOT_NULL ( r0 );
    TEST_ASSERT_EQUAL_INT ( WATCH_MODE_EXPR_DEREF, r0->mode );
    TEST_ASSERT_EQUAL_STRING ( "[HL]", r0->expr_text );
    TEST_ASSERT_NOT_NULL ( r0->expr_ast );
    TEST_ASSERT_NULL ( watch_get_expr_error ( r0 ) );

    /* scalar arithmetic */
    int i1 = watch_add_expr ( NULL, "0xC080 + 1", WATCH_MODE_EXPR_SCALAR,
                                WATCH_TYPE_U16LE );
    TEST_ASSERT_TRUE ( i1 >= 0 );
    const st_WATCH_ROW *r1 = watch_get ( (size_t) i1 );
    TEST_ASSERT_NOT_NULL ( r1 );
    TEST_ASSERT_EQUAL_INT ( WATCH_MODE_EXPR_SCALAR, r1->mode );
    TEST_ASSERT_NOT_NULL ( r1->expr_ast );
    TEST_ASSERT_NULL ( watch_get_expr_error ( r1 ) );

    /* user var */
    int i2 = watch_add_expr ( "fc", "$framecnt", WATCH_MODE_EXPR_SCALAR,
                                WATCH_TYPE_I8 );
    TEST_ASSERT_TRUE ( i2 >= 0 );
    const st_WATCH_ROW *r2 = watch_get ( (size_t) i2 );
    TEST_ASSERT_NOT_NULL ( r2->expr_ast );
}


/**
 * Invalidni vyrazy = expr_ast NULL + expr_error nastaven.
 */
void test_expr_parse_invalid ( void ) {
    int i0 = watch_add_expr ( NULL, "[[", WATCH_MODE_EXPR_SCALAR,
                                WATCH_TYPE_U8 );
    TEST_ASSERT_TRUE ( i0 >= 0 );  /* radek se vlozi i pri parse erroru */
    const st_WATCH_ROW *r0 = watch_get ( (size_t) i0 );
    TEST_ASSERT_NOT_NULL ( r0 );
    TEST_ASSERT_NULL ( r0->expr_ast );
    TEST_ASSERT_NOT_NULL ( watch_get_expr_error ( r0 ) );

    int i1 = watch_add_expr ( NULL, "1 +", WATCH_MODE_EXPR_SCALAR,
                                WATCH_TYPE_U8 );
    TEST_ASSERT_TRUE ( i1 >= 0 );
    const st_WATCH_ROW *r1 = watch_get ( (size_t) i1 );
    TEST_ASSERT_NULL ( r1->expr_ast );
    TEST_ASSERT_NOT_NULL ( watch_get_expr_error ( r1 ) );

    /* prazdny / NULL expr = -1 (zadny radek). */
    int i2 = watch_add_expr ( NULL, "", WATCH_MODE_EXPR_SCALAR,
                                WATCH_TYPE_U8 );
    TEST_ASSERT_EQUAL_INT ( -1, i2 );
}


/**
 * watch_set_expr update na existujicim radku.
 */
void test_expr_set_update ( void ) {
    int i = watch_add_expr ( NULL, "1", WATCH_MODE_EXPR_SCALAR,
                              WATCH_TYPE_U8 );
    TEST_ASSERT_TRUE ( i >= 0 );

    /* Update na novy validni vyraz. */
    TEST_ASSERT_TRUE ( watch_set_expr ( i, "2 + 3", WATCH_MODE_EXPR_SCALAR ) );
    const st_WATCH_ROW *r = watch_get ( (size_t) i );
    TEST_ASSERT_EQUAL_STRING ( "2 + 3", r->expr_text );
    TEST_ASSERT_NOT_NULL ( r->expr_ast );
    TEST_ASSERT_NULL ( watch_get_expr_error ( r ) );

    /* Update na invalidni - radek zustane ale parse error nastaven. */
    TEST_ASSERT_TRUE ( watch_set_expr ( i, "garbage @@@", WATCH_MODE_EXPR_SCALAR ) );
    r = watch_get ( (size_t) i );
    TEST_ASSERT_NULL ( r->expr_ast );
    TEST_ASSERT_NOT_NULL ( watch_get_expr_error ( r ) );

    /* Update na ADDRESS mode = vycisteni expr fieldu. */
    TEST_ASSERT_TRUE ( watch_set_expr ( i, NULL, WATCH_MODE_ADDRESS ) );
    r = watch_get ( (size_t) i );
    TEST_ASSERT_EQUAL_INT ( WATCH_MODE_ADDRESS, r->mode );
    TEST_ASSERT_NULL ( r->expr_text );
    TEST_ASSERT_NULL ( r->expr_ast );
    TEST_ASSERT_NULL ( r->expr_error );
}


/**
 * Eval scalar bez memory access = konstantni aritmetika.
 *
 * Nevyzaduje plne nainicializovany g_mzarch_main.cpu - bp_expr_eval
 * pro pure konstanty vrati spravne (cpu pole nepouzite).
 */
void test_expr_eval_scalar_const ( void ) {
    int i = watch_add_expr ( NULL, "0x10 + 5", WATCH_MODE_EXPR_SCALAR,
                              WATCH_TYPE_U8 );
    TEST_ASSERT_TRUE ( i >= 0 );
    const st_WATCH_ROW *r = watch_get ( (size_t) i );
    TEST_ASSERT_NOT_NULL ( r->expr_ast );

    int32_t v = watch_eval_expr ( r );
    TEST_ASSERT_EQUAL_INT32 ( 21, v );
}


/**
 * mode_to_str / mode_from_str roundtrip.
 */
void test_mode_string_roundtrip ( void ) {
    const en_WATCH_MODE modes[] = {
        WATCH_MODE_ADDRESS, WATCH_MODE_EXPR_SCALAR, WATCH_MODE_EXPR_DEREF,
    };
    for ( size_t i = 0; i < sizeof ( modes ) / sizeof ( modes[0] ); i++ ) {
        const char *s = watch_mode_to_str ( modes[ i ] );
        TEST_ASSERT_NOT_NULL ( s );
        en_WATCH_MODE back;
        TEST_ASSERT_TRUE ( watch_mode_from_str ( s, &back ) );
        TEST_ASSERT_EQUAL_INT ( modes[ i ], back );
    };
    en_WATCH_MODE dummy;
    TEST_ASSERT_FALSE ( watch_mode_from_str ( "unknown", &dummy ) );
    TEST_ASSERT_FALSE ( watch_mode_from_str ( NULL, &dummy ) );
}


/**
 * Persist roundtrip s mode/expr fieldy.
 */
void test_persist_roundtrip_mode ( void ) {
    const char *tmpfile = "test_watch_roundtrip_mode.watch";

    /* Mix vsech tri modu. */
    watch_add ( "addr_one", 0xC000, -1 );  /* ADDRESS */
    int ie = watch_add_expr ( "expr_scl", "0xFF + 1",
                                WATCH_MODE_EXPR_SCALAR, WATCH_TYPE_U16LE );
    TEST_ASSERT_TRUE ( ie >= 0 );
    int id = watch_add_expr ( "expr_drf", "[HL]",
                                WATCH_MODE_EXPR_DEREF, WATCH_TYPE_U8 );
    TEST_ASSERT_TRUE ( id >= 0 );

    TEST_ASSERT_TRUE ( watch_save_to_filepath ( tmpfile ) );

    watch_clear_storage ( );
    TEST_ASSERT_TRUE ( watch_load_from_filepath ( tmpfile ) );

    TEST_ASSERT_EQUAL_INT ( 3, (int) watch_count ( ) );

    const st_WATCH_ROW *r0 = watch_get ( 0 );
    TEST_ASSERT_EQUAL_INT ( WATCH_MODE_ADDRESS, r0->mode );
    TEST_ASSERT_NULL ( r0->expr_text );

    const st_WATCH_ROW *r1 = watch_get ( 1 );
    TEST_ASSERT_EQUAL_INT ( WATCH_MODE_EXPR_SCALAR, r1->mode );
    TEST_ASSERT_EQUAL_STRING ( "0xFF + 1", r1->expr_text );
    TEST_ASSERT_NOT_NULL ( r1->expr_ast );  /* reparse pri load */

    const st_WATCH_ROW *r2 = watch_get ( 2 );
    TEST_ASSERT_EQUAL_INT ( WATCH_MODE_EXPR_DEREF, r2->mode );
    TEST_ASSERT_EQUAL_STRING ( "[HL]", r2->expr_text );
    TEST_ASSERT_NOT_NULL ( r2->expr_ast );

    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, tmpfile );
    if ( resolved ) {
        remove ( resolved );
        g_free ( resolved );
    };
}


/* ========================================================================= */
/*  Phase E - watch_cache + snapshot baseline + min/max + change tracking    */
/* ========================================================================= */


/**
 * Sign-extend helper: u8 unsigned vs i8 signed.
 */
void test_cache_value_to_signed ( void ) {
    /* Unsigned. */
    TEST_ASSERT_EQUAL_INT64 ( 0xFF,
        watch_cache_value_to_signed ( WATCH_TYPE_U8, 0xFFu ) );
    TEST_ASSERT_EQUAL_INT64 ( 0xFFFFu,
        watch_cache_value_to_signed ( WATCH_TYPE_U16LE, 0xFFFFu ) );

    /* Signed. */
    TEST_ASSERT_EQUAL_INT64 ( -1,
        watch_cache_value_to_signed ( WATCH_TYPE_I8, 0xFFu ) );
    TEST_ASSERT_EQUAL_INT64 ( -1,
        watch_cache_value_to_signed ( WATCH_TYPE_I16LE, 0xFFFFu ) );
    TEST_ASSERT_EQUAL_INT64 ( -128,
        watch_cache_value_to_signed ( WATCH_TYPE_I8, 0x80u ) );
    TEST_ASSERT_EQUAL_INT64 ( -32768,
        watch_cache_value_to_signed ( WATCH_TYPE_I16BE, 0x8000u ) );
    TEST_ASSERT_EQUAL_INT64 ( (int32_t)0x80000000,
        watch_cache_value_to_signed ( WATCH_TYPE_I32LE, 0x80000000u ) );
}


/**
 * Min/max tracking - sequence hodnot, prislusne min/max po update.
 */
void test_watch_min_max_tracking ( void ) {
    int idx = watch_add ( "tst", 0x1000, -1 );
    TEST_ASSERT_GREATER_OR_EQUAL_INT ( 0, idx );
    const st_WATCH_ROW *r = watch_get ( (size_t) idx );
    TEST_ASSERT_NOT_NULL ( r );
    int id = r->id;

    /* Posloupnost: 0x10 -> 0x05 -> 0x20 -> 0x18 (vsechno u8). */
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x10, NULL, 0, 1 );
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x05, NULL, 0, 2 );
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x20, NULL, 0, 3 );
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x18, NULL, 0, 4 );

    const st_WATCH_CACHE_VIEW *c = watch_cache_get ( id );
    TEST_ASSERT_NOT_NULL ( c );
    TEST_ASSERT_TRUE ( c->valid );
    TEST_ASSERT_TRUE ( c->min_max_valid );
    TEST_ASSERT_EQUAL_INT64 ( 0x05, c->min_int );
    TEST_ASSERT_EQUAL_INT64 ( 0x20, c->max_int );
    /* Change count: 3 (0x10->0x05, 0x05->0x20, 0x20->0x18); prvni sample neni
     * pocitan jako zmena. */
    TEST_ASSERT_EQUAL_UINT64 ( 3, c->change_count );
}


/**
 * Min/max tracking pro signed typ - sign-extend semantika.
 */
void test_watch_min_max_signed ( void ) {
    int idx = watch_add ( "sig", 0x1000, -1 );
    const st_WATCH_ROW *r = watch_get ( (size_t) idx );
    int id = r->id;

    /* i8: 0x10 (+16) -> 0xFF (-1) -> 0x7F (+127) -> 0x80 (-128). */
    watch_cache_update ( id, WATCH_TYPE_I8, WATCH_MODE_ADDRESS, 0x10, NULL, 0, 1 );
    watch_cache_update ( id, WATCH_TYPE_I8, WATCH_MODE_ADDRESS, 0xFF, NULL, 0, 2 );
    watch_cache_update ( id, WATCH_TYPE_I8, WATCH_MODE_ADDRESS, 0x7F, NULL, 0, 3 );
    watch_cache_update ( id, WATCH_TYPE_I8, WATCH_MODE_ADDRESS, 0x80, NULL, 0, 4 );

    const st_WATCH_CACHE_VIEW *c = watch_cache_get ( id );
    TEST_ASSERT_NOT_NULL ( c );
    TEST_ASSERT_EQUAL_INT64 ( -128, c->min_int );
    TEST_ASSERT_EQUAL_INT64 ( 127, c->max_int );
}


/**
 * Reset min/max API - po reset_one min_max_valid=false a change_count=0.
 */
void test_watch_min_max_reset ( void ) {
    int idx = watch_add ( "tst", 0x2000, -1 );
    const st_WATCH_ROW *r = watch_get ( (size_t) idx );
    int id = r->id;

    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x10, NULL, 0, 1 );
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x20, NULL, 0, 2 );

    const st_WATCH_CACHE_VIEW *c = watch_cache_get ( id );
    TEST_ASSERT_TRUE ( c->min_max_valid );
    TEST_ASSERT_EQUAL_UINT64 ( 1, c->change_count );

    watch_cache_reset_one ( id );
    c = watch_cache_get ( id );
    TEST_ASSERT_NOT_NULL ( c );
    TEST_ASSERT_FALSE ( c->valid );
    TEST_ASSERT_FALSE ( c->min_max_valid );
    TEST_ASSERT_EQUAL_UINT64 ( 0, c->change_count );
    TEST_ASSERT_EQUAL_INT ( WATCH_DELTA_NONE, c->delta_dir );
}


/**
 * Change count: increment jen pri skutecne zmene (= ne kazdy frame).
 */
void test_watch_change_count ( void ) {
    int idx = watch_add ( "tst", 0x3000, -1 );
    const st_WATCH_ROW *r = watch_get ( (size_t) idx );
    int id = r->id;

    /* Stejna hodnota opakovane = zadna zmena. */
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x42, NULL, 0, 1 );
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x42, NULL, 0, 2 );
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x42, NULL, 0, 3 );

    const st_WATCH_CACHE_VIEW *c = watch_cache_get ( id );
    TEST_ASSERT_EQUAL_UINT64 ( 0, c->change_count );
    TEST_ASSERT_EQUAL_INT ( WATCH_DELTA_NONE, c->delta_dir );

    /* Jedna zmena. */
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x43, NULL, 0, 4 );
    c = watch_cache_get ( id );
    TEST_ASSERT_EQUAL_UINT64 ( 1, c->change_count );
    TEST_ASSERT_EQUAL_INT ( WATCH_DELTA_UP, c->delta_dir );

    /* Snizit. */
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x40, NULL, 0, 5 );
    c = watch_cache_get ( id );
    TEST_ASSERT_EQUAL_UINT64 ( 2, c->change_count );
    TEST_ASSERT_EQUAL_INT ( WATCH_DELTA_DOWN, c->delta_dir );
}


/**
 * Type change resetuje prev a min/max.
 */
void test_watch_cache_type_change_reset ( void ) {
    int idx = watch_add ( "tst", 0x4000, -1 );
    const st_WATCH_ROW *r = watch_get ( (size_t) idx );
    int id = r->id;

    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x10, NULL, 0, 1 );
    watch_cache_update ( id, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x20, NULL, 0, 2 );

    /* Prepnuti na u16 = reset min/max + change_count. */
    watch_cache_update ( id, WATCH_TYPE_U16LE, WATCH_MODE_ADDRESS, 0x1234, NULL, 0, 3 );

    const st_WATCH_CACHE_VIEW *c = watch_cache_get ( id );
    TEST_ASSERT_TRUE ( c->valid );
    TEST_ASSERT_TRUE ( c->min_max_valid );
    TEST_ASSERT_EQUAL_INT64 ( 0x1234, c->min_int );
    TEST_ASSERT_EQUAL_INT64 ( 0x1234, c->max_int );
    TEST_ASSERT_EQUAL_UINT64 ( 0, c->change_count );
}


/**
 * Reset_all - vsechny radky reset.
 */
void test_watch_cache_reset_all ( void ) {
    int i1 = watch_add ( "a", 0x1000, -1 );
    int i2 = watch_add ( "b", 0x2000, -1 );
    int id1 = watch_get ( (size_t) i1 )->id;
    int id2 = watch_get ( (size_t) i2 )->id;

    watch_cache_update ( id1, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x11, NULL, 0, 1 );
    watch_cache_update ( id1, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x22, NULL, 0, 2 );
    watch_cache_update ( id2, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x33, NULL, 0, 3 );

    watch_cache_reset_all ( );

    const st_WATCH_CACHE_VIEW *c1 = watch_cache_get ( id1 );
    const st_WATCH_CACHE_VIEW *c2 = watch_cache_get ( id2 );
    TEST_ASSERT_FALSE ( c1->valid );
    TEST_ASSERT_FALSE ( c2->valid );
    TEST_ASSERT_EQUAL_UINT64 ( 0, c1->change_count );
    TEST_ASSERT_EQUAL_UINT64 ( 0, c2->change_count );
}


/**
 * Prune_stale - cache + snapshot drop entries pro id ktere uz nejsou v live_ids.
 */
void test_watch_cache_prune_stale ( void ) {
    int i1 = watch_add ( "a", 0x1000, -1 );
    int i2 = watch_add ( "b", 0x2000, -1 );
    int id1 = watch_get ( (size_t) i1 )->id;
    int id2 = watch_get ( (size_t) i2 )->id;

    watch_cache_update ( id1, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x11, NULL, 0, 1 );
    watch_cache_update ( id2, WATCH_TYPE_U8, WATCH_MODE_ADDRESS, 0x22, NULL, 0, 2 );

    TEST_ASSERT_NOT_NULL ( watch_cache_get ( id1 ) );
    TEST_ASSERT_NOT_NULL ( watch_cache_get ( id2 ) );

    /* Smazat id2 z live - prune ho odebere. */
    int live[] = { id1 };
    watch_cache_prune_stale ( live, 1 );

    TEST_ASSERT_NOT_NULL ( watch_cache_get ( id1 ) );
    TEST_ASSERT_NULL ( watch_cache_get ( id2 ) );
}


/**
 * Snapshot capture + restore + delta porovnani.
 */
void test_watch_snapshot_capture_restore ( void ) {
    int i1 = watch_add ( "a", 0x1000, -1 );
    int i2 = watch_add ( "b", 0x2000, -1 );
    int id1 = watch_get ( (size_t) i1 )->id;
    int id2 = watch_get ( (size_t) i2 )->id;

    TEST_ASSERT_FALSE ( watch_snapshot_active ( ) );

    /* Capture: id1 = u8 0x10, id2 = bytes "AB". */
    watch_snapshot_begin ( 42 );
    watch_snapshot_put ( id1, WATCH_TYPE_U8, 0x10, NULL, 0 );
    uint8_t bb[3] = { 'A', 'B', 0 };
    watch_snapshot_put ( id2, WATCH_TYPE_BYTES, 0, bb, 2 );

    TEST_ASSERT_TRUE ( watch_snapshot_active ( ) );
    TEST_ASSERT_EQUAL_INT ( 42, watch_snapshot_frame ( ) );

    const st_WATCH_VALUE_SNAP *s1 = watch_snapshot_get ( id1 );
    const st_WATCH_VALUE_SNAP *s2 = watch_snapshot_get ( id2 );
    TEST_ASSERT_NOT_NULL ( s1 );
    TEST_ASSERT_NOT_NULL ( s2 );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_U8, s1->type );
    TEST_ASSERT_EQUAL_UINT64 ( 0x10, s1->value_int );
    TEST_ASSERT_EQUAL_INT ( WATCH_TYPE_BYTES, s2->type );
    TEST_ASSERT_EQUAL_INT ( 2, (int) s2->len );
    TEST_ASSERT_EQUAL_UINT8 ( 'A', s2->bytes[0] );
    TEST_ASSERT_EQUAL_UINT8 ( 'B', s2->bytes[1] );

    /* Clear -> deactivate. */
    watch_snapshot_clear ( );
    TEST_ASSERT_FALSE ( watch_snapshot_active ( ) );
    TEST_ASSERT_NULL ( watch_snapshot_get ( id1 ) );
    TEST_ASSERT_NULL ( watch_snapshot_get ( id2 ) );
}


/**
 * Snapshot remove - explicitni cleanup pro jeden radek.
 */
void test_watch_snapshot_remove ( void ) {
    int i1 = watch_add ( "a", 0x1000, -1 );
    int i2 = watch_add ( "b", 0x2000, -1 );
    int id1 = watch_get ( (size_t) i1 )->id;
    int id2 = watch_get ( (size_t) i2 )->id;

    watch_snapshot_begin ( 100 );
    watch_snapshot_put ( id1, WATCH_TYPE_U8, 1, NULL, 0 );
    watch_snapshot_put ( id2, WATCH_TYPE_U8, 2, NULL, 0 );
    TEST_ASSERT_TRUE ( watch_snapshot_active ( ) );

    watch_snapshot_remove ( id1 );
    TEST_ASSERT_NULL ( watch_snapshot_get ( id1 ) );
    TEST_ASSERT_NOT_NULL ( watch_snapshot_get ( id2 ) );
    TEST_ASSERT_TRUE ( watch_snapshot_active ( ) );

    /* Remove posledniho = deaktivace. */
    watch_snapshot_remove ( id2 );
    TEST_ASSERT_FALSE ( watch_snapshot_active ( ) );
}


/**
 * String/bytes cache - memcmp detekce zmen + delta_dir = WATCH_DELTA_STRING.
 */
void test_watch_cache_string_change ( void ) {
    int idx = watch_add ( "txt", 0x5000, -1 );
    const st_WATCH_ROW *r = watch_get ( (size_t) idx );
    int id = r->id;

    uint8_t a[3] = { 'A', 'B', 'C' };
    uint8_t b[3] = { 'A', 'B', 'C' };
    uint8_t c[3] = { 'A', 'B', 'X' };

    watch_cache_update ( id, WATCH_TYPE_ASCII, WATCH_MODE_ADDRESS, 0, a, 3, 1 );
    const st_WATCH_CACHE_VIEW *cv = watch_cache_get ( id );
    TEST_ASSERT_TRUE ( cv->valid );
    TEST_ASSERT_EQUAL_INT ( WATCH_DELTA_NONE, cv->delta_dir );
    TEST_ASSERT_FALSE ( cv->min_max_valid ); /* string -> ne */
    TEST_ASSERT_EQUAL_UINT64 ( 0, cv->change_count );

    /* Stejny obsah = no change. */
    watch_cache_update ( id, WATCH_TYPE_ASCII, WATCH_MODE_ADDRESS, 0, b, 3, 2 );
    cv = watch_cache_get ( id );
    TEST_ASSERT_EQUAL_UINT64 ( 0, cv->change_count );
    TEST_ASSERT_EQUAL_INT ( WATCH_DELTA_NONE, cv->delta_dir );

    /* Zmena obsahu. */
    watch_cache_update ( id, WATCH_TYPE_ASCII, WATCH_MODE_ADDRESS, 0, c, 3, 3 );
    cv = watch_cache_get ( id );
    TEST_ASSERT_EQUAL_UINT64 ( 1, cv->change_count );
    TEST_ASSERT_EQUAL_INT ( WATCH_DELTA_STRING, cv->delta_dir );
    TEST_ASSERT_EQUAL_INT ( 3, cv->last_change_frame );
}


/* ========================================================================= */
/*  MAIN                                                                     */
/* ========================================================================= */


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    /* Lifecycle */
    RUN_TEST ( test_init_idempotent );
    RUN_TEST ( test_count_get_basic );

    /* CRUD */
    RUN_TEST ( test_add_remove );
    RUN_TEST ( test_remove_out_of_range );
    RUN_TEST ( test_rename );
    RUN_TEST ( test_add_name_truncate );
    RUN_TEST ( test_move );
    RUN_TEST ( test_move_noop_same_index );
    RUN_TEST ( test_add_anonymous_row );
    RUN_TEST ( test_clear_storage );

    /* Parse */
    RUN_TEST ( test_parse_addr_hex_cstyle );
    RUN_TEST ( test_parse_addr_hex_iasm_suffix );
    RUN_TEST ( test_parse_addr_decimal );
    RUN_TEST ( test_parse_addr_out_of_range );
    RUN_TEST ( test_parse_addr_invalid );
    RUN_TEST ( test_parse_addr_symbol );

    /* Type/fmt mapping */
    RUN_TEST ( test_watch_type_string_roundtrip );
    RUN_TEST ( test_watch_fmt_string_roundtrip );
    RUN_TEST ( test_type_metadata );
    RUN_TEST ( test_set_type_defaults );

    /* Format */
    RUN_TEST ( test_watch_format_int_unsigned );
    RUN_TEST ( test_watch_format_int_signed );
    RUN_TEST ( test_watch_format_bit );
    RUN_TEST ( test_watch_format_ascii_basic );
    RUN_TEST ( test_watch_format_ascii_mz );
    RUN_TEST ( test_watch_format_bytes_short );
    RUN_TEST ( test_watch_format_bytes_long );
    RUN_TEST ( test_watch_format_bytes_no_inline_limit );

    /* Read (vyzaduje memory init z mztest_init) */
    RUN_TEST ( test_watch_u16le_vs_be );
    RUN_TEST ( test_watch_read_bit );
    RUN_TEST ( test_watch_u32_byteorder );
    RUN_TEST ( test_watch_i32_signed );

    /* Persistence */
    RUN_TEST ( test_persist_roundtrip );
    RUN_TEST ( test_persist_roundtrip_types );
    RUN_TEST ( test_persist_load_phase_a_format );
    RUN_TEST ( test_persist_load_missing_file );
    RUN_TEST ( test_persist_unknown_type_skip );
    /* V1.E.6.B - cmd_origin persist + tolerantní load */
    RUN_TEST ( test_persist_origin_roundtrip );
    RUN_TEST ( test_persist_origin_bc_missing );
    RUN_TEST ( test_persist_origin_unknown_token );

    /* Phase C - L2 expression */
    RUN_TEST ( test_expr_parse_valid );
    RUN_TEST ( test_expr_parse_invalid );
    RUN_TEST ( test_expr_set_update );
    RUN_TEST ( test_expr_eval_scalar_const );
    RUN_TEST ( test_mode_string_roundtrip );
    RUN_TEST ( test_persist_roundtrip_mode );

    /* Phase E - watch_cache + snapshot baseline. */
    RUN_TEST ( test_cache_value_to_signed );
    RUN_TEST ( test_watch_min_max_tracking );
    RUN_TEST ( test_watch_min_max_signed );
    RUN_TEST ( test_watch_min_max_reset );
    RUN_TEST ( test_watch_change_count );
    RUN_TEST ( test_watch_cache_type_change_reset );
    RUN_TEST ( test_watch_cache_reset_all );
    RUN_TEST ( test_watch_cache_prune_stale );
    RUN_TEST ( test_watch_snapshot_capture_restore );
    RUN_TEST ( test_watch_snapshot_remove );
    RUN_TEST ( test_watch_cache_string_change );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
