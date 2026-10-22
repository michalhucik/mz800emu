/**
 * @file   cputrack.c
 * @brief  Implementace CPU Tracking Log subsystému trace-suite.
 *
 * Viz @ref cputrack.h pro popis formátu eventů a HALT collapse logiky.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "cputrack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "libs/cfgfile/cfgmodule.h"
#include "emulator/cfgmain.h"
#include "emulator/mzarch/mzarch.h"
#include "emulator/mzarch/mzarch_config.h"
#include "emulator/debugger/debugger.h"
#include "emulator/debugger/trace/tlog_common.h"
#include "emulator/debugger/trace/reclife.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"

#if MZARCH == 800
#include "emulator/mzarch/mz800/gdg/mz800_gdg.h"
#elif MZARCH == 1500
#include "emulator/mzarch/mz1500/gdg/mz1500_gdg.h"
#endif

/* Globální konfigurace + stav */
st_CPUTRACK_CONFIG g_cputrack_config;
int g_cputrack_active = 0;

/* INI mezičlánky pro range-scope filtr. cfgfile CFGENTYPE_UNSIGNED handler
 * zapisuje sizeof(unsigned) (= 4 B, cfgelement.c:856/908), proto nelze
 * navázat přímo na uint16_t pole g_cputrack_config.pc_range_{lo,hi} (přepis
 * sousedních bytů). Načteme do unsigned a po propagate ořízneme na uint16_t. */
static unsigned s_ini_pc_range_lo = 0x0000;
static unsigned s_ini_pc_range_hi = 0xFFFF;

/* Writer + collapse stav (private) */
static st_TLOG_WRITER s_writer;
static int s_writer_open = 0;

/* HALT/self-loop collapse state */
static uint16_t s_prev_pc = 0xFFFF;     /**< PC před poslední vykonanou instr */
static int s_prev_pc_valid = 0;         /**< Inicializace flagu */
static uint64_t s_pending_wait_clk = 0; /**< Akumulované T-states v collapse */
static uint16_t s_collapse_pc = 0;      /**< PC instrukce která se opakuje */
static uint8_t s_collapse_insn_len = 1; /**< Délka instr (bytes) */
static uint8_t s_collapse_insn_bytes[ 4 ];

/* Formát v3 (registry). s_seg_regs platí od cputrack_start() do dalšího
 * startu; čte ho háček, proto se mění jen při zavřeném writeru. */
static int s_seg_regs = 0;                  /**< 1 = otevřený segment je v3 (32 B) */
/**
 * @brief Stav, který čtenář odvodí z posledního zapsaného záznamu v3.
 *
 * @invariant Při @c s_chain_valid == 1 je to přesně stav nesený posledním
 *            zapsaným záznamem (instrukce, sbalená smyčka, přijetí
 *            přerušení, synchronizace), včetně alternativní sady (tu
 *            záznam instrukce nenese, čtenář ji odvodí z EXX/EX AF,AF').
 */
static st_CPUTRACK_REGS s_chain;
static int s_chain_valid = 0;               /**< 0 = řetěz přerušen (filtr PC, zahozená smyčka) */
static st_CPUTRACK_REGS s_collapse_regs;    /**< Stav po poslední iteraci rozpracované sbalené smyčky */

/* Počáteční stav posledního segmentu (manifest initial_regs, trace_status). */
static st_CPUTRACK_REGS s_initial_regs;
static uint8_t s_initial_q = 0;
static int s_initial_valid = 0;

static void cputrack_v2_on_instruction ( uint16_t pc, uint8_t insn_tstates,
                                         uint32_t wait_extra_tstates );
static void cputrack_v3_on_instruction ( uint16_t pc, uint8_t t_std, uint32_t wait );
static void cputrack_v3_on_interrupt_accept ( uint16_t isr_pc, uint8_t t );
static void cputrack_v3_flush_collapse ( void );
static void cputrack_v3_emit_sync ( const st_CPUTRACK_REGS *cur, unsigned reason );

/**
 * @brief Sestavit jeden 12-byte event záznam (formát v2) a appendovat do writeru.
 *
 * @param pc          Adresa instrukce.
 * @param insn_len    Počet platných bajtů v @p insn_bytes (1-4).
 * @param insn_bytes  Bajty instrukce z fetch.
 * @param wait_clk    WAIT T-stavy navíc (u sbaleného záznamu celý čas smyčky).
 * @param t_std       Standardní délka instrukce v T-stavech (bajt 11; 0 u
 *                    sbaleného záznamu HALT / smyčky na sebe).
 * @return 0 OK, -1 writer zavřený nebo chyba zápisu.
 */
static int emit_event ( uint16_t pc, uint8_t insn_len, const uint8_t *insn_bytes,
                        uint32_t wait_clk, uint8_t t_std )
{
    if ( !s_writer_open ) return -1;

    uint8_t buf[ 12 ];
    buf[ 0 ] = (uint8_t)( pc & 0xFF );
    buf[ 1 ] = (uint8_t)( ( pc >> 8 ) & 0xFF );
    buf[ 2 ] = insn_len;
    /* insn_bytes - copy first insn_len, zero zbytek do 4 B */
    buf[ 3 ] = ( insn_len >= 1 ) ? insn_bytes[ 0 ] : 0;
    buf[ 4 ] = ( insn_len >= 2 ) ? insn_bytes[ 1 ] : 0;
    buf[ 5 ] = ( insn_len >= 3 ) ? insn_bytes[ 2 ] : 0;
    buf[ 6 ] = ( insn_len >= 4 ) ? insn_bytes[ 3 ] : 0;
    buf[ 7 ] = (uint8_t)( wait_clk & 0xFF );
    buf[ 8 ] = (uint8_t)( ( wait_clk >> 8 ) & 0xFF );
    buf[ 9 ] = (uint8_t)( ( wait_clk >> 16 ) & 0xFF );
    buf[ 10 ] = (uint8_t)( ( wait_clk >> 24 ) & 0xFF );
    buf[ 11 ] = t_std;

    return tlog_writer_append ( &s_writer, buf, sizeof ( buf ) );
}


/**
 * @brief Zachytit stav registrů CPU (g_mzarch_main.cpu).
 *
 * @param o  Výstup; @c pad = 0, takže se dá porovnat memcmp.
 */
