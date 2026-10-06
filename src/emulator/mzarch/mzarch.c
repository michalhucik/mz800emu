#include "main.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "iface/iface_audio.h"

#include "emulator.h"
#include "emulator_measuring.h"
#include "customspeed.h"
#include "bootstrap.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "mzarch.h"
#include "mzarch_platform.h"
#include "libs/dasm-z80/z80_dasm.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/framebuffer.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/pioz80/pioz80.h"
#include "hw-generic/printer/printer.h"
#include "hw-generic/mz1p16/mz1p16_emu.h"
#include "hw-generic/joy/joymz-1x03.h"
#include "audio.h"
#include "videorec/videorec.h"

#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#endif

#if CFG_HWEXT_HAVE_IDE8
#include "hw-generic/ide8/ide8.h"
#endif

#include "hw-generic/unicard/unicard.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#include "debugger/bptmap.h"
#include "debugger/breakpoints.h"
#include "debugger/stack_regions.h"
#include "debugger/stack_history.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/trace/cputrack.h"
#include "debugger/trace/intlog.h"
#include "debugger/trace/eventlog.h"
#include "debugger/callstack.h"
#include "debugger/freeze/freeze.h"
#include "mzarch/interrupt.h"
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include <json-glib/json-glib.h>
#include "mcp/event_bus.h"
#endif
#endif

// TODO: tohle volame v mz800_main_do_emulator_paused()
#define iface_sdl_update_window_in_beam_interval(a, b)

st_mzarch_main g_mzarch_main;

/*******************************************************************************
 *
 *
 *                  Event callbacks
 *                  ===============
 *
 *
 *******************************************************************************/

// inlined functions
#include "customspeed_event.c"

/**
 * This event is called every time it should have passed 20 ms in the real world.
 *
 * @param event_ticks Number of ticks since the start of the emulation
 * @return
 */

/* Forward declaration - mzarch_main_reset je definovaná níže (ř. 654),
 * ale mzzarch_main_do_emulator_paused() ji volá při zpracování reset
 * requestu z paused stavu. */
static void mzarch_main_reset(void);

static inline void mzarch_main_event_callback_20ms(unsigned event_ticks)
{
    audiolog_finish_20ms_frame(gdg_compute_total_ticks(event_ticks));

    if (!EMULATOR_TEST_PAUSED)
    {
#ifndef MZ800EMU_CFG_AUDIO_DISABLED
        iface_audio_20ms_sync();
#else
        emulator_sync_20ms_delay();
#endif

        if ((!EMULATOR_TEST_MAX_SPEED) && EMULATOR_MEASURING_TEST_FRAME_TIMING_ENABLED)
        {
            emulator_measuring_frame_timing_event();
        };
    };

    customspeed_event_set_next();
    framebudef_set_flag_20ms_passed();

    /*
     *
     * Nasleduje spousta ukonu, ktere vubec nesouvisi s emulaci, ale chceme je obcas volat a ted je na to vhodna chvile
     *
     */

    if (!sdlapp_is_running(g_sdlapp))
    {
        emulator_quit(EXIT_SUCCESS);
    };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /*
     * debugger_animation() bezpodmínečně - per-frame trigger animations
     * v debugger UI. Globální gate animated_updates byl odstraněn jako
     * nadbytečný; auto-follow PC v disasm view je teď řízený per-instance
     * flagem follow_pc (viz DisassembledView).
     */
    debugger_animation();
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
}

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/**
 * @brief Příznak "skončil snímek, per-frame body debuggeru čekají".
 *
 * Nastavuje ho mz800_main_event_callback_screen_done(), maže
 * mzarch_main_frame_done_debugger_hooks(). Drain fronty dbgapi a Freeze
 * Bytes se tím přesouvají z callbacku konce snímku (uvnitř
 * mzarch_main_process_events(), před gdg_on_screen_done_event(), často
 * uvnitř insideop rozpracované instrukce) do hlavní smyčky za
 * mzarch_main_process_events().
 *
 * Proč: dbgapi příkaz s I/O nebo paměťovou operací (IO_WRITE, IO_READ -
 * ověřeno; MEM_WRITE, REGION_WRITE a Freeze Bytes do VRAM nebo E00x jdou
 * podle kódu stejnou cestou, měřením neověřeno) volá přes callbacky sběrnice
 * mzarch_main_insideop() a ta mzarch_main_process_events(). Uvnitř
 * callbacku konce snímku událost konce snímku ještě visí, takže by se
 * zpracovala podruhé a délka snímku by se od unsigned
 * g_gdg.total_elapsed.ticks odečetla dvakrát. Čítač podtekl na ~2^32
 * a emulace pak ~240 s zpracovávala události bez provádění instrukcí,
 * další port I/O příkaz v tom okně zablokoval celé MCP.
 *
 * Pokud konec snímku zpracuje insideop příkazu vykonaného v paused smyčce
 * (port I/O těsně před koncem snímku), příznak zůstane nastavený a Freeze
 * Bytes tohoto snímku se uplatní až po rozběhu emulace (první průchod
 * per-event blokem). Paused smyčka per-frame body záměrně nevolá.
 *
 * @invariant Čte a píše ho jen EMU vlákno, proto není atomický.
 */
static bool s_mzarch_frame_done_hooks_due = false;
#endif

/**
 * This event is called every time a video frame is to be completed (regardless of whether it will be rendered or not)
 *
 * Volá se z gdg_process_events() při zpracování události konce posledního
 * řádku snímku, PŘED gdg_on_screen_done_event() (odečet délky snímku).
 * Proto zde nesmí běžet nic, co volá mzarch_main_insideop() (port I/O,
 * paměť VRAM a E00x) - to by událost konce snímku zpracovalo podruhé.
 * Takové per-frame body jen ohlásí přes s_mzarch_frame_done_hooks_due.
 *
 * @param
 * @return
 */
static inline void mz800_main_event_callback_screen_done(void)
{
#ifdef MZ800EMU_CFG_CLK1M1_FAST
    ctc8253_on_screen_done_event();
#endif
#if HAVE_PIOZ80
    pioz80_on_screen_done_event();
    /* Per-frame krokování 8050 plotteru MZ-1P16. Když plotter neaktivní,
     * okamžitý návrat (1 atomic read + branch) = ~zero impact na hot path.
     * Plotter běží asynchronně vůči Z80; krokujeme ho mimo CPU smyčku zde. */
    mz1p16_emu_on_screen_done();
    /* Po dokrokování plotteru periodicky zasynchronizuj stav tiskárny/plotteru
     * (BUSY/PA0, status/PA1) do interruptu brány A. Tiskárna je externí
     * asynchronní zařízení - interrupt nemusí být cyklus-přesný, stačí
     * per-frame resync. SAME_INPUT pojistka uvnitř zajistí, že se interrupt
     * vyvolá jen při skutečné změně. */
    pioz80_input_resync();
#endif
    cmt_on_screen_done_event();
    customspeed_on_screen_done();

    /* Video záznam: per-frame bod (1 atomické čtení, když se nenahrává). */
    videorec_on_screen_done();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* dbgapi CMDRQ drain a Freeze Bytes se tady jen OHLÁSÍ. Samotné
     * provedení je až v mzarch_main_frame_done_debugger_hooks() v hlavní
     * smyčce, v konzistentním bodě mezi instrukcemi. Tady jsme uprostřed
     * zpracování události konce snímku (gdg_on_screen_done_event() ještě
     * neodečetl délku snímku) a často i uprostřed instrukce (insideop),
     * takže by I/O a paměťové operace příkazů událost konce snímku
     * zpracovaly podruhé. Viz s_mzarch_frame_done_hooks_due. */
    s_mzarch_frame_done_hooks_due = true;
#endif
}

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/**
 * @brief Vybere a vykoná všechny čekající dbgapi příkazy (CMDRQ drain).
 *
 * UI i MCP submitují příkazy přes dbgapi_ui_submit_cmd_sync*(); EMU vlákno
 * je vykonává tady. V default stavu (prázdná fronta) je
 * dbgapi_emu_has_pending() jedno atomické čtení.
 *
 * Příkazy jako IO_WRITE, IO_READ, MEM_WRITE nebo REGION_WRITE volají stejné
 * callbacky sběrnice jako instrukce (port_write_cb, memory_write_byte) a ty
 * volají mzarch_main_insideop() a mzarch_main_process_events(). Insideop
 * synchronizuje čas jako I/O uvnitř instrukce: přičte k
 * g_gdg.total_elapsed.ticks rozpracované T-stavy (cpu->op_tstate, mezi
 * instrukcemi = délka předchozí instrukce, až 23 T), stejnou hodnotu zapíše
 * do instruction_insideop_sync_ticks a u PSG a VRAM přidá čekací stavy
 * (cpu->wait_cycles). Následující instrukce by sync_ticks odečetla od svých
 * tiků. Pokud ale předstih mezitím překročil konec snímku (pauza těsně před
 * koncem snímku, drain paused smyčky), délka snímku se už odečetla a ve
 * čítači zbývá méně, než instrukce odečte: unsigned čítač podteče a emulace
 * ~240 s zpracovává události bez instrukcí. Reprodukováno testem
 * tests/mcp/test_dbgapi_io_running.py (fáze E) na všech platformách.
 *
 * Proto se po každém příkazu účty insideop vynulují stejně jako po
 * bootstrapu (bootstrap.c, mzarch_bootstrap_init()): sync_ticks,
 * instruction_wait_tstates a cpu->wait_cycles na 0, cpu->op_tstate na
 * hodnotu před příkazem (mezi instrukcemi je jen informativní). Daň:
 * příkaz s operací na sběrnici posune čas emulace natrvalo o předstih
 * (nejvýš délka jedné instrukce, u PSG zápisu navíc jeho čekací stavy).
 * Pro ladicí příkaz zvenčí je to přijatelné. Příkazy bez operace na
 * sběrnici účty nemění, takže se jich úklid nijak nedotkne.
 *
 * @pre Volá se jen z EMU vlákna v konzistentním bodě mezi instrukcemi
 *      (hlavní smyčka za mzarch_main_process_events() nebo paused smyčka):
 *      nikdy z callbacku události ani z insideop. V obou bodech jsou účty
 *      insideop nulové (hlavní smyčka je nuluje po každém z80_step(),
 *      cpu->wait_cycles spotřebuje z80_step()).
 * @post Fronta byla v okamžiku kontroly prázdná. Příkaz zařazený během
 *       drainu se vykoná ještě v tomto volání. Každý vyzvednutý požadavek
 *       je dokončen (dbgapi_emu_complete()) a jeho volající odblokován.
 *       Účty insideop jsou nulové, cpu->op_tstate má hodnotu z doby před
 *       příkazem.
 */
