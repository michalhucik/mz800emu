/**
 * @file   eventlog.c
 * @brief  Implementace in-memory ringu pro Event Viewer.
 *
 * Viz @ref eventlog.h pro datový model a kontrakty.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 *
 * Licence: GPLv3
 */

#include "main.h"
#include "emulator/mzarch/mzhal.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "eventlog.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <glib.h>

#include "libs/cfgfile/cfgmodule.h"
#include "emulator/cfgmain.h"
#include "emulator/debugger/trace/tlog_common.h"

#include "emulator/mzarch/mzarch.h"
#include "emulator/debugger/bp_event.h"
#include "emulator/hw-generic/memory/memory.h"

/* ===========================================================================
 *  Globální stav
 * =========================================================================== */

/**
 * Konfigurace načtená z cfg sekce @c [EVENT_LOG].
 *
 * Defaulty viz @ref eventlog_init(). Pole @c categories_mask se plní v
 * @ref propagate_categories_mask() z text reprezentace.
 */
st_EVENTLOG_CONFIG g_eventlog_config = {
    .mode            = EVENTLOG_MODE_OFF,
    .capacity        = EVENTLOG_DEFAULT_CAPACITY,
    .categories_mask = UINT64_C ( 0xFFFFFFFFFFFFFFFF ),
};

int      g_eventlog_active      = 0;
uint64_t g_eventlog_active_mask = UINT64_C ( 0xFFFFFFFFFFFFFFFF );

/* Pause-on-match trigger gate + callback pointer (Commit 19).
 * Default OFF - hot-path má jen 1 load + 1 branch režii. UI vrstva
 * registruje callback při init Events okna a přepíná gate při toggle
 * checkboxu "Pause on match" + úspěšném parse filteru. */
int               g_eventlog_pause_trigger_active = 0;
evlog_pause_cb_t  g_eventlog_pause_callback       = NULL;

/* Auto-mark on match trigger gate + callback pointer (Commit 20).
 * Identický pattern s pause triggerem - nezávislá feature, oba mohou
 * běžet paralelně. Re-entry guard (USER_MARK skip) řeší UI callback,
 * eventlog vrstva o tom neví. */
int                  g_eventlog_automark_trigger_active = 0;
evlog_automark_cb_t  g_eventlog_automark_callback       = NULL;

st_EVENTLOG_RING g_eventlog = {
    .events   = NULL,
    .capacity = 0,
    .head     = 0,
    .count    = 0,
    .overflow = false,
};

/**
 * Interní flag = Events okno aktuálně otevřené v UI.
 *
 * V Commit 1 zůstává vždy 0 (= mode WHEN_WINDOW_OPEN se chová jako OFF).
 * UI v Commit 7 přidá setter (zatím nedeklarovaný v public API), který
 * tento flag přepíná z imgui Render() / Close() callbacku.
 *
 * Thread safety: int write je atomický na x86, krátký data race vůči
 * @ref eventlog_recompute_active() je acceptable.
 */
static int s_event_viewer_window_open = 0;

/**
 * Cfg vázaný text buffer pro @c categories_mask (hex string).
 *
 * cfgmodule vlastní pointer, my ho jen čteme v
 * @ref propagate_categories_mask(). Default v
 * @ref eventlog_init() = @c "0xFFFFFFFFFFFFFFFF" (= všechny kategorie on).
 */
static char *s_cfg_categories_mask_text = NULL;

/* ===========================================================================
 *  Pomocné funkce
 * =========================================================================== */

/**
 * @brief Clamp capacity do platného rozsahu.
 */
static size_t clamp_capacity ( size_t cap )
{
    if ( cap < EVENTLOG_MIN_CAPACITY ) cap = EVENTLOG_MIN_CAPACITY;
    if ( cap > EVENTLOG_MAX_CAPACITY ) cap = EVENTLOG_MAX_CAPACITY;
    return cap;
}

