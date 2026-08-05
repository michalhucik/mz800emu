/**
 * @file   hwlog.c
 * @brief  Implementace HW Log subsystému trace-suite.
 *
 * Per-chip hooky volá kód v iorq.c / gdg.c / pio8255.c / ... vždy
 * při změně relevantního stavu (write do registru, mode switch atd.).
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "hwlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "libs/cfgfile/cfgmodule.h"
#include "emulator/cfgmain.h"
#include "emulator/debugger/trace/tlog_common.h"
#include "emulator/debugger/trace/reclife.h"
#include "emulator/debugger/trace/eventlog.h"
#include "emulator/mzarch/mzarch.h"
#include "emulator/hw-generic/psg/psg.h"
#include "emulator/hw-generic/pio8255/pio8255.h"
#include "emulator/hw-generic/ctc8253/ctc8253.h"
#include "emulator/hw-generic/memory/memext.h"
#include "hw-generic/gdg/gdg_state.h"
#include "emulator/mzarch/mzhal.h"
#include "emulator/hw-generic/pioz80/pioz80.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

st_HWLOG_CONFIG g_hwlog_config;
int g_hwlog_active = 0;

static st_TLOG_WRITER s_writer;
static int s_writer_open = 0;


/**
 * @brief Mapování @ref en_HWLOG_CHIP na @ref en_EVENTLOG_CATEGORY.
 *
 * Helper pro fan-out emit v @ref hwlog_record(). Pokud chip nemá
 * odpovídající eventlog kategorii, vrátí @c EVENTLOG_CAT_COUNT
 * (= caller fan-out skipne).
 *
 * Mapping (1:1, žádný chip nemapuje na více kategorií):
 *  - HWLOG_CHIP_GDG_MODE     -> EVENTLOG_CAT_GDG_MODE
 *  - HWLOG_CHIP_GDG_BANKING  -> EVENTLOG_CAT_GDG_BANKING
 *  - HWLOG_CHIP_GDG_HWSCROLL -> EVENTLOG_CAT_GDG_HWSCROLL
 *  - HWLOG_CHIP_GDG_COLORS   -> EVENTLOG_CAT_GDG_COLORS
 *  - HWLOG_CHIP_GDG_WFRF     -> EVENTLOG_CAT_GDG_WFRF
 *  - HWLOG_CHIP_GDG_VIDEO    -> EVENTLOG_CAT_GDG_VIDEO
 *  - HWLOG_CHIP_PIO8255      -> EVENTLOG_CAT_PIO8255
 *  - HWLOG_CHIP_CTC8253      -> EVENTLOG_CAT_CTC8253
 *  - HWLOG_CHIP_PIOZ80       -> EVENTLOG_CAT_PIOZ80
 *  - HWLOG_CHIP_PSG          -> EVENTLOG_CAT_PSG
 *  - HWLOG_CHIP_QD           -> EVENTLOG_CAT_QD
 *  - HWLOG_CHIP_FDC          -> EVENTLOG_CAT_FDC
 *  - HWLOG_CHIP_MEMEXT       -> EVENTLOG_CAT_MEMEXT
 *  - HWLOG_CHIP_RD           -> EVENTLOG_CAT_RD
 */
static inline uint8_t hwlog_chip_to_eventlog_cat ( en_HWLOG_CHIP chip )
{
    switch ( chip ) {
        case HWLOG_CHIP_GDG_MODE:     return EVENTLOG_CAT_GDG_MODE;
        case HWLOG_CHIP_GDG_BANKING:  return EVENTLOG_CAT_GDG_BANKING;
        case HWLOG_CHIP_GDG_HWSCROLL: return EVENTLOG_CAT_GDG_HWSCROLL;
        case HWLOG_CHIP_GDG_COLORS:   return EVENTLOG_CAT_GDG_COLORS;
        case HWLOG_CHIP_GDG_WFRF:     return EVENTLOG_CAT_GDG_WFRF;
        case HWLOG_CHIP_GDG_VIDEO:    return EVENTLOG_CAT_GDG_VIDEO;
        case HWLOG_CHIP_PIO8255:      return EVENTLOG_CAT_PIO8255;
        case HWLOG_CHIP_CTC8253:      return EVENTLOG_CAT_CTC8253;
        case HWLOG_CHIP_PIOZ80:       return EVENTLOG_CAT_PIOZ80;
        case HWLOG_CHIP_PSG:          return EVENTLOG_CAT_PSG;
        case HWLOG_CHIP_QD:           return EVENTLOG_CAT_QD;
        case HWLOG_CHIP_FDC:          return EVENTLOG_CAT_FDC;
        case HWLOG_CHIP_MEMEXT:       return EVENTLOG_CAT_MEMEXT;
        case HWLOG_CHIP_RD:           return EVENTLOG_CAT_RD;
        default:                      return (uint8_t) EVENTLOG_CAT_COUNT;
    }
}


/**
 * @brief Pack 6 B hwlog payload do 4 B eventlog payload.
 *
 * Vezme první 4 B z 6 B payloadu hwlog jako LE uint32. Pro chipy které
 * v 5./6. B nesly další data (PIOZ80 24-bit delta v B[3..5]) jde o
 * lossy redukci - UI dekodér delta rekonstruuje z baseline (= initial
 * state PIOZ80 + sumace REGISTER_WRITE eventů).
 *
 * @param payload  6 B pole z hwlog, nebo NULL (= 0).
 * @return Spakovaný 4 B uint32 (LE).
 */