static void mzarch_main_dbgapi_drain(void)
{
    while (dbgapi_emu_has_pending(&g_dbgapi_cmdrq_queue))
    {
        st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue);
        if (rq)
        {
            z80_t *cpu = g_mzarch_main.cpu;
            const int saved_op_tstate = cpu->op_tstate;

            dbgapi_emu_dispatch(rq);

            /* Úklid účtů insideop po operaci mimo instrukci (viz výše). */
            g_mzarch_main.instruction_insideop_sync_ticks = 0;
            g_mzarch_main.instruction_wait_tstates = 0;
            cpu->wait_cycles = 0;
            cpu->op_tstate = saved_op_tstate;

            dbgapi_emu_complete(rq);
        };
    };
}

/**
 * @brief Per-frame body debuggeru odložené z konce snímku do hlavní smyčky.
 *
 * Volá se z hlavní smyčky po mzarch_main_process_events(), pokud od
 * posledního volání skončil snímek (s_mzarch_frame_done_hooks_due). V tu
 * chvíli je událost konce snímku kompletně zpracovaná
 * (gdg_on_screen_done_event() odečetl délku snímku, čítač snímků je
 * zvýšený, další událost je naplánovaná) a CPU stojí mezi instrukcemi.
 *
 * Pořadí: napřed drain fronty dbgapi, potom Freeze Bytes (stejně jako
 * dřív v callbacku konce snímku), aby se bajt zafrozený příkazem uplatnil
 * ve stejném snímku.
 *
 * @pre EMU vlákno, bod mezi instrukcemi, mzarch_main_process_events() už
 *      proběhlo (g_gdg.total_elapsed.ticks < g_mzarch_main.event.ticks).
 * @post s_mzarch_frame_done_hooks_due je false (pokud ho některý příkaz
 *       znovu nenastavil vnořeným koncem snímku), fronta dbgapi je prázdná.
 */
static void mzarch_main_frame_done_debugger_hooks(void)
{
    s_mzarch_frame_done_hooks_due = false;

    mzarch_main_dbgapi_drain();

    /* V1 Freeze Bytes - per-frame apply všech zafrozených bajtů (cheat
     * engine semantika). Pokud žádný entry, vrátí okamžitě (1 atomic byte
     * load + branch). Zápis může jít přes memory_write_byte (LOGICAL
     * region) a tedy přes insideop - proto také mimo callback konce
     * snímku. Viz src/emulator/debugger/freeze/freeze.h. */
    freeze_apply_all ( );
}
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

/*******************************************************************************
 *
 *
 *                  Events
 *                  =======
 *
 *
 *******************************************************************************/

/**
 * Search for the nearest following HW event and set it to g_mz800_main.event
 *
 * CTC0
 * CMT
 * (g_ctc8253[CTC_CS0].clk1m1_event)
 *
 * PIOZ80
 * (g_pioz80.icena_event)
 *
 * GDG
 * (g_gdg.event)
 *
 * CUSTOM_SPEED_SYNC
 * (g_mz800_main.speed_sync_event)
 *
 *
 * @param
 * @return
 */
static inline void mzarch_main_queue_next_event(void)
{
    st_EMUEVENT *proximate;
    st_EMUEVENT *ev_gdg = gdg_get_event_pointer();
    st_EMUEVENT *ev_pioz80 = pioz80_get_icena_event_pointer();

#ifdef MZ800EMU_CFG_CLK1M1_FAST

    st_EMUEVENT *ev_ctc = ctc8253_get_ctc1m1_event_pointer();

    if (ev_pioz80->ticks <= ev_ctc->ticks)
    {
        proximate = ev_pioz80;
    }
    else
    {
        proximate = ev_ctc;
    };

    if (ev_gdg->ticks <= proximate->ticks)
    {
        proximate = ev_gdg;
    };

#else  // MZ800EMU_CFG_CLK1M1_FAST

    if (ev_gdg->ticks <= ev_pioz80->ticks)
    {
        proximate = ev_gdg;
    }
    else
    {
        proximate = ev_pioz80;
    };
#endif // MZ800EMU_CFG_CLK1M1_FAST

    st_EMUEVENT *ev_cspd = customspeed_get_event_pointer();
    if (ev_cspd->ticks <= proximate->ticks)
    {
        proximate = ev_cspd;
    };

    g_mzarch_main.event.event_name = proximate->event_name;
    g_mzarch_main.event.ticks = proximate->ticks;
}

/*
 * Touto direktivou si includujeme inline funkce pro zpracovani GDG eventu
 */
#define INCLUDED_FROM_MZARCH_C
#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_gdg_event.c"
#else
#if MZARCH == 1500
#include "mzarch/mz1500/gdg/mz1500_gdg_event.c"
#else
#if MZARCH == 700
#include "mzarch/mz700/gdg/mz700_gdg_event.c"
#else
#error "Unsupported MZARCH value"
#endif
#endif
#endif
#undef INCLUDED_FROM_MZARCH_C

static inline void mzarch_main_process_events(void)
{
    while (g_gdg.total_elapsed.ticks >= g_mzarch_main.event.ticks)
    {

        // Nejprve odbavime eventy, ktere nepochazeji z GDG
        while (g_mzarch_main.event.event_name >= MZEVENT_NO_GDG)
        {
#ifdef MZ800EMU_CFG_CLK1M1_FAST
            st_EMUEVENT *ev_ctc = ctc8253_get_ctc1m1_event_pointer();

            if (ev_ctc->ticks <= g_mzarch_main.event.ticks)
            {
                ctc8253_ctc1m1_event(g_mzarch_main.event.ticks);
            };
#endif
            st_EMUEVENT *ev_pioz80 = pioz80_get_icena_event_pointer();
            if (ev_pioz80->ticks <= g_mzarch_main.event.ticks)
            {
                pioz80_icena_event();
            };

            st_EMUEVENT *ev_cspd = customspeed_get_event_pointer();
            if (ev_cspd->ticks <= g_mzarch_main.event.ticks)
            {
                mzarch_main_event_callback_20ms(g_mzarch_main.event.ticks);
            };

            mzarch_main_queue_next_event();

            if (!(g_gdg.total_elapsed.ticks >= g_mzarch_main.event.ticks))
                return;
        };

        gdg_process_events();
        mzarch_main_queue_next_event();
    };
}

#ifdef MZ800EMU_CFG_CLK1M1_SLOW