/**
 * @brief Re-parse text masky z cfg do @c g_eventlog_config.categories_mask
 *        a propagovat do hot-path mirror @ref g_eventlog_active_mask.
 *
 * Tolerantní k formátu - akceptuje hex s prefixem (0x / 0X) i bez,
 * decimální. Při prázdném / NULL textu zachová stávající hodnotu
 * (= robustnost vůči chybnému ini).
 */
static void propagate_categories_mask ( void )
{
    if ( !s_cfg_categories_mask_text || !s_cfg_categories_mask_text[0] ) return;
    uint64_t v = g_ascii_strtoull ( s_cfg_categories_mask_text, NULL, 0 );
    g_eventlog_config.categories_mask = v;
    g_eventlog_active_mask = v;
}

/* ===========================================================================
 *  Lifecycle
 * =========================================================================== */

void eventlog_init ( size_t capacity )
{
    /* --- Cfg registrace -------------------------------------------------- */
    /* Idempotence: cfgroot_register_new_module pro stejné jméno již druhým
     * voláním vrací existující module - není problém volat opakovaně
     * (= testy mohou volat init/destroy/init v loop). */
    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "EVENT_LOG" );
    if ( cmod ) {
        CFGELM *elm;
        /* Dual-keyword sjednocení s reclife/tlog: WITH_WINDOW je alias pro
         * WHEN_WINDOW_OPEN (stejná enum hodnota 1). cfgelement_property_get_
         * keyword_by_value vrací PRVNÍ keyword se shodnou hodnotou, proto
         * WHEN_WINDOW_OPEN zůstává první = zápis do INI se NEMĚNÍ (zpětná
         * kompatibilita s uloženými .ini). Čtení akceptuje OBA keywordy. */
        elm = cfgmodule_register_new_element ( cmod, "mode", CFGENTYPE_KEYWORD,
                                                EVENTLOG_MODE_OFF,
                                                EVENTLOG_MODE_OFF,              "OFF",
                                                EVENTLOG_MODE_WHEN_WINDOW_OPEN, "WHEN_WINDOW_OPEN",
                                                EVENTLOG_MODE_WHEN_WINDOW_OPEN, "WITH_WINDOW",
                                                EVENTLOG_MODE_ALWAYS,           "ALWAYS",
                                                -1 );
        cfgelement_set_handlers ( elm, (void *) &g_eventlog_config.mode,
                                       (void *) &g_eventlog_config.mode );

        elm = cfgmodule_register_new_element ( cmod, "capacity", CFGENTYPE_UNSIGNED,
                                                (int) EVENTLOG_DEFAULT_CAPACITY,
                                                (int) EVENTLOG_MIN_CAPACITY,
                                                (int) EVENTLOG_MAX_CAPACITY );
        cfgelement_set_handlers ( elm, (void *) &g_eventlog_config.capacity,
                                       (void *) &g_eventlog_config.capacity );

        /* Bitmask 64-bit ukládáme jako hex string - CFGENTYPE_UNSIGNED je
         * jen 32-bit int. Default "0xFFFFFFFFFFFFFFFF" = všechny kategorie
         * povoleny. */
        elm = cfgmodule_register_new_element ( cmod, "categories_mask", CFGENTYPE_TEXT,
                                                "0xFFFFFFFFFFFFFFFF" );
        cfgelement_bind ( elm, (void *) &s_cfg_categories_mask_text );

        cfgmodule_parse ( cmod );
        cfgmodule_propagate ( cmod );

        /* Po propagate převeď text -> 64-bit číslo do hot-path mirror. */
        propagate_categories_mask ( );
    }

    /* --- Alokace ringu --------------------------------------------------- */
    size_t cap = ( capacity != 0 ) ? capacity : (size_t) g_eventlog_config.capacity;
    if ( cap == 0 ) cap = EVENTLOG_DEFAULT_CAPACITY;
    cap = clamp_capacity ( cap );

    /* Pokud už máme alokáno se stejnou capacity, jen vynulujeme stav. */
    if ( g_eventlog.events != NULL && g_eventlog.capacity == cap ) {
        g_eventlog.head     = 0;
        g_eventlog.count    = 0;
        g_eventlog.overflow = false;
        return;
    }

    free ( g_eventlog.events );
    g_eventlog.events = (st_EVENTLOG_EVENT *)
        calloc ( cap, sizeof ( st_EVENTLOG_EVENT ) );
    g_eventlog.capacity = ( g_eventlog.events != NULL ) ? cap : 0;
    g_eventlog.head     = 0;
    g_eventlog.count    = 0;
    g_eventlog.overflow = false;
}