static void regs_capture ( st_CPUTRACK_REGS *o )
{
    const z80_t *c = g_mzarch_main.cpu;
    *o = (st_CPUTRACK_REGS) {
        .pc = c->pc, .af = c->af.w, .bc = c->bc.w, .de = c->de.w, .hl = c->hl.w,
        .ix = c->ix.w, .iy = c->iy.w, .sp = c->sp, .wz = c->wz.w,
        .af2 = c->af2.w, .bc2 = c->bc2.w, .de2 = c->de2.w, .hl2 = c->hl2.w,
        .i = c->i, .r = c->r,
        .flags = (uint8_t) ( ( c->iff1 ? CPUTRACK_REGS_F_IFF1 : 0u )
                             | ( c->iff2 ? CPUTRACK_REGS_F_IFF2 : 0u )
                             | ( ( (unsigned) c->im & 3u ) << CPUTRACK_REGS_F_IM_SHIFT )
                             | ( c->halted ? CPUTRACK_REGS_F_HALTED : 0u ) ),
        .pad = 0,
    };
}


/** @brief Zapsat uint16_t LE do bufferu záznamu. */
static inline void put16 ( uint8_t *p, uint16_t v )
{
    p[ 0 ] = (uint8_t) ( v & 0xFF );
    p[ 1 ] = (uint8_t) ( v >> 8 );
}


/**
 * @brief Sestavit jeden 32B záznam formátu v3 a appendovat do writeru.
 *
 * Bajty 0-11 jako emit_event() (v2), za nimi stav @p r. U synchronizačního
 * záznamu (@p insn_len == CPUTRACK_INSN_LEN_SYNC) nesou bajty 3-10
 * alternativní sadu z @p r a bajt 11 je 0 (@p insn_bytes, @p wait_clk
 * a @p t_std se ignorují).
 *
 * @param pc          Adresa instrukce (u synchronizace PC příští instrukce).
 * @param insn_len    0 (přijetí přerušení), 1-4, nebo CPUTRACK_INSN_LEN_SYNC.
 * @param insn_bytes  Bajty instrukce (aspoň @p insn_len, u 0 a SYNC nečteno).
 * @param wait_clk    WAIT T-stavy (u sbaleného záznamu celý čas smyčky).
 * @param t_std       Standardní T-stavy (0 u sbaleného záznamu).
 * @param r           Stav CPU po události záznamu.
 * @param kind        Bajt 31: CPUTRACK_KIND_*, důvod synchronizace,
 *                    CPUTRACK_FLAG_NO_BEFORE.
 * @return 0 OK, -1 writer zavřený nebo chyba zápisu.
 */
static int emit_event_v3 ( uint16_t pc, uint8_t insn_len, const uint8_t *insn_bytes,
                           uint32_t wait_clk, uint8_t t_std,
                           const st_CPUTRACK_REGS *r, uint8_t kind )
{
    if ( !s_writer_open ) return -1;

    uint8_t buf[ CPUTRACK_RECORD_SIZE_REGS ];
    put16 ( buf, pc );
    buf[ 2 ] = insn_len;
    if ( insn_len == CPUTRACK_INSN_LEN_SYNC ) {
        put16 ( buf + 3, r->af2 );
        put16 ( buf + 5, r->bc2 );
        put16 ( buf + 7, r->de2 );
        put16 ( buf + 9, r->hl2 );
        buf[ 11 ] = 0;
    } else {
        for ( unsigned i = 0; i < 4; i++ ) buf[ 3 + i ] = ( i < insn_len ) ? insn_bytes[ i ] : 0;
        buf[ 7 ] = (uint8_t) ( wait_clk & 0xFF );
        buf[ 8 ] = (uint8_t) ( ( wait_clk >> 8 ) & 0xFF );
        buf[ 9 ] = (uint8_t) ( ( wait_clk >> 16 ) & 0xFF );
        buf[ 10 ] = (uint8_t) ( ( wait_clk >> 24 ) & 0xFF );
        buf[ 11 ] = t_std;
    }
    uint8_t *p = buf + CPUTRACK_V3_OFF_REGS;
    put16 ( p + 0, r->af );
    put16 ( p + 2, r->bc );
    put16 ( p + 4, r->de );
    put16 ( p + 6, r->hl );
    put16 ( p + 8, r->ix );
    put16 ( p + 10, r->iy );
    put16 ( p + 12, r->sp );
    put16 ( p + 14, r->wz );
    buf[ CPUTRACK_V3_OFF_I ] = r->i;
    buf[ CPUTRACK_V3_OFF_R ] = r->r;
    buf[ CPUTRACK_V3_OFF_FLAGS ] = r->flags;
    buf[ CPUTRACK_V3_OFF_KIND ] = kind;

    return tlog_writer_append ( &s_writer, buf, sizeof ( buf ) );
}


/**
 * @brief Read callback pro z80ex_dasm: bajty z fetch, zbytek z paměti.
 *
 * Bajty, které CPU při fetch instrukce na @p user_data adrese skutečně
 * načetlo (g_debugger_insn_fetch), mají přednost; adresy za nimi se čtou
 * z aktuální mapy (debugger_dasm_read_cb).
 *
 * @param addr      Adresa čteného bajtu.
 * @param user_data Ukazatel na uint16_t adresu začátku instrukce.
 * @return Bajt instrukce.
 */
static uint8_t insn_byte_read_cb ( uint16_t addr, void *user_data )
{
    uint16_t pc = *(const uint16_t *) user_data;
    uint16_t off = (uint16_t) ( addr - pc );
    if ( g_debugger_insn_fetch.addr == pc && off < g_debugger_insn_fetch.count ) {
        return g_debugger_insn_fetch.byte[ off ];
    }
    return debugger_dasm_read_cb ( addr, NULL );
}


/**
 * @brief Pomocná - zjistit délku a bajty právě vykonané instrukce.
 *
 * Vrací reálný počet bajtů (1-4). Bajty bere z g_debugger_insn_fetch
 * (= co CPU načetlo při fetch; cputrack si vynucuje logovací čtecí
 * callback, viz TEST_DEBUGGER_NEED_DEBUG_CALLBACKS), takže sedí i u
 * instrukce, která přepíše vlastní bajty nebo přepne banking pod sebou.
 * Chybějící bajty (fetch je nenačetl, např. instrukce přerušená, nebo
 * buffer patří jiné adrese) doplní z aktuální mapy paměti.
 *
 * @param pc      Adresa začátku instrukce.
 * @param out_buf Výstup, 4 bajty (za délkou nuly).
 * @return Délka instrukce 1-4.
 */