static inline void mzarch_sync_ctc0_and_cmt(unsigned instruction_ticks)
{

    g_gdg.total_elapsed.ticks -= g_gdg.ctc0clk;
    instruction_ticks += g_gdg.ctc0clk;

    while (instruction_ticks > GDGCLK_CTC0_DIVIDER - 1)
    {

        g_gdg.total_elapsed.ticks += GDGCLK_CTC0_DIVIDER;
        instruction_ticks -= GDGCLK_CTC0_DIVIDER;

        ctc8253_clkfall(CTC_CS0, g_gdg.total_elapsed.ticks);

        // uz neexistuje
#if 0
        /* TODO: prozatim si sem povesime i pomaly cmt_step() */
        if ( TEST_CMT_PLAYING ) {
            cmt_step ( );
        };
#endif
    };

    g_gdg.ctc0clk = instruction_ticks;
    g_gdg.total_elapsed.ticks += instruction_ticks;
}

#endif

/*******************************************************************************
 *
 *
 *                  Inside operations
 *                  =================
 *
 *
 *******************************************************************************/

#if MZARCH == 800
/**
 * Lookup tabulka WAIT (W+R) pro MZ-800 graficke rezimy.
 *
 * Index je pozice prvniho WRITE v ramci radku v CLK0 mod 80
 * (lcm(16 = video fetch column period, 5 = CPU T-state) = 80).
 * Hodnota = pocet TW (1 TW = 5 CLK0 = 1 CPU T-state) penalizace
 * pri READu nasledujicim po WRITE uvnitr horke faze (32 CLK0
 * v 320x200 / 17 CLK0 v 640x200).
 *
 * Pro 640x200 plati TW_640(p) = TW_320((p+16) mod 80) - rotace o jednu
 * column period 320x200.
 *
 * Zdroj: gate-level HDL simulace GDG, viz mz800-knowledge
 * public/reference/agent/hw/10b-vram-timing.md sekce "Pouziti v emulatoru".
 *
 * [neovereno] HW measurement - HDL netlist je gate-accurate, ale realne
 * HW mereni zatim chybi. Hodnoty 6-9 kolisaji v ramci sub-column phase.
 */
static const uint8_t mz800_vram_wait_tw_wr[ 80 ] = {
    9, 6, 9, 9, 9, 8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 6,
    6, 9, 9, 9, 8, 9, 8, 8, 8, 7, 8, 7, 7, 7, 6, 7,
    9, 9, 6, 9, 9, 8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 9,
    6, 6, 9, 9, 8, 8, 9, 8, 8, 7, 7, 8, 7, 7, 6, 6,
    7, 9, 9, 8, 9, 9, 8, 8, 7, 8, 8, 7, 7, 6, 7, 7
};

/**
 * Lookup tabulka WAIT (W+W) pro MZ-800 graficke rezimy.
 *
 * Pro WRITE+WRITE je penalizace mensi (3-6 TW), protoze druhy WRITE
 * generuje vlastni VRAM strobe ktery urychluje reset interniho WR_FF.
 *
 * Indexovani a rotace 640 vs 320 viz @ref mz800_vram_wait_tw_wr.
 */
static const uint8_t mz800_vram_wait_tw_ww[ 80 ] = {
    3, 3, 5, 6, 5, 5, 5, 4, 5, 4, 4, 4, 3, 4, 3, 3,
    3, 6, 6, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 3, 3, 3,
    6, 3, 5, 5, 6, 5, 5, 4, 4, 5, 4, 4, 3, 3, 4, 3,
    3, 5, 6, 6, 5, 5, 4, 5, 5, 4, 4, 3, 4, 4, 3, 3,
    6, 6, 3, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 3, 3, 6
};
#endif /* MZARCH == 800 */

static inline void mzarch_main_insideop(const en_INSIDEOP insideop)
{

    unsigned tstates = 0;
    unsigned instruction_ticks = 0;
    unsigned ticks_to_sync = 0;
    unsigned tstates_to_psg_sync = 0;

    if (g_mzarch_main.event.event_name >= MZEVENT_BREAK)
    {
        mzarch_main_queue_next_event();
    };

#if 0
    if ( insideop == INSIDEOP_IORQ ) {
        // dbg
        uint8_t byte = memory_read_byte ( g_mzarch_main.instruction_addr );
        if ( byte != 0xd3 ) {
            printf ( "0x%04x: 0x%02x, 0x%02x\n", g_mzarch_main.instruction_addr, byte, memory_read_byte ( g_mzarch_main.instruction_addr ) );
            byte = 0;
        };
    };
#endif

    switch (insideop)
    {
    case INSIDEOP_MREQ_MZ700_VRAMCTRL:
        /* V tuto chvili pocitame s tim, ze uz mame synchronizovano po g_gdg.total_elapsed.ticks */
        ticks_to_sync = VIDEO_BEAM_HBLN_FIRST_COLUMN - VIDEO_GET_SCREEN_COL(g_gdg.total_elapsed.ticks);
        ticks_to_sync += GDGCLK2CPU_DIVIDER - (ticks_to_sync % GDGCLK2CPU_DIVIDER);
        {
            unsigned wait_added = (ticks_to_sync / GDGCLK2CPU_DIVIDER);
            z80_add_wait_states(g_mzarch_main.cpu, wait_added);
            /* trace-suite: saturujici akumulace WAIT T-states pro cputrack. */
            uint64_t sum = (uint64_t) g_mzarch_main.instruction_wait_tstates + wait_added;
            g_mzarch_main.instruction_wait_tstates = (sum > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t) sum;
        }
        instruction_ticks = g_mzarch_main.instruction_insideop_sync_ticks + ticks_to_sync;
        break;

#if MZARCH == 800
    case INSIDEOP_MREQ_MZ800_VRAMCTRL_READ:
    case INSIDEOP_MREQ_MZ800_VRAMCTRL_WRITE: {
        /* HDL-presny WAIT model pro MZ-800 graficke rezimy (DMD bit 3 = 0,
         * VRAM 0x8000-0xBFFF). Zdroj: 10b-vram-timing.md.
         *
         * Predpoklad: pred volanim teto cesty doslo k mzarch_main_insideop_mreq()
         * (default sync), takze g_gdg.total_elapsed.ticks odpovida konci aktualniho
         * MREQ M-cyklu (T3f) tohoto VRAM pristupu.
         *
         * Pravidla:
         *  - READ negeneruje WAIT sam o sobe a nikdy negeneruje VRAM strobe
         *    (= horka faze pokracuje a po READu netreba update). Pokud ovsem
         *    aktualni READ pripadl uvnitr predchozi horke faze, dostava
         *    WAIT podle tw_wr[] (6-9 TW v 320x200 / rotace +16 v 640x200).
         *  - WRITE se vzdy chova jako start nove horke faze (po pripadnem
         *    WAITu se hot_phase_end_total_ticks aktualizuje). Pokud aktualni
         *    WRITE pripadl uvnitr predchozi horke faze, dostane WAIT podle
         *    tw_ww[] (3-6 TW).
         *
         * 1 TW = GDGCLK2CPU_DIVIDER CLK0 = jeden CPU T-state navic.
         */

        uint64_t now_total = gdg_get_total_ticks( );
        unsigned wait_tw = 0;
        unsigned hot_len = ( g_gdg.regDMD & REGISTER_DMD_FLAG_SCRW640 ) ? 17u : 32u;

        bool inside_hot = ( g_gdg.vram800_hot_phase_end_total_ticks != 0 )
                          && ( now_total < g_gdg.vram800_hot_phase_end_total_ticks );

        if ( inside_hot ) {
            /* Pozice prvniho WRITE v ramci radku v CLK0 mod 80 (= prvni WRITE
             * teto horke faze, ulozeny pri jejim startu). Pro 640x200 rotace
             * o 16 CLK0 (= jedna column period 320x200). */
            unsigned phase = g_gdg.vram800_hot_phase_clk0_phase;
            if ( g_gdg.regDMD & REGISTER_DMD_FLAG_SCRW640 ) {
                phase = ( phase + 16u ) % 80u;
            }
            if ( insideop == INSIDEOP_MREQ_MZ800_VRAMCTRL_WRITE ) {
                wait_tw = mz800_vram_wait_tw_ww[ phase ];
            } else {
                wait_tw = mz800_vram_wait_tw_wr[ phase ];
            }
            z80_add_wait_states( g_mzarch_main.cpu, (int) wait_tw );
            /* trace-suite: saturujici akumulace WAIT T-states pro cputrack. */
            uint64_t wsum = (uint64_t) g_mzarch_main.instruction_wait_tstates + wait_tw;
            g_mzarch_main.instruction_wait_tstates = ( wsum > 0xFFFFFFFFu ) ? 0xFFFFFFFFu : (uint32_t) wsum;
            ticks_to_sync = wait_tw * GDGCLK2CPU_DIVIDER;
            now_total += ticks_to_sync;
        }

        if ( insideop == INSIDEOP_MREQ_MZ800_VRAMCTRL_WRITE ) {
            /* Start nove horke faze: T3f tohoto WRITE = now_total
             * (po pripadnem WAITu).
             *
             * [neovereno] HDL detail v 10b-vram-timing.md ukazuje WR_FF set
             * "T3f+30" (32 CLK0 prog. v 320x200), tj. ze T3f okamzik je primy
             * T3 falling. Po standard mreq() je total_elapsed.ticks na konci
             * MREQ pulsu - pro ucely tabulkove periodicity (mod 80 CLK0)
             * sub-CLK0 nepresnosti nehraji roli, hot_len ma sub-T-state
             * granularity v ramci HDL kolisani 3-6/6-9 TW. */
            g_gdg.vram800_hot_phase_end_total_ticks = now_total + hot_len;
            /* Pozice WRITE T3f v ramci scanline v CLK0 mod 80. Pouzivame
             * "ticks po pripadnem WAITu" (= now_total), protoze tento
             * WRITE definuje novou horkou fazi a jeho vlastni T3f je az
             * po WAIT periode. */
            uint64_t scanline_pos = now_total % VIDEO_SCREEN_WIDTH;
            g_gdg.vram800_hot_phase_clk0_phase = (unsigned) ( scanline_pos % 80u );
        }
        /* READ neaktualizuje hot phase state - WR_FF reset cas se neposouva
         * (READ negeneruje VRAM strobe). */

        instruction_ticks = g_mzarch_main.instruction_insideop_sync_ticks + ticks_to_sync;
        break;
    }
#endif /* MZARCH == 800 */

#if MZARCH != 700
    case INSIDEOP_IORQ_PSG_WRITE:
        tstates_to_psg_sync = 16 - (gdg_get_total_ticks() / GDGCLK2CPU_DIVIDER) % 16;
        // tohle je muj odhad, kdyz jsem to testoval s analyzerem, tak 1 cpu takt nestacil, zatimco 5 uz bylo OK
        if (tstates_to_psg_sync < 4)
        {
            tstates_to_psg_sync += 16;
        };
        tstates_to_psg_sync += 16;
        z80_add_wait_states(g_mzarch_main.cpu, tstates_to_psg_sync);
        {
            /* trace-suite: saturujici akumulace WAIT T-states pro cputrack. */
            uint64_t sum = (uint64_t) g_mzarch_main.instruction_wait_tstates + tstates_to_psg_sync;
            g_mzarch_main.instruction_wait_tstates = (sum > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t) sum;
        }
        ticks_to_sync = tstates_to_psg_sync * GDGCLK2CPU_DIVIDER;
        instruction_ticks = g_mzarch_main.instruction_insideop_sync_ticks + ticks_to_sync;
        break;
#endif /* MZARCH != 700 */

    default:
        tstates = g_mzarch_main.instruction_tstates + g_mzarch_main.cpu->op_tstate;
        instruction_ticks = tstates * GDGCLK2CPU_DIVIDER;
        ticks_to_sync = instruction_ticks - g_mzarch_main.instruction_insideop_sync_ticks;
        break;
    };

#ifdef MZ800EMU_CFG_CLK1M1_SLOW
    mzarch_sync_ctc0_and_cmt(ticks_to_sync);
#else
    g_gdg.total_elapsed.ticks += ticks_to_sync;
#endif

    g_mzarch_main.instruction_insideop_sync_ticks = instruction_ticks;

    mzarch_main_process_events();

    if (g_mzarch_main.interrupt)
    {
        MZ800_MAIN_SET_EVENT(MZEVENT_BREAK_MZARCH_INTERRUPT, 0);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    }
    else if ((EMULATOR_TEST_PAUSED) || (TEST_DEBUGGER_STEP_CALL))
    {
#else
    }
    else if (EMULATOR_TEST_PAUSED)
    {
#endif
        MZ800_MAIN_SET_EVENT(MZEVENT_BREAK_EMULATION_PAUSED, 0);
    };
}

