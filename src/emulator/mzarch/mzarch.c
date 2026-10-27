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
#include "hw-generic/cmt/cmt_cli.h"
#include "hw-generic/fdc/fdc_cli.h"
#include "hw-generic/pioz80/pioz80.h"
#include "hw-generic/printer/printer.h"
#include "hw-generic/mz1p16/mz1p16_emu.h"
#include "hw-generic/joy/joymz-1x03.h"
#include "hw-generic/devbus/devbus.h"
#include "audio.h"
#include "videorec/videorec.h"
#include "screenshot/screenshot_series.h"
#include "audio_capture.h"
#include "automation.h"

#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_vramwait.h"
#endif

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
#include "debugger/trace/ramlog.h"
#include "debugger/profiler_pc.h"
#include "debugger/coverage/coverage_hook.h"
#include "debugger/trace/intlog.h"
#include "debugger/trace/eventlog.h"
#include "debugger/callstack.h"
#include "debugger/freeze/freeze.h"
#include "debugger/run_control/run_control.h"
#include "debugger/run_control/run_log.h"
#include "mzarch/interrupt.h"
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include <json-glib/json-glib.h>
#include "mcp/event_bus.h"
#include "mcp/hid_script.h"
#endif
#endif

#include "replay.h"
#include "state_rev.h"
#include "snapshot/snapshot_request.h"
#include "device_request.h"
#include "replay_cli.h"

// TODO: tohle volame v mz800_main_do_emulator_paused()
#define iface_sdl_update_window_in_beam_interval(a, b)

st_mzarch_main g_mzarch_main;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
uint32_t g_mzarch_post_insn_hooks = 0;


void mzarch_post_insn_hooks_recompute(void)
{
    uint32_t mask = 0;
    if (TEST_TRACE_CPUTRACK_ACTIVE)
        mask |= MZARCH_POST_INSN_CPUTRACK;
    if (TEST_PROFILER_PC_ACTIVE)
        mask |= MZARCH_POST_INSN_PROFILER_PC;
    if (g_coverage_active)
        mask |= MZARCH_POST_INSN_COVERAGE;
    g_mzarch_post_insn_hooks = mask;
}
#endif

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
 * @brief Konec snímku ohlásil bod B_f záznamu vstupů (replay.h).
 *
 * Nastaví callback konce snímku jen při běžícím záznamu / přehrávání nebo
 * čekajícím požadavku (replay_frame_hook_wanted()); vyřídí ho per-event
 * blok hlavní smyčky za run bounds, před testem pauzy, ve všech buildech.
 *
 * @invariant Čte a píše ho jen EMU vlákno, proto není atomický.
 */
static bool s_mzarch_replay_due = false;

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
#if MZARCH == 700
    /* Volitelná zařízení devbus: per-frame bod (bez zařízení s frame_done jen
     * čtení čítače). Zařízení tu nesmí volat insideop (I/O, VRAM). */
    devbus_frame_done();
#endif
    /* --cmt-boot: uvolní drženou klávesu C, až ROM zapne motor (bez
     * držené klávesy jen jedno čtení příznaku). */
    cmt_cli_on_screen_done();
    customspeed_on_screen_done();

    /* Video záznam: per-frame bod (1 atomické čtení, když se nenahrává). */
    videorec_on_screen_done();

    /* Série screenshotů: per-frame bod (1 atomické čtení, když neběží). */
    screenshot_series_on_screen_done();

    /* Automatizace: čítač snímků a časové podmínky (--exit-after-frames,
     * --timeout-*). Až po videu a sérii, aby poslední snímek byl v záznamu.
     * Bez automatizačních voleb jen čtení příznaku. Konec se tu jen vyžádá
     * (pauza), vyřídí ho paused smyčka (automation_on_paused). */
    if (g_automation_frame_hook)
    {
        automation_on_screen_done();
    };

    /* Záznam a přehrání vstupů: bod B_f se tu jen ohlásí, provede ho
     * per-event blok mezi instrukcemi (replay.h). Bez záznamu jedno čtení. */
    if (replay_frame_hook_wanted())
    {
        s_mzarch_replay_due = true;
    };

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
 * @brief Vykoná jeden vyzvednutý dbgapi příkaz a uklidí účty insideop.
 *
 * UI i MCP submitují příkazy přes dbgapi_ui_submit_cmd_sync*(); EMU vlákno
 * je vykonává přes tuto funkci z drainu běžící emulace
 * (mzarch_main_dbgapi_drain()) i paused smyčky
 * (dbgapi_emu_drain_paused()). Dokončení (dbgapi_emu_complete()) dělá
 * volající.
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
 * Výjimka IO_READ a IO_WRITE (MCP io_read / io_write, okno I/O): před
 * příkazem se cpu->op_tstate dočasně vynuluje. Přístup debuggeru k portu
 * leží přesně mezi instrukcemi, insideop pak čas nedorovná o délku
 * předchozí instrukce a příkaz čas emulace neposune (dřív o délku
 * poslední instrukce, MZ-700 typicky 20 taktů GDG, MZ-800 35, každým
 * příkazem; test mcp_dbgapi_io_paused_time). Čekací stavy zápisu do PSG
 * (INSIDEOP_IORQ_PSG_WRITE) zůstávají. Ostatní příkazy op_tstate
 * nemění: snapshot ukládá op_tstate do skrytého stavu CPU.
 *
 * @pre Volá se jen z EMU vlákna v konzistentním bodě mezi instrukcemi
 *      (hlavní smyčka za mzarch_main_process_events() nebo paused smyčka):
 *      nikdy z callbacku události ani z insideop. V obou bodech jsou účty
 *      insideop nulové (hlavní smyčka je nuluje po každém z80_step(),
 *      cpu->wait_cycles spotřebuje z80_step()).
 * @param rq Vyzvednutý slot (dbgapi_emu_dequeue()).
 * @post Účty insideop jsou nulové, cpu->op_tstate má hodnotu z doby před
 *       příkazem. Slot ještě není dokončený.
 */