static uint8_t read_insn_bytes ( uint16_t pc, uint8_t out_buf[ 4 ] )
{
    /* Délku instrukce zjistíme dasm metodou - máme z80ex_dasm. */
    char mnemonic[ 64 ];
    int t_states, t_states2;
    extern unsigned z80ex_dasm ( char *output, unsigned output_size, int eindex,
                                 int *t_states, int *t_states2,
                                 uint8_t (*read_cb)( uint16_t addr, void *user_data ),
                                 uint16_t base_addr, void *user_data );
    unsigned len = z80ex_dasm ( mnemonic, sizeof ( mnemonic ) - 1, 0,
                                &t_states, &t_states2,
                                insn_byte_read_cb, pc, &pc );
    if ( len < 1 ) len = 1;
    if ( len > 4 ) len = 4;
    for ( unsigned i = 0; i < len; i++ ) {
        out_buf[ i ] = insn_byte_read_cb ( (uint16_t)( pc + i ), &pc );
    }
    for ( unsigned i = len; i < 4; i++ ) out_buf[ i ] = 0;
    return (uint8_t)len;
}


/**
 * @brief Add accumulated T-states do pending_wait_clk se saturací na 0xFFFFFFFF.
 */
static void accumulate_wait ( uint8_t tstates )
{
    if ( s_pending_wait_clk + tstates >= 0xFFFFFFFFu ) {
        s_pending_wait_clk = 0xFFFFFFFFu;
    } else {
        s_pending_wait_clk += tstates;
    }
}


void cputrack_reset_collapse_state ( void )
{
    /* v3: zahozená sbalená smyčka mohla změnit registry (R, u LDIR i HL,
     * DE, BC); řetěz obnoví synchronizační záznam z cputrack_sync_check(). */
    if ( s_pending_wait_clk > 0 ) s_chain_valid = 0;
    s_prev_pc = 0xFFFF;
    s_prev_pc_valid = 0;
    s_pending_wait_clk = 0;
    s_collapse_pc = 0;
    s_collapse_insn_len = 1;
    memset ( s_collapse_insn_bytes, 0, 4 );
}


/**
 * @brief Standardní délka instrukce (bez WAIT) saturovaná do uint8_t.
 *
 * insn_tstates po z80_step zahrnuje i přidané WAIT cykly. Pro cputrack
 * chceme "standard length" (= ISA-derived, bez WAIT). standard_t =
 * insn_tstates - wait_extra_tstates. Saturujeme do uint8_t (Z80 ISA max
 * ~23 T-states).
 */
static inline uint8_t cputrack_standard_t ( uint32_t insn_tstates, uint32_t wait_extra_tstates )
{
    unsigned standard_t = (unsigned) insn_tstates;
    if ( standard_t >= wait_extra_tstates ) {
        standard_t -= wait_extra_tstates;
    } else {
        standard_t = 0;
    }
    if ( standard_t > 0xFFu ) standard_t = 0xFFu;
    return (uint8_t) standard_t;
}


/**
 * @brief Cold-path obal hot-path hooku - viz @ref cputrack_hot_path_hook.
 *
 * Atribut @c noinline drží tělo @ref mzarch_main_emulator_run kompaktní:
 * range-scope filtr (@c pc_range_lo / @c pc_range_hi), odečet WAIT cyklů
 * a volání @ref cputrack_on_instruction_complete žijí zde, mimo hot smyčku.
 * V OFF stavu (g_cputrack_active==0) se sem nikdy nezavolá. Segment v3
 * předá práci cputrack_v3_on_instruction() (filtr PC tam drží řetěz
 * registrů).
 *
 * Logika v2 je bit-identická s původním in-line kódem hot smyčky (Z17b),
 * jen fyzicky přesunutá - žádná změna chování (Gate A: framebuffer/trace
 * beze změny).
 */
__attribute__((noinline))
void cputrack_hot_path_hook ( uint16_t pc,
                              uint32_t insn_tstates,
                              uint32_t wait_extra_tstates )
{
    /* Segment s registry: vlastní cesta (filtr PC s udržením řetězu). */
    if ( s_seg_regs ) {
        cputrack_v3_on_instruction ( pc, cputrack_standard_t ( insn_tstates, wait_extra_tstates ),
                                     wait_extra_tstates );
        return;
    }

    /* Z17b range-scoped tracking: nahrávej jen instrukce, jejichž PC leží
     * v [pc_range_lo, pc_range_hi] (oboustranně uzavřeno). Default lo=0/
     * hi=0xFFFF pokrývá celý prostor (= bez filtru). */
    if ( pc < g_cputrack_config.pc_range_lo ||
         pc > g_cputrack_config.pc_range_hi ) {
        return;
    }

    cputrack_v2_on_instruction ( pc,
                                 cputrack_standard_t ( insn_tstates, wait_extra_tstates ),
                                 wait_extra_tstates );
}


void cputrack_on_instruction_complete ( uint16_t pc,
                                        uint8_t insn_tstates,
                                        uint32_t wait_extra_tstates )
{
    if ( s_seg_regs ) {
        cputrack_v3_on_instruction ( pc, insn_tstates, wait_extra_tstates );
    } else {
        cputrack_v2_on_instruction ( pc, insn_tstates, wait_extra_tstates );
    }
}


/**
 * @brief Záznam instrukce ve formátu v2 (12 B) včetně sbalení smyček.
 *
 * Tělo dřívější cputrack_on_instruction_complete(); filtr PC uplatnil
 * volající. Parametry viz cputrack_on_instruction_complete().
 */
static void cputrack_v2_on_instruction ( uint16_t pc,
                                         uint8_t insn_tstates,
                                         uint32_t wait_extra_tstates )
{
    if ( !s_writer_open ) return;

    /* Nový PC po z80_step() - zjistíme z CPU. Caller předal pc = adresu
     * právě vykonané instrukce (= PC PŘED stepem = instruction_addr).
     * Aktuální cpu->pc je PC PO stepu (target další instrukce). */
    uint16_t new_pc = (uint16_t) g_mzarch_main.cpu->pc;

    if ( s_prev_pc_valid && new_pc == pc ) {
        /* Self-loop / HALT detection: PC nedrobil = stejná instrukce
         * se znova vykoná. Suppress emit, akumuluj T-states. */
        if ( s_pending_wait_clk == 0 ) {
            /* První iterace collapse - zapamatuj insn_bytes. */
            s_collapse_pc = pc;
            s_collapse_insn_len = read_insn_bytes ( pc, s_collapse_insn_bytes );
        }
        accumulate_wait ( insn_tstates );
        if ( wait_extra_tstates > 0 ) {
            uint64_t sum = (uint64_t) s_pending_wait_clk + wait_extra_tstates;
            s_pending_wait_clk = ( sum > 0xFFFFFFFFu ) ? 0xFFFFFFFFu : (uint32_t) sum;
        }
        s_prev_pc = pc;
        return;
    }

    /* PC se změnilo (nebo první instr) - flush případný pending collapse. */
    if ( s_pending_wait_clk > 0 ) {
        uint32_t w = ( s_pending_wait_clk > 0xFFFFFFFFu ) ?
                     0xFFFFFFFFu : (uint32_t) s_pending_wait_clk;
        emit_event ( s_collapse_pc, s_collapse_insn_len, s_collapse_insn_bytes, w, 0 );
        s_pending_wait_clk = 0;
    }

    /* Emit běžný event pro právě vykonanou instr. */
    uint8_t insn_bytes[ 4 ];
    uint8_t insn_len = read_insn_bytes ( pc, insn_bytes );
    emit_event ( pc, insn_len, insn_bytes, wait_extra_tstates, insn_tstates );

    s_prev_pc = pc;
    s_prev_pc_valid = 1;
}