void mzarch_main_insideop_iorq(void)
{
    mzarch_main_insideop(INSIDEOP_IORQ);
}

void mzarch_main_insideop_mreq(void)
{
    mzarch_main_insideop(INSIDEOP_MREQ);
}

void mzarch_main_insideop_mreq_e00x(void)
{
    mzarch_main_insideop(INSIDEOP_MREQ_E00x);
}

void mzarch_main_insideop_mreq_mz700_vramctrl(void)
{
    mzarch_main_insideop(INSIDEOP_MREQ_MZ700_VRAMCTRL);
}

void mzarch_main_insideop_mreq_mz800_vramctrl_read(void)
{
#if MZARCH == 800
    mzarch_main_insideop(INSIDEOP_MREQ_MZ800_VRAMCTRL_READ);
#endif
}

void mzarch_main_insideop_mreq_mz800_vramctrl_write(void)
{
#if MZARCH == 800
    mzarch_main_insideop(INSIDEOP_MREQ_MZ800_VRAMCTRL_WRITE);
#endif
}

void mzarch_main_insideop_iorq_psg_write(void)
{
    mzarch_main_insideop(INSIDEOP_IORQ_PSG_WRITE);
}

void mzarch_main_report_screen_done_underflow(unsigned ticks)
{
    /* Jen EMU vlákno - čítač nemusí být atomický. Omezení výpisu chrání
     * stderr před zahlcením, kdyby se chyba opakovala každý snímek. */
    static unsigned s_reported = 0;
    if (s_reported < 8)
    {
        s_reported++;
        fprintf(stderr,
                "WARNING: frame end processed twice (total_elapsed.ticks=%u < frame length %u), clamped to 0\n",
                ticks, (unsigned)VIDEO_SCREEN_TICKS);
        if (s_reported == 8)
        {
            fprintf(stderr, "WARNING: further occurrences suppressed\n");
        };
    };
}

/*******************************************************************************
 *
 *
 *                  Interrupts
 *                  ==========
 *
 *
 *******************************************************************************/

/**
 * Mezi instrukcnimi takty testujeme, zda na nas neceka nejaky interrupt.
 * Pokud ano, tak jej zkusime poslat do Z80.
 */