void eventlog_destroy ( void )
{
    free ( g_eventlog.events );
    g_eventlog.events   = NULL;
    g_eventlog.capacity = 0;
    g_eventlog.head     = 0;
    g_eventlog.count    = 0;
    g_eventlog.overflow = false;
    g_eventlog_active   = 0;
    /* Pause-on-match gate + callback - po destroy nemůžeme volat UI
     * vrstvu, vyresetujeme gate (callback pointer ponecháme - UI ho
     * případně registruje znovu při dalším initu). */
    g_eventlog_pause_trigger_active = 0;
    /* Auto-mark gate má stejný kontrakt jako pause. */
    g_eventlog_automark_trigger_active = 0;
    /* s_cfg_categories_mask_text vlastní cfgmodule, neuvolňujeme. */
}


int eventlog_start ( void )
{
    if ( g_eventlog.events == NULL || g_eventlog.capacity == 0 ) return -1;
    if ( g_eventlog_active ) return 0;
    g_eventlog_active = 1;
    fprintf ( stderr, "[event-viewer] eventlog: recording started\n" );
    return 0;
}


void eventlog_stop ( void )
{
    if ( !g_eventlog_active ) return;
    g_eventlog_active = 0;
    fprintf ( stderr, "[event-viewer] eventlog: recording stopped\n" );
}


void eventlog_recompute_active ( int debugger_active )
{
    (void) debugger_active;  /* Commit 1: zatím nevyužito (parita API s tlog). */

    int new_active = 0;
    switch ( g_eventlog_config.mode ) {
        case EVENTLOG_MODE_OFF:
            new_active = 0;
            break;
        case EVENTLOG_MODE_WHEN_WINDOW_OPEN:
            new_active = s_event_viewer_window_open;
            break;
        case EVENTLOG_MODE_ALWAYS:
            new_active = 1;
            break;
    }

    if ( new_active && !g_eventlog_active ) {
        eventlog_start ( );
    } else if ( !new_active && g_eventlog_active ) {
        eventlog_stop ( );
    }
}


void eventlog_notify_window_open ( int is_open )
{
    s_event_viewer_window_open = is_open ? 1 : 0;
    /* debugger_active arg zatím nevyužit (parita API s tlog). */
    eventlog_recompute_active ( 0 );
}


void eventlog_clear ( void )
{
    g_eventlog.head     = 0;
    g_eventlog.count    = 0;
    g_eventlog.overflow = false;
    /* events[] obsah zachováme - count = 0 zaručí, že ho nikdo nepřečte. */
}


void eventlog_set_capacity ( size_t new_capacity )
{
    new_capacity = clamp_capacity ( new_capacity );
    if ( new_capacity == g_eventlog.capacity && g_eventlog.events != NULL ) {
        /* Stejná velikost - jen vyprázdníme. */
        eventlog_clear ( );
        return;
    }

    int was_active = g_eventlog_active;
    if ( was_active ) eventlog_stop ( );

    free ( g_eventlog.events );
    g_eventlog.events = (st_EVENTLOG_EVENT *)
        calloc ( new_capacity, sizeof ( st_EVENTLOG_EVENT ) );
    g_eventlog.capacity = ( g_eventlog.events != NULL ) ? new_capacity : 0;
    g_eventlog.head     = 0;
    g_eventlog.count    = 0;
    g_eventlog.overflow = false;
    g_eventlog_config.capacity = (unsigned) g_eventlog.capacity;

    if ( was_active && g_eventlog.events != NULL ) eventlog_start ( );
}

/* ===========================================================================
 *  Ambient state capture (Vlna 4 Commit 24)
 * =========================================================================== */