static void mzarch_main_dbgapi_dispatch_one(st_DBGAPI_CMDRQ *rq)
{
    z80_t *cpu = g_mzarch_main.cpu;
    const int saved_op_tstate = cpu->op_tstate;
    const en_DBGAPI_CMD cmd = (en_DBGAPI_CMD)(rq->cmd & DBGAPI_CMD_MASK);

    /* Port I/O debuggeru mezi instrukcemi: insideop bez předstihu (viz výše). */
    if (cmd == DBGAPI_CMD_IO_READ || cmd == DBGAPI_CMD_IO_WRITE) {
        cpu->op_tstate = 0;
    }

    dbgapi_emu_dispatch(rq);

    /* cputrack v3: příkaz mohl změnit registry mimo instrukci (set_register,
     * příznaky CPU, run MZF, snapshot, reset, ...). Bez změny nic nezapíše. */
    cputrack_sync_check();

    /* Úklid účtů insideop po operaci mimo instrukci (viz výše). */
    g_mzarch_main.instruction_insideop_sync_ticks = 0;
    g_mzarch_main.instruction_wait_tstates = 0;
    cpu->wait_cycles = 0;
    cpu->op_tstate = saved_op_tstate;
}

/**
 * @brief Drain fronty dbgapi běžící emulace (per-frame bod hlavní smyčky).
 *
 * Každý příkaz provede mzarch_main_dbgapi_dispatch_one() (dispatch + úklid
 * účtů insideop, viz výše) a dokončí ho.
 *
 * @pre Viz mzarch_main_dbgapi_dispatch_one().
 * @post Fronta byla v okamžiku kontroly prázdná. Příkaz zařazený během
 *       drainu se vykoná ještě v tomto volání.
 */
