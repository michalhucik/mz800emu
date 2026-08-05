/**
 * @file membrowser_io.cpp
 * @brief Emu backend wrapper kolem dbgapi_regions_* - implementace.
 *
 * Jediné místo v Memory Browser kódu kde se volá dbgapi_regions_*.
 * Core hex view chodí pres st_HEX_VIEW_BACKEND vtable.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_io.h"

#include <cstring>
#include <cstdint>

extern "C" {
#include "emulator/debugger/dbgapi_regions.h"
}

/* Static buffer pro region snapshot - vlastní per session, refresh přepíše. */
static st_REGION_DESC s_regions_buf[MEMBROWSER_MAX_REGIONS];
static int s_regions_count = 0;


/* ===========================================================================
 *  Page cache (V0-leftovers F5)
 * ---------------------------------------------------------------------------
 *  16 KB chunks, LRU eviction, klíč (region_id, page_offset).
 *  Cíl: eliminovat per-row dbgapi_regions_read call lag při scrollování
 *  Ramdisk 16 MB (4 GB volání během dlouhého scrollu).
 *
 *  Capacity: 16 stránek × 16 KB = 256 KB residentní paměť. Pokrývá 8×
 *  visible viewport (typicky <2 KB visible) + scroll lookahead. Pro
 *  Memext (32 × 4 KB = 128 KB) a Ramdisk STD (256 × 64 KB = 16 MB)
 *  random access hraje cache do karet.
 *
 *  LRU: monotonní access counter, eviction = lowest counter.
 *  Invalidate granularity: full flush (jednoduché, dost rychlé pro velikost).
 * ========================================================================= */

#define MB_CACHE_PAGE_SIZE   (16 * 1024)   /**< 16 KB - chunky alignment. */
#define MB_CACHE_PAGES       16             /**< LRU capacity (256 KB total). */

typedef struct {
    int region_id;          /**< -1 = slot prázdný. */
    uint64_t page_offset;   /**< Začátek stránky v rámci regionu (align 16 KB). */
    uint64_t lru_tick;      /**< Monotonní counter pro LRU (0 = unused). */
    int valid_bytes;        /**< Kolik bajtů v stránce je platných (poslední stránka < page_size). */
    uint8_t data[MB_CACHE_PAGE_SIZE];
} st_MB_CACHE_PAGE;

static st_MB_CACHE_PAGE s_cache[MB_CACHE_PAGES];
static uint64_t s_cache_tick = 0;     /**< Globální monotonní tick. */
static uint64_t s_cache_hits = 0;
static uint64_t s_cache_misses = 0;
static bool s_cache_initialized = false;


/* Inicializace cache - vynuluje slot region_id na -1 (= prázdný). */
static void cache_init_if_needed ( void )
{
    if ( s_cache_initialized ) return;
    for ( int i = 0; i < MB_CACHE_PAGES; i++ ) {
        s_cache[i].region_id = -1;
        s_cache[i].page_offset = 0;
        s_cache[i].lru_tick = 0;
        s_cache[i].valid_bytes = 0;
    }
    s_cache_initialized = true;
}


/* Najde slot s daným (region_id, page_offset). Vrací index nebo -1. */
static int cache_find ( int region_id, uint64_t page_offset )
{
    for ( int i = 0; i < MB_CACHE_PAGES; i++ ) {
        if ( s_cache[i].region_id == region_id
                && s_cache[i].page_offset == page_offset ) {
            return i;
        }
    }
    return -1;
}


/* LRU eviction - vrátí index slotu s nejmenším lru_tick (= nejstarší).
 * Prázdný slot (region_id=-1) má lru_tick=0 a vyhraje hned. */
static int cache_pick_victim ( void )
{
    int victim = 0;
    uint64_t min_tick = s_cache[0].lru_tick;
    for ( int i = 1; i < MB_CACHE_PAGES; i++ ) {
        if ( s_cache[i].lru_tick < min_tick ) {
            min_tick = s_cache[i].lru_tick;
            victim = i;
        }
    }
    return victim;
}