/**
 * @brief Per-arch banking summary decoder.
 *
 * Volá @c memmap_query() pro klíčové 4KB stránky a klasifikuje do
 * jednoho ze 3-bit kódů @ref en_EVENTLOG_AMBIENT_BANKING. Per-arch
 * sémantika - každá architektura mapuje banking jinak (viz tabulka
 * v eventlog.h).
 *
 * Hot-path overhead: 1-3 volání @c memmap_query (= read @c g_memory.map
 * + případně @c g_gdg.regDMD). Bez heap, bez locků, side-effect free.
 *
 * @return Hodnota @ref en_EVENTLOG_AMBIENT_BANKING (0..7).
 */
static uint8_t eventlog_get_banking_summary ( void )
{
    /* Memmap query pro page 0 (= ROM_LOW oblast 0x0000-0x0FFF) a
     * page 14 (= ROM_HIGH oblast 0xE000-0xEFFF). Stačí na základní
     * klasifikaci ROM ON/OFF. Page 8 (= VRAM oblast 0x8000-0x8FFF na
     * MZ-800) doplňuje informaci o VRAM/CGROM mappingu. */
    en_MEMMAP_REGION_KIND p0  = memmap_query ( 0 );
    en_MEMMAP_REGION_KIND p14 = memmap_query ( 14 );

    /* Runtime dle g_mzhal.arch (mzhal 11f) - klasifikace ambient
     * banking je per arch, enum hodnoty jsou zmrazeny trace kontrakt. */
    if (g_mzhal.arch == 800) {
    /* MZ-800 (native i MZ-700 compat mode). */
    en_MEMMAP_REGION_KIND p8  = memmap_query ( 8 );
    en_MEMMAP_REGION_KIND p10 = memmap_query ( 10 );

    bool low_rom  = ( p0  == MEMMAP_KIND_ROM_LOW );
    bool high_rom = ( p14 == MEMMAP_KIND_ROM_HIGH );
    bool vram_lo  = ( p8  == MEMMAP_KIND_VRAM_I );
    bool vram_hi  = ( p10 == MEMMAP_KIND_VRAM_II );
    bool cgrom    = ( p0  == MEMMAP_KIND_CGROM ) || ( memmap_query ( 1 ) == MEMMAP_KIND_CGROM );

    if ( low_rom && high_rom && vram_lo && !vram_hi ) {
        return EVENTLOG_AMBIENT_BANKING_DEFAULT;
    }
    if ( !low_rom && !high_rom ) {
        return EVENTLOG_AMBIENT_BANKING_ALL_RAM;
    }
    if ( !low_rom && high_rom ) {
        return EVENTLOG_AMBIENT_BANKING_ROM_LOW_OFF;
    }
    if ( low_rom && !high_rom ) {
        return EVENTLOG_AMBIENT_BANKING_ROM_HIGH_OFF;
    }
    if ( cgrom ) {
        return EVENTLOG_AMBIENT_BANKING_CGROM;
    }
    if ( vram_hi ) {
        return EVENTLOG_AMBIENT_BANKING_VRAM_640;
    }
    return EVENTLOG_AMBIENT_BANKING_OTHER;

    }
    if (g_mzhal.arch == 700) {
    /* MZ-700 - jednodušší banking: ROM low + ROM high + RAM + speciální
     * regiony 0xD000-0xE00F (VRAM_TEXT / CGRAM / MAPPED_PORTS). */
    bool low_rom  = ( p0  == MEMMAP_KIND_ROM_LOW );
    bool high_rom = ( p14 == MEMMAP_KIND_ROM_HIGH );

    if ( low_rom && high_rom ) {
        return EVENTLOG_AMBIENT_BANKING_DEFAULT;
    }
    if ( !low_rom && !high_rom ) {
        return EVENTLOG_AMBIENT_BANKING_ALL_RAM;
    }
    if ( !low_rom && high_rom ) {
        return EVENTLOG_AMBIENT_BANKING_ROM_LOW_OFF;
    }
    if ( low_rom && !high_rom ) {
        return EVENTLOG_AMBIENT_BANKING_ROM_HIGH_OFF;
    }
    return EVENTLOG_AMBIENT_BANKING_OTHER;

    }
    if (g_mzhal.arch == 1500) {
    /* MZ-1500 - ROM_LOW / ROM_HIGH + SPEC banky (CGROM / PCG_1/2/3) v
     * 0xD000-0xEFFF. Klíčové = page 13 (0xD000). */
    en_MEMMAP_REGION_KIND p13 = memmap_query ( 13 );

    bool low_rom  = ( p0  == MEMMAP_KIND_ROM_LOW );
    bool high_rom = ( p14 == MEMMAP_KIND_ROM_HIGH );

    if ( low_rom && high_rom && p13 == MEMMAP_KIND_RAM ) {
        return EVENTLOG_AMBIENT_BANKING_DEFAULT;
    }
    if ( !low_rom && !high_rom ) {
        return EVENTLOG_AMBIENT_BANKING_ALL_RAM;
    }
    if ( p13 == MEMMAP_KIND_CGROM ) {
        return EVENTLOG_AMBIENT_BANKING_CGROM;
    }
    if ( p13 == MEMMAP_KIND_PCG_1 ) {
        return EVENTLOG_AMBIENT_BANKING_VRAM_640;
    }
    if ( p13 == MEMMAP_KIND_PCG_2 || p13 == MEMMAP_KIND_PCG_3 ) {
        return EVENTLOG_AMBIENT_BANKING_PCG_HIGH;
    }
    return EVENTLOG_AMBIENT_BANKING_OTHER;
    }
    return EVENTLOG_AMBIENT_BANKING_OTHER;
}