static inline uint32_t hwlog_pack_payload_4b ( const uint8_t payload[ 6 ] )
{
    if ( !payload ) return 0u;
    return    (uint32_t) payload[ 0 ]
           | ( (uint32_t) payload[ 1 ] << 8 )
           | ( (uint32_t) payload[ 2 ] << 16 )
           | ( (uint32_t) payload[ 3 ] << 24 );
}


void hwlog_record ( en_HWLOG_CHIP chip, uint8_t sub, const uint8_t payload[ 6 ] )
{
    /* Cesta 1: existující tlog disk write (= žádná změna logiky). */
    if ( s_writer_open ) {
        uint64_t pxclk_total = tlog_common_get_pxclk_total ( );
        uint32_t screens = tlog_common_get_screens_total ( );
        uint32_t pxclk_in = tlog_common_get_pxclk_in_screen ( );

        uint8_t buf[ 24 ];
        for ( int i = 0; i < 8; i++ ) buf[ i ] = (uint8_t)( pxclk_total >> ( i * 8 ) );
        for ( int i = 0; i < 4; i++ ) buf[ 8 + i ] = (uint8_t)( screens >> ( i * 8 ) );
        for ( int i = 0; i < 4; i++ ) buf[ 12 + i ] = (uint8_t)( pxclk_in >> ( i * 8 ) );
        buf[ 16 ] = (uint8_t) chip;
        buf[ 17 ] = sub;
        if ( payload ) {
            memcpy ( buf + 18, payload, 6 );
        } else {
            memset ( buf + 18, 0, 6 );
        }
        tlog_writer_append ( &s_writer, buf, sizeof ( buf ) );
    }

    /* Cesta 2: paralelní fan-out do in-memory eventlog ringu.
     * Pattern: hot-path gate test + per-kategorie mask test v call-site
     * (= zero overhead navíc proti tlog gating, pokud eventlog OFF). */
    if ( TEST_TRACE_EVENTLOG_ACTIVE ) {
        uint8_t cat = hwlog_chip_to_eventlog_cat ( chip );
        if ( cat < (uint8_t) EVENTLOG_CAT_COUNT
             && ( g_eventlog_active_mask & ( 1ULL << cat ) ) ) {
            uint32_t pl = hwlog_pack_payload_4b ( payload );
            uint16_t pc = (uint16_t) g_mzarch_main.cpu->pc;
            eventlog_record ( cat, sub, pc, pl );
        }
    }
}


void hwlog_record_byte ( en_HWLOG_CHIP chip, uint8_t sub, uint8_t value )
{
    uint8_t payload[ 6 ] = { value, 0, 0, 0, 0, 0 };
    hwlog_record ( chip, sub, payload );
}


void hwlog_record_2byte ( en_HWLOG_CHIP chip, uint8_t sub, uint8_t a, uint8_t b )
{
    uint8_t payload[ 6 ] = { a, b, 0, 0, 0, 0 };
    hwlog_record ( chip, sub, payload );
}


/**
 * Sdílený counter pro decimaci HS/HBLN edges. Inkrementuje se na každé
 * volání hwlog_record_gdg_video_hs_edge(), emit jen když
 * g_hwlog_config.hs_decimation > 0 && (counter % hs_decimation) == 0.
 */
static unsigned s_hs_edge_counter = 0;


void hwlog_record_gdg_video_hs_edge ( uint8_t sub )
{
    /* Gate: jeden ze sinků musí být aktivní + decimation > 0. Při OR
     * gate test (hwlog ON nebo eventlog ON) se zachová původní chování
     * pro pure-hwlog provoz a navíc se respektuje fan-out při čistě
     * eventlog provozu. */
    if ( !s_writer_open && !TEST_TRACE_EVENTLOG_ACTIVE ) return;
    if ( g_hwlog_config.hs_decimation == 0 ) return;     /* default OFF */

    /* Counter inkrementujeme vždy (i pro suppressované) - jinak by 1. event
     * byl náhodný v závislosti na fázi spuštění. Emitujeme když
     * counter % decimation == 0 (= včetně prvního, counter=0). */
    if ( ( s_hs_edge_counter % g_hwlog_config.hs_decimation ) == 0 ) {
        hwlog_record_byte ( HWLOG_CHIP_GDG_VIDEO, sub, 0u );
    }
    s_hs_edge_counter++;
}


/* ===========================================================================
 *  Initial state binary snapshot - field-by-field LE serializace
 * ===========================================================================
 *
 * Soubor `<dir>/<name>_initial_state.bin` obsahuje sekvenci per-chip TLV
 * záznamů zachycujících stav HW chipů v okamžiku startu hwlog recordingu.
 * Externí parser podle těchto baseline + sumace všech REGISTER_WRITE
 * eventů rekonstruuje stav v libovolném okamžiku.
 *
 * Velké data buffery (RAM 64 KB, VRAM 32 KB, Memext RAM 512 KB) **NEJSOU
 * v hwlog initial state** - patří do cputrack initial dump (analogický
 * mechanismus, ale jiný subsystém), aby se neduplikovaly.
 *
 * TLV format:
 *   [0]    chip_id (1 B = en_HWLOG_CHIP)
 *   [1..4] payload_length (4 B uint32 LE)
 *   [5...] payload (chip-specific blob - **field-by-field LE encoded**,
 *          NEZÁVISLÉ na compiler ABI / struct padding)
 *
 * EOF marker: chip_id = 0xFF, length = 0.
 *
 * Specku per-chip layoutu viz docs/cz/debugger/formats/HW-log_INITIAL_STATE_format.md.
 */


/**
 * Pomocná growable byte buffer pro per-chip serializaci. Externí parser
 * potřebuje znát přesnou velikost payloadu (= TLV length pole), proto se
 * nejprve serializuje do paměti, pak zapíše s correct length.
 */