static inline void mzarch_main_process_interrupt(void)
{

    if (!g_mzarch_main.interrupt)
        return;

    z80_t *cpu = g_mzarch_main.cpu;

    if (cpu->iff1 && !cpu->ei_delay)
    {
        z80_int(cpu);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* trace-suite intlog: snapshot stavu PIOZ80 PŘED z80_process_interrupt().
         * Uvnitř z80_process_interrupt() se v IM 2 cestě volá intread_cb
         * (= pioz80_interrupt_ack_im2_cb), která mění port_int na RECEIVED a
         * cestou pioz80_interrupt_manager(CPUBUS_INTACK) propaguje
         * g_pioz80.interrupt na PIOZ80_INTERRUPT_RECEIVED + nastaví
         * g_pioz80.interrupt_port_id. Pokud byl PIOZ80 v stavu PENDING,
         * znamená to že vektor pochází z PIOZ80 daisy chain. Jinak (typicky
         * CTC2 v mzdos) intread_cb early-returnne 0, CPU dispatch skočí na
         * (I:00) entry a my zdroj identifikujeme z g_mzarch_main.interrupt
         * masky. */
        int was_pioz80_pending = (g_pioz80.interrupt == PIOZ80_INTERRUPT_PENDING);
        uint8_t mzarch_irq_mask_before = g_mzarch_main.interrupt;
        /* Callstack: snapshot PC PŘED z80_process_interrupt(). Po
         * accept se cpu->pc změní na ISR adresu, ale původní PC (= adresa
         * instrukce na kterou se RETI vrátí) potřebujeme pro push frame.
         * Tato adresa odpovídá tomu, co z80_process_interrupt pushne na
         * CPU stack. */
        uint16_t pc_before_int = cpu->pc;
#endif

        int interrupt_tstates = z80_process_interrupt(cpu);

        if (interrupt_tstates > 0)
        { /* interrupt byl prijat */

            pioz80_interrupt_ack();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            /* trace-suite intlog: zaznamenat IM 2 IRQ accept event.
             *
             * Hook musí být zde (= v mzarch.c) a ne v pioz80_interrupt_ack_im2_cb,
             * protože ten má early return na ř. 874 pro non-PIOZ80 IRQ a v takovém
             * případě se hook nikdy nevolá, ačkoliv CPU dispatch reálně proběhl.
             *
             * vector_table_addr = (I << 8) | (vector & 0xFE), kde vector je hodnota
             * kterou CPU přečetl z datové sběrnice (cpu->int_vector). Pro PIOZ80
             * IRQ je to port->interrupt_vector, pro non-PIOZ80 je to "duch
             * sběrnice" (g_mzarch_main.regDBUS_latch = poslední byte na DBUS),
             * protože MZ-800 nemá zařízení dodávající vektor.
             *
             * isr_addr = cpu->pc - PC je v tomto okamžiku už nastavený na ISR
             * adresu (= hodnota přečtená z (I:vector & 0xFE)).
             *
             * source_chip rozlišuje kdo dodal vector:
             *  - PIOZ80_PORT_A/B = vektor přišel z PIOZ80 daisy chain
             *  - CTC2 = INT pin tažený CTC2 (vector = 0, CPU skočil na (I:00))
             *  - FDC = INT pin tažený FDC kontrolérem
             *  - VECTOR_BUS_LATCH = jiný / neresolvable zdroj */
            if (TEST_TRACE_INTLOG_DISPATCH && cpu->im == 2) {
                uint16_t vector_table_addr = ((uint16_t)cpu->i << 8) | (cpu->int_vector & 0xFE);
                uint16_t isr_addr = cpu->pc;
                en_INTLOG_SOURCE_CHIP src;
                if (was_pioz80_pending) {
                    /* g_pioz80.interrupt_port_id po CPUBUS_INTACK obsahuje port,
                     * který byl vyhodnocen v pioz80_interrupt_ack_im2_cb. */
                    src = (g_pioz80.interrupt_port_id == PIOZ80_PORT_A)
                              ? INTLOG_CHIP_PIOZ80_PORT_A
                              : INTLOG_CHIP_PIOZ80_PORT_B;
                } else if (mzarch_irq_mask_before & MZARCH_INTERRUPT_CTC2) {
                    src = INTLOG_CHIP_CTC2;
                } else if (mzarch_irq_mask_before & MZARCH_INTERRUPT_FDC) {
                    src = INTLOG_CHIP_FDC;
                } else {
                    src = INTLOG_CHIP_VECTOR_BUS_LATCH;
                }
                intlog_record_irq_ack_im2(src, vector_table_addr, isr_addr);

                /* PIO_STATE IM2_JUMP - emit jen pokud PIOZ80 byl skutečným
                 * zdrojem dispatchu. Pro non-PIOZ80 IRQ by IM2_JUMP byl
                 * zavádějící ("PIOZ80 jumped" pro CTC IRQ). */
                if (was_pioz80_pending) {
                    uint32_t state = INTLOG_STATE_BIT_PIO_IM2_JUMP;
                    if (g_pioz80.interrupt & PIOZ80_INTERRUPT_INT_BIT) state |= INTLOG_STATE_BIT_PIO_READY;
                    if (g_pioz80.port[PIOZ80_PORT_A].icena == PIOZ80_ICENA_ENABLED
                        || g_pioz80.port[PIOZ80_PORT_B].icena == PIOZ80_ICENA_ENABLED) {
                        state |= INTLOG_STATE_BIT_PIO_ARMED;
                    }
                    intlog_record_pio_state(state);
                }
            }

            /* V1.5.A8 - IRQ BP enforce hook (POST-dispatch, breaking vs V1).
             *
             * Volá se vždy (i mimo TEST_TRACE_INTLOG_DISPATCH), pokud existuje
             * alespoň jeden registrovaný IRQ BP. Pro IM 2 jsou vector_addr
             * a isr_addr smysluplně definované; pro IM 0/1 posíláme
             * vector_addr=0 (= filter-aware kód v breakpoints_enforce_irq
             * BP s aktivním filtrem nefiruje). isr_addr posíláme cpu->pc
             * pro všechny IM (= konzistentní ctx pro condition / log akce).
             *
             * mzarch_irq_mask_before je local snapshot z ř.486 (= před
             * z80_process_interrupt), tedy stav INT busu před INTACK. */
            if ( g_bptmap.per_type_active[ BPTMAP_IDX_IRQ ] ) {
                uint16_t bp_vector_addr = 0;
                uint16_t bp_isr_addr = cpu->pc;
                if ( cpu->im == 2 ) {
                    bp_vector_addr = ( (uint16_t) cpu->i << 8 ) | ( cpu->int_vector & 0xFE );
                };
                /* V1.5.A8.5: int_vector_byte = raw cpu->int_vector
                 * (= RST opcode v IM 0, vec byte v IM 2). */
                breakpoints_enforce_irq ( mzarch_irq_mask_before, cpu->im,
                                           bp_vector_addr, bp_isr_addr,
                                           cpu->int_vector );
            }

            /* Callstack push pro IRQ entry (Fáze 1C). Volá se po
             * z80_process_interrupt() pokud byl IRQ skutečně přijat
             * (= interrupt_tstates > 0). pc_before_int byl uložen
             * před voláním z80_process_interrupt - je to návratová
             * adresa (= co se pushne na CPU stack a kam RETI vrátí).
             * isr_addr = cpu->pc je už nastavené na vstup ISR rutiny.
             * Pro IM 2 počítáme vector table adresu z I:vector. Pro
             * IM 0/1 je im2_vec irelevantní (= 0). Vnitřní gate test
             * (g_callstack_active) řeší callstack_on_irq_accept sám. */
            {
                uint16_t cs_isr_addr = cpu->pc;
                uint16_t cs_im2_vec = 0;
                if ( cpu->im == 2 ) {
                    cs_im2_vec = ( (uint16_t) cpu->i << 8 ) | ( cpu->int_vector & 0xFE );
                }
                callstack_on_irq_accept ( pc_before_int, cs_isr_addr,
                                           cpu->im, cs_im2_vec );
            }
#endif

            unsigned interrupt_ticks = interrupt_tstates * GDGCLK2CPU_DIVIDER;

#ifdef MZ800EMU_CFG_CLK1M1_SLOW
            mzarch_sync_ctc0_and_cmt(interrupt_ticks);
#else
            g_gdg.total_elapsed.ticks += interrupt_ticks;
#endif

            if (g_gdg.total_elapsed.ticks >= g_mzarch_main.event.ticks)
            {
                mzarch_main_process_events();
            };
        }
    }
    else
    { /* interrupt nebyl prijat - IFF1 je 0 nebo jsme po EI */
        MZ800_MAIN_SET_EVENT(MZEVENT_BREAK_MZARCH_INTERRUPT, 0);
    };
}

/*******************************************************************************
 *
 *
 *                  Paused Emulation
 *                  ================
 *
 *
 *******************************************************************************/