/**
 * @brief Capture aktuálního ambient HW state pro event record.
 *
 * Sbírá:
 *  - IFF1 (1 bit) z @c g_mzarch_main.cpu->iff1
 *  - IM mode (2 bity) z @c g_mzarch_main.cpu->im
 *  - Fire reason (3 bity) z @c g_bp_fire_reason (typicky platí jen v rámci
 *    BP fire stack-u, jinak @c BP_REASON_NONE = mapuje na @c REASON_NONE)
 *  - Banking summary (3 bity) z @ref eventlog_get_banking_summary
 *
 * Volá se v emu vlákně z @ref eventlog_record (= hot path). Overhead
 * podle @c memmap_query složitosti (= 2-4 reads + bit packing).
 *
 * Defenzivní: pokud @c g_mzarch_main.cpu == NULL (= před mzarch_init),
 * IFF1 a IM jsou nulové, banking se přesto čte (= @c g_memory.map je
 * inicializován dříve než cpu).
 *
 * @return 16-bit ambient hodnota připravená k zápisu do
 *         @c st_EVENTLOG_EVENT::ambient.
 */
static uint16_t eventlog_capture_ambient ( void )
{
    uint16_t a = 0;

    /* IFF1 + IM - bit 0 + bity 1..2. */
    const z80_t *cpu = g_mzarch_main.cpu;
    if ( cpu != NULL ) {
        if ( cpu->iff1 ) {
            a |= EVENTLOG_AMBIENT_IFF1;
        }
        uint8_t im = (uint8_t) ( cpu->im & 0x03u );
        a |= (uint16_t) ( (uint16_t) im << EVENTLOG_AMBIENT_IM_SHIFT );
    }

    /* Reason - bity 3..5. BP_REASON_NONE = 0xFF (nevejde se do 3 bitů),
     * mapujeme na EVENTLOG_AMBIENT_REASON_NONE = 7. Ostatní hodnoty 0..6
     * jsou 1:1 (= shoda en_BP_FIRE_REASON x en_EVENTLOG_AMBIENT_REASON). */
    uint8_t r;
    if ( g_bp_fire_reason <= (uint8_t) BP_REASON_IFF_RETN ) {
        r = g_bp_fire_reason;
    } else {
        r = (uint8_t) EVENTLOG_AMBIENT_REASON_NONE;
    }
    a |= (uint16_t) ( (uint16_t) ( r & 0x07u ) << EVENTLOG_AMBIENT_REASON_SHIFT );

    /* Banking summary - bity 6..8. */
    uint8_t b = eventlog_get_banking_summary ( );
    a |= (uint16_t) ( (uint16_t) ( b & 0x07u ) << EVENTLOG_AMBIENT_BANKING_SHIFT );

    return a;
}


