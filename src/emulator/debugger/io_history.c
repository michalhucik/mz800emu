/*
 * io_history.c - Implementace ringu pro IORQ event history.
 *
 * Viz io_history.h pro datovy model a kontrakty.
 *
 * Licence: GPLv3
 */

#include "io_history.h"

#include <stdlib.h>
#include <string.h>

#include "libs/cfgfile/cfgmodule.h"


unsigned g_io_history_cfg_capacity = IO_HISTORY_DEFAULT_CAPACITY;


/**
 * Globalni instance ringu.
 */
st_IO_HISTORY_RING g_io_history = {
    .events    = NULL,
    .capacity  = 0,
    .head      = 0,
    .count     = 0,
    .overflow  = false,
};


/** @brief 4 jedničky inicializátoru masky (pomocné makro, jen tady). */
#define IO_HISTORY_ONES_4    1u, 1u, 1u, 1u
/** @brief 16 jedniček inicializátoru masky. */
#define IO_HISTORY_ONES_16   IO_HISTORY_ONES_4, IO_HISTORY_ONES_4, IO_HISTORY_ONES_4, IO_HISTORY_ONES_4
/** @brief 64 jedniček inicializátoru masky. */
#define IO_HISTORY_ONES_64   IO_HISTORY_ONES_16, IO_HISTORY_ONES_16, IO_HISTORY_ONES_16, IO_HISTORY_ONES_16

/* Inicializátor níže musí pokrýt celou mapu (4 x 64). */
_Static_assert ( IO_HISTORY_RECORD_MAP_SIZE == 256u,
                 "inicializátor g_io_history_record_enabled počítá s 256 porty" );


/**
 * Per-port record_enabled mapa (V1.7+ 2.6).
 *
 * Výchozí stav = všech 256 portů 1 už při překladu (statický
 * inicializátor), aby nezávisel na pořadí initu. Z INI ji naplní propagate
 * klíče `record_mask` (io_history_register_persistence); io_history_init()
 * ani io_history_set_capacity() ji nemění. Filtr aplikován
 * v io_history_record().
 */
uint8_t g_io_history_record_enabled[ IO_HISTORY_RECORD_MAP_SIZE ] = {
    IO_HISTORY_ONES_64, IO_HISTORY_ONES_64, IO_HISTORY_ONES_64, IO_HISTORY_ONES_64
};


/**
 * Helper - clamp capacity do platneho rozsahu.
 */
static size_t clamp_capacity ( size_t cap )
{
    if ( cap < IO_HISTORY_MIN_CAPACITY ) cap = IO_HISTORY_MIN_CAPACITY;
    if ( cap > IO_HISTORY_MAX_CAPACITY ) cap = IO_HISTORY_MAX_CAPACITY;
    return cap;
}


void io_history_record_enable_all ( void )
{
    /* Nastaví všech 256 portů na 1 = výchozí záznam. Bajtové zápisy
     * jsou atomické - souběžně čtoucí emu vlákno uvidí mix starých
     * a nových hodnot, obě platné.
     */
    for ( size_t i = 0; i < IO_HISTORY_RECORD_MAP_SIZE; i++ ) {
        g_io_history_record_enabled[ i ] = 1u;
    }
}


void io_history_init ( size_t capacity )
{
    /* Masku zaznamenávaných portů (g_io_history_record_enabled) init
     * záměrně nemění: drží uživatelskou volbu z INI (propagate proběhne
     * dřív než init) i přes změnu kapacity (set_capacity volá init). */
    if ( capacity == 0 ) capacity = IO_HISTORY_DEFAULT_CAPACITY;
    capacity = clamp_capacity ( capacity );

    /* Pokud uz mame alokovano, jen vynulujeme stav. Realokace probiha
     * pres io_history_set_capacity(). */
    if ( g_io_history.events != NULL && g_io_history.capacity == capacity ) {
        g_io_history.head     = 0;
        g_io_history.count    = 0;
        g_io_history.overflow = false;
        return;
    }

    /* Realokace */
    free ( g_io_history.events );
    g_io_history.events = (st_IO_HISTORY_EVENT *)
        calloc ( capacity, sizeof ( st_IO_HISTORY_EVENT ) );
    g_io_history.capacity = capacity;
    g_io_history.head     = 0;
    g_io_history.count    = 0;
    g_io_history.overflow = false;
}


void io_history_destroy ( void )
{
    free ( g_io_history.events );
    g_io_history.events   = NULL;
    g_io_history.capacity = 0;
    g_io_history.head     = 0;
    g_io_history.count    = 0;
    g_io_history.overflow = false;
}