typedef struct st_HWLOG_BUF
{
    uint8_t *data;
    size_t   len;
    size_t   cap;
} st_HWLOG_BUF;


static void hwbuf_init ( st_HWLOG_BUF *b )
{
    b->cap = 256;
    b->len = 0;
    b->data = (uint8_t *) g_malloc ( b->cap );
}


static void hwbuf_free ( st_HWLOG_BUF *b )
{
    g_free ( b->data );
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}


static void hwbuf_reserve ( st_HWLOG_BUF *b, size_t need )
{
    if ( b->len + need <= b->cap ) return;
    size_t new_cap = b->cap;
    while ( b->len + need > new_cap ) new_cap *= 2;
    b->data = (uint8_t *) g_realloc ( b->data, new_cap );
    b->cap = new_cap;
}


static inline void hwbuf_put_u8 ( st_HWLOG_BUF *b, uint8_t v )
{
    hwbuf_reserve ( b, 1 );
    b->data[ b->len++ ] = v;
}


static inline void hwbuf_put_u16 ( st_HWLOG_BUF *b, uint16_t v )
{
    hwbuf_reserve ( b, 2 );
    b->data[ b->len++ ] = (uint8_t) ( v & 0xff );
    b->data[ b->len++ ] = (uint8_t) ( ( v >> 8 ) & 0xff );
}


static inline void hwbuf_put_u32 ( st_HWLOG_BUF *b, uint32_t v )
{
    hwbuf_reserve ( b, 4 );
    for ( int i = 0; i < 4; i++ ) b->data[ b->len++ ] = (uint8_t) ( ( v >> ( i * 8 ) ) & 0xff );
}


static inline void hwbuf_put_u64 ( st_HWLOG_BUF *b, uint64_t v )
{
    hwbuf_reserve ( b, 8 );
    for ( int i = 0; i < 8; i++ ) b->data[ b->len++ ] = (uint8_t) ( ( v >> ( i * 8 ) ) & 0xff );
}


static inline void hwbuf_put_bytes ( st_HWLOG_BUF *b, const void *src, size_t n )
{
    hwbuf_reserve ( b, n );
    memcpy ( b->data + b->len, src, n );
    b->len += n;
}


/**
 * Počítaný zápis na disk pro hwlog initial-state dumpy.
 *
 * Wrapper nad @c fwrite(), který po úspěšném zápisu připočte počet
 * reálně zapsaných bajtů do kumulativního disk čítače trace-suite
 * (@ref tlog_common_disk_bytes_add). Tím se hwlog initial-state dump
 * (jinak raw @c fwrite cesta bez accountingu) započítá do byte backstopu
 * (0019 vrstva 3), takže auto-pauza chrání disk i proti hwlog floodu.
 *
 * Stejný vzor jako @c cputrack.c write_bin_file (po úspěšném @c fwrite
 * přičti počet bajtů). Accountuje jen skutečně zapsané bajty - při short
 * write (chyba) se připočte jen úspěšně zapsaná část.
 *
 * @param data    Buffer k zápisu (nesmí být NULL, pokud @p n > 0).
 * @param n       Počet bajtů k zápisu.
 * @param fp      Otevřený výstupní stream (wb).
 * @return Počet úspěšně zapsaných bajtů (= návratová hodnota @c fwrite).
 *
 * Side effects: zápis na disk + inkrement disk čítače trace-suite.
 */
static size_t hwlog_fwrite_counted ( const void *data, size_t n, FILE *fp )
{
    size_t wrote = fwrite ( data, 1, n, fp );
    if ( wrote ) tlog_common_disk_bytes_add ( (uint64_t) wrote );
    return wrote;
}


/**
 * Zapíše chip TLV s payloadem z bufferu (data + len). Buffer není bez
 * volání hwbuf_free() uvolněn.
 *
 * Zápis jde přes @ref hwlog_fwrite_counted - hlavička i payload se
 * započítají do disk čítače trace-suite (byte backstop).
 */
static void write_chip_tlv_buf ( FILE *fp, uint8_t chip_id, const st_HWLOG_BUF *b )
{
    uint8_t hdr[ 5 ];
    hdr[ 0 ] = chip_id;
    uint32_t len = (uint32_t) b->len;
    for ( int i = 0; i < 4; i++ ) hdr[ 1 + i ] = (uint8_t)( len >> ( i * 8 ) );
    hwlog_fwrite_counted ( hdr, 5, fp );
    if ( len ) hwlog_fwrite_counted ( b->data, len, fp );
}


/**
 * Zapíše TLV s prázdným nebo NULL payloadem (typ. EOF marker).
 *
 * Zápis jde přes @ref hwlog_fwrite_counted (byte backstop accounting).
 */
static void write_chip_tlv_empty ( FILE *fp, uint8_t chip_id )
{
    uint8_t hdr[ 5 ] = { chip_id, 0, 0, 0, 0 };
    hwlog_fwrite_counted ( hdr, 5, fp );
}


/* -------------------------------------------------------------------------
 * Per-chip serializery (LE encoding)
 *
 * Každá funkce serializuje stav chipu do předaného bufferu. Pořadí polí
 * je závazné a zdokumentované v
 *   docs/cz/debugger/formats/HW-log_INITIAL_STATE_format.md.
 * Změna pořadí / přidání pole = bump verze formátu.
 * ------------------------------------------------------------------------- */