void cputrack_on_interrupt_accept ( uint16_t isr_pc, uint32_t ack_tstates )
{
    if ( !s_writer_open ) return;
    if ( s_seg_regs ) {
        cputrack_v3_on_interrupt_accept ( isr_pc, ( ack_tstates > 0xFFu ) ? 0xFFu : (uint8_t) ack_tstates );
        return;
    }
    if ( isr_pc < g_cputrack_config.pc_range_lo ||
         isr_pc > g_cputrack_config.pc_range_hi ) {
        return;
    }

    /* Sbalená smyčka (HALT, JR $), kterou přerušení ukončilo, patří před
     * přijetí. */
    if ( s_pending_wait_clk > 0 ) {
        uint32_t w = ( s_pending_wait_clk > 0xFFFFFFFFu ) ?
                     0xFFFFFFFFu : (uint32_t) s_pending_wait_clk;
        emit_event ( s_collapse_pc, s_collapse_insn_len, s_collapse_insn_bytes, w, 0 );
        s_pending_wait_clk = 0;
    }

    static const uint8_t no_bytes[ 4 ] = { 0, 0, 0, 0 };
    uint8_t t = ( ack_tstates > 0xFFu ) ? 0xFFu : (uint8_t) ack_tstates;
    emit_event ( isr_pc, 0, no_bytes, 0, t );
    /* Další instrukce (první v obsluze) se nesmí považovat za opakování. */
    s_prev_pc_valid = 0;
}


/* ===========================================================================
 *  Formát v3: záznam s registry a řetěz stavu
 * =========================================================================== */

/** @brief Leží PC v rozsahu filtru (pc_range_lo..pc_range_hi včetně)? */
static inline int cputrack_pc_in_range ( uint16_t pc )
{
    return pc >= g_cputrack_config.pc_range_lo && pc <= g_cputrack_config.pc_range_hi;
}


/** @brief Bajt 31 se stavem řetězu (CPUTRACK_FLAG_NO_BEFORE při přerušeném). */
static inline uint8_t cputrack_v3_kind ( unsigned kind )
{
    return (uint8_t) ( kind | ( s_chain_valid ? 0u : CPUTRACK_FLAG_NO_BEFORE ) );
}


/**
 * @brief Zapsat rozpracovaný sbalený záznam (v3) se stavem po poslední iteraci.
 *
 * @post Bez rozpracované smyčky no-op; jinak je řetěz platný a nese stav
 *       po poslední sbalené iteraci.
 */
static void cputrack_v3_flush_collapse ( void )
{
    if ( s_pending_wait_clk == 0 ) return;
    uint32_t w = ( s_pending_wait_clk > 0xFFFFFFFFu ) ?
                 0xFFFFFFFFu : (uint32_t) s_pending_wait_clk;
    emit_event_v3 ( s_collapse_pc, s_collapse_insn_len, s_collapse_insn_bytes, w, 0,
                    &s_collapse_regs, cputrack_v3_kind ( CPUTRACK_KIND_LOOP ) );
    s_pending_wait_clk = 0;
    s_chain = s_collapse_regs;
    s_chain_valid = 1;
}


/**
 * @brief Zapsat synchronizační záznam (v3) s aktuálním stavem.
 *
 * @param cur     Aktuální stav CPU.
 * @param reason  CPUTRACK_SYNC_*.
 * @pre Rozpracovaná sbalená smyčka je zapsaná (cputrack_v3_flush_collapse()).
 * @post Řetěz je platný a nese @p cur; další instrukce se nepovažuje za
 *       opakování předchozí.
 */
static void cputrack_v3_emit_sync ( const st_CPUTRACK_REGS *cur, unsigned reason )
{
    emit_event_v3 ( cur->pc, CPUTRACK_INSN_LEN_SYNC, NULL, 0, 0, cur,
                    (uint8_t) ( CPUTRACK_KIND_SYNC | ( reason << CPUTRACK_SYNC_REASON_SHIFT ) ) );
    s_chain = *cur;
    s_chain_valid = 1;
    s_prev_pc_valid = 0;
}


/**
 * @brief Záznam instrukce ve formátu v3 (filtr PC, sbalení smyček, řetěz).
 *
 * Instrukce mimo filtr se nezapíše a přeruší řetěz; vede-li do rozsahu
 * filtru (PC po kroku v něm), zapíše se synchronizační záznam
 * @ref CPUTRACK_SYNC_PC_RANGE, takže i první instrukce v rozsahu má známý
 * stav před. Opakování instrukce na stejném PC (HALT, smyčka na sebe) se
 * sbalí jako ve v2; sbalený záznam nese stav po poslední iteraci.
 *
 * @param pc      Adresa vykonané instrukce.
 * @param t_std   Standardní T-stavy instrukce (bez WAIT).
 * @param wait    WAIT T-stavy navíc.
 */
static void cputrack_v3_on_instruction ( uint16_t pc, uint8_t t_std, uint32_t wait )
{
    if ( !s_writer_open ) return;

    st_CPUTRACK_REGS cur;
    regs_capture ( &cur );
    uint16_t new_pc = cur.pc;

    if ( !cputrack_pc_in_range ( pc ) ) {
        cputrack_v3_flush_collapse ( );
        s_chain_valid = 0;
        s_prev_pc_valid = 0;
        if ( cputrack_pc_in_range ( new_pc ) ) {
            cputrack_v3_emit_sync ( &cur, CPUTRACK_SYNC_PC_RANGE );
        }
        return;
    }

    if ( s_prev_pc_valid && new_pc == pc ) {
        /* Smyčka na sebe / HALT: akumulace jako ve v2, registry po iteraci. */
        if ( s_pending_wait_clk == 0 ) {
            s_collapse_pc = pc;
            s_collapse_insn_len = read_insn_bytes ( pc, s_collapse_insn_bytes );
        }
        accumulate_wait ( t_std );
        if ( wait > 0 ) {
            uint64_t sum = (uint64_t) s_pending_wait_clk + wait;
            s_pending_wait_clk = ( sum > 0xFFFFFFFFu ) ? 0xFFFFFFFFu : (uint32_t) sum;
        }
        s_collapse_regs = cur;
        s_prev_pc = pc;
        return;
    }

    cputrack_v3_flush_collapse ( );

    uint8_t insn_bytes[ 4 ];
    uint8_t insn_len = read_insn_bytes ( pc, insn_bytes );
    emit_event_v3 ( pc, insn_len, insn_bytes, wait, t_std, &cur,
                    cputrack_v3_kind ( CPUTRACK_KIND_INSN ) );
    s_chain = cur;
    s_chain_valid = 1;
    s_prev_pc = pc;
    s_prev_pc_valid = 1;
}