/* Naplní cache slot daty z dbgapi_regions_read. */
static void cache_fill_page ( int slot, int region_id, uint64_t page_offset,
                              uint64_t region_size )
{
    s_cache[slot].region_id = region_id;
    s_cache[slot].page_offset = page_offset;
    s_cache[slot].lru_tick = ++s_cache_tick;

    uint64_t end = page_offset + MB_CACHE_PAGE_SIZE;
    if ( end > region_size ) end = region_size;
    uint32_t to_read = ( uint32_t ) ( end - page_offset );
    if ( to_read > MB_CACHE_PAGE_SIZE ) to_read = MB_CACHE_PAGE_SIZE;

    int got = dbgapi_regions_read ( region_id, ( uint32_t ) page_offset,
                                     s_cache[slot].data, to_read );
    if ( got < 0 ) got = 0;
    s_cache[slot].valid_bytes = got;
    /* Pokud read vrátil méně, doplníme 0 do zbytku stránky (vrátí se
     * jako 0x00 čtenářům - konzistentní s emu_backend_read fallback). */
    if ( got < ( int ) to_read ) {
        std::memset ( s_cache[slot].data + got, 0, to_read - got );
        s_cache[slot].valid_bytes = to_read;
    }
}


/* Read přes cache: split request na stránky, fetch missing, vrátit data.
 * Nahrazuje dbgapi_regions_read v emu_backend_read pro region size > 64 KB
 * (malé regiony nepotřebují cache, jednoduchý read je rychlejší). */
static int cache_read ( int region_id, uint64_t offset, uint8_t *out,
                        uint32_t len, uint64_t region_size )
{
    cache_init_if_needed ( );
    if ( offset >= region_size ) return 0;
    if ( offset + len > region_size ) len = ( uint32_t ) ( region_size - offset );

    uint32_t copied = 0;
    while ( copied < len ) {
        uint64_t pos = offset + copied;
        uint64_t page_off = pos & ~( uint64_t ) ( MB_CACHE_PAGE_SIZE - 1 );
        uint32_t page_pos = ( uint32_t ) ( pos - page_off );
        uint32_t chunk = MB_CACHE_PAGE_SIZE - page_pos;
        if ( chunk > len - copied ) chunk = len - copied;

        int slot = cache_find ( region_id, page_off );
        if ( slot < 0 ) {
            s_cache_misses++;
            slot = cache_pick_victim ( );
            cache_fill_page ( slot, region_id, page_off, region_size );
        } else {
            s_cache_hits++;
            s_cache[slot].lru_tick = ++s_cache_tick;
        }

        if ( page_pos + chunk > ( uint32_t ) s_cache[slot].valid_bytes ) {
            /* Chunk přesahuje valid_bytes (= za koncem regionu) - zkrať.
             * Nemělo by nastat po clamp na region_size výše. */
            chunk = ( uint32_t ) s_cache[slot].valid_bytes - page_pos;
            if ( chunk == 0 ) break;
        }
        std::memcpy ( out + copied, s_cache[slot].data + page_pos, chunk );
        copied += chunk;
    }
    return ( int ) copied;
}


/* Invalidate konkrétní oblast (po write_bytes) - smaže ovlivněné stránky. */
static void cache_invalidate_range ( int region_id, uint64_t offset, uint32_t len )
{
    cache_init_if_needed ( );
    uint64_t end = offset + len;
    for ( int i = 0; i < MB_CACHE_PAGES; i++ ) {
        if ( s_cache[i].region_id != region_id ) continue;
        uint64_t p_start = s_cache[i].page_offset;
        uint64_t p_end = p_start + MB_CACHE_PAGE_SIZE;
        if ( offset < p_end && end > p_start ) {
            /* Overlap - invalidate slot. */
            s_cache[i].region_id = -1;
            s_cache[i].lru_tick = 0;
            s_cache[i].valid_bytes = 0;
        }
    }
}