static inline void mzzarch_main_do_emulator_paused(void)
{
    /* Video záznam: emulace stojí - běh k dočasnému BP (step over / run to cursor)
     * skončil. Musí být před prvním drainem dbgapi níže: pokračování zpracované
     * v první iteraci smyčky by jinak příznak nechalo přežít do volného běhu. */
    videorec_on_emulation_stopped();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

    debugger_update_all();

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    /* Mutant mcp-server V1.A.5: step_done event emit. Pokud entry do
     * paused stavu pochází z dokončeného STEP_INTO / STEP_OVER /
     * STEP_N kroku (= TEST_DEBUGGER_STEP_CALL je 1 dokud ho dolejší
     * debugger_step_call(0) nesmaže), emit "step_done" do MCP
     * subscriberů PŘED resetem flagu. Guarded subscriberem - bez
     * subscriberů žádný payload se nealokuje. */
    if ( g_debugger.step_call && event_bus_has_subscriber ( "step_done" ) ) {
        JsonObject *payload = json_object_new ( );
        json_object_set_int_member ( payload, "pc",
            (gint64) ( g_mzarch_main.cpu ? g_mzarch_main.cpu->pc : 0 ) );
        json_object_set_int_member ( payload, "cycles",
            (gint64) ( g_mzarch_main.cpu ? g_mzarch_main.cpu->total_cycles : 0 ) );
        event_bus_emit ( "step_done", payload );
    }
#endif

    /* Video záznam: návrat z kroku debuggeru (rozpoznání krokování od pauzy, record_debugger_steps). */
    if (g_debugger.step_call)
        videorec_on_debugger_step();

    debugger_step_call(0);

    framebuffer_border_changed();
    if (!GDG_MZ800_DMD_TEST_MZ700)
    {
        framebuffer_MZ800_screen_changed();
    };
    unsigned screen_elapsed_ticks = g_gdg.total_elapsed.ticks;
    if (g_debugger.screen_refresh_at_step)
    {
        debugger_forced_screen_update();
    }
    else
    {
        iface_sdl_update_window_in_beam_interval(g_gdg.screen_is_already_rendered_at_beam_pos, screen_elapsed_ticks);
    };
    g_gdg.screen_is_already_rendered_at_beam_pos = screen_elapsed_ticks;

    while (EMULATOR_TEST_PAUSED && (!TEST_DEBUGGER_STEP_CALL))
    {
        if (!sdlapp_is_running(g_sdlapp))
        {
            emulator_quit(EXIT_SUCCESS);
        };

        /* dbgapi CMDRQ drain v paused stavu. Bez tohoto by UI Step/Run/
         * Reset přes dbgapi_ui_submit_cmd_sync zatuhly (= EMU vlákno by
         * frontu nikdy nedrainovalo). Tady jen non-blocking has_pending
         * check; na nový příkaz smyčka čeká na konci iterace přes
         * dbgapi_emu_wait_for_cmd() (nejvýš 20 ms). Stejný helper jako drain
         * běžící emulace (per-frame bod v hlavní smyčce). */
        mzarch_main_dbgapi_drain();

        /* Přemapování MemExt vyžádané z UI i během pauzy (do ~20 ms). */
        memext_map_request_poll();

        if (iface_video_get_redraw_full_screen_request())
        {
            framebuffer_screen_done();
        };

        /* Reset request z UI nemůže čekat až do dalšího hot loop iteru
         * (= po unpause), protože uživatel chce vidět "Reset proběhl"
         * okamžitě. Reset z paused stavu zachová pause (= mzarch_main_reset
         * neodpauzne). */
        APP_MUTEX_LOCK(g_mzarch_main.reset_request_mutex);
        bool do_reset = g_mzarch_main.reset_request;
        if (do_reset)
            g_mzarch_main.reset_request = false;
        APP_MUTEX_UNLOCK(g_mzarch_main.reset_request_mutex);
        if (do_reset)
            mzarch_main_reset();

        /* Video záznam: stop požadovaný v pauze se provede hned (jinak až po
         * odpauzování). Bez nahrávání 1 atomické čtení. */
        videorec_on_emulation_paused();

        /* Čekání na další iteraci: nejvýš 20 ms jako dřív (g_usleep), ale
         * příkaz z dbgapi fronty smyčku probudí hned (queue_cond). Bez toho
         * čekal každý synchronní příkaz UI v pauze až 20 ms - u editace
         * v Memory Browseru (zápis přes frontu po každém bajtu) a u
         * vícekrokových MCP handlerů se to sčítalo. Návratová hodnota je
         * nepodstatná, drain proběhne na začátku další iterace. */
        (void)dbgapi_emu_wait_for_cmd(&g_dbgapi_cmdrq_queue, 20);
    };

    /* Po opuštění pause smyčky (= unpause přes F5/F4/F7/F8 nebo Run To
     * Cursor) nastavíme skip_bp_at_pc na aktuální PC. Hot loop pak
     * přeskočí BP enforcement pro 1 instrukci na této adrese, aby
     * uživatel neuvízl v infinite pause loopu (= BP hit → pause →
     * unpause → BP znovu hit). Vázáno na konkrétní PC, takže pokud
     * mezi tím proběhne reset (= PC se změní na 0000), skip se
     * neuplatní na novou adresu a BP na 0000 se správně aktivuje. */
    g_debugger.skip_bp_at_pc = (int) g_mzarch_main.cpu->pc;

    if (TEST_DEBUGGER_STEP_CALL)
    {
        MZ800_MAIN_SET_EVENT(MZEVENT_BREAK_EMULATION_PAUSED, 0);
    }
    else
    {
        mzarch_main_queue_next_event();
    };

    cmt_update_output();

#else // MZ800_DEBUGGER neni povolen

    iface_sdl_update_window_in_beam_interval(g_gdg.screen_is_already_rendered_at_beam_pos, g_gdg.total_elapsed.ticks);

    while (EMULATOR_TEST_PAUSED)
    {
        // iface_sdl_pool_all_events();
        if (!sdlapp_is_running(g_sdlapp))
        {
            emulator_quit(EXIT_SUCCESS);
        };

        if (iface_video_get_redraw_full_screen_request())
        {
            framebuffer_screen_done();
        };

        /* Přemapování MemExt vyžádané z UI i během pauzy (do ~20 ms). */
        memext_map_request_poll();

        mzarch_main_queue_next_event();

        /* Video záznam: stop požadovaný v pauze se provede hned (jinak až po
         * odpauzování). Bez nahrávání 1 atomické čtení. */
        videorec_on_emulation_paused();

        /* Spánek v paused smyčce - bez něj by EMU vlákno v pauze busy-spinilo
         * na 100 % jádra. Parita s debugger větví (g_usleep výše). UI běží na
         * vlastním vlákně, takže pauza zůstává okamžitě zrušitelná (Alt+P). */
        g_usleep(20 * 1000);
    };
#endif
}


void mzarch_forced_full_screen_refresh(void)
{
    iface_video_create_redraw_full_screen_request();

    if (GDG_MZ800_DMD_TEST_MZ700)
    {
        framebuffer_update_MZ700_all_rows();
    }
    else
    {
        framebuffer_MZ800_all_screen_rows_fill();
    };
    framebuffer_border_all_rows_fill();

    /* Vynucený plný refresh - SDL consumer (video_sdl3_update_surface_from_framebuffer)
     * gateuje memcpy snapshot -> surface na "framebuffer_state != FB_STATE_NOT_CHANGED".
     * Pokud je emulátor pauznutý, state je obvykle NOT_CHANGED (poslední screen_done
     * ho resetoval), takže by bez tohoto force nastavení consumer memcpy přeskočil
     * a obraz na ploše by zůstal starý i přes redraw_full_screen_request flag.
     * Označíme state jako SCREEN+BORDER changed - reálně jsme obě části vyplnili. */
    g_framebuffer.framebuffer_state |= FB_STATE_SCREEN_CHANGED | FB_STATE_BORDER_CHANGED;

    framebuffer_screen_done();
}

/*******************************************************************************
 *
 *
 *                  MZ-800 main
 *                  ===========
 *
 *
 *******************************************************************************/

static void mzarch_main_reset(void)
{
    printf("\nMZ800 Reset!\n");
    if (EMULATOR_TEST_PAUSED)
    {
        printf("But emulation is still PAUSED! :-)\n");
    };
    printf("\n");

    g_mzarch_main.reset_count++;
    videorec_on_reset(); /* událost reset + auto marker (bez nahrávání no-op) */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* PC se změnilo na 0000 - zruš případný BP skip vázaný na předchozí PC,
     * aby se BP na nové adrese (typicky 0000) správně aktivoval. */
    g_debugger.skip_bp_at_pc = -1;
    /* Frame-bounded run (mcp-debug-control request 0021): pokud reset přijde
     * za běhu frame-bounded runu, zrušíme aktivní flag. Cílová hodnota
     * run_frames_target byla vázána na předreset screens; po resetu (gdg_reset
     * vynuluje screens) by porovnání mohlo dát nedeterministický výsledek.
     * Bezpečné je run-bounded operaci zrušit - klient dostane zastavený stav
     * přes fallback v dispatch vrstvě. */
    g_debugger.run_frames_active = 0;
    /* D.3 - HW event BP hook (reset). */
    if ( g_bp_event_active[ BP_EVENT_CPU_RESET ] ) {
        bp_event_fire ( BP_EVENT_CPU_RESET, 0 );
    }
#endif

    // TODO: tohle se nedeje
    g_mzarch_main.interrupt = 0;

    g_mzarch_main.instruction_addr = 0x0000;

    gdg_reset();
    memory_reset();
    /* MZ-1X03 joystick (na MZ-800 buildu je to no-op stub). */
    joymz_reset();

    z80_reset(g_mzarch_main.cpu);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* D.4 - re-init SP polling baseline po resetu, aby se nehodil
     * falešný edge (z80_reset nastaví SP na 0xFFFF, dále program ho
     * pravděpodobně rychle modifikuje). */
    g_mzarch_main.bp_sp_prev = g_mzarch_main.cpu->sp;
    /* Stack regions: zachovat definice (= user-defined data) ale
     * resetovat watermarks + counters (= konzistentní s "spustit znovu
     * od začátku"). Vlastní definice zmizí až při shutdown/clear. */
    stack_regions_reset_all_stats ( );
    /* Stack history: vyprázdnit ring buffer při emu resetu - cycles
     * counter padá zpět na 0 a smíchaná stará/nová history by zlomila
     * monotónnost X-osy ve sparkline + slope výpočet. Aktivační flag
     * zachován (= pokud uživatel zapnul recording, pokračuje od čistého
     * stavu). */
    stack_history_reset ( );
    /* Callstack: shadow stack + statistiky vynulovat při emu resetu.
     * SP se mění (z80_reset → 0xFFFF), staré shadow framy by neměly
     * odpovídající CPU stack obsah a první RET sekvence by zlomila
     * divergence detekci. Aktivační flag (g_callstack_active) zachován
     * - subsystém zůstává zapnutý/vypnutý nezávisle na resetu. */
    callstack_reset ( );
#endif
#if HAVE_PIOZ80
    pioz80_reset();
    printer_reset(); // resync STROBE baseline; capture soubor zůstává
    mz1p16_emu_reset(); // reset jádra 8050 + mechaniky plotteru
#endif
    cmthack_reset();

#if CFG_HWEXT_HAVE_FDC
    fdc_reset();
#endif

    unicard_reset();

#if CFG_HWEXT_HAVE_IDE8
    ide8_reset();
#endif

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    debugger_update_all();
    /* Vlna 5 Commit 31 - SYS lifecycle event do Event Viewer.
     * Projekt aktuálně nerozlišuje cold/warm reset (= mzarch_main_reset je
     * generický reset pro tlačítko Reset / restart emu). Emit COLD_RESET. */
    eventlog_sys_event ( EVENTLOG_SYS_COLD_RESET, 0 );
#endif
}