/**
 * @brief Záznam přijetí přerušení ve formátu v3.
 *
 * Obsluha mimo filtr PC: zapíše se jen rozpracovaná sbalená smyčka
 * a řetěz se přeruší (obnoví ho návrat do rozsahu). Jinak záznam
 * `insn_length` 0 se stavem po přijetí.
 *
 * @param isr_pc  PC po přijetí (adresa obsluhy).
 * @param t       T-stavy přijetí (saturované).
 */
static void cputrack_v3_on_interrupt_accept ( uint16_t isr_pc, uint8_t t )
{
    st_CPUTRACK_REGS cur;
    regs_capture ( &cur );
    cputrack_v3_flush_collapse ( );
    s_prev_pc_valid = 0;
    if ( !cputrack_pc_in_range ( isr_pc ) ) {
        s_chain_valid = 0;
        return;
    }
    static const uint8_t no_bytes[ 4 ] = { 0, 0, 0, 0 };
    emit_event_v3 ( isr_pc, 0, no_bytes, 0, t, &cur, cputrack_v3_kind ( CPUTRACK_KIND_INT ) );
    s_chain = cur;
    s_chain_valid = 1;
}


void cputrack_sync_check ( void )
{
    if ( !s_writer_open || !s_seg_regs ) return;

    st_CPUTRACK_REGS cur;
    regs_capture ( &cur );

    if ( s_pending_wait_clk > 0 ) {
        /* Smyčka pokračuje beze změny (typicky pauza v HALT). */
        if ( memcmp ( &cur, &s_collapse_regs, sizeof ( cur ) ) == 0 ) return;
        cputrack_v3_flush_collapse ( );
    }
    if ( s_chain_valid ) {
        if ( memcmp ( &cur, &s_chain, sizeof ( cur ) ) == 0 ) return;
    } else if ( !cputrack_pc_in_range ( cur.pc ) ) {
        /* Řetěz přerušil filtr a příští instrukce je mimo rozsah: obnoví
         * ho až návrat do rozsahu (jinak by každý příkaz dbgapi za běhu
         * mimo rozsah zapsal synchronizační záznam). */
        return;
    }
    cputrack_v3_emit_sync ( &cur, CPUTRACK_SYNC_WRITE );
}


int cputrack_is_recording ( void )
{
    return s_writer_open ? 1 : 0;
}


int cputrack_segment_has_regs ( void )
{
    return ( s_writer_open && s_seg_regs ) ? 1 : 0;
}


int cputrack_get_initial_regs ( st_CPUTRACK_REGS *out, uint8_t *q )
{
    if ( !s_initial_valid ) return 0;
    *out = s_initial_regs;
    if ( q ) *q = s_initial_q;
    return 1;
}


/* ===========================================================================
 *  Header / RAM dumps
 * =========================================================================== */

/**
 * @brief Zapsat pomocný binární soubor.
 */
static int write_bin_file ( const char *dir, const char *basename,
                            const char *suffix, const void *data, size_t n )
{
    char fname[ 256 ];
    g_snprintf ( fname, sizeof ( fname ), "%s_%s.bin", basename, suffix );
    char *path = g_build_filename ( dir, fname, NULL );

    FILE *fp = g_fopen ( path, "wb" );
    if ( !fp ) {
        fprintf ( stderr, "[trace-suite] cputrack: cannot open '%s' for writing: %s\n",
                  path, g_strerror ( errno ) );
        g_free ( path );
        return -1;
    }
    size_t wrote = fwrite ( data, 1, n, fp );
    int err = ferror ( fp );
    fclose ( fp );
    int rc = ( wrote == n && !err ) ? 0 : -1;
    if ( rc != 0 ) {
        fprintf ( stderr, "[trace-suite] cputrack: short write to '%s' (%zu/%zu)\n",
                  path, wrote, n );
    } else {
        /* 0019 v3: initial-state dump (RAM/VRAM/EXVRAM/memext) reálně zapsán
         * -> kumulativní disk čítač (byte backstop). Per fire ~640 KB, takže
         * undercount byl jen kvůli g_stat na index souboru (viz V19). */
        tlog_common_disk_bytes_add ( (uint64_t) wrote );
    }
    g_free ( path );
    return rc;
}


/**
 * @brief Zapsat všechny initial memory dumps.
 */
static int write_initial_memory_dumps ( const char *dir, const char *basename )
{
    int rc = 0;
    rc |= write_bin_file ( dir, basename, "initial_ram",
                           g_memory.RAM, MEMORY_SIZE_RAM );
    rc |= write_bin_file ( dir, basename, "initial_vram",
                           g_memory.VRAM, MEMORY_SIZE_VRAM );
#if MZARCH == 800
    rc |= write_bin_file ( dir, basename, "initial_exvram",
                           g_memory.EXVRAM, MEMORY_SIZE_VRAM );
#elif MZARCH == 1500
    rc |= write_bin_file ( dir, basename, "initial_pcg",
                           g_memory.PCG, MEMORY_SIZE_PCG );
#endif
    if ( MEMEXT_TEST_CONNECTED ) {
        rc |= write_bin_file ( dir, basename, "initial_memext",
                               g_memext.RAM, MEMEXT_RAM_SIZE );
    }
    /* Ramdisky - TODO: ramdisk dump v navazující fázi (zatím jen RAM). */
    return rc;
}


/**
 * @brief Sestavit subsys_header JSON fragment (initial state CPU regs).
 *
 * Caller g_free() na vrácený string.
 */
