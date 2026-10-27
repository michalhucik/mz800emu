/*
 * io_activity.c - Implementace sliding window IORQ activity trackeru.
 *
 * Viz io_activity.h pro datový model a kontrakty.
 *
 * V1.5 fix #1: 16-bit port indexing - g_io_activity má 65536 slotů.
 * Bez 16-bit klíče se Unicard sub-registry (0xCF01..0xCF07) v Overview
 * panelu zobrazovaly s nulovou Activity (= sdílely jeden 8-bit slot 0xCF).
 *
 * V1.5.D fix #2: counters rozdelene per smer (IN vs OUT). Drive jediny
 * counter set => napr. port 0xF0 (W=Palette, R=JOY0) zobrazoval aktivitu
 * pro oba smery, i kdyz programe pouze pisal palette.
 *
 * Licence: GPLv3
 */

#include "io_activity.h"

#include <stdbool.h>
#include <string.h>


/**
 * Globální tabulka aktivit - 65536 slotů, indexováno plnou 16-bit IORQ
 * adresou (= obsah BC při IN/OUT). BSS alokace ~18 MB (V1.5.D fix #2);
 * nealokuje se na heap, takže vznik / rušení panelu je zdarma.
 */
st_IO_PORT_ACTIVITY g_io_activity[ IO_ACTIVITY_TABLE_SIZE ];


/**
 * UI tracking flag - default 0 (= zero overhead při zavřeném panelu).
 */
uint8_t g_io_window_tracking_active = 0;

/** @brief Maska klientů sledování (bit = IO_TRACKING_CLIENT_*); jen UI vlákno. */
static unsigned s_tracking_clients = 0;


bool io_activity_tracking_set_client ( unsigned client, bool on )
{
    if ( client >= 32 ) return false;
    if ( on ) s_tracking_clients |= ( 1u << client );
    else s_tracking_clients &= ~( 1u << client );
    uint8_t want = s_tracking_clients ? 1u : 0u;
    if ( g_io_window_tracking_active == want ) return false;
    g_io_window_tracking_active = want;
    return true;
}


bool io_activity_tracking_client_active ( unsigned client )
{
    return client < 32 && ( s_tracking_clients & ( 1u << client ) ) != 0;
}


void io_activity_record_ghost ( uint16_t port )
{
    st_IO_PORT_ACTIVITY *a = &g_io_activity[ port ];
    if ( a->ghost_hits != UINT32_MAX ) a->ghost_hits++;
}


void io_activity_record_conflict ( uint16_t port )
{
    st_IO_PORT_ACTIVITY *a = &g_io_activity[ port ];
    if ( a->conflict_hits != UINT32_MAX ) a->conflict_hits++;
}


void io_activity_get_totals_8bit ( uint8_t low_byte, uint64_t *out_ghost, uint64_t *out_conflict,
                                   uint64_t *out_total_in, uint64_t *out_total_out )
{
    uint64_t g = 0, c = 0, ti = 0, to = 0;
    for ( unsigned hb = 0; hb < 256; hb++ ) {
        const st_IO_PORT_ACTIVITY *a = &g_io_activity[ ( hb << 8 ) | low_byte ];
        g += a->ghost_hits;
        c += a->conflict_hits;
        ti += a->total_hits_in;
        to += a->total_hits_out;
    }
    if ( out_ghost ) *out_ghost = g;
    if ( out_conflict ) *out_conflict = c;
    if ( out_total_in ) *out_total_in = ti;
    if ( out_total_out ) *out_total_out = to;
}


void io_activity_init ( void )
{
    memset ( g_io_activity, 0, sizeof ( g_io_activity ) );
    g_io_window_tracking_active = 0;
}


void io_activity_reset_all ( void )
{
    memset ( g_io_activity, 0, sizeof ( g_io_activity ) );
}


void io_activity_reset_port ( uint16_t port )
{
    memset ( &g_io_activity[ port ], 0,
             sizeof ( st_IO_PORT_ACTIVITY ) );
}


void io_activity_record_hit ( uint16_t port, uint8_t value,
                               bool is_in, uint32_t frame )
{
    st_IO_PORT_ACTIVITY *a = &g_io_activity[ port ];
    /* V1.5.D fix #2: inkrementuj per-smer bucket. Saturated 16-bit counter
     * - velmi vysoky hit rate v jednom frame (>65535) je nerealny, ale
     * chranime se pred wraparound. */
    if ( is_in ) {
        if ( a->hits_per_frame_in[ a->current_bucket ] < 0xFFFF ) {
            a->hits_per_frame_in[ a->current_bucket ]++;
        }
        a->total_hits_in++;
    } else {
        if ( a->hits_per_frame_out[ a->current_bucket ] < 0xFFFF ) {
            a->hits_per_frame_out[ a->current_bucket ]++;
        }
        a->total_hits_out++;
    }

    /* Fix #10: cache last value per smer pro UI Overview Hex zobrazeni.
     * Frame 0 = valid context (= start emu); ale init nuluje last_*_frame
     * na 0 takze get_last_value vraci false dokud neprijde aspon jeden hit.
     * Aby se rozlisil "no data" vs "first hit at frame 0", inkrementujeme
     * frame o 1 pri ukladani (= 0 zustava sentinelem). */
    uint32_t stamp = ( frame == 0xFFFFFFFFu ) ? 0xFFFFFFFFu : frame + 1u;
    if ( is_in ) {
        a->last_read_value = value;
        a->last_read_frame = stamp;
    } else {
        a->last_write_value = value;
        a->last_write_frame = stamp;
    }
}