/**
 * Serializace PIO8255 (8255 PPI). Klávesnicová matice (raw 10 B) +
 * autotype state nepatří do hwlog initial state (jsou to UI / input
 * driver internals), ukládáme jen stavy portů + rozparsované signály.
 *
 * Layout (pole, velikost, pořadí):
 *  - signal_PA (u32)
 *  - signal_PA_keybord_column (u32)
 *  - signal_PA_joy1_enabled (u32)
 *  - signal_PA_joy2_enabled (u32)
 *  - signal_PC (u32)
 *  - signal_pc00 (u32)
 *  - signal_pc01 (u32)
 *  - signal_pc02 (u32)
 *  - signal_pc03 (u32)
 *  - signal_pc04 (u32)
 *  - keyboard_matrix[10] (10 B)
 */
static void hwlog_serialize_initial_pio8255 ( st_HWLOG_BUF *b )
{
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_PA );
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_PA_keybord_column );
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_PA_joy1_enabled );
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_PA_joy2_enabled );
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_PC );
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_pc00 );
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_pc01 );
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_pc02 );
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_pc03 );
    hwbuf_put_u32 ( b, (uint32_t) g_pio8255.signal_pc04 );
    hwbuf_put_bytes ( b, g_pio8255.keyboard_matrix, sizeof ( g_pio8255.keyboard_matrix ) );
}


/**
 * Serializace 3× CTC8253 counter. Per counter v pořadí 0,1,2:
 *  - mode (u8), bcd (u8), rlf (u8), state (u8)
 *  - out (u8), gate (u8)
 *  - load_done (u8), rl_byte (u8)
 *  - latch_op (u8), padding 1 B
 *  - read_latch (u32), preset_value (u32), preset_latch (u32), value (u32)
 *  - mode3_destination_value (u32), mode3_half_value (u32)
 *
 * Per-counter velikost = 34 B, celkem 102 B.
 */
static void hwlog_serialize_initial_ctc8253 ( st_HWLOG_BUF *b )
{
    for ( int i = 0; i < 3; i++ ) {
        const st_CTC8253 *c = &g_ctc8253[ i ];
        hwbuf_put_u8 ( b, (uint8_t) c->mode );
        hwbuf_put_u8 ( b, (uint8_t) c->bcd );
        hwbuf_put_u8 ( b, (uint8_t) c->rlf );
        hwbuf_put_u8 ( b, (uint8_t) c->state );
        hwbuf_put_u8 ( b, (uint8_t) c->out );
        hwbuf_put_u8 ( b, (uint8_t) c->gate );
        hwbuf_put_u8 ( b, (uint8_t) c->load_done );
        hwbuf_put_u8 ( b, (uint8_t) c->rl_byte );
        hwbuf_put_u8 ( b, (uint8_t) c->latch_op );
        hwbuf_put_u8 ( b, 0 ); /* padding */
        hwbuf_put_u32 ( b, (uint32_t) c->read_latch );
        hwbuf_put_u32 ( b, (uint32_t) c->preset_value );
        hwbuf_put_u32 ( b, (uint32_t) c->preset_latch );
        hwbuf_put_u32 ( b, (uint32_t) c->value );
        hwbuf_put_u32 ( b, (uint32_t) c->mode3_destination_value );
        hwbuf_put_u32 ( b, (uint32_t) c->mode3_half_value );
    }
}


/**
 * Serializace PSG modulu (= 1× nebo 2× SN76489).
 *  - stereo (u8)
 *  - psg_count (u8) = 2 vždy (g_psg_module.psg má 2 sloty, druhý je pseudo
 *    pokud mono - ukládáme oba pro layout konzistenci)
 *  - per psg in [0..1]:
 *     - latch_cs (u32), latch_attn (u32)
 *     - per channel in [0..3] (PSG_CHANNELS_COUNT):
 *        - type (u8), attn (u8) [= en_ATTENUATOR enum, 0..15]
 *        - timer (u32)
 *        - tone.divider (u32), tone.latch_divider (u32)
 *        - noise.div_type (u8), noise.type (u8), noise.last_noise_type (u8)
 *        - noise.shiftregister (u16)
 *        - output_signal (u32)
 *
 * Pro HAVE_PSG=0 (MZ-700) se PSG TLV blok do hwlogu vubec nezapisuje.
 */
static void hwlog_serialize_initial_psg ( st_HWLOG_BUF *b )
{
    hwbuf_put_u8 ( b, (uint8_t) ( g_psg_module.stereo ? 1 : 0 ) );
    hwbuf_put_u8 ( b, (uint8_t) PSG_MAX_COUNT );
    for ( int p = 0; p < PSG_MAX_COUNT; p++ ) {
        const st_PSG *psg = &g_psg_module.psg[ p ];
        hwbuf_put_u32 ( b, (uint32_t) psg->latch_cs );
        hwbuf_put_u32 ( b, (uint32_t) psg->latch_attn );
        for ( int ch = 0; ch < PSG_CHANNELS_COUNT; ch++ ) {
            const st_PSG_CHANNEL *c = &psg->channel[ ch ];
            hwbuf_put_u8 ( b, (uint8_t) c->type );
            hwbuf_put_u8 ( b, (uint8_t) c->attn );
            hwbuf_put_u32 ( b, (uint32_t) c->timer );
            hwbuf_put_u32 ( b, (uint32_t) c->tone.divider );
            hwbuf_put_u32 ( b, (uint32_t) c->tone.latch_divider );
            hwbuf_put_u8 ( b, (uint8_t) c->noise.div_type );
            hwbuf_put_u8 ( b, (uint8_t) c->noise.type );
            hwbuf_put_u8 ( b, (uint8_t) c->noise.last_noise_type );
            hwbuf_put_u16 ( b, c->noise.shiftregister );
            hwbuf_put_u32 ( b, (uint32_t) c->output_signal );
        }
    }
}