static char *build_subsys_header_json ( void )
{
    z80_t *cpu = g_mzarch_main.cpu;
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

    json_builder_set_member_name ( b, "initial_regs" );
    json_builder_begin_object ( b );

#define ADD_REG_INT( name_str, value_expr ) \
    do { json_builder_set_member_name ( b, name_str ); \
         json_builder_add_int_value ( b, (gint64) ( value_expr ) ); } while (0)

    ADD_REG_INT ( "AF",     z80_get_reg ( cpu, Z80_REG_AF ) );
    ADD_REG_INT ( "BC",     z80_get_reg ( cpu, Z80_REG_BC ) );
    ADD_REG_INT ( "DE",     z80_get_reg ( cpu, Z80_REG_DE ) );
    ADD_REG_INT ( "HL",     z80_get_reg ( cpu, Z80_REG_HL ) );
    ADD_REG_INT ( "AF_alt", z80_get_reg ( cpu, Z80_REG_AF2 ) );
    ADD_REG_INT ( "BC_alt", z80_get_reg ( cpu, Z80_REG_BC2 ) );
    ADD_REG_INT ( "DE_alt", z80_get_reg ( cpu, Z80_REG_DE2 ) );
    ADD_REG_INT ( "HL_alt", z80_get_reg ( cpu, Z80_REG_HL2 ) );
    ADD_REG_INT ( "IX",     z80_get_reg ( cpu, Z80_REG_IX ) );
    ADD_REG_INT ( "IY",     z80_get_reg ( cpu, Z80_REG_IY ) );
    ADD_REG_INT ( "SP",     z80_get_reg ( cpu, Z80_REG_SP ) );
    ADD_REG_INT ( "PC",     z80_get_reg ( cpu, Z80_REG_PC ) );
    ADD_REG_INT ( "WZ",     z80_get_reg ( cpu, Z80_REG_WZ ) );
    ADD_REG_INT ( "IR",     z80_get_reg ( cpu, Z80_REG_IR ) );
    ADD_REG_INT ( "I",      cpu->i );
    ADD_REG_INT ( "R",      cpu->r );
    ADD_REG_INT ( "IM",     cpu->im );
    ADD_REG_INT ( "IFF1",   cpu->iff1 );
    ADD_REG_INT ( "IFF2",   cpu->iff2 );
    ADD_REG_INT ( "Q",      cpu->q );
    ADD_REG_INT ( "halted", (unsigned) cpu->halted );

#undef ADD_REG_INT
    json_builder_end_object ( b );

    char fname_buf[256];
    g_snprintf ( fname_buf, sizeof ( fname_buf ),
                 "%s_initial_ram.bin", g_cputrack_config.name );
    json_builder_set_member_name ( b, "initial_ram_file" );
    json_builder_add_string_value ( b, fname_buf );

    g_snprintf ( fname_buf, sizeof ( fname_buf ),
                 "%s_initial_vram.bin", g_cputrack_config.name );
    json_builder_set_member_name ( b, "initial_vram_file" );
    json_builder_add_string_value ( b, fname_buf );

#if MZARCH == 800
    g_snprintf ( fname_buf, sizeof ( fname_buf ),
                 "%s_initial_exvram.bin", g_cputrack_config.name );
    json_builder_set_member_name ( b, "initial_exvram_file" );
    json_builder_add_string_value ( b, fname_buf );
#elif MZARCH == 1500
    g_snprintf ( fname_buf, sizeof ( fname_buf ),
                 "%s_initial_pcg.bin", g_cputrack_config.name );
    json_builder_set_member_name ( b, "initial_pcg_file" );
    json_builder_add_string_value ( b, fname_buf );
#endif

    json_builder_set_member_name ( b, "initial_memext_file" );
    if ( MEMEXT_TEST_CONNECTED ) {
        g_snprintf ( fname_buf, sizeof ( fname_buf ),
                     "%s_initial_memext.bin", g_cputrack_config.name );
        json_builder_add_string_value ( b, fname_buf );
        json_builder_set_member_name ( b, "memext_size" );
        json_builder_add_int_value ( b, (gint64) MEMEXT_RAM_SIZE );
    } else {
        json_builder_add_null_value ( b );
    }

    json_builder_set_member_name ( b, "event_record_size" );
    json_builder_add_int_value ( b, s_seg_regs ? CPUTRACK_RECORD_SIZE_REGS : TLOG_RECORD_SIZE_CPUTRACK );

    /* Formát záznamu v2: bajt 11 = standardní T-stavy instrukce; v3 navíc
     * registry po události a synchronizační záznamy (cputrack.h). */
    json_builder_set_member_name ( b, "event_record_version" );
    json_builder_add_int_value ( b, s_seg_regs ? CPUTRACK_RECORD_VERSION_REGS : CPUTRACK_RECORD_VERSION );

    json_builder_end_object ( b );

    JsonGenerator *gen = json_generator_new ( );
    JsonNode *root = json_builder_get_root ( b );
    json_generator_set_root ( gen, root );
    json_generator_set_pretty ( gen, TRUE );
    json_generator_set_indent ( gen, 2 );

    gsize len = 0;
    gchar *out = json_generator_to_data ( gen, &len );

    g_object_unref ( gen );
    if ( root ) json_node_unref ( root );
    g_object_unref ( b );

    return out;
}


/* ===========================================================================
 *  Lifecycle: start / stop / finalize
 * =========================================================================== */

int cputrack_start ( void )
{
    if ( s_writer_open ) return 0;
    if ( !g_cputrack_config.dir || !g_cputrack_config.dir[0]
         || !g_cputrack_config.name || !g_cputrack_config.name[0] ) {
        fprintf ( stderr, "[trace-suite] cputrack: empty dir or name, recording NOT started\n" );
        return -1;
    }

    /* Resolve dir relative to work_dir (output operation). */
    char *resolved_dir = sdlapp_paths_resolve_work ( g_sdlapp->paths,
                                                     g_cputrack_config.dir );

    uint64_t init_px = tlog_common_get_pxclk_total ( );
    uint64_t init_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t init_sc = tlog_common_get_screens_total ( );

    if ( tlog_writer_open ( &s_writer, "cputrack",
                            resolved_dir, g_cputrack_config.name,
                            g_cputrack_config.chunk_mb,
                            g_cputrack_config.max_total_mb,
                            init_px, init_cpu, init_sc ) != 0 ) {
        g_free ( resolved_dir );
        return -1;
    }
    s_writer_open = 1;
    /* Formát segmentu se volí jednou při startu (velikost záznamu je
     * v segmentu pevná). */
    s_seg_regs = g_cputrack_config.regs ? 1 : 0;
    tlog_writer_set_record_size ( &s_writer, s_seg_regs ? CPUTRACK_RECORD_SIZE_REGS
                                                       : TLOG_RECORD_SIZE_CPUTRACK );
    cputrack_reset_collapse_state ( );
    s_chain_valid = 0;
    regs_capture ( &s_initial_regs );
    s_initial_q = g_mzarch_main.cpu->q;
    s_initial_valid = 1;

    /* Memory dumps + manifest <name>.json initial. */
    write_initial_memory_dumps ( resolved_dir, g_cputrack_config.name );
    char *hdr = build_subsys_header_json ( );
    tlog_writer_update_meta ( &s_writer, hdr );
    g_free ( hdr );
    g_free ( resolved_dir );

    /* v3: první záznam segmentu nese počáteční stav (řetěz registrů). */
    if ( s_seg_regs ) {
        cputrack_v3_emit_sync ( &s_initial_regs, CPUTRACK_SYNC_START );
    }

    fprintf ( stderr, "[trace-suite] cputrack: recording started (dir='%s', name='%s'%s)\n",
              g_cputrack_config.dir, g_cputrack_config.name, s_seg_regs ? ", regs" : "" );
    return 0;
}