/* ===========================================================================
 *  Hot-path
 * =========================================================================== */

void eventlog_record ( uint8_t category, uint8_t subtype,
                       uint16_t pc, uint32_t payload )
{
    /* Defenzivní guard - caller už podle kontraktu testoval gate, ale ring
     * mohl být zničený mezitím (= UI vlákno během shutdown). */
    if ( g_eventlog.events == NULL || g_eventlog.capacity == 0 ) return;

    st_EVENTLOG_EVENT *e = &g_eventlog.events[ g_eventlog.head ];
    e->pxclk_total     = tlog_common_get_pxclk_total ( );
    e->screens_total   = tlog_common_get_screens_total ( );
    e->pxclk_in_screen = tlog_common_get_pxclk_in_screen ( );
    e->category        = category;
    e->subtype         = subtype;
    e->pc              = pc;
    e->payload         = payload;
    /* Vlna 4 Commit 24 - ambient state snapshot v okamžiku emit. */
    e->ambient         = eventlog_capture_ambient ( );
    /* Reserved pole MUSÍ být nula (= rezerva pro budoucí ambient).
     * Inicializaci provádíme i tady kvůli ringovým slotům, které byly
     * recyklované po wrap-around (= calloc inicializuje jen poprvé). */
    e->reserved16      = 0;
    e->reserved32      = 0;

    g_eventlog.head = ( g_eventlog.head + 1 ) % g_eventlog.capacity;
    if ( g_eventlog.count < g_eventlog.capacity ) {
        g_eventlog.count++;
    } else {
        g_eventlog.overflow = true;
    }

    /* Pause-on-match hook (Commit 19) - eval pause filter na nově
     * zapsaný event, match -> request emu pause (async). Per-event
     * overhead v default OFF stavu: 1 load + 1 branch. */
    if ( g_eventlog_pause_trigger_active && g_eventlog_pause_callback ) {
        g_eventlog_pause_callback ( e );
    }

    /* Auto-mark on match hook (Commit 20) - paralelní s pause, eval
     * jiný filter, match -> marklog_record(). Re-entry guard (= skip
     * USER_MARK kategorie) řeší UI callback, ne eventlog vrstva. */
    if ( g_eventlog_automark_trigger_active && g_eventlog_automark_callback ) {
        g_eventlog_automark_callback ( e );
    }
}


/* ===========================================================================
 *  SYS lifecycle emit API (Vlna 5 Commit 31)
 * =========================================================================== */

void eventlog_sys_event ( uint8_t sub, uint32_t payload )
{
    /* Gate identicky s ostatními record cestami. PC = 0 (= SYS eventy
     * nejsou bound na CPU instruction). */
    if ( !TEST_TRACE_EVENTLOG_ACTIVE ) return;
    if ( !( g_eventlog_active_mask & ( UINT64_C ( 1 ) << EVENTLOG_CAT_SYS ) ) ) return;
    eventlog_record ( EVENTLOG_CAT_SYS, sub, 0, payload );
}


uint32_t eventlog_filename_hash ( const char *filename )
{
    if ( filename == NULL ) return 0;

    /* Vzít jen basename (= za posledním '/' nebo '\\'). */
    const char *base = filename;
    for ( const char *p = filename; *p; p++ ) {
        if ( *p == '/' || *p == '\\' ) base = p + 1;
    }
    if ( *base == '\0' ) return 0;

    /* Klasický djb2 (D. J. Bernstein). */
    uint32_t hash = 5381u;
    for ( const char *p = base; *p; p++ ) {
        hash = ( ( hash << 5 ) + hash ) + (uint8_t) *p;
    }
    return hash;
}


size_t eventlog_get_count ( void )
{
    return g_eventlog.count;
}