/**
 * Serializace MEMEXT - jen mapping table + connection/type info, bez
 * RAM/FLASH/WOM bufferů (patří do cputrack).
 *
 *  - connection (u8), type (u8)
 *  - addr_mask (u16)
 *  - map[16] (16× u32 = 64 B)
 */
static void hwlog_serialize_initial_memext ( st_HWLOG_BUF *b )
{
    hwbuf_put_u8 ( b, (uint8_t) g_memext.connection );
    hwbuf_put_u8 ( b, (uint8_t) g_memext.type );
    hwbuf_put_u16 ( b, g_memext.addr_mask );
    for ( int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++ ) {
        hwbuf_put_u32 ( b, g_memext.map[ i ] );
    }
}


/**
 * Serializace GDG (= cluster celého video subsystému). Ukládáme jen
 * "logická" pole relevantní pro re-simulaci (registry, raster pozice,
 * status flagy). Vynecháváme:
 *   - st_EMUEVENT event (= dynamický scheduling)
 *   - screen_need_update_from / last_updated_border_pixel (= rendering hint)
 *   - tempo_divider (= konstanta architektury)
 *
 * Společná pole (oba MZ-800 i MZ-1500):
 *  - total_elapsed.screens (u32), total_elapsed.ticks (u32)
 *  - beam_row (u32)
 *  - sts_vsync (u8), sts_hsync (u8), hbln (u8), vbln (u8)
 *  - regDMD (u8), regBOR (u8)
 *  - regct53g7 (u8)
 *  - tempo (u32)
 *  - arch_marker (u8): 800 = 0x80, 1500 = 0x15
 *
 * MZ-800 specifická pole (po společných, jen pokud arch_marker = 0x80):
 *  - regPALGRP (u8), regPAL0..3 (4× u8)
 *
 * MZ-1500 specifická pole (po společných, jen pokud arch_marker = 0x15):
 *  - mode_color[8] (8× i32 = 32 B; LE encoding přes u32)
 */
static void hwlog_serialize_initial_gdg ( st_HWLOG_BUF *b )
{
    hwbuf_put_u32 ( b, (uint32_t) g_gdg.total_elapsed.screens );
    hwbuf_put_u32 ( b, (uint32_t) g_gdg.total_elapsed.ticks );
    hwbuf_put_u32 ( b, (uint32_t) g_gdg.beam_row );
    hwbuf_put_u8 ( b, (uint8_t) g_gdg.sts_vsync );
    hwbuf_put_u8 ( b, (uint8_t) g_gdg.sts_hsync );
    hwbuf_put_u8 ( b, (uint8_t) g_gdg.hbln );
    hwbuf_put_u8 ( b, (uint8_t) g_gdg.vbln );
    hwbuf_put_u8 ( b, (uint8_t) g_gdg.regDMD );
    hwbuf_put_u8 ( b, (uint8_t) g_gdg.regBOR );
    hwbuf_put_u8 ( b, (uint8_t) g_gdg.regct53g7 );
    hwbuf_put_u32 ( b, (uint32_t) g_gdg.tempo );
    /* Runtime dle g_mzhal.arch (mzhal 11f) - markery a TLV obsah per
     * arch jsou zmrazeny trace on-disk kontrakt. */
    if ( g_mzhal.arch == 800 ) {
        hwbuf_put_u8 ( b, 0x80 );
        hwbuf_put_u8 ( b, (uint8_t) g_gdg.regPALGRP );
        hwbuf_put_u8 ( b, (uint8_t) g_gdg.regPAL0 );
        hwbuf_put_u8 ( b, (uint8_t) g_gdg.regPAL1 );
        hwbuf_put_u8 ( b, (uint8_t) g_gdg.regPAL2 );
        hwbuf_put_u8 ( b, (uint8_t) g_gdg.regPAL3 );
    } else if ( g_mzhal.arch == 1500 ) {
        hwbuf_put_u8 ( b, 0x15 );
        for ( int i = 0; i < 8; i++ ) {
            hwbuf_put_u32 ( b, (uint32_t) g_gdg.mode_color[ i ] );
        }
    } else {
        /* MZ-700: TODO mzarch - tracelog GDG state dump zatím není
         * podporován. Marker 0x07, žádné state body. */
        hwbuf_put_u8 ( b, 0x07 );
    }
}


/**
 * Serializace Z80 PIO. Per port (A, B):
 *  - mode (u8), io_mask (u8)
 *  - iclvl (u8), icmask (u8), icena (u8), icfnc (u8)
 *  - interrupt_vector (u8)
 *  - port_int (u8), masked_input (u8)
 *  - data_output (u8) [= "output" v jazyku zadání]
 *  - ctrl_expect (u8)
 *  - last_intfnc_result (u8)
 *  - bus_input (u8) [= raw vstupní piny per port - dopočítáno funkcí
 *                     pioz80_port_get_raw_input(); pro PA závisí na
 *                     CTC0/VBLN dynamicky]
 *
 * Globální:
 *  - interrupt (u8) [= g_pioz80.interrupt]
 *  - interrupt_port_id (i8) [= g_pioz80.interrupt_port_id, -1 = NONE]
 *
 * Pozn.: V mz800new st_PIOZ80_PORT NEMÁ samostatné pole `bus_input` ani
 * `input_latch` - raw stav pinů se počítá on-demand. Logujeme tedy
 * současný snímek raw vstupu, který z PA závisí na CTC0 OUT0 + VBLN.
 * `input_latch` (Mode 1 handshake latch) v emu neexistuje (Mode 1 se
 * v MZ-800 nepoužívá), pole se neuvádí.
 */