void cputrack_stop ( void )
{
    if ( !s_writer_open ) return;

    /* Flush případný pending collapse před close. */
    if ( s_seg_regs ) {
        cputrack_v3_flush_collapse ( );
    } else if ( s_pending_wait_clk > 0 ) {
        uint32_t w = ( s_pending_wait_clk > 0xFFFFFFFFu ) ?
                     0xFFFFFFFFu : (uint32_t) s_pending_wait_clk;
        emit_event ( s_collapse_pc, s_collapse_insn_len, s_collapse_insn_bytes, w, 0 );
        s_pending_wait_clk = 0;
    }

    uint64_t now_px = tlog_common_get_pxclk_total ( );
    uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t now_sc = tlog_common_get_screens_total ( );
    tlog_writer_close ( &s_writer, now_px, now_cpu, now_sc );

    /* Refresh meta s subsys_header (final). Pozn.: writer je už closed,
     * tedy ho znovu nepoužijeme - meta byl flushnut v close. */
    s_writer_open = 0;
    fprintf ( stderr, "[trace-suite] cputrack: recording stopped\n" );
}


void cputrack_finalize ( void )
{
    if ( g_cputrack_config.save_on_exit && s_writer_open ) {
        cputrack_stop ( );
    }
    g_free ( g_cputrack_config.dir );
    g_free ( g_cputrack_config.name );
    g_cputrack_config.dir = NULL;
    g_cputrack_config.name = NULL;
}


int cputrack_is_truncated ( void )
{
    /* Pokud writer není otevřený, recording neexistuje, tedy ani
     * "truncated" stav nedává smysl. */
    if ( !s_writer_open ) return 0;
    return tlog_writer_is_truncated ( &s_writer );
}


int cputrack_save_segment ( const char *path )
{
    /* Analogie mhmap_export, ale pro streamovaný tlog: chunk soubory se
     * zapisují průběžně do g_cputrack_config.dir/name. "Save" tedy znamená
     * uzavřít (flush+close) aktuální segment, čímž se finalizuje manifest <name>.json a
     * poslední částečný chunk na disku.
     *
     * Pokud je @p path zadané, přesměruje se dir+name z path PŘED restartem,
     * takže NÁSLEDNÝ segment streamuje do požadované lokace. Už zapsané chunk
     * soubory tím přejmenovány NEjsou - tlog writer váže jména při open. */
    int was_active = ( g_cputrack_active != 0 );

    /* Prázdná cesta = "force save now": flush rozdělaného segmentu na disk
     * (pending chunk + manifest <name>.json) BEZ uzavření/reopenu - segment pokračuje do
     * TÉHOŽ manifestu. Dřív se segment uzavřel a hned otevřel nový na stejném
     * jméně, který přepsal manifest <name>.json (chunks=[]) i initial_*.bin předchozího
     * segmentu; pokud pak nepřitekla žádná další data (hned trace_stop),
     * zůstala právě nahraná data osiřelá (mzdos 0022). */
    if ( !( path && path[ 0 ] ) ) {
        if ( s_writer_open ) {
            uint64_t now_px = tlog_common_get_pxclk_total ( );
            uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
            uint32_t now_sc = tlog_common_get_screens_total ( );
            tlog_writer_flush_chunk ( &s_writer, now_px, now_cpu, now_sc );
        }
        return 0;
    }

    /* Neprázdná cesta = přesměrování výstupu na NOVÝ segment: uzavřít aktuální
     * (finalizuje meta), přesměrovat dir+name, otevřít čerstvý segment. Už
     * zapsané chunk soubory tím přejmenovány NEjsou - tlog writer váže jména
     * při open. */
    if ( s_writer_open ) {
        cputrack_stop ( );
    }
    reclife_redirect_path ( &g_cputrack_config.dir, &g_cputrack_config.name, path );
    if ( was_active ) {
        return cputrack_start ( );
    }
    return 0;
}


/* ===========================================================================
 *  Mode logic + active recompute
 * =========================================================================== */

/* Sdílený lifecycle deskriptor (reclife). mode je první pole configu, tedy
 * &g_cputrack_config.mode == (int*)&g_cputrack_config; en_TLOG_MODE je
 * int-kompatibilní s en_RECLIFE_MODE (hodnoty 0/1/2). fn_reset = reset
 * collapse stavu, fn_save = uzavření/přesměrování segmentu. */
static const st_RECLIFE_DESC s_cputrack_reclife = {
    .subsys_name = "cputrack",
    .mode_ptr    = (int *) &g_cputrack_config.mode,
    .active_ptr  = &g_cputrack_active,
    .fn_start    = cputrack_start,
    .fn_stop     = cputrack_stop,
    .fn_reset    = cputrack_reset_collapse_state,
    .fn_save     = cputrack_save_segment,
};


void cputrack_recompute_active ( int debugger_active )
{
    reclife_recompute_active ( &s_cputrack_reclife, debugger_active );
    mzarch_post_insn_hooks_recompute ( );
}


/* ===========================================================================
 *  Init / config / CLI options
 * =========================================================================== */