extern "C" void membrowser_io_cache_invalidate_all ( void )
{
    cache_init_if_needed ( );
    for ( int i = 0; i < MB_CACHE_PAGES; i++ ) {
        s_cache[i].region_id = -1;
        s_cache[i].lru_tick = 0;
        s_cache[i].valid_bytes = 0;
    }
    s_cache_hits = 0;
    s_cache_misses = 0;
}


extern "C" void membrowser_io_cache_stats ( int *out_pages_used, int *out_pages_max,
                                            uint64_t *out_hits, uint64_t *out_misses )
{
    if ( out_pages_max ) *out_pages_max = MB_CACHE_PAGES;
    if ( out_pages_used ) {
        int used = 0;
        for ( int i = 0; i < MB_CACHE_PAGES; i++ ) {
            if ( s_cache[i].region_id >= 0 ) used++;
        }
        *out_pages_used = used;
    }
    if ( out_hits ) *out_hits = s_cache_hits;
    if ( out_misses ) *out_misses = s_cache_misses;
}


extern "C" int membrowser_io_refresh_regions ( st_MEMBROWSER_REGION_SNAPSHOT *out )
{
    if ( !out ) return 0;

    /* Snapshot fingerprint - levný hash (count + size + connected flagy)
     * detekuje strukturální změnu mapování regionů (= banking změnilo
     * region_id přiřazení). Při změně flush cache. Per-frame stejný
     * snapshot → no-op, cache zůstává. */
    static uint32_t s_last_fingerprint = 0;

    int n = dbgapi_regions_enumerate ( s_regions_buf, MEMBROWSER_MAX_REGIONS );
    if ( n < 0 ) n = 0;
    s_regions_count = n;

    uint32_t fp = ( uint32_t ) n;
    for ( int i = 0; i < n; i++ ) {
        fp = fp * 31u + ( uint32_t ) s_regions_buf[i].size;
        fp = fp * 31u + ( s_regions_buf[i].connected ? 1u : 0u );
        fp = fp * 31u + ( uint32_t ) s_regions_buf[i].kind;
        fp = fp * 31u + ( uint32_t ) s_regions_buf[i].sub_id;
    }
    if ( fp != s_last_fingerprint ) {
        membrowser_io_cache_invalidate_all ( );
        s_last_fingerprint = fp;
    }

    out->count = n;
    out->items = s_regions_buf;
    return n;
}


extern "C" int membrowser_io_find_region ( const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                                           int kind, int sub_id )
{
    if ( !snap || !snap->items ) return -1;

    for ( int i = 0; i < snap->count; i++ ) {
        const st_REGION_DESC *r = &snap->items[i];
        if ( ( int ) r->kind == kind && r->sub_id == sub_id ) {
            /* connected=0 znamená disconnected HW - chovejme se jako
             * region neexistuje. Volající fallne back na LOGICAL. */
            if ( !r->connected ) return -1;
            return r->id;
        }
    }
    return -1;
}


/* ---- Backend vtable implementace --------------------------------------- */


/* Region size threshold pro zapnutí page cache - regiony pod tímto limitem
 * jsou dost malé na to, aby raw dbgapi_regions_read bylo rychlejší než
 * cache lookup. Memext bank (4 KB), VRAM plane (8 KB), CG-ROM (4 KB)
 * → bez cache. Ramdisk STD bank (64 KB), PEZIK (512 KB), celá Ramdisk
 * agregace 16 MB → s cache. */
#define MB_CACHE_REGION_SIZE_THRESHOLD  (32u * 1024u)