const st_EVENTLOG_EVENT *eventlog_get_event ( size_t idx )
{
    if ( idx >= g_eventlog.count ) return NULL;
    if ( g_eventlog.events == NULL ) return NULL;

    size_t phys;
    if ( !g_eventlog.overflow ) {
        phys = idx;
    } else {
        phys = ( g_eventlog.head + idx ) % g_eventlog.capacity;
    }
    return &g_eventlog.events[ phys ];
}


/* ===========================================================================
 *  Export / Import (Vlna 4 Commit 29)
 * =========================================================================== */

int eventlog_export_to_file ( const char *path )
{
    if ( path == NULL || path[0] == '\0' ) {
        fprintf ( stderr, "[event-viewer] eventlog_export: NULL/empty path\n" );
        return -1;
    }
    if ( g_eventlog.events == NULL || g_eventlog.capacity == 0 ) {
        fprintf ( stderr, "[event-viewer] eventlog_export: ring not initialized\n" );
        return -1;
    }

    FILE *fp = fopen ( path, "wb" );
    if ( fp == NULL ) {
        fprintf ( stderr, "[event-viewer] eventlog_export: fopen('%s') failed: %s\n",
                  path, g_strerror ( errno ) );
        return -1;
    }

    /* Připrav hlavičku. Záměrně inicializujeme přes memcpy magic, aby
     * trailing 0 bajt z C string literálu (= EVENTLOG_EXPORT_MAGIC je 9 B
     * včetně NUL) neskončil v poli velikosti 8 B. */
    st_EVENTLOG_EXPORT_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    memcpy ( hdr.magic, EVENTLOG_EXPORT_MAGIC, EVENTLOG_EXPORT_MAGIC_LEN );
    hdr.version      = EVENTLOG_EXPORT_VERSION;
    hdr.record_size  = (uint32_t) sizeof ( st_EVENTLOG_EVENT );
    hdr.record_count = (uint64_t) g_eventlog.count;
    hdr.timestamp    = (uint64_t) time ( NULL );

    if ( fwrite ( &hdr, sizeof ( hdr ), 1, fp ) != 1 ) {
        fprintf ( stderr, "[event-viewer] eventlog_export: header fwrite failed: %s\n",
                  g_strerror ( errno ) );
        fclose ( fp );
        return -1;
    }

    /* Records v chronologickém pořadí (= oldest first přes get_event).
     * Při overflow = false je memcpy bloku [0..count) totéž; při overflow
     * = true je oldest na fyzickém indexu head, takže iterujeme přes
     * eventlog_get_event(), ne přes events[]. */
    for ( size_t i = 0; i < g_eventlog.count; i++ ) {
        const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
        if ( e == NULL ) {
            /* Defenzivní - count se v emu vlákně může změnit, ale ne nahoru
             * (= eventlog_record přidává, ne smaže). Pokud se přesto stane,
             * skončíme partial-OK (zapsaný count bude < hdr.record_count). */
            fprintf ( stderr, "[event-viewer] eventlog_export: unexpected NULL at idx %zu\n", i );
            fclose ( fp );
            return -1;
        }
        if ( fwrite ( e, sizeof ( st_EVENTLOG_EVENT ), 1, fp ) != 1 ) {
            fprintf ( stderr, "[event-viewer] eventlog_export: record %zu fwrite failed: %s\n",
                      i, g_strerror ( errno ) );
            fclose ( fp );
            return -1;
        }
    }

    if ( fclose ( fp ) != 0 ) {
        fprintf ( stderr, "[event-viewer] eventlog_export: fclose failed: %s\n",
                  g_strerror ( errno ) );
        return -1;
    }

    fprintf ( stderr, "[event-viewer] eventlog_export: %zu events -> '%s'\n",
              g_eventlog.count, path );
    return 0;
}