/* Debug helper: read byte z aktualne mapovane pameti pro z80_dasm callback.
 * Pouzity v mzarch_main hot loop pro per-instruction PC + disassembly trace. */
static uint8_t mzarch_debug_dasm_read(uint16_t addr, void *user_data)
{
    (void)user_data;
    return memory_read_byte(addr);
}

/**
 * @brief Hodnota CLI volby s cestou souboru převedená do UTF-8.
 *
 * Interní cesty emulátoru jsou v UTF-8 (GLib). Na Windows je ale `argv`
 * v ANSI kódové stránce procesu, takže cesta s diakritikou z `argv` by
 * v UTF-8 API (g_fopen, g_file_test, avi_writer_open) selhala. Na Windows se
 * proto hodnota vezme z UTF-16 příkazové řádky (g_win32_get_command_line()),
 * bezeztrátově i pro znaky mimo ANSI kódovou stránku. Hledá se stejně jako
 * v sdlapp_options: token přesně `name` (hodnota v dalším tokenu), nebo
 * `name=hodnota`. Jinde (a když se na Windows volba nenajde) vrátí kopii
 * hodnoty z sdlapp_option_value().
 *
 * @param name Jméno volby včetně `--` (např. "--record").
 * @return Nově alokovaná hodnota v UTF-8 (g_free), nebo NULL bez hodnoty.
 */
static gchar *mzarch_cli_option_value_utf8(const char *name)
{
    const char *raw = sdlapp_option_value(name);
    if (!raw) return NULL;
#ifdef G_OS_WIN32
    gchar **wargv = g_win32_get_command_line();
    gchar *res = NULL;
    size_t nlen = strlen(name);
    for (int i = 1; wargv && wargv[i] && !res; i++) {
        if (strcmp(wargv[i], name) == 0 && wargv[i + 1]) {
            res = g_strdup(wargv[i + 1]);
        } else if (strncmp(wargv[i], name, nlen) == 0 && wargv[i][nlen] == '=') {
            res = g_strdup(wargv[i] + nlen + 1);
        }
    }
    g_strfreev(wargv);
    if (res) return res;
#endif
    return g_strdup(raw);
}