bool io_activity_get_last_value ( uint16_t port, bool is_in,
                                   uint32_t current_frame,
                                   uint8_t *out_value,
                                   uint32_t *out_age_frames )
{
    if ( !out_value || !out_age_frames ) return false;
    const st_IO_PORT_ACTIVITY *a = &g_io_activity[ port ];
    uint32_t stamp = is_in ? a->last_read_frame : a->last_write_frame;
    if ( stamp == 0 ) return false;  /* zadny zaznam */
    /* stamp je ulozen jako frame+1 (= 0 = sentinel "no data"). */
    uint32_t actual = stamp - 1u;
    *out_value = is_in ? a->last_read_value : a->last_write_value;
    *out_age_frames = ( current_frame >= actual )
        ? ( current_frame - actual ) : 0u;
    return true;
}


uint32_t io_activity_get_hits_per_sec ( uint16_t port, bool is_in )
{
    const st_IO_PORT_ACTIVITY *a = &g_io_activity[ port ];
    /* V1.5.D fix #2: per-smer counter. Drive sdileny ring. */
    const uint16_t *ring = is_in ? a->hits_per_frame_in
                                 : a->hits_per_frame_out;
    uint32_t sum = 0;
    /* Sečti posledních IO_ACTIVITY_WINDOW_BUCKETS bucketů (= sliding window).
     * Pozn.: nesčítáme aktuální bucket (= ten ještě roste), bereme předchozích
     * IO_ACTIVITY_WINDOW_BUCKETS dokončených bucketů. */
    uint8_t cur = a->current_bucket;
    for ( uint8_t k = 0; k < IO_ACTIVITY_WINDOW_BUCKETS; k++ ) {
        /* index = (cur - 1 - k) modulo 60 */
        uint8_t idx;
        if ( cur >= ( 1 + k ) ) {
            idx = cur - 1 - k;
        } else {
            idx = (uint8_t)( IO_ACTIVITY_BUCKET_COUNT - ( 1 + k - cur ) );
        }
        sum += ring[ idx ];
    }
    return sum;
}


uint32_t io_activity_get_hits_per_sec_8bit ( uint8_t low_byte, bool is_in )
{
    uint32_t sum = 0;
    /* Iteruj všech 256 high-byte slotů pro daný low byte. */
    for ( unsigned hb = 0; hb < 256; hb++ ) {
        uint16_t addr = (uint16_t)( ( hb << 8 ) | low_byte );
        sum += io_activity_get_hits_per_sec ( addr, is_in );
    }
    return sum;
}


bool io_activity_get_last_value_8bit ( uint8_t low_byte, bool is_in,
                                        uint32_t current_frame,
                                        uint8_t *out_value,
                                        uint32_t *out_age_frames )
{
    if ( !out_value || !out_age_frames ) return false;
    bool found = false;
    uint32_t best_age = 0xFFFFFFFFu;
    uint8_t  best_value = 0;
    /* Iteruj 256 high-byte slotů, vyber nejmladší cache (= min age). */
    for ( unsigned hb = 0; hb < 256; hb++ ) {
        uint16_t addr = (uint16_t)( ( hb << 8 ) | low_byte );
        uint8_t v = 0;
        uint32_t age = 0;
        if ( io_activity_get_last_value ( addr, is_in, current_frame,
                                            &v, &age ) ) {
            if ( !found || age < best_age ) {
                best_age = age;
                best_value = v;
                found = true;
            }
        }
    }
    if ( !found ) return false;
    *out_value = best_value;
    *out_age_frames = best_age;
    return true;
}


void io_activity_reset_port_8bit ( uint8_t low_byte )
{
    for ( unsigned hb = 0; hb < 256; hb++ ) {
        uint16_t addr = (uint16_t)( ( hb << 8 ) | low_byte );
        memset ( &g_io_activity[ addr ], 0,
                 sizeof ( st_IO_PORT_ACTIVITY ) );
    }
}


void io_activity_advance_frame ( void )
{
    /* Skip pokud tracking neni aktivni - i pri 18 MB tabulce by jinak
     * advance_frame iteroval 65536 entries / vsync (= 50/s) = 3.3 M
     * inkrementu/s pro nic. Skip = zero overhead. */
    if ( !g_io_window_tracking_active ) return;

    for ( unsigned p = 0; p < IO_ACTIVITY_TABLE_SIZE; p++ ) {
        st_IO_PORT_ACTIVITY *a = &g_io_activity[ p ];
        a->current_bucket = (uint8_t)( ( a->current_bucket + 1 ) % IO_ACTIVITY_BUCKET_COUNT );
        /* V1.5.D fix #2: vynuluj oba smery v novem bucketu. */
        a->hits_per_frame_in[ a->current_bucket ] = 0;
        a->hits_per_frame_out[ a->current_bucket ] = 0;
    }
}