void io_history_set_capacity ( size_t new_capacity )
{
    new_capacity = clamp_capacity ( new_capacity );
    io_history_destroy ( );
    io_history_init ( new_capacity );
    /* Zvolená kapacita se při uložení konfigurace zapíše do INI. */
    g_io_history_cfg_capacity = (unsigned) new_capacity;
}


/**
 * @brief Hex znak (0-9 / A-F / a-f) na 4bitový nibble.
 * @param c Znak.
 * @return 0..15, nebo -1 pro neplatný znak.
 */
static int io_history_hex_nibble ( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'A' && c <= 'F' ) return 10 + ( c - 'A' );
    if ( c >= 'a' && c <= 'f' ) return 10 + ( c - 'a' );
    return -1;
}


/**
 * @brief Propagate callback klíče `record_mask`: 64 hex znaků -> 256 flagů.
 *
 * Formát: 64 hex znaků = 32 bajtů; bajt n (znaky 2n, 2n+1) nese porty
 * n*8 .. n*8+7, bit 0 = nejnižší port (port i = bit i%8 bajtu i/8).
 * Při chybě (jiná délka než 64, neplatný znak) se zaznamenávají všechny
 * porty (io_history_record_enable_all) - bezpečný návrat k výchozímu stavu.
 *
 * @param e    st_CFGELEMENT* prvku `record_mask`.
 * @param data Nepoužito.
 *
 * @pre Volá cfgmodule_propagate() při startu (debugger_init), před
 *      spuštěním emu vlákna.
 * @post g_io_history_record_enabled odpovídá hodnotě z INI (nebo
 *       výchozímu "vše").
 */
static void io_history_cfg_propagate_record_mask ( void *e, void *data )
{
    (void) data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *) e;
    const char *txt = cfgelement_get_text_value ( elm );
    if ( !txt || strlen ( txt ) != 64 ) {
        io_history_record_enable_all ( );
        return;
    }
    for ( size_t byte = 0; byte < 32; byte++ ) {
        int hi = io_history_hex_nibble ( txt[ byte * 2 ] );
        int lo = io_history_hex_nibble ( txt[ byte * 2 + 1 ] );
        if ( hi < 0 || lo < 0 ) {
            /* Už zapsané bajty přepíše výchozí stav. */
            io_history_record_enable_all ( );
            return;
        }
        uint8_t b = (uint8_t) ( ( hi << 4 ) | lo );
        for ( int bit = 0; bit < 8; bit++ ) {
            g_io_history_record_enabled[ byte * 8 + (size_t) bit ] =
                (uint8_t) ( ( b >> bit ) & 1u );
        }
    }
}


/**
 * @brief Save callback klíče `record_mask`: 256 flagů -> 64 hex znaků.
 *
 * Inverze io_history_cfg_propagate_record_mask() (velká písmena A-F).
 *
 * @param e    st_CFGELEMENT* prvku `record_mask`.
 * @param data Nepoužito.
 *
 * @post Textová hodnota prvku odpovídá aktuální masce.
 */
static void io_history_cfg_save_record_mask ( void *e, void *data )
{
    (void) data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *) e;
    char buf[ 65 ];
    static const char hex[] = "0123456789ABCDEF";
    for ( size_t byte = 0; byte < 32; byte++ ) {
        uint8_t b = 0;
        for ( int bit = 0; bit < 8; bit++ ) {
            if ( g_io_history_record_enabled[ byte * 8 + (size_t) bit ] ) {
                b |= (uint8_t) ( 1u << bit );
            }
        }
        buf[ byte * 2 ]     = hex[ ( b >> 4 ) & 0x0Fu ];
        buf[ byte * 2 + 1 ] = hex[ b & 0x0Fu ];
    }
    buf[ 64 ] = '\0';
    cfgelement_set_text_value ( elm, buf );
}


void io_history_register_persistence ( void *cmod_void )
{
    if ( !cmod_void ) return;
    st_CFGMODULE *cmod = (st_CFGMODULE *) cmod_void;

    /* UNSIGNED 1000..50000, default 10000. */
    st_CFGELEMENT *elm = cfgmodule_register_new_element ( cmod,
        (char *) "history_capacity",
        CFGENTYPE_UNSIGNED, (int) IO_HISTORY_DEFAULT_CAPACITY,
        (int) IO_HISTORY_MIN_CAPACITY, (int) IO_HISTORY_MAX_CAPACITY );
    cfgelement_set_handlers ( elm,
        (void *) &g_io_history_cfg_capacity,
        (void *) &g_io_history_cfg_capacity );

    /* Maska zaznamenávaných portů (V1.7+ 2.6): 64 hex znaků, výchozí
     * samé F = všechny porty. Pro 256 portů by jednotlivé klíče byly
     * v INI nečitelné; řetězec jde i ručně upravit. */
    elm = cfgmodule_register_new_element ( cmod,
        (char *) "record_mask", CFGENTYPE_TEXT,
        (char *) "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF" );
    cfgelement_set_propagate_cb ( elm,
        io_history_cfg_propagate_record_mask, NULL );
    cfgelement_set_save_cb ( elm,
        io_history_cfg_save_record_mask, NULL );
}