static void hwlog_serialize_initial_pioz80 ( st_HWLOG_BUF *b )
{
    /* Per-port pole. Pořadí PA (=0), PB (=1). */
    for ( int p = 0; p < PIOZ80_PORT_COUNT; p++ ) {
        const st_PIOZ80_PORT *port = &g_pioz80.port[ p ];
        hwbuf_put_u8 ( b, (uint8_t) port->mode );
        hwbuf_put_u8 ( b, port->io_mask );
        hwbuf_put_u8 ( b, (uint8_t) port->iclvl );
        hwbuf_put_u8 ( b, port->icmask );
        hwbuf_put_u8 ( b, (uint8_t) port->icena );
        hwbuf_put_u8 ( b, (uint8_t) port->icfnc );
        hwbuf_put_u8 ( b, port->interrupt_vector );
        hwbuf_put_u8 ( b, (uint8_t) port->port_int );
        hwbuf_put_u8 ( b, port->masked_input );
        hwbuf_put_u8 ( b, port->data_output );
        hwbuf_put_u8 ( b, (uint8_t) port->ctrl_expect );
        hwbuf_put_u8 ( b, (uint8_t) port->last_intfnc_result );
        /* bus_input - raw piny per port. Stejná logika jako
         * pioz80_port_get_raw_input() v pioz80.c (= inline static, nelze
         * přímo zavolat z venku). */
        uint8_t raw;
        if ( p == PIOZ80_PORT_A ) {
            raw = 0x03;
            /* PA4 = invertovaný CTC0 OUT0 */
            raw |= ( CTC8253_OUT ( 0 ) == 1 ) ? 0u : ( 1u << 4 );
            /* PA5 = VBLN */
            raw |= SIGNAL_GDG_VBLNK ? ( 1u << 5 ) : 0u;
        } else {
            raw = 0xff;
        }
        hwbuf_put_u8 ( b, raw );
    }
    /* Globální pole. */
    hwbuf_put_u8 ( b, (uint8_t) g_pioz80.interrupt );
    hwbuf_put_u8 ( b, (uint8_t) g_pioz80.interrupt_port_id );
}


static int dump_initial_state ( const char *resolved_dir, const char *name )
{
    char *fname = g_strdup_printf ( "%s_initial_state.bin", name );
    char *path = g_build_filename ( resolved_dir, fname, NULL );
    FILE *fp = g_fopen ( path, "wb" );
    g_free ( fname );
    if ( !fp ) {
        fprintf ( stderr, "[trace-suite] hwlog: cannot open %s for writing initial state\n", path );
        g_free ( path );
        return -1;
    }

    st_HWLOG_BUF buf;

    hwbuf_init ( &buf );
    hwlog_serialize_initial_pio8255 ( &buf );
    write_chip_tlv_buf ( fp, HWLOG_CHIP_PIO8255, &buf );
    hwbuf_free ( &buf );

    hwbuf_init ( &buf );
    hwlog_serialize_initial_ctc8253 ( &buf );
    write_chip_tlv_buf ( fp, HWLOG_CHIP_CTC8253, &buf );
    hwbuf_free ( &buf );

    if (g_mzhal.psg_count >= 1) { /* TLV blok jen na platformach s PSG (mzhal krok 8) */
    hwbuf_init ( &buf );
    hwlog_serialize_initial_psg ( &buf );
    write_chip_tlv_buf ( fp, HWLOG_CHIP_PSG, &buf );
    hwbuf_free ( &buf );
    }

    if ( MEMEXT_TEST_CONNECTED ) {
        hwbuf_init ( &buf );
        hwlog_serialize_initial_memext ( &buf );
        write_chip_tlv_buf ( fp, HWLOG_CHIP_MEMEXT, &buf );
        hwbuf_free ( &buf );
    }

    hwbuf_init ( &buf );
    hwlog_serialize_initial_gdg ( &buf );
    write_chip_tlv_buf ( fp, HWLOG_CHIP_GDG_MODE, &buf );
    hwbuf_free ( &buf );

    if (g_mzhal.have_pioz80) { /* TLV blok jen na platformach s PIO-Z80 (mzhal krok 8) */
    hwbuf_init ( &buf );
    hwlog_serialize_initial_pioz80 ( &buf );
    write_chip_tlv_buf ( fp, HWLOG_CHIP_PIOZ80, &buf );
    hwbuf_free ( &buf );
    }

    /* EOF marker. */
    write_chip_tlv_empty ( fp, 0xFF );

    fclose ( fp );
    g_free ( path );
    return 0;
}


/* ===========================================================================
 *  Lifecycle
 * =========================================================================== */

/**
 * @brief Sestaví JSON fragment subsys_header pro hwlog.
 *
 * Volaný z hwlog_start() po dump_initial_state() a předaný do
 * tlog_writer_update_meta() jako pre-formatted JSON string. tlog_common
 * fragment parsuje (JsonParser) a embedu jako JsonNode pod klíč
 * "subsys_header" v meta.json.
 *
 * @return Heap-alokovaný string s JSON object {...}; caller uvolní
 *         přes g_free().
 */