void cputrack_init ( void )
{
    memset ( &g_cputrack_config, 0, sizeof ( g_cputrack_config ) );
    g_cputrack_config.mode = TLOG_MODE_OFF;
    g_cputrack_config.chunk_mb = TLOG_DEFAULT_CHUNK_MB;
    g_cputrack_config.max_total_mb = TLOG_DEFAULT_MAX_TOTAL_MB;
    g_cputrack_config.save_on_exit = 1;
    /* Range-scope filtr: výchozí celý adresový prostor (= bez filtru). */
    g_cputrack_config.pc_range_lo = 0x0000;
    g_cputrack_config.pc_range_hi = 0xFFFF;

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "TRACE_CPUTRACK" );
    CFGELM *elm;

    elm = cfgmodule_register_new_element ( cmod, "mode", CFGENTYPE_KEYWORD, TLOG_MODE_OFF,
                                           TLOG_MODE_OFF,         "OFF",
                                           TLOG_MODE_WITH_WINDOW, "WITH_WINDOW",
                                           TLOG_MODE_ALWAYS,      "ALWAYS",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_cputrack_config.mode, (void*) &g_cputrack_config.mode );

    elm = cfgmodule_register_new_element ( cmod, "dir", CFGENTYPE_TEXT, "trace-suite" );
    cfgelement_bind ( elm, (void*) &g_cputrack_config.dir );

    elm = cfgmodule_register_new_element ( cmod, "name", CFGENTYPE_TEXT, "cputrack" );
    cfgelement_bind ( elm, (void*) &g_cputrack_config.name );

    elm = cfgmodule_register_new_element ( cmod, "chunk_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_CHUNK_MB, 1, 4096 );
    cfgelement_set_handlers ( elm, (void*) &g_cputrack_config.chunk_mb, (void*) &g_cputrack_config.chunk_mb );

    elm = cfgmodule_register_new_element ( cmod, "max_total_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_MAX_TOTAL_MB, 0, 1048576 );
    cfgelement_set_handlers ( elm, (void*) &g_cputrack_config.max_total_mb, (void*) &g_cputrack_config.max_total_mb );

    elm = cfgmodule_register_new_element ( cmod, "save_on_exit", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_cputrack_config.save_on_exit, (void*) &g_cputrack_config.save_on_exit );

    /* Formát v3 s registry (32 B na záznam), platí od dalšího startu segmentu. */
    elm = cfgmodule_register_new_element ( cmod, "regs", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void*) &g_cputrack_config.regs, (void*) &g_cputrack_config.regs );

    /* Range-scope filtr - viz s_ini_pc_range_* (4 B mezičlánky, ořez po propagate).
     * Rozsah 0..0xFFFF, default lo=0 / hi=0xFFFF = celý adresový prostor (bez filtru). */
    elm = cfgmodule_register_new_element ( cmod, "pc_range_lo", CFGENTYPE_UNSIGNED, 0x0000, 0, 0xFFFF );
    cfgelement_set_handlers ( elm, (void*) &s_ini_pc_range_lo, (void*) &s_ini_pc_range_lo );

    elm = cfgmodule_register_new_element ( cmod, "pc_range_hi", CFGENTYPE_UNSIGNED, 0xFFFF, 0, 0xFFFF );
    cfgelement_set_handlers ( elm, (void*) &s_ini_pc_range_hi, (void*) &s_ini_pc_range_hi );

    /* Načíst hodnoty z .ini (pokud sekce existuje) a propagovat do g_cputrack_config.
     * Bez tohoto kroku zůstanou pole NULL (zejm. dir/name) i když cfgmodule_register
     * deklaruje default values - ty se aplikují až přes propagate. CLI override pak
     * běží nad propagovanými hodnotami v cputrack_apply_cli_options(). */
    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );

    /* Ořez 4 B INI mezičlánků do uint16_t config polí. */
    g_cputrack_config.pc_range_lo = (uint16_t) ( s_ini_pc_range_lo & 0xFFFF );
    g_cputrack_config.pc_range_hi = (uint16_t) ( s_ini_pc_range_hi & 0xFFFF );
}


void cputrack_apply_pc_range_live ( void )
{
    CFGMOD *cmod = cfgroot_get_module_by_name ( g_cfgmain, (char *) "TRACE_CPUTRACK" );
    if ( !cmod ) return;

    /* Aktuální hodnota elementu = to, co právě zapsal settings_set
     * (cfgelement_set_unsigned_value -> CFGELVAR_VALUE). Bound mezičlánky
     * s_ini_pc_range_* ani g_cputrack_config se přes settings_set samy
     * neaktualizují, proto čteme přímo z elementu a ořízneme na uint16_t. */
    unsigned lo = cfgmodule_get_element_unsigned_value_by_name ( cmod, (char *) "pc_range_lo" );
    unsigned hi = cfgmodule_get_element_unsigned_value_by_name ( cmod, (char *) "pc_range_hi" );

    g_cputrack_config.pc_range_lo = (uint16_t) ( lo & 0xFFFF );
    g_cputrack_config.pc_range_hi = (uint16_t) ( hi & 0xFFFF );
}


void cputrack_apply_cli_options ( void )
{
    const char *v;

    v = sdlapp_option_value ( "--cputrack-mode" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_cputrack_config.mode = TLOG_MODE_OFF;
        else if ( g_ascii_strcasecmp ( v, "window" ) == 0 ) g_cputrack_config.mode = TLOG_MODE_WITH_WINDOW;
        else if ( g_ascii_strcasecmp ( v, "always" ) == 0 ) g_cputrack_config.mode = TLOG_MODE_ALWAYS;
        else fprintf ( stderr, "Invalid --cputrack-mode value: %s (expected off|window|always)\n", v );
    }
    v = sdlapp_option_value ( "--cputrack-dir" );
    if ( v ) {
        char *new_dir = g_strdup ( v );
        if ( new_dir ) {
            g_free ( g_cputrack_config.dir );
            g_cputrack_config.dir = new_dir;
        }
    }
    v = sdlapp_option_value ( "--cputrack-name" );
    if ( v ) {
        char *new_name = g_strdup ( v );
        if ( new_name ) {
            g_free ( g_cputrack_config.name );
            g_cputrack_config.name = new_name;
        }
    }
    v = sdlapp_option_value ( "--cputrack-chunk-mb" );
    if ( v ) g_cputrack_config.chunk_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--cputrack-max-total-mb" );
    if ( v ) g_cputrack_config.max_total_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--cputrack-save-on-exit" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "on" ) == 0 ) g_cputrack_config.save_on_exit = 1;
        else if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_cputrack_config.save_on_exit = 0;
        else fprintf ( stderr, "Invalid --cputrack-save-on-exit value: %s (expected on|off)\n", v );
    }
    v = sdlapp_option_value ( "--cputrack-regs" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "on" ) == 0 ) g_cputrack_config.regs = 1;
        else if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_cputrack_config.regs = 0;
        else fprintf ( stderr, "Invalid --cputrack-regs value: %s (expected on|off)\n", v );
    }
    /* Range-scope filtr. Akceptuje dec i hex (0x prefix). Hodnota se ořízne
     * na 16 bitů (adresový prostor Z80). */
    v = sdlapp_option_value ( "--cputrack-pc-lo" );
    if ( v ) g_cputrack_config.pc_range_lo = (uint16_t) ( g_ascii_strtoull ( v, NULL, 0 ) & 0xFFFF );
    v = sdlapp_option_value ( "--cputrack-pc-hi" );
    if ( v ) g_cputrack_config.pc_range_hi = (uint16_t) ( g_ascii_strtoull ( v, NULL, 0 ) & 0xFFFF );
}

const st_TLOG_WRITER *cputrack_get_writer ( void )
{
    return &s_writer;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