static void mzarch_main_dbgapi_drain(void)
{
    while (dbgapi_emu_has_pending(&g_dbgapi_cmdrq_queue))
    {
        st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue);
        if (rq)
        {
            mzarch_main_dbgapi_dispatch_one(rq);
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

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    /* Sekvence vstupu MCP (hid_script.c): stisky a uvolnění na hranici
     * snímku emulace, nezávisle na vykreslení a rychlosti emulace. Před
     * drainem, aby sekvence spuštěná v tomto drainu začala počítat až od
     * dalšího snímku a zrušení z drainu vidělo stav po kroku. Bez běžící
     * sekvence jedno čtení ukazatele. */
    if (g_hid_script_active)
    {
        hid_script_on_frame();
    };
#endif

    /* Během kroku debuggeru (STEP_INTO, step over bez CALL) frontu
     * nevybírat: paused smyčka ji vybere hned po kroku (krok emulaci
     * z pauzy nevyvede, vstup do pauzy následuje v tomto bloku událostí).
     * Příkaz, který klient poslal po dokončení příkazu kroku, by se tu
     * jinak zpracoval ještě před koncem kroku: další STEP_INTO by se
     * ztratil (debugger_step_call(1) při nastaveném kroku nic neudělá).
     * Změřeno přes MCP step_n: na každé hranici snímku jeden ztracený krok. */
    if (!TEST_DEBUGGER_STEP_CALL)
    {
        mzarch_main_dbgapi_drain();
    };

    /* Přehrání zásahů debuggeru zaznamenaných v drainu tohoto snímku
     * (poloha `d`, replay.h). Bez záznamu jedno čtení. */
    if (replay_frame_hook_wanted())
    {
        replay_on_drain_point();
    };

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
 * @brief Nejdelší smysluplný odstup uvolnění WRITE FF od přístupu (CLK0).
 *
 * Zápis ve viditelné části režimu MZ-700 se do DRAM dostane nejpozději
 * v prvním CPU slotu po jejím konci, tedy do jednoho řádku. Hodnota
 * vzdálenější do budoucnosti je pozůstatek jiné časové osy (např. po
 * přenastavení času) a model ji zahodí, aby nevznikl nesmyslně dlouhý WAIT.
 */
#define VRAMWAIT_WRFF_MAX_AHEAD ( 2u * VRAMWAIT_LINE_CLK0 )

#if VIDEO_SCREEN_WIDTH != 1136
#error "Model WAIT (mz800_vramwait.h) předpokládá řádek PAL 1136 CLK0"
#endif
#if GDGCLK2CPU_DIVIDER != 5
#error "Model WAIT (mz800_vramwait.h) předpokládá T-stav 5 CLK0"
#endif
#endif /* MZARCH == 800 */

/**
 * @brief Synchronizace GDG uvnitř instrukce (přístup do I/O, VRAM, PSG).
 *
 * Dorovná čas GDG k aktuálnímu T-stavu instrukce (s čekacími stavy podle
 * druhu přístupu) a zpracuje události, které do té chvíle nastaly (i konec
 * snímku). Per-event blok hlavní smyčky pak vynutí hned za instrukcí
 * (událost s takty 0), když čeká přerušení, pauza nebo krok debuggeru,
 * nebo bod B_f záznamu vstupů (s_mzarch_replay_due). Bez vynucení by B_f
 * proběhl až s další naplánovanou událostí a pauza vzniklá v callbacku
 * konce snímku (např. --exit-after-frames) by ho posunula jen při záznamu,
 * ne při přehrávání: rozdílný stav v posledním hashi (změřeno).
 *
 * @param insideop Druh přístupu (určuje čekací stavy).
 * @pre Emu vlákno uvnitř instrukce (callback přístupu CPU).
 */
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
        /* WAIT u přístupu CPU do VRAM přes GDG (mz800_vramwait.h): grafická
         * VRAM 8000h-0BFFFh v režimu MZ-800, VRAM a CG-RAM 0C000h-0DFFFh
         * v režimu MZ-700. Rozvrh CPU slotů podle DMD bit 3, počet
         * zápisových slotů podle rozlišení.
         *
         * Předpoklad: předtím proběhlo mzarch_main_insideop_mreq(), takže
         * g_gdg.total_elapsed.ticks je konec M-cyklu bez WAIT, tedy T1r
         * + 3 T (u instrukčního výběru M1 + 4 T).
         *
         * 1 TW = GDGCLK2CPU_DIVIDER CLK0 = jeden T-stav navíc. */
        unsigned cycle_t = ( ( insideop == INSIDEOP_MREQ_MZ800_VRAMCTRL_READ ) && g_mzarch_main.mreq_m1 ) ? 4u : 3u;
        uint64_t t1r = gdg_get_vramwait_ticks( ) - cycle_t * GDGCLK2CPU_DIVIDER;
        en_VRAMWAIT_GRID grid = GDG_MZ800_DMD_TEST_MZ700 ? VRAMWAIT_GRID_MZ700 : VRAMWAIT_GRID_MZ800;
        unsigned wait_tw;

        if ( g_gdg.vram_wrff_release > t1r + VRAMWAIT_WRFF_MAX_AHEAD ) {
            g_gdg.vram_wrff_release = 0;
        }

        if ( insideop == INSIDEOP_MREQ_MZ800_VRAMCTRL_WRITE ) {
            int two_slots = ( grid == VRAMWAIT_GRID_MZ800 ) && !GDG_MZ800_DMD_TEST_SCRW640;
            wait_tw = vramwait_write( &g_gdg.vram_wrff_release, t1r, grid, two_slots );
        } else {
            wait_tw = vramwait_read( g_gdg.vram_wrff_release, t1r, grid );
        }

        if ( wait_tw ) {
            z80_add_wait_states( g_mzarch_main.cpu, (int) wait_tw );
            /* trace-suite: saturujici akumulace WAIT T-states pro cputrack. */
            uint64_t wsum = (uint64_t) g_mzarch_main.instruction_wait_tstates + wait_tw;
            g_mzarch_main.instruction_wait_tstates = ( wsum > 0xFFFFFFFFu ) ? 0xFFFFFFFFu : (uint32_t) wsum;
        }
        ticks_to_sync = wait_tw * GDGCLK2CPU_DIVIDER;

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
    else if ((EMULATOR_TEST_PAUSED) || (TEST_DEBUGGER_STEP_CALL) || (s_mzarch_replay_due))
    {
#else
    }
    else if ((EMULATOR_TEST_PAUSED) || (s_mzarch_replay_due))
    {
#endif
        /* Bod B_f záznamu vstupů hned za touto instrukcí, nezávisle na
         * pauze (viz Doxygen výše). Bez záznamu je s_mzarch_replay_due
         * vždy false. */
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
 *
 * Žádost o přerušení je HW linka INT (`g_mzarch_main.interrupt`, úrovňový
 * signál čipů) nebo čekající NMI / IRQ nastavené mimo HW (MCP `nmi_inject`,
 * `irq_inject` = `cpu->nmi_pending` / `cpu->int_pending`). NMI jádro přijme
 * vždy, INT jen při IFF1 a mimo instrukci po EI. Linka INT se do jádra
 * předá (z80_int) jen při povolených přerušeních, takže se úrovňový signál
 * nezapamatuje; čekající IRQ z injectu (vlastní vektor) se nepřepisuje.
 * Když INT čeká a přerušení jsou zakázaná, kontrola se zopakuje po další
 * instrukci (MZEVENT_BREAK_MZARCH_INTERRUPT).
 *
 * Bez žádosti jedno čtení masky a dvou příznaků jádra (per-event cesta,
 * ne instrukční smyčka).
 */
static inline void mzarch_main_process_interrupt(void)
{
    z80_t *cpu = g_mzarch_main.cpu;

    if (!g_mzarch_main.interrupt && !cpu->nmi_pending && !cpu->int_pending)
        return;

    bool int_enabled = cpu->iff1 && !cpu->ei_delay;
    /* NMI má v jádře přednost; přijetí níže je tedy NMI. */
    bool is_nmi = cpu->nmi_pending;

    if (is_nmi || int_enabled)
    {
        if (g_mzarch_main.interrupt && int_enabled && !cpu->int_pending)
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
        /* Záznam zápisů (ramlog): push návratové adresy při přijetí jde mimo
         * instrukci - zdroj INT_ACK / NMI_ACK, insn_pc = návratová adresa
         * (z HALT o 1 dál, viz z80.c handle_interrupts_internal). */
        uint32_t ramlog_saved = 0;
        if (g_ramlog_active)
            ramlog_saved = ramlog_source_push(cpu->nmi_pending ? RAMLOG_SRC_NMI_ACK : RAMLOG_SRC_INT_ACK,
                                              (uint16_t)(cpu->pc + (cpu->halted ? 1 : 0)));
#endif

        int interrupt_tstates = z80_process_interrupt(cpu);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        if (g_ramlog_active)
            ramlog_source_pop(ramlog_saved);
#endif

        if (interrupt_tstates > 0)
        { /* interrupt byl prijat */

            /* Potvrzení INTACK patří jen přijetí INT, NMI ho nemá. */
            if (!is_nmi)
                pioz80_interrupt_ack();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            /* cputrack v2: záznam přijetí přerušení (adresa obsluhy, T-stavy
             * přijetí), aby součet T-stavů záznamů odpovídal času CPU.
             * Cesta přijetí přerušení, ne instrukční smyčka. */
            if (TEST_TRACE_CPUTRACK_ACTIVE)
            {
                cputrack_on_interrupt_accept((uint16_t)cpu->pc, (uint32_t)interrupt_tstates);
            };
            /* Pokrytí kódu: první instrukce obsluhy (příští krok) dostane
             * příznak INT_ENTRY. Cesta přijetí přerušení, ne instrukční
             * smyčka (stejná výjimka jako ramlog výše). */
            if (g_coverage_active)
            {
                g_coverage_pending_flags |= COVERAGE_F_INT_ENTRY;
            };
#endif

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
            if (TEST_TRACE_INTLOG_DISPATCH && !is_nmi && cpu->im == 2) {
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
            if ( !is_nmi && g_bptmap.per_type_active[ BPTMAP_IDX_IRQ ] ) {
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
            if ( is_nmi )
            {
                /* NMI z MCP nmi_inject: rámec CS_KIND_NMI (cíl 0066h). */
                callstack_on_nmi_accept ( pc_before_int );
            }
            else
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
    { /* INT čeká (linka nebo inject), ale IFF1 je 0 nebo jsme po EI */
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

/**
 * @brief Po výstupu z pauzy znovu naplánovat kontrolu čekajícího přerušení.
 *
 * Výstup z pauzy přeplánuje další událost (mzarch_main_queue_next_event())
 * a tím zahodí přerušení zkontrolovat po příští instrukci
 * (MZEVENT_BREAK_MZARCH_INTERRUPT s takty 0), které při nepřerušeném běhu
 * drží mzarch_main_process_interrupt(), insideop a změna IFF, dokud INT
 * čeká. Bez obnovení se přerušení čekající v okamžiku pauzy (typicky
 * pauza mezi EI a další instrukcí, nebo v HALT při zakázaném přerušení)
 * přijme až s další skutečnou událostí, tedy později než bez pauzy, a běh
 * se časově rozejde (N85: goto přehrávání a přehrávání záznamu po pauze).
 * Kontrola navíc je bez vlivu na čas: blok událostí bez čekající události
 * nic nezmění a mzarch_main_process_interrupt() při zakázaném přerušení
 * jen znovu naplánuje kontrolu.
 *
 * @pre Emu vlákno, výstup z paused smyčky po mzarch_main_queue_next_event().
 * @post Čeká-li linka INT, NMI nebo IRQ z injectu, je naplánovaná kontrola
 *       po příští instrukci; jinak beze změny.
 */
static inline void mzarch_main_resume_interrupt_check(void)
{
    z80_t *cpu = g_mzarch_main.cpu;
    if (g_mzarch_main.interrupt || cpu->nmi_pending || cpu->int_pending)
    {
        MZ800_MAIN_SET_EVENT(MZEVENT_BREAK_MZARCH_INTERRUPT, 0);
    };
}


#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
/**
 * @brief Pošle MCP událost `state_changed`, pokud v pauze stav stroje změnil
 *        jiný zdroj než MCP klient.
 *
 * Volá se jednou za iteraci paused smyčky, takže změny jedné iterace (např.
 * editace v Memory Browseru po bajtech) dají nejvýš jednu událost. Payload
 * `{state_rev, sources}`: revize po změnách a jména zdrojů
 * (state_rev_src_name()) v pořadí bitů; zdroj `mcp` je v seznamu, jen když
 * se v téže iteraci sešel s jiným zdrojem. Změny jen od MCP klienta
 * (odpověď nástroje nese `state_rev`) a z běhu (`paused`, `resumed`) se
 * neohlašují. Revize se označí za ohlášenou vždy, i bez odběratele.
 *
 * @pre Emu vlákno, emulace v pauze.
 * @post state_rev_take_unannounced() vynulovalo zdroje.
 */
static void mzarch_main_emit_state_changed(void)
{
    uint64_t rev = 0;
    uint32_t src = state_rev_take_unannounced(&rev);
    if (!(src & ~(uint32_t)(STATE_REV_SRC_MCP | STATE_REV_SRC_RUN))) return;
    if (!event_bus_has_subscriber("state_changed")) return;
    JsonObject *payload = json_object_new();
    json_object_set_int_member(payload, "state_rev", (gint64)rev);
    JsonArray *names = json_array_new();
    for (int i = 0; i < STATE_REV_SRC_COUNT_BITS; i++)
    {
        uint32_t bit = 1u << i;
        if ((src & bit) && bit != STATE_REV_SRC_RUN) json_array_add_string_element(names, state_rev_src_name(bit));
    };
    json_object_set_array_member(payload, "sources", names);
    event_bus_emit("state_changed", payload);
}
#endif


static inline void mzzarch_main_do_emulator_paused(void)
{
    /* Video záznam: emulace stojí - běh k dočasnému BP (step over / run to cursor)
     * skončil. Musí být před prvním drainem dbgapi níže: pokračování zpracované
     * v první iteraci smyčky by jinak příznak nechalo přežít do volného běhu. */
    videorec_on_emulation_stopped();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Záznam zastavení a jediná událost "paused" na každé zastavení (BP,
     * dočasný BP, krok, run(frames), deadline, pauza). Před automatizací,
     * aby záznam existoval i u žádosti o konec, a před
     * debugger_step_call(0), aby se poznal dokončený krok. */
    run_control_on_stop();
#endif

    /* Automatizace: čekající žádost o konec (akce exit, --exit-after-frames,
     * --timeout-*) se vyřídí tady, mezi instrukcemi a v pauze (snapshot
     * smí vzniknout). Kde smí proces skončit, se odtud nevrací. */
    automation_on_paused();

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
        /* cycles = cpu->total_cycles (uint32_t, přeteče asi po 20 min
         * emulace); ticks = takty GDG od startu emulátoru (64 bit, bez
         * přetečení; reset stroje je nenuluje). */
        json_object_set_int_member ( payload, "cycles",
            (gint64) ( g_mzarch_main.cpu ? g_mzarch_main.cpu->total_cycles : 0 ) );
        json_object_set_int_member ( payload, "ticks", (gint64) gdg_get_total_ticks ( ) );
        json_object_set_int_member ( payload, "run_seq", (gint64) run_log_current ( NULL ) );
        json_object_set_int_member ( payload, "state_rev", (gint64) g_state_rev );
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
         * dbgapi_emu_wait_for_cmd() (nejvýš 20 ms). Na rozdíl od drainu
         * běžící emulace skončí po příkazu, který vyžádal krok (zbytek fronty
         * se zpracuje až po kroku). */
        (void)dbgapi_emu_drain_paused(&g_dbgapi_cmdrq_queue, mzarch_main_dbgapi_dispatch_one);

        /* Přemapování MemExt vyžádané z UI i během pauzy (do ~20 ms). */
        memext_map_request_poll();

        /* Zásahy GUI do zařízení (device_request.h) i během pauzy. Bez
         * zásahu jedno atomické čtení. */
        device_request_poll();

        /* Záznam a přehrání vstupů: start, stop, reset a autotype v pauze
         * (replay.h). Bez záznamu a požadavku jen dvě čtení. */
        replay_on_paused();

        /* Snapshot z GUI (Load / Save / Quick Load / Quick Save) vyřídí
         * emu vlákno v pauze (snapshot_request.h). Bez požadavku jedno
         * atomické čtení. */
        snapshot_request_poll();

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

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
        /* Událost state_changed: změny stavu stroje v této iteraci od jiného
         * zdroje než MCP klient (sloučené, nejvýš jedna za iteraci). */
        mzarch_main_emit_state_changed();
#endif

        /* Čekání na další iteraci: nejvýš 20 ms jako dřív (g_usleep), ale
         * příkaz z dbgapi fronty smyčku probudí hned (queue_cond). Bez toho
         * čekal každý synchronní příkaz UI v pauze až 20 ms - u editace
         * v Memory Browseru (zápis přes frontu po každém bajtu) a u
         * vícekrokových MCP handlerů se to sčítalo. Návratová hodnota je
         * nepodstatná, drain proběhne na začátku další iterace.
         *
         * Čeká se jen tehdy, když emulace po drainu pořád stojí. Když drain
         * právě zpracoval Run nebo Step (pauza zrušena nebo krok vyžádán),
         * smyčka hned skončí; jinak by se rozběh odložil o celých 20 ms,
         * protože nový příkaz, který by čekání probudil, nepřijde - klient
         * čeká na výsledek (step_done, breakpoint). Změřeno přes MCP pipe:
         * krok i rozběh k breakpointu 21 ms -> pod 1 ms. Odpauzování mimo
         * frontu (klávesa Pause v GUI z UI vlákna) čekání neprobudí, tam
         * zůstává nejvýš 20 ms jako dřív. */
        if (EMULATOR_TEST_PAUSED && !TEST_DEBUGGER_STEP_CALL)
        {
            (void)dbgapi_emu_wait_for_cmd(&g_dbgapi_cmdrq_queue, 20);
        };
    };

    /* Po opuštění pause smyčky (= unpause přes F5/F4/F7/F8 nebo Run To
     * Cursor) nastavíme skip_bp_at_pc na aktuální PC. Hot loop pak
     * přeskočí BP enforcement pro 1 instrukci na této adrese, aby
     * uživatel neuvízl v infinite pause loopu (= BP hit → pause →
     * unpause → BP znovu hit). Vázáno na konkrétní PC, takže pokud
     * mezi tím proběhne reset (= PC se změní na 0000), skip se
     * neuplatní na novou adresu a BP na 0000 se správně aktivuje. */
    g_debugger.skip_bp_at_pc = (int) g_mzarch_main.cpu->pc;

    /* cputrack v3: pojistka pro změny registrů během pauzy mimo dbgapi. */
    cputrack_sync_check();

    /* Nové číslo běhu, vynulovaný důvod pauzy, událost "resumed" (jen
     * u rozběhu, ne u kroku). */
    run_control_on_resume();

    if (TEST_DEBUGGER_STEP_CALL)
    {
        MZ800_MAIN_SET_EVENT(MZEVENT_BREAK_EMULATION_PAUSED, 0);
    }
    else
    {
        mzarch_main_queue_next_event();
        mzarch_main_resume_interrupt_check();
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

        /* Zásahy GUI do zařízení (device_request.h) i během pauzy. Bez
         * zásahu jedno atomické čtení. */
        device_request_poll();

        /* Záznam a přehrání vstupů: start, stop, reset a autotype v pauze
         * (replay.h). Bez záznamu a požadavku jen dvě čtení. */
        replay_on_paused();

        /* Snapshot z GUI (Load / Save / Quick Load / Quick Save) vyřídí
         * emu vlákno v pauze (snapshot_request.h). Bez požadavku jedno
         * atomické čtení. */
        snapshot_request_poll();

        mzarch_main_queue_next_event();

        /* Video záznam: stop požadovaný v pauze se provede hned (jinak až po
         * odpauzování). Bez nahrávání 1 atomické čtení. */
        videorec_on_emulation_paused();

        /* Spánek v paused smyčce - bez něj by EMU vlákno v pauze busy-spinilo
         * na 100 % jádra. Parita s debugger větví (g_usleep výše). UI běží na
         * vlastním vlákně, takže pauza zůstává okamžitě zrušitelná (Alt+P). */
        g_usleep(20 * 1000);
    };
    mzarch_main_resume_interrupt_check();
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
    /* Revize stavu (state_rev.h, Z4): reset z GUI, MCP i záznamu vstupů;
     * reset vyžádaný příkazem MCP klienta se připíše MCP. */
    STATE_REV_BUMP(g_state_rev_reset_src ? g_state_rev_reset_src : STATE_REV_SRC_RESET);
    g_state_rev_reset_src = 0;
    videorec_on_reset(); /* událost reset + auto marker (bez nahrávání no-op) */
    cmt_cli_on_reset(); /* --cmt-boot: pustit drženou klávesu C */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* PC se změnilo na 0000 - zruš případný BP skip vázaný na předchozí PC,
     * aby se BP na nové adrese (typicky 0000) správně aktivoval. */
    g_debugger.skip_bp_at_pc = -1;
    /* Frame-bounded run (mcp-debug-control request 0021): pokud reset přijde
     * za běhu frame-bounded runu, zrušíme aktivní flag - klient dostane
     * zastavený stav přes fallback v dispatch vrstvě. (gdg_reset() čítač
     * snímků nenuluje, nuluje ho jen gdg_init() při startu - změřeno; cíl by
     * tedy platil dál, zrušení zůstává kvůli dosavadnímu chování.) Deadline
     * běhu (run_control.h) reset neruší, běží v témže čítači. */
    g_debugger.run_frames_active = 0;
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    /* Sekvence vstupu MCP má cíle vázané na předreset screens - zrušit
     * (uvolní drženou klávesu, dispatch dostane cancelled). */
    hid_script_on_reset();
#endif
    /* D.3 - HW event BP hook (reset). */
    if ( g_bp_event_active[ BP_EVENT_CPU_RESET ] ) {
        bp_event_fire ( BP_EVENT_CPU_RESET, 0 );
    }
    /* Pokrytí kódu: reset je nespojitost běhu (data se nemažou, jen se
     * započte; s reset_on_discontinuity se smažou). */
    coverage_note_discontinuity ( );
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
    /* cputrack v3: reset změnil registry mimo instrukci (synchronizační záznam). */
    cputrack_sync_check();
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
    /* Záznam zápisů: záznamy se týkají stavu před resetem - vyprázdnit
     * (běžící záznam pokračuje v novém segmentu). */
    ramlog_clear ( );
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

#if MZARCH == 700
    /* Volitelná zařízení devbus: signál RESET (obsah RAM zařízení se nemění). */
    devbus_reset_all();
#endif

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    debugger_update_all();
    /* Vlna 5 Commit 31 - SYS lifecycle event do Event Viewer.
     * Projekt aktuálně nerozlišuje cold/warm reset (= mzarch_main_reset je
     * generický reset pro tlačítko Reset / restart emu). Emit COLD_RESET. */
    eventlog_sys_event ( EVENTLOG_SYS_COLD_RESET, 0 );
#endif
}

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/**
 * @brief Rozbočovač háčků po dokončení instrukce (cputrack, profiler podle PC, pokrytí kódu).
 *
 * Hlavní smyčka sem vstupuje jedinou podmínkou `g_mzarch_post_insn_hooks`
 * (maska MZARCH_POST_INSN_*); teprve tady se podle bitů rozhoduje, který
 * háček je aktivní. Informaci o instrukci (st_MZARCH_INSN_INFO) spočítá
 * jednou pro všechny konzumenty. Atribut noinline drží tělo hlavní smyčky
 * kompaktní (viz cputrack.h @section hot_path_compactness).
 *
 * @param hooks   Kopie masky g_mzarch_post_insn_hooks z brány (nenulová).
 *
 * @pre Volá se z emu vlákna hned po z80_step(), před blokem událostí
 *      (přijetí přerušení); cpu->pc je PC po kroku.
 */
__attribute__((noinline))
static void mzarch_main_post_instruction_hooks(uint32_t hooks)
{
    const st_MZARCH_INSN_INFO insn = {
        .pc = (uint16_t) g_mzarch_main.instruction_addr,
        .next_pc = (uint16_t) g_mzarch_main.cpu->pc,
        .tstates = (uint32_t) g_mzarch_main.instruction_tstates,
        .wait = g_mzarch_main.instruction_wait_tstates,
    };
    if ( hooks & MZARCH_POST_INSN_CPUTRACK ) {
        cputrack_hot_path_hook ( insn.pc, insn.tstates, insn.wait );
    }
    if ( hooks & MZARCH_POST_INSN_PROFILER_PC ) {
        profiler_pc_on_instruction ( insn.pc, insn.tstates, insn.wait );
    }
    if ( hooks & MZARCH_POST_INSN_COVERAGE ) {
        coverage_hook_on_instruction ( &insn );
    }
}
#endif

/* Debug helper: read byte z aktualne mapovane pameti pro z80_dasm callback.
 * Pouzity v mzarch_main hot loop pro per-instruction PC + disassembly trace. */
static uint8_t mzarch_debug_dasm_read(uint16_t addr, void *user_data)
{
    (void)user_data;
    return memory_read_byte(addr);
}

/* Dokumentace viz mzarch.h. */
char *mzarch_cli_option_value_utf8(const char *name)
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

char *mzarch_cli_option_value_utf8_nth(const char *name, int n)
{
    const char *raw = sdlapp_option_value_nth(name, n);
    if (!raw) return NULL;
#ifdef G_OS_WIN32
    gchar **wargv = g_win32_get_command_line();
    gchar *res = NULL;
    size_t nlen = strlen(name);
    int k = 0;
    for (int i = 1; wargv && wargv[i] && !res; i++) {
        bool spaced = (strcmp(wargv[i], name) == 0);
        bool joined = !spaced && strncmp(wargv[i], name, nlen) == 0 && wargv[i][nlen] == '=';
        if (!spaced && !joined) continue;
        if (k++ != n) continue;
        if (joined) {
            res = g_strdup(wargv[i] + nlen + 1);
        } else if (wargv[i + 1]) {
            res = g_strdup(wargv[i + 1]);
        }
        break;
    }
    g_strfreev(wargv);
    if (res) return res;
#endif
    return g_strdup(raw);
}

void mzarch_main_reset_and_run_mzf(const char *filename)
{
    /* Stejná sekvence jako start s CLI --run-mzf v mzarch_main(). */
    mzarch_main_reset();
    mzarch_bootstrap_run_mzf(filename);
}

void mzarch_main_reset_now(void)
{
    mzarch_main_reset();
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

#if CFG_HWEXT_HAVE_FDC
    /* CLI volby --fdd, --fdd<N>: disky vložila už fdc_init(); chyba, kterou
     * šlo poznat až tam (odpojený FDC1, obraz nejde otevřít), = konec
     * s chybou (v GUI pauza se zprávou). */
    (void) fdc_cli_report();
#endif

    /* CLI volby --cmt, --cmt-play, --cmt-play-pause, --cmt-boot: vložení
     * pásky do virtuálního CMT (po --run-mzf, aby program spuštěný z CLI
     * mohl další díly nahrát z této pásky). */
    cmt_cli_apply();

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

    /* CLI volby --audio-capture, --audio-capture-frames: zachycení zvuku
     * hned po bootu (a po --run-mzf). Začne na konci zvukového snímku, se
     * sdl.wav po rozjezdu výstupní cesty (audio_capture.h). */
    if (sdlapp_option_present("--audio-capture"))
    {
        st_AUDIO_CAPTURE_PARAMS cap;
        audio_capture_params_from_settings(&cap);
        gchar *cap_dir = mzarch_cli_option_value_utf8("--audio-capture");
        g_strlcpy(cap.dir, cap_dir ? cap_dir : "", sizeof(cap.dir));
        g_free(cap_dir);
        const char *cap_frames = sdlapp_option_value("--audio-capture-frames");
        if (cap_frames)
        {
            cap.frames = g_ascii_strtoull(cap_frames, NULL, 10);
        };
        cap.quit_after_stop = sdlapp_option_present("--headless");
        char cap_err[AUDIO_CAPTURE_TEXT_MAX];
        if (!audio_capture_start(&cap, cap_err, sizeof(cap_err)))
        {
            fprintf(stderr, "CLI --audio-capture: %s\n", cap_err);
        };
    }

    /* Automatizace: začátek počítání snímků (po --run-mzf a ostatních
     * volbách), breakpointy z --bpt / --exit-on-*. */
    automation_start();

    /* Záznam a přehrání vstupů z příkazové řádky (--replay-record,
     * --replay): požadavek vyřídí první hranice snímku. */
    replay_cli_start();

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
        /* Háčky po instrukci (trace-suite cputrack, profiler podle PC) -
         * 1 událost na dokončenou CPU instrukci. Brána čte jedinou
         * proměnnou, masku g_mzarch_post_insn_hooks (přepočítávanou při
         * startu a stopu konzumentů), branch predictor se naučí "vždy
         * false" v default OFF stavu. Detail v
         * docs/cz/debugger/Trace_Suite.md a profiler_pc.h.
         *
         * wait_extra_tstates = g_mzarch_main.instruction_wait_tstates,
         * akumulovany v INSIDEOP_MREQ_MZ700_VRAMCTRL a INSIDEOP_IORQ_PSG_WRITE
         * paths (jediná místa volající z80_add_wait_states v hot loop).
         * V T-states units (z80 ISA), saturated na 0xFFFFFFFF. */
        uint32_t post_insn_hooks = g_mzarch_post_insn_hooks;
        if ( post_insn_hooks ) {
            /* RPFIX: celá práce háčků je vynesena do NOINLINE helperu,
             * aby tělo této hot smyčky zůstalo kompaktní (stabilnější
             * code-layout / icache - viz cputrack.h @section hot_path_compactness).
             * V OFF stavu se sem řízení vůbec nedostane. */
            mzarch_main_post_instruction_hooks ( post_insn_hooks );
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

            /* Zásahy GUI do zařízení (menu a okna zařízení,
             * device_request.h). CPU stojí mezi instrukcemi; funguje i bez
             * debuggeru. Bez zásahu 1 atomické čtení v per-event bloku. */
            device_request_poll();

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
             * PERF: check je UVNITŘ per-event bloku, NE v per-instruction hot
             * path -> zanedbatelný dopad. V default stavu (oba příznaky 0)
             * jedno bitové OR a jedna větev, branch predictor naučí "vždy
             * false". */
            if ( g_debugger.run_frames_active | g_debugger.run_deadline_active )
            {
                /* Frame-bounded run i deadline v emulovaném čase (run_control.h):
                 * jedna podmínka v default stavu, vyhodnocení mimo smyčku. */
                run_control_check_run_bounds ( );
            };
#endif

            /* Záznam a přehrání vstupů: bod B_f (replay.h). Až za run
             * bounds, aby pauza run(frames) / deadline vznikla na téže
             * hranici; před testem pauzy, aby pauza ze záznamu (konec
             * přehrávání, start) vstoupila do paused smyčky hned. V per-event
             * bloku, ne v instrukční smyčce: bez záznamu jedna větev. */
            if (s_mzarch_replay_due)
            {
                s_mzarch_replay_due = false;
                replay_on_frame_boundary();
            };

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
    /* Revize stavu stroje: náhodná epocha procesu (state_rev.h). */
    state_rev_init();

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