static char *build_subsys_header_json ( void )
{
    JsonBuilder *b = json_builder_new ( );
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "start_pxclk" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_total ( ) );

    json_builder_set_member_name ( b, "start_pxclk_in_screen" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_in_screen ( ) );

    json_builder_set_member_name ( b, "start_screens" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_screens_total ( ) );

    json_builder_set_member_name ( b, "start_cpuclk" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_cpuclk_total ( ) );

    json_builder_set_member_name ( b, "event_record_size" );
    json_builder_add_int_value ( b, 24 );

    json_builder_set_member_name ( b, "v1_chip_coverage" );
    json_builder_begin_array ( b );
    static const char * const v1_chips[] = {
        "GDG_MODE", "GDG_BANKING", "GDG_HWSCROLL", "GDG_COLORS",
        "GDG_WFRF", "GDG_VIDEO", "PSG", "MEMEXT",
        "CTC8253", "PIO8255", "PIOZ80", "QD", "FDC", "RD"
    };
    for ( size_t i = 0; i < G_N_ELEMENTS ( v1_chips ); i++ ) {
        json_builder_add_string_value ( b, v1_chips[i] );
    }
    json_builder_end_array ( b );

    json_builder_set_member_name ( b, "v1_chip_pending" );
    json_builder_begin_array ( b );
    json_builder_end_array ( b );

    json_builder_set_member_name ( b, "chip_dividers" );
    json_builder_begin_object ( b );
    json_builder_set_member_name ( b, "psgclk_divider_cpuclk" );
    /* Runtime z g_mzhal (mzhal 10b, cold - manifest 1x per trace session). */
    json_builder_add_int_value ( b, (gint64) ( g_mzhal.psg_divider / g_mzhal.gdgclk2cpu_divider ) );
    json_builder_set_member_name ( b, "tempo_divider_screen_rows" );
    json_builder_add_int_value ( b, 229 );
    json_builder_set_member_name ( b, "cpu_divider_pxclk" );
    json_builder_add_int_value ( b, (gint64) g_mzhal.gdgclk2cpu_divider );
    json_builder_set_member_name ( b, "ctc0_clk1m1_divider_pxclk" );
    json_builder_add_int_value ( b, (gint64) g_mzhal.gdgclk_ctc0_divider );
    json_builder_set_member_name ( b, "pio8255_cursor_divider_screens" );
    json_builder_add_int_value ( b, 25 );
    json_builder_end_object ( b );

    char initial_state_file[256];
    g_snprintf ( initial_state_file, sizeof ( initial_state_file ),
                 "%s_initial_state.bin",
                 g_hwlog_config.name ? g_hwlog_config.name : "hwlog" );
    json_builder_set_member_name ( b, "initial_state_file" );
    json_builder_add_string_value ( b, initial_state_file );

    json_builder_set_member_name ( b, "initial_state_format" );
    json_builder_add_string_value ( b,
        "tlv: chip_id(1) length(4 LE) payload(N); "
        "EOF marker chip_id=0xFF length=0" );

    json_builder_end_object ( b );

    JsonGenerator *gen = json_generator_new ( );
    JsonNode *root = json_builder_get_root ( b );
    json_generator_set_root ( gen, root );
    json_generator_set_pretty ( gen, TRUE );
    json_generator_set_indent ( gen, 2 );

    gsize len = 0;
    gchar *out = json_generator_to_data ( gen, &len );

    g_object_unref ( gen );
    g_object_unref ( b );

    return out;  /* caller uvolní přes g_free */
}


int hwlog_start ( void )
{
    if ( s_writer_open ) return 0;
    if ( !g_hwlog_config.dir || !g_hwlog_config.dir[0]
         || !g_hwlog_config.name || !g_hwlog_config.name[0] ) {
        return -1;
    }
    char *resolved_dir = sdlapp_paths_resolve_work ( g_sdlapp->paths, g_hwlog_config.dir );
    uint64_t init_px = tlog_common_get_pxclk_total ( );
    uint64_t init_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t init_sc = tlog_common_get_screens_total ( );
    if ( tlog_writer_open ( &s_writer, "hwlog", resolved_dir, g_hwlog_config.name,
                            g_hwlog_config.chunk_mb, g_hwlog_config.max_total_mb,
                            init_px, init_cpu, init_sc ) != 0 ) {
        g_free ( resolved_dir );
        return -1;
    }
    s_writer_open = 1;
    s_hs_edge_counter = 0;

    /* Initial state binary snapshot - per-chip TLV dump pro externí
     * re-simulaci. */
    dump_initial_state ( resolved_dir, g_hwlog_config.name );

    char *hdr = build_subsys_header_json ( );
    tlog_writer_update_meta ( &s_writer, hdr );
    g_free ( hdr );
    g_free ( resolved_dir );
    fprintf ( stderr, "[trace-suite] hwlog: recording started\n" );
    return 0;
}


void hwlog_stop ( void )
{
    if ( !s_writer_open ) return;
    uint64_t now_px = tlog_common_get_pxclk_total ( );
    uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t now_sc = tlog_common_get_screens_total ( );
    tlog_writer_close ( &s_writer, now_px, now_cpu, now_sc );
    s_writer_open = 0;
    fprintf ( stderr, "[trace-suite] hwlog: recording stopped\n" );
}


void hwlog_finalize ( void )
{
    if ( g_hwlog_config.save_on_exit && s_writer_open ) hwlog_stop ( );
    g_free ( g_hwlog_config.dir );
    g_free ( g_hwlog_config.name );
    g_hwlog_config.dir = NULL;
    g_hwlog_config.name = NULL;
}


int hwlog_is_truncated ( void )
{
    /* Pokud writer není otevřený, recording neexistuje, tedy ani
     * "truncated" stav nedává smysl. */
    if ( !s_writer_open ) return 0;
    return tlog_writer_is_truncated ( &s_writer );
}