int eventlog_import_from_file ( const char *path )
{
    if ( path == NULL || path[0] == '\0' ) {
        fprintf ( stderr, "[event-viewer] eventlog_import: NULL/empty path\n" );
        return -1;
    }

    FILE *fp = fopen ( path, "rb" );
    if ( fp == NULL ) {
        fprintf ( stderr, "[event-viewer] eventlog_import: fopen('%s') failed: %s\n",
                  path, g_strerror ( errno ) );
        return -1;
    }

    st_EVENTLOG_EXPORT_HEADER hdr;
    if ( fread ( &hdr, sizeof ( hdr ), 1, fp ) != 1 ) {
        fprintf ( stderr, "[event-viewer] eventlog_import: header fread failed\n" );
        fclose ( fp );
        return -1;
    }

    /* Validace magic - 8 B literal, bez NUL. */
    if ( memcmp ( hdr.magic, EVENTLOG_EXPORT_MAGIC, EVENTLOG_EXPORT_MAGIC_LEN ) != 0 ) {
        fprintf ( stderr, "[event-viewer] eventlog_import: invalid magic\n" );
        fclose ( fp );
        return -1;
    }

    /* Validace verze. */
    if ( hdr.version != EVENTLOG_EXPORT_VERSION ) {
        fprintf ( stderr, "[event-viewer] eventlog_import: unsupported version %u (expected %u)\n",
                  (unsigned) hdr.version, (unsigned) EVENTLOG_EXPORT_VERSION );
        fclose ( fp );
        return -1;
    }

    /* Validace record_size - layout struktury MUSÍ matchovat. */
    if ( hdr.record_size != (uint32_t) sizeof ( st_EVENTLOG_EVENT ) ) {
        fprintf ( stderr, "[event-viewer] eventlog_import: record_size mismatch "
                          "(%u vs %u)\n",
                  (unsigned) hdr.record_size,
                  (unsigned) sizeof ( st_EVENTLOG_EVENT ) );
        fclose ( fp );
        return -1;
    }

    /* Capacity check - pokud import má víc records než current capacity,
     * resize ringu (clamped do EVENTLOG_MAX_CAPACITY). Records mimo nový
     * limit se zahodí (= UI/CLI dostane warning, ne error). */
    size_t want = (size_t) hdr.record_count;
    if ( want > EVENTLOG_MAX_CAPACITY ) {
        fprintf ( stderr, "[event-viewer] eventlog_import: record_count %zu > MAX %u, clamping\n",
                  want, (unsigned) EVENTLOG_MAX_CAPACITY );
        want = EVENTLOG_MAX_CAPACITY;
    }
    if ( g_eventlog.events == NULL || want > g_eventlog.capacity ) {
        size_t new_cap = ( want < EVENTLOG_MIN_CAPACITY ) ? EVENTLOG_MIN_CAPACITY : want;
        eventlog_set_capacity ( new_cap );
        if ( g_eventlog.events == NULL ) {
            fprintf ( stderr, "[event-viewer] eventlog_import: capacity allocation failed\n" );
            fclose ( fp );
            return -1;
        }
    } else {
        eventlog_clear ( );
    }

    /* Fread records do ringu. Pořadí v souboru je chronologické (= oldest
     * first), takže zápis přímo do events[0..want) je správně - head se
     * nastaví za poslední, overflow zůstane false. */
    size_t actually_read = fread ( g_eventlog.events,
                                    sizeof ( st_EVENTLOG_EVENT ),
                                    want, fp );
    if ( actually_read != want ) {
        fprintf ( stderr, "[event-viewer] eventlog_import: fread got %zu of %zu records\n",
                  actually_read, want );
        fclose ( fp );
        /* Partial načtení necháme v ringu - aspoň částečná data jsou
         * lepší než nic. Caller dostane -1 a může reagovat. */
        g_eventlog.count = actually_read;
        g_eventlog.head  = actually_read % g_eventlog.capacity;
        return -1;
    }

    g_eventlog.count    = want;
    g_eventlog.head     = want % g_eventlog.capacity;
    g_eventlog.overflow = false;

    fclose ( fp );

    fprintf ( stderr, "[event-viewer] eventlog_import: '%s' -> %zu events (timestamp %llu)\n",
              path, want, (unsigned long long) hdr.timestamp );
    return 0;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
