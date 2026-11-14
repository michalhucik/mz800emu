/**
 * @file   test_tmz_player.c
 * @brief  Smoke test playeru TMZ: profil ROM-800-NOMINAL, blok, CMT stream.
 *
 * Sestaví MZF s tělem 1 KB v paměti, vytvoří z něj blok TMZ profilem
 * ROM-800-NOMINAL a přehraje ho playerem při 44 100 Hz a při GDGCLK_BASE.
 * Ověřuje délku a velikost streamu a chování bloku Pause 0 (stop pásky).
 * Naměřené hodnoty se vypisují na výstup testu (printf).
 */

#include "mztest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_player.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "mzarch/mz800/gdg/mz800_gdgclk.h"

/** @brief Velikost těla testovacího MZF. */
#define BODY_SIZE 1024

/** @brief Sestaví blok ROM-800-NOMINAL z MZF 1 KB; volající uvolní tmz_block_free(). */
static st_TZX_BLOCK* build_test_block ( void ) {
    st_MZF_HEADER header;
    uint8_t body[BODY_SIZE];
    memset ( &header, 0, sizeof ( header ) );
    header.ftype = 0x01;
    mzf_tools_set_fname ( &header, "EMUTEST" );
    header.fsize = BODY_SIZE;
    header.fstrt = 0x1200;
    header.fexec = 0x1200;
    for ( int i = 0; i < BODY_SIZE; i++ ) body[i] = (uint8_t) ( i & 0xFF );

    st_TMZ_PROFILE_ARGS args;
    tmz_profile_args_init ( &args );
    en_TMZ_PROFILE_ERROR perr = 0;
    st_TZX_BLOCK *blk = tmz_profile_build_block ( "ROM-800-NOMINAL", &args, &header, body, BODY_SIZE, &perr );
    TEST_ASSERT_NOT_NULL ( blk );
    return blk;
}

/** @brief Hlavní smoke test: 44 100 Hz a GDGCLK_BASE. */
void test_player_rates ( void ) {
    st_TZX_BLOCK *blk = build_test_block ();

    st_TMZ_PLAYER_CONFIG cfg;
    en_TMZ_PLAYER_ERROR err = TMZ_PLAYER_OK;
    tmz_player_config_init ( &cfg );
    cfg.sample_rate = 44100;
    st_CMT_STREAM *s1 = tmz_player_play_block ( blk, &cfg, &err );
    TEST_ASSERT_NOT_NULL ( s1 );
    TEST_ASSERT_EQUAL_INT ( TMZ_PLAYER_OK, err );
    double len1 = cmt_stream_get_length ( s1 );
    uint32_t size1 = (uint32_t) cmt_stream_get_size ( s1 );
    printf ( "TMZ 44100 Hz: length=%.4f s size=%u B err=%d\n", len1, (unsigned) size1, (int) err );
    TEST_ASSERT_TRUE ( len1 >= 10.0 && len1 <= 40.0 );

    err = TMZ_PLAYER_OK;
    cfg.sample_rate = GDGCLK_BASE;
    st_CMT_STREAM *s2 = tmz_player_play_block ( blk, &cfg, &err );
    printf ( "TMZ GDGCLK_BASE (%u Hz): stream=%s err=%d\n", (unsigned) GDGCLK_BASE, s2 ? "ok" : "NULL", (int) err );
    if ( s2 ) {
        double len2 = cmt_stream_get_length ( s2 );
        uint32_t size2 = (uint32_t) cmt_stream_get_size ( s2 );
        printf ( "TMZ GDGCLK_BASE: length=%.4f s size=%u B ratio_len=%.5f ratio_size=%.3f\n",
                 len2, (unsigned) size2, len2 / len1, (double) size2 / size1 );
        TEST_ASSERT_TRUE ( fabs ( len2 - len1 ) <= 0.01 * len1 );
        TEST_ASSERT_TRUE ( size2 <= 4u * size1 );
        cmt_stream_destroy ( s2 );
    }
    TEST_ASSERT_NOT_NULL ( s2 );
    TEST_ASSERT_EQUAL_INT ( TMZ_PLAYER_OK, err );

    cmt_stream_destroy ( s1 );
    tmz_block_free ( blk );
}

/** @brief Blok TZX 0x20 Pause 0 (stop pásky) nevytváří stream a není chybou. */
void test_pause_zero_returns_null ( void ) {
    uint8_t data[2] = { 0x00, 0x00 };
    st_TZX_BLOCK blk;
    memset ( &blk, 0, sizeof ( blk ) );
    blk.id = TZX_BLOCK_ID_PAUSE;
    blk.length = 2;
    blk.data = data;

    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init ( &cfg );
    cfg.sample_rate = 44100;
    en_TMZ_PLAYER_ERROR err = TMZ_PLAYER_ERROR_NULL_INPUT;
    st_CMT_STREAM *s = tmz_player_play_block ( &blk, &cfg, &err );
    TEST_ASSERT_NULL ( s );
    TEST_ASSERT_EQUAL_INT ( TMZ_PLAYER_OK, err );
}

void setUp ( void ) {
}

void tearDown ( void ) {
}

int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ();
    UNITY_BEGIN ();
    RUN_TEST ( test_player_rates );
    RUN_TEST ( test_pause_zero_returns_null );
    int result = UNITY_END ();
    mztest_teardown ();
    return result;
}