int hwlog_save_segment ( const char *path )
{
    /* Uzavřít aktuální segment + volitelně přesměrovat na path. Vzor sdílený
     * s cputrack_save_segment - viz tam pro detaily semantiky. */
    int was_active = ( g_hwlog_active != 0 );

    /* Prázdná cesta = "force save now": flush rozdělaného segmentu BEZ
     * uzavření/reopenu (segment pokračuje do téhož manifestu). Viz
     * cputrack_save_segment + mzdos 0022 pro detaily. */
    if ( !( path && path[ 0 ] ) ) {
        if ( s_writer_open ) {
            uint64_t now_px = tlog_common_get_pxclk_total ( );
            uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
            uint32_t now_sc = tlog_common_get_screens_total ( );
            tlog_writer_flush_chunk ( &s_writer, now_px, now_cpu, now_sc );
        }
        return 0;
    }

    if ( s_writer_open ) {
        hwlog_stop ( );
    }
    reclife_redirect_path ( &g_hwlog_config.dir, &g_hwlog_config.name, path );
    if ( was_active ) {
        return hwlog_start ( );
    }
    return 0;
}


/* Sdílený lifecycle deskriptor (reclife) - viz cputrack.c pro detaily vzoru. */
static const st_RECLIFE_DESC s_hwlog_reclife = {
    .subsys_name = "hwlog",
    .mode_ptr    = (int *) &g_hwlog_config.mode,
    .active_ptr  = &g_hwlog_active,
    .fn_start    = hwlog_start,
    .fn_stop     = hwlog_stop,
    .fn_reset    = NULL,
    .fn_save     = hwlog_save_segment,
};


void hwlog_recompute_active ( int debugger_active )
{
    reclife_recompute_active ( &s_hwlog_reclife, debugger_active );
}


void hwlog_init ( void )
{
    memset ( &g_hwlog_config, 0, sizeof ( g_hwlog_config ) );
    g_hwlog_config.mode = TLOG_MODE_OFF;
    g_hwlog_config.chunk_mb = TLOG_DEFAULT_CHUNK_MB;
    g_hwlog_config.max_total_mb = TLOG_DEFAULT_MAX_TOTAL_MB;
    g_hwlog_config.save_on_exit = 1;
    g_hwlog_config.hs_decimation = 0;     /* default: HS/HBLN edges OFF */

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "TRACE_HWLOG" );
    CFGELM *elm;
    elm = cfgmodule_register_new_element ( cmod, "mode", CFGENTYPE_KEYWORD, TLOG_MODE_OFF,
                                           TLOG_MODE_OFF,         "OFF",
                                           TLOG_MODE_WITH_WINDOW, "WITH_WINDOW",
                                           TLOG_MODE_ALWAYS,      "ALWAYS",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_hwlog_config.mode, (void*) &g_hwlog_config.mode );
    elm = cfgmodule_register_new_element ( cmod, "dir", CFGENTYPE_TEXT, "trace-suite" );
    cfgelement_bind ( elm, (void*) &g_hwlog_config.dir );
    elm = cfgmodule_register_new_element ( cmod, "name", CFGENTYPE_TEXT, "hwlog" );
    cfgelement_bind ( elm, (void*) &g_hwlog_config.name );
    elm = cfgmodule_register_new_element ( cmod, "chunk_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_CHUNK_MB, 1, 4096 );
    cfgelement_set_handlers ( elm, (void*) &g_hwlog_config.chunk_mb, (void*) &g_hwlog_config.chunk_mb );
    elm = cfgmodule_register_new_element ( cmod, "max_total_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_MAX_TOTAL_MB, 0, 1048576 );
    cfgelement_set_handlers ( elm, (void*) &g_hwlog_config.max_total_mb, (void*) &g_hwlog_config.max_total_mb );
    elm = cfgmodule_register_new_element ( cmod, "save_on_exit", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_hwlog_config.save_on_exit, (void*) &g_hwlog_config.save_on_exit );
    elm = cfgmodule_register_new_element ( cmod, "hs_decimation", CFGENTYPE_UNSIGNED, 0, 0, 1000000 );
    cfgelement_set_handlers ( elm, (void*) &g_hwlog_config.hs_decimation, (void*) &g_hwlog_config.hs_decimation );

    /* Propagace .ini / default hodnot do g_hwlog_config (jinak by dir/name
     * zůstaly NULL). Viz analogický komentář v cputrack_init(). */
    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


void hwlog_apply_cli_options ( void )
{
    const char *v;
    v = sdlapp_option_value ( "--hwlog-mode" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_hwlog_config.mode = TLOG_MODE_OFF;
        else if ( g_ascii_strcasecmp ( v, "window" ) == 0 ) g_hwlog_config.mode = TLOG_MODE_WITH_WINDOW;
        else if ( g_ascii_strcasecmp ( v, "always" ) == 0 ) g_hwlog_config.mode = TLOG_MODE_ALWAYS;
    }
    v = sdlapp_option_value ( "--hwlog-dir" );
    if ( v ) { g_free ( g_hwlog_config.dir ); g_hwlog_config.dir = g_strdup ( v ); }
    v = sdlapp_option_value ( "--hwlog-name" );
    if ( v ) { g_free ( g_hwlog_config.name ); g_hwlog_config.name = g_strdup ( v ); }
    v = sdlapp_option_value ( "--hwlog-chunk-mb" );
    if ( v ) g_hwlog_config.chunk_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--hwlog-max-total-mb" );
    if ( v ) g_hwlog_config.max_total_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--hwlog-save-on-exit" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "on" ) == 0 ) g_hwlog_config.save_on_exit = 1;
        else if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_hwlog_config.save_on_exit = 0;
    }
    v = sdlapp_option_value ( "--hwlog-hs-decimation" );
    if ( v ) g_hwlog_config.hs_decimation = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