/* Read wrapper: ctx = (void*)(intptr_t)region_id. Pro velké regiony
 * (> 32 KB) jde přes 16 KB page cache, jinak direct call na dbgapi.
 *
 * Výjimka: REGION_KIND_LOGICAL (= banking-aware Z80 view 64 KB) cache
 * NIKDY nepoužije. Důvod: cache fingerprint (count+size+connected+kind+
 * sub_id) NEDETEKUJE změnu banking flagů g_memory.map ani změnu memext
 * bank pointerů memram_read[]. Po unmap ROM nebo Memext bank switch by
 * cache vrátila zastaralá data (= bug objevený Michalem: 0x0000-0x0FFF
 * v RAM banking ukazoval staré ROM bajty; 0x3100+ v memext bank ukazoval
 * 0x00 z předchozí konfigurace).
 *
 * dbgapi_regions_read pro LOGICAL volá per-byte memory_read_byte_no_se
 * která projde aktuální banking dispatch + MEMORY_RAM_READ_BYTE =
 * g_memory.memram_read[bank][offset] (tj. respektuje Memext indirekci).
 * Logical má jen 64 KB → per-frame read je laciný. */
static int emu_backend_read ( void *ctx, uint64_t offset, uint8_t *out, uint32_t len )
{
    int region_id = ( int ) ( intptr_t ) ctx;
    /* dbgapi_regions_read má uint32_t offset - clamp pro safety. */
    if ( offset > 0xFFFFFFFFu ) return -1;

    /* Cache path: jen pokud region je dost velký aby měla smysl
     * A NENÍ to banking-aware LOGICAL region. */
    if ( region_id >= 0 && region_id < s_regions_count ) {
        const st_REGION_DESC *r = &s_regions_buf[region_id];
        uint64_t rsize = ( uint64_t ) r->size;
        /* Bypass cache pro banking-aware regiony - jejich obsah se mění
         * při Memext bank switch / ROM swap a fingerprint
         * (count+size+connected+kind+sub_id) tyto změny nedetekuje.
         * LOGICAL = banking-aware CPU view.
         * RAM = banking-aware "User RAM" view přes memram_write[]. */
        if ( rsize > MB_CACHE_REGION_SIZE_THRESHOLD
                && r->kind != REGION_KIND_LOGICAL
                && r->kind != REGION_KIND_RAM ) {
            return cache_read ( region_id, offset, out, len, rsize );
        }
    }
    return dbgapi_regions_read ( region_id, ( uint32_t ) offset, out, len );
}


/* Write wrapper: ctx = (void*)(intptr_t)region_id. Po úspěšném write
 * invalidate ovlivněné cache stránky (jinak by další read vrátil starou
 * hodnotu). */
static int emu_backend_write ( void *ctx, uint64_t offset, const uint8_t *in, uint32_t len )
{
    int region_id = ( int ) ( intptr_t ) ctx;
    if ( offset > 0xFFFFFFFFu ) return -1;
    int wr = dbgapi_regions_write ( region_id, ( uint32_t ) offset, in, len );
    if ( wr > 0 ) {
        cache_invalidate_range ( region_id, offset, ( uint32_t ) wr );
    }
    return wr;
}


/* Total size: enumerate again a najdi velikost pro toto ID. O(n) ale
 * volá se zřídka (jednou per render na celý view). */
static uint64_t emu_backend_total_size ( void *ctx )
{
    int region_id = ( int ) ( intptr_t ) ctx;
    if ( region_id < 0 || region_id >= s_regions_count ) return 0;
    /* Spoléháme že s_regions_buf je čerstvý ze posledního refresh. */
    return ( uint64_t ) s_regions_buf[region_id].size;
}


extern "C" void membrowser_io_make_emu_backend ( st_HEX_VIEW_BACKEND *be,
                                                 int region_id, bool is_writable )
{
    if ( !be ) return;
    std::memset ( be, 0, sizeof ( *be ) );

    be->ctx = ( void * ) ( intptr_t ) region_id;
    be->read_bytes = emu_backend_read;
    be->write_bytes = is_writable ? emu_backend_write : nullptr;
    be->total_size = emu_backend_total_size;
    be->region_label = nullptr;  /* V0 bez per-row label. */
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