void io_history_init_from_cfg ( void )
{
    io_history_init ( (size_t) g_io_history_cfg_capacity );
}


void io_history_record ( bool is_in, uint16_t port, uint8_t value,
                          uint16_t pc, uint32_t frame,
                          uint16_t scanline, uint16_t px,
                          uint32_t cpu_cycle )
{
    if ( g_io_history.events == NULL || g_io_history.capacity == 0 ) {
        /* Auto-init pri prvnim volani (= pripad kdyby debugger_init
         * nebyl zavolan v testovacim kontextu). */
        io_history_init ( IO_HISTORY_DEFAULT_CAPACITY );
        if ( g_io_history.events == NULL ) return;
    }

    /* V1.7+ 2.6 selective per-port filter. Low byte port adresy slouzi
     * jako 8-bit I/O index do record_enabled mapy. Atomic byte read -
     * UI vlakno muze tu hodnotu menit bez locku. Default vse = 1
     * (= zero-overhead beyond one branch). */
    if ( !g_io_history_record_enabled[ port & 0xFFu ] ) return;

    st_IO_HISTORY_EVENT *e = &g_io_history.events[ g_io_history.head ];
    e->frame     = frame;
    e->cpu_cycle = cpu_cycle;
    e->port      = port;
    e->pc        = pc;
    e->value     = value;
    /* IORQ event: bit 0 = is_read, bit 1 = 0 (= ne memory). */
    e->flags     = is_in ? IO_HISTORY_FLAG_READ : 0u;
    e->scanline  = scanline;
    e->px        = px;
    e->_pad      = 0;

    g_io_history.head = ( g_io_history.head + 1 ) % g_io_history.capacity;
    if ( g_io_history.count < g_io_history.capacity ) {
        g_io_history.count++;
    } else {
        g_io_history.overflow = true;
    }
}


void io_history_record_mem ( bool is_read, uint16_t addr, uint8_t value,
                              uint16_t pc, uint32_t frame,
                              uint16_t scanline, uint16_t px,
                              uint32_t cpu_cycle )
{
    if ( g_io_history.events == NULL || g_io_history.capacity == 0 ) {
        io_history_init ( IO_HISTORY_DEFAULT_CAPACITY );
        if ( g_io_history.events == NULL ) return;
    }

    st_IO_HISTORY_EVENT *e = &g_io_history.events[ g_io_history.head ];
    e->frame     = frame;
    e->cpu_cycle = cpu_cycle;
    e->port      = addr;     /* MMIO addr ulozeno do `port` field */
    e->pc        = pc;
    e->value     = value;
    /* Memory event: bit 0 = is_read, bit 1 = 1 (= memory). */
    e->flags     = ( is_read ? IO_HISTORY_FLAG_READ : 0u )
                   | IO_HISTORY_FLAG_MEMORY;
    e->scanline  = scanline;
    e->px        = px;
    e->_pad      = 0;

    g_io_history.head = ( g_io_history.head + 1 ) % g_io_history.capacity;
    if ( g_io_history.count < g_io_history.capacity ) {
        g_io_history.count++;
    } else {
        g_io_history.overflow = true;
    }
}


void io_history_clear ( void )
{
    g_io_history.head     = 0;
    g_io_history.count    = 0;
    g_io_history.overflow = false;
    /* Necleanujeme events[] obsah - count=0 zarucuje, ze se nepouzije. */
}


const st_IO_HISTORY_EVENT* io_history_get ( size_t idx )
{
    if ( idx >= g_io_history.count ) return NULL;
    if ( g_io_history.events == NULL ) return NULL;

    /* Pri non-overflow: data jsou na indexu 0..count-1 v fyzickem ringu.
     * Pri overflow: oldest je na head, fyzicky idx = (head + idx) % capacity. */
    size_t phys;
    if ( !g_io_history.overflow ) {
        phys = idx;
    } else {
        phys = ( g_io_history.head + idx ) % g_io_history.capacity;
    }
    return &g_io_history.events[ phys ];
}