void mzarch_main(void)
{
    mzarch_main_reset();

    /* CLI option --run-mzf: po resetu automaticky spustit zadaný MZF. */
    {
        const char *mzf_path = sdlapp_option_value("--run-mzf");
        if (mzf_path)
        {
            g_print("CLI --run-mzf: %s\n", mzf_path);
            mzarch_bootstrap_run_mzf(mzf_path);
        };
    }

    /* CLI option --record: start video záznamu hned po bootu (a po --run-mzf).
     * Vlastní začátek nahrávání nastane na konci prvního emulovaného snímku. */
    if (sdlapp_option_present("--record"))
    {
        st_VIDEOREC_START rec;
        memset(&rec, 0, sizeof(rec));
        /* Cesta v UTF-8 i na Windows (argv je tam v ANSI kódové stránce). */
        gchar *rec_path = mzarch_cli_option_value_utf8("--record");
        g_strlcpy(rec.path, rec_path ? rec_path : "", sizeof(rec.path));
        g_free(rec_path);
        iface_audio_build_videorec_levels(rec.level, VIDEOREC_AUDIO_MAX_CHANNELS);
        const char *rec_frames = sdlapp_option_value("--record-frames");
        if (rec_frames)
        {
            rec.stop_after_frames = g_ascii_strtoull(rec_frames, NULL, 10);
        };
        rec.quit_after_stop = sdlapp_option_present("--headless");
        if (!videorec_request_start(&rec))
        {
            fprintf(stderr, "CLI --record: %s\n", videorec_get_last_error());
        };
    }

    g_print("%s main loop started\n", g_mzarch_full_name);

    while (1)
    {
        // Neprisel reset?
        APP_MUTEX_LOCK(g_mzarch_main.reset_request_mutex);
        if (g_mzarch_main.reset_request)
        {
            g_mzarch_main.reset_request = false;
            APP_MUTEX_UNLOCK(g_mzarch_main.reset_request_mutex);
            mzarch_main_reset();
        }
        else
        {
            APP_MUTEX_UNLOCK(g_mzarch_main.reset_request_mutex);
        };

        g_mzarch_main.instruction_addr = g_mzarch_main.cpu->pc;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* BP enforcement check před vykonáním instrukce.
         *
         * PC_EXEC fast path: 1 array lookup + branch (legacy bpmap[] z A').
         * V default OFF stavu (žádný BP, bpmap[] = BREAKPOINT_TYPE_NONE)
         * branch predictor naučí "vždy false" -> ~zero impact.
         *
         * skip_bp_at_pc: pokud rovno aktuálnímu PC, přeskočí kontrolu
         * (= "continue past BP"). Nastavuje se při unpause v
         * mzzarch_main_do_emulator_paused() na aktuální PC. Vázáno na PC,
         * ne jen flag - pokud reset změní PC mezi pause a unpause,
         * skip neplatí pro nové PC a BP na nové adrese se správně
         * aktivuje.
         *
         * D.2: breakpoints_enforce_pc_exec rozliší temporary vs trvalý:
         *  - temporary (Run To Cursor / Step Over CALL/RST/DJNZ/ED):
         *    pause + show debugger (bez BP okna)
         *  - trvalý: full enforcement smyčka (skip_count + condition +
         *    hit_count + action + MSG)
         *
         * GLOBAL hook: condition-only BP per-instruction. Test
         * any_active flagu šetří per-typ scan v default stavu.
         */
        if ( g_debugger.skip_bp_at_pc == (int) g_mzarch_main.instruction_addr ) {
            g_debugger.skip_bp_at_pc = -1;
        } else {
            if ( g_bptmap.bpmap[ g_mzarch_main.instruction_addr ] != BREAKPOINT_TYPE_NONE ) {
                breakpoints_enforce_pc_exec ( g_mzarch_main.instruction_addr );
            };
            /* V1.6+ TODO 4.3: PC_EXEC MASK BP plny support pres per-type
             * list (sparse mask nelze enumerate v bpmap[]). Default OFF
             * stav = jeden array load + branch (= zero impact). */
            if ( g_bptmap.per_type_active[ BPTMAP_IDX_PC_EXEC_NONSINGLE ] ) {
                breakpoints_enforce_pc_exec_nonsingle ( g_mzarch_main.instruction_addr );
            };
            if ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ] ) {
                breakpoints_enforce_global ( );
            };
            if ( EMULATOR_TEST_PAUSED ) {
                mzzarch_main_do_emulator_paused ( );
                continue;   /* zpět na začátek - reset check, znovu instruction_addr,
                             * skip_bp_at_pc přeskočí BP check, instrukce se vykoná */
            }
        }
#endif

#if 0
#if MZARCH == 700
        /* Debug trace: PC + disassembly aktualni instrukce pred jejim vykonanim. */
        {
            z80_dasm_inst_t _di;
            char _dbuf[64];
            z80_dasm(&_di, mzarch_debug_dasm_read, NULL, g_mzarch_main.instruction_addr);
            z80_dasm_to_str(_dbuf, (int)sizeof(_dbuf), &_di, NULL);
            printf("PC=%04X  %s\n", g_mzarch_main.instruction_addr, _dbuf);
        }
#endif
#endif
        g_mzarch_main.instruction_tstates = z80_step(g_mzarch_main.cpu);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* trace-suite cputrack hook - 1 event per dokončenou CPU instrukci.
         * Test je 1 ALU op + branch, branch predictor naučí "vždy false"
         * v default OFF stavu (= zero impact na hot path). Detail v
         * docs/cz/debugger/Trace_Suite.md.
         *
         * wait_extra_tstates = g_mzarch_main.instruction_wait_tstates,
         * akumulovany v INSIDEOP_MREQ_MZ700_VRAMCTRL a INSIDEOP_IORQ_PSG_WRITE
         * paths (jediná místa volající z80_add_wait_states v hot loop).
         * V T-states units (z80 ISA), saturated na 0xFFFFFFFF. */
        if ( TEST_TRACE_CPUTRACK_ACTIVE ) {
            /* RPFIX: celá práce hooku (Z17b range-scope filtr, odečet WAIT,
             * emit) je vynesena do NOINLINE cold helperu cputrack_hot_path_hook,
             * aby tělo této hot smyčky zůstalo kompaktní (stabilnější
             * code-layout / icache - viz cputrack.h @section hot_path_compactness).
             * V OFF stavu (g_cputrack_active==0) se sem řízení vůbec nedostane
             * = zero impact na hot path. */
            cputrack_hot_path_hook (
                (uint16_t) g_mzarch_main.instruction_addr,
                g_mzarch_main.instruction_tstates,
                g_mzarch_main.instruction_wait_tstates );
        }

        /* D.4 - SP-change polling (SP_THRESHOLD BP + stack regions).
         *
         * Default OFF stav (= žádný SP_THRESHOLD BP a žádný stack region):
         * 2 atomic byte read (per_type_active[idx] + g_stack_regions_active)
         * + branch. Branch predictor naučí "vždy false" -> ~zero overhead.
         *
         * Aktivní stav (= alespoň 1 z obou consumerů):
         * 1 register read + 1 compare na SP změnu, dispatch jen pokud
         * SP změnil hodnotu (= push/pop/call/ret/ld sp,X/ex (sp),hl/...).
         * Uvnitř if-bloku se každý consumer kontroluje svým flagem
         * samostatně - sdílíme jen tracking prev_sp a SP-change detekci,
         * ne dispatch.
         *
         * prev_sp tracking: g_mzarch_main.bp_sp_prev. Inicializace na
         * aktuální cpu->sp se provádí v mzarch_main_reset() (= žádný
         * falešný edge bezprostředně po resetu). Při změně threshold
         * BP zapnutí "za běhu" je první vyhodnocení proti staré
         * bp_sp_prev hodnotě - to je akceptovatelné (= odpovídá tomu,
         * kdyby BP existoval celou dobu).
         *
         * Pro stack regions: tracking watermark + push/pop counters bez
         * ohledu na zapnutí BPT - hook si interně testuje
         * g_stack_regions_active. */
        if ( g_bptmap.per_type_active[ BPTMAP_IDX_SP_THRESHOLD ]
              || g_stack_regions_active
              || g_stack_history_active ) {
            uint16_t curr_sp = g_mzarch_main.cpu->sp;
            if ( curr_sp != g_mzarch_main.bp_sp_prev ) {
                if ( g_bptmap.per_type_active[ BPTMAP_IDX_SP_THRESHOLD ] ) {
                    breakpoints_enforce_sp_threshold (
                        g_mzarch_main.bp_sp_prev, curr_sp );
                };
                if ( g_stack_regions_active ) {
                    stack_regions_on_sp_change (
                        g_mzarch_main.bp_sp_prev, curr_sp );
                };
                if ( g_stack_history_active ) {
                    /* V2: zaznamenej vzorek SP do ring bufferu (Option B
                     * sampling = sample jen při změně SP). Timestamp =
                     * cpu->total_cycles (monotónně rostoucí T-state counter).
                     * Reset na 0 při emu reset; pri tom history hook
                     * dostane vyprázdnění přes stack_history_reset z
                     * mzarch_main_reset. */
                    stack_history_record (
                        curr_sp, g_mzarch_main.cpu->total_cycles );
                };
                g_mzarch_main.bp_sp_prev = curr_sp;
            };
        }
#endif

#ifdef MZ800EMU_CFG_CLK1M1_SLOW
        mzarch_sync_ctc0_and_cmt(((g_mz800_main.instruction_tstates * GDGCLK2CPU_DIVIDER) - g_mz800_main.instruction_insideop_sync_ticks));
#else
        g_gdg.total_elapsed.ticks += ((g_mzarch_main.instruction_tstates * GDGCLK2CPU_DIVIDER) - g_mzarch_main.instruction_insideop_sync_ticks);
#endif

        g_mzarch_main.instruction_tstates = 0;
        g_mzarch_main.instruction_insideop_sync_ticks = 0;
        g_mzarch_main.instruction_wait_tstates = 0;

        if (g_gdg.total_elapsed.ticks >= g_mzarch_main.event.ticks)
        {

            mzarch_main_process_events();

            /* Přemapování MemExt vyžádané z UI (okno MemExt Map Settings,
             * memext_map_request). Tady CPU stojí mezi instrukcemi, stejně
             * jako u drainu dbgapi níže; funguje i bez debuggeru. Bez
             * požadavku 1 atomické čtení v per-event bloku, ne v
             * per-instruction hot path. */
            memext_map_request_poll();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            /* Per-frame body debuggeru (drain fronty dbgapi, Freeze Bytes),
             * ohlášené callbackem konce snímku. Tady je událost konce snímku
             * kompletně zpracovaná a CPU stojí mezi instrukcemi, takže port
             * I/O a zápisy do paměti z příkazů nemohou zpracovat konec snímku
             * podruhé. Kontrola je v per-event bloku, ne v per-instruction
             * hot path. Pokud konec snímku zpracovala insideop uvnitř
             * instrukce, provede se drain v nejbližším dalším průchodu tímto
             * blokem (nejpozději s další událostí). Před
             * mzarch_main_process_interrupt(), aby IRQ/NMI inject a změny
             * registrů z příkazů viděla obsluha přerušení ihned, stejně jako
             * při drainu v callbacku. */
            if (s_mzarch_frame_done_hooks_due)
            {
                mzarch_main_frame_done_debugger_hooks();
            };
#endif

            /* Ceka na nas nejaky interrupt? */
            mzarch_main_process_interrupt();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            /* Frame-bounded run (emu_run blokující path, mcp-debug-control
             * request 0021): emu se pausne DETERMINISTICKY přesně na cílové
             * frame hranici (ne async PAUSE z dispatch vlákna - to zastavovalo
             * na wall-clock-nedeterministickém cycle bodě). mzarch_main_process_events()
             * právě inkrementoval g_gdg.total_elapsed.screens (přes
             * gdg_on_screen_done_event makro), takže porovnání vidí aktuální
             * hodnotu. Po emulator_pause(true) stávající EMULATOR_TEST_PAUSED
             * blok níže okamžitě vstoupí do pause loopu.
             *
             * PERF: check je UVNITŘ per-frame bloku (~50 Hz), NE v
             * per-instruction hot path -> zanedbatelný dopad. V default stavu
             * (run_frames_active == 0) branch predictor naučí "vždy false". */
            if ( g_debugger.run_frames_active
                 && g_gdg.total_elapsed.screens >= g_debugger.run_frames_target )
            {
                g_debugger.run_frames_active = 0;
                g_emulator.pause_reason = EMU_PAUSE_REASON_FRAMES;
                emulator_pause ( true );
            };
#endif

            /* jsme v pauze? */
            if (EMULATOR_TEST_PAUSED)
            {
                mzzarch_main_do_emulator_paused();
            };
        };
    }
}

void mzarch_main_init(void)
{
    mzarch_main_queue_next_event();
}

/************************************************
 *
 *  Volani z UI
 *
 */

void mzarch_mode_sw_set(en_MZ800_MODE_SW mode)
{
#if MZARCH != 700
    g_mzarch_main.mode_sw = (mode == MZ800_MODE_SW_MZ800) ? MZ800_MODE_SW_MZ800 : MZ800_MODE_SW_MZ700;
#else
    /* MZ-700 nativní: přepínač MZ-700 / MZ-800 neexistuje, no-op */
    (void)mode;
#endif
}

bool mzarch_mode_sw_parse_cli(const char *text, en_MZ800_MODE_SW *out)
{
    if (text && strcmp(text, "700") == 0)
    {
        *out = MZ800_MODE_SW_MZ700;
        return true;
    }
    if (text && strcmp(text, "800") == 0)
    {
        *out = MZ800_MODE_SW_MZ800;
        return true;
    }
    fprintf(stderr, "Error: --mode-switch requires 700 or 800 (got: %s)\n", text ? text : "<none>");
    return false;
}

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

/**
 *  Pri Run to Cursor a Step Over nechceme, aby se nam prepinal focus na main window
 */
void mzarch_run_to_temporary_breakpoint(void)
{
    /* Video záznam: běh k dočasnému breakpointu (step over, run to cursor) je krokování. */
    videorec_on_debugger_run();
    g_emulator.paused = false;
    iface_audio_pause_emulation(0);

    // zkusime to bez spinner window
    g_debugger.run_to_temporary_breakpoint = 1;
#if 0
    if ( TEST_DEBUGGER_ACTIVE ) { // tady je to zrejme zbytecna podminka
        ui_debugger_show_spinner_window ( );
    };
#endif
}

#endif
