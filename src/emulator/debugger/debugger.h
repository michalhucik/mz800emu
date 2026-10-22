/*
 * File:   debugger.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 23. srpna 2015, 16:18
 *
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef DEBUGGER_H
#define DEBUGGER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "libs/cpu-z80/z80.h"

    /*
     * Stav debuggeru
     */
    typedef enum en_DEBUGGER_STATE
    {
        DEBUGGER_STATE_NONE = 0,                /* žádná aktivita debuggeru, nebudeme zachytávat historii instrukcí */
        DEBUGGER_STATE_WINDOW_SHOWN = (1 << 0), /* hlavní okno debuggeru je zobrazeno */
        DEBUGGER_STATE_ACTIVE_BPT = (1 << 1),   /* aktivní breakpoint */
    } en_DEBUGGER_STATE;

    /**
     * @brief Režim aktivace CPU Instruction History (cpuhist).
     *
     * @c WITH_WINDOW (default) = cpuhist běží jen pokud je otevřené debug okno.
     * @c ALWAYS = cpuhist běží trvale, nezávisle na stavu debug okna.
     * @c OFF = cpuhist se nikdy nezaznamenává (ani při otevřeném okně).
     *
     * @note Toto je informativní instrukční historie zobrazovaná v Debugger
     *       okně (32-instr ring). Pro plnohodnotné CPU tracking logy
     *       (s timestampy, RAM dumpem atd.) viz subsystém @c cputrack
     *       v @c trace/cputrack.h.
     */
    typedef enum en_DEBUGGER_CPUHIST_MODE
    {
        DEBUGGER_CPUHIST_MODE_WITH_WINDOW = 0,   /**< jen při otevřeném debug okně */
        DEBUGGER_CPUHIST_MODE_ALWAYS,            /**< trvale (i když okno není otevřené) */
        DEBUGGER_CPUHIST_MODE_OFF,               /**< vůbec nezaznamenávat */
        DEBUGGER_CPUHIST_MODE_WITH_WINDOW_OR_BP, /**< default - při otevřeném okně NEBO když je aspoň jeden BP enabled */
    } en_DEBUGGER_CPUHIST_MODE;

    /**
     * @brief Režim aktivace Memory Heatmap.
     *
     * @c OFF (default) = MH se vůbec nezaznamenává, žádná režie.
     * @c WITH_WINDOW = MH běží jen pokud je otevřené debug okno.
     * @c ALWAYS = MH běží trvale, nezávisle na stavu debug okna.
     *
     * MH defaultně OFF (na rozdíl od Trace Log) - zaznamenávání je drahé
     * a uživatel ho explicitně zapíná přes UI nebo API.
     */
    typedef enum en_DEBUGGER_MHMAP_MODE
    {
        DEBUGGER_MHMAP_MODE_OFF = 0,              /**< default - žádné MH recording */
        DEBUGGER_MHMAP_MODE_WITH_WINDOW,          /**< jen při otevřeném debug okně */
        DEBUGGER_MHMAP_MODE_ALWAYS,               /**< trvale */
    } en_DEBUGGER_MHMAP_MODE;

    typedef struct st_DEBUGGER
    {
        en_DEBUGGER_STATE state;
        unsigned active;                          /**< Debug window otevřené */
        en_DEBUGGER_CPUHIST_MODE cpuhist_mode;    /**< Režim aktivace CPU Instruction History (default WITH_WINDOW_OR_BP) */
        en_DEBUGGER_MHMAP_MODE mhmap_mode;        /**< Režim aktivace Memory Heatmap / CDL (default OFF) */
        unsigned cdl_export_on_exit;              /**< Exportovat CDL data při ukončení emulátoru (default 0) */
        char *cdl_export_dir;                     /**< Cesta k cílovému adresáři CDL exportu (default "./cdl-export/") */
        char *cdl_export_name;                    /**< Basename pro meta.json bez extenze (default "cdl-export") */
        unsigned step_call;
        /* Frame-bounded run (mcp-debug-control request 0021) - deterministický
         * stop emulace přesně na N-té frame hranici. Nastavuje se v handleru
         * DBGAPI_CMD_RUN_FRAMES, vyhodnocuje v mzarch_main hot loopu (per-frame
         * blok). Cílem je nahradit dřívější nedeterministický async PAUSE z
         * dispatch vlákna (= emu zastavoval na wall-clock-závislém cycle bodě). */
        unsigned run_frames_active;               /**< 1 = probíhá frame-bounded run; emu se sám pausne, jakmile g_gdg.total_elapsed.screens dosáhne run_frames_target. 0 = neaktivní. */
        uint32_t run_frames_target;               /**< Cílová hodnota g_gdg.total_elapsed.screens, při jejímž dosažení (>=) se emu deterministicky pausne. Platné jen pokud run_frames_active != 0. */
        /* Deadline běhu v emulovaném čase (run_control.h). Kontroluje se ve
         * stejném per-event bloku jako frame-bounded run, jednou podmínkou
         * (run_frames_active | run_deadline_active). Platí do nejbližší pauzy. */
        unsigned run_deadline_active;             /**< 1 = běh má deadline; emu se pausne, jakmile run_control_mono_ticks() dosáhne run_deadline_ticks. 0 = neaktivní. */
        uint64_t run_deadline_ticks;              /**< Deadline v monotónních taktech GDG (run_control_mono_ticks()). Platné jen pokud run_deadline_active != 0. */
        unsigned memop_call;                      /**< 1 = právě probíhá debugger-iniciovaný memory write (debugger_memory_write_byte). Sledováno vramctrl handlery pro detekci VRAM touch. */
        unsigned memop_vram_touched;              /**< Per-call signal z vramctrl handlerů: 1 = během aktuálního debugger_memory_write_byte() byl proveden zápis do VRAM/CGRAM (banking-aware). Čte se na konci debugger_memory_write_byte pro screen refresh on edit. Nastavuje se jen pokud memop_call != 0. */
        unsigned run_to_temporary_breakpoint;
        int skip_bp_at_pc;                        /**< Adresa kde má BP enforcement přeskočit kontrolu (= "continue past BP" pattern). -1 = neaktivní. Nastavuje se při unpause na aktuální PC (jinak by uživatel uvízl v infinite pause loopu při BP hit). Vázáno na PC, ne jen "once" - pokud reset změní PC mezi pause a unpause, skip se neuplatní na nové PC a BP na 0000 se správně aktivuje. */
        unsigned auto_save_breakpoints;
        unsigned screen_refresh_on_edit;
        unsigned screen_refresh_at_step;
        unsigned disasm_show_branch_arrows;       /**< Zobrazovat vizualizaci skoků (šipky v ICONS gutteru) v dolní disasm tabulce (default 1). UI-only, persistovaný v cfgmain. */
        unsigned disasm_show_coverage;            /**< Zobrazovat značky pokrytí kódu (ICONS gutter, tooltip) v oknech Disassembly #1-#5, když data pokrytí existují (default 1). UI-only, persistovaný v cfgmain. */
        uint32_t user_cycle_origin;               /**< V3.1: počátek uživatelského cycle counteru pro CPU window (display = cpu->total_cycles - user_cycle_origin). Default 0 = od resetu. Nastavováno přes DBGAPI_CMD_SET_USER_CYCLE_ORIGIN. */
        /* DBG Workplace - která auxiliary debug okna se mají automaticky
         * otevírat a zavírat společně s hlavním oknem debuggeru. User
         * preferenece v menu Debugger -> DBG Workplace, persistované
         * v cfgmain. Při show/hide hlavního okna se aplikuje na
         * g_gui->show* flagy odpovídajících oken. */
        unsigned wp_cpu_registers;                /**< Workplace: CPU Registers (default 1). */
        unsigned wp_memory_map;                   /**< Workplace: Memory Map (default 1). */
        unsigned wp_stack_monitor;                /**< Workplace: Stack Monitor (default 0). */
        unsigned wp_callstack;                    /**< Workplace: Callstack (default 0). */
        unsigned wp_breakpoints;                  /**< Workplace: Breakpoints (default 0). */
        unsigned wp_watch;                        /**< Workplace: Watch panel (default 0). */
        unsigned wp_membrowser;                   /**< Workplace: Memory Browser okno (default 0, membrowser mutant V0). */
        /**
         * V3 multi-view: workplace slots pro 4 sekundární Memory Browser
         * okna (#2 - #5). Default 0 (opt-in). Index 0 = #2, 1 = #3,
         * 2 = #4, 3 = #5. Pri otevreni/zavreni hlavniho debug okna se
         * persistovany flag aplikuje na g_gui->showMemoryBrowserWindowExtra[i].
         */
        unsigned wp_membrowser_extra[4];
        unsigned wp_profiler;                     /**< Workplace: CPU Profiler okno (default 0). */
        unsigned wp_bookmarks;                    /**< Workplace: Bookmarks okno (default 0). */
        unsigned wp_symbols;                      /**< Workplace: Symbols okno (default 0). */
        unsigned wp_variables;                    /**< Workplace: Variables okno (default 0). */
        unsigned wp_disasm_extra[4];              /**< Workplace: Disassembly #2..#5 (default 0). */
        /* Per-chip-panels F1 scaffold: workplace slots pro CTC/PPI/Z80
         * PIO/PSG state okna. Default 0 (opt-in). */
        unsigned wp_show_ctc;                     /**< Workplace: CTC State okno (default 0). */
        unsigned wp_show_ppi;                     /**< Workplace: PPI State okno (default 0). */
        unsigned wp_show_pioz80;                  /**< Workplace: Z80 PIO State okno (default 0). */
        unsigned wp_show_psg;                     /**< Workplace: PSG State okno (default 0). */
        unsigned wp_show_gdg;                     /**< Workplace: GDG State okno (default 0, gdg-panel F1). */
        /* psg-audio-scope mutant F1: workplace slot pro PSG Audio Scope
         * (samostatné okno, default 0). */
        unsigned wp_show_psg_audio_scope;         /**< Workplace: PSG Audio Scope okno (default 0). */
    } st_DEBUGGER;

    extern st_DEBUGGER g_debugger;

#define TEST_DEBUGGER_MEMOP_CALL (g_debugger.memop_call != 0)
#define TEST_DEBUGGER_STEP_CALL (g_debugger.step_call != 0)
#define TEST_DEBUGGER_ACTIVE (g_debugger.active != 0)
/** Test, zda právě probíhá frame-bounded run (= emu se sám pausne na cílové frame hranici). */
#define TEST_DEBUGGER_RUN_FRAMES_ACTIVE (g_debugger.run_frames_active != 0)

    /**
     * @brief Hook pro vramctrl write byte funkce - signalizace debuggeru,
     *        že proběhl zápis do VRAM/CGRAM v aktuálním banking kontextu.
     *
     * Volá se z `vramctrl_mz800_memop_write_byte` (MZ-800 mode VRAM)
     * a z makra `vramctrl_mz700_memop_write_byte_internal` (MZ-700/MZ-1500
     * VRAM/CGRAM/PCG) na vstupu - tj. když je jasné, že banking-aware
     * dispatch zaroutoval zápis do nějaké formy VRAM.
     *
     * Hook je aktivní jen pokud `g_debugger.memop_call != 0` (= zápis
     * byl iniciován z UI debuggeru přes `debugger_memory_write_byte`).
     * CPU-driven VRAM writes ho ignorují - branch predictor uvidí
     * vždy 0 a větvení optimalizuje na noop.
     *
     * Bez `MZ800EMU_CFG_DEBUGGER_ENABLED` se expanduje na prázdný stub.
     */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#define VRAMCTRL_DBG_NOTIFY_VRAM_TOUCH()                                  \
    do {                                                                  \
        if ( g_debugger.memop_call ) g_debugger.memop_vram_touched = 1;   \
    } while ( 0 )
#else
#define VRAMCTRL_DBG_NOTIFY_VRAM_TOUCH() do { } while ( 0 )
#endif

    /**
     * @brief Test, zda CPU Instruction History (cpuhist) aktuálně běží.
     *
     *  - WITH_WINDOW: aktivní jen pokud @c TEST_DEBUGGER_ACTIVE
     *  - ALWAYS: aktivní vždy
     *  - OFF: nikdy aktivní
     */
    /* Režimy:
     *  - ALWAYS: vždy.
     *  - WITH_WINDOW: jen když je okno Disassembled otevřené.
     *  - WITH_WINDOW_OR_BP: okno otevřené NEBO je aspoň jeden BP enabled
     *    (g_bptmap.has_enabled_bp) - historie je k dispozici v okamžiku, kdy
     *    BP zastaví, bez nutnosti mít okno otevřené.
     *  - OFF: nikdy (respektováno - žádný BP to nepřebíjí). */
#define TEST_DEBUGGER_CPUHIST_ACTIVE \
    ((g_debugger.cpuhist_mode == DEBUGGER_CPUHIST_MODE_ALWAYS) || \
     ((g_debugger.cpuhist_mode == DEBUGGER_CPUHIST_MODE_WITH_WINDOW) && TEST_DEBUGGER_ACTIVE) || \
     ((g_debugger.cpuhist_mode == DEBUGGER_CPUHIST_MODE_WITH_WINDOW_OR_BP) && \
      (TEST_DEBUGGER_ACTIVE || g_bptmap.has_enabled_bp)))

    /**
     * @brief Test, zda Memory Heatmap aktuálně běží.
     *
     *  - OFF: nikdy aktivní (žádná režie)
     *  - WITH_WINDOW: aktivní jen pokud @c TEST_DEBUGGER_ACTIVE
     *  - ALWAYS: aktivní vždy
     */
#define TEST_DEBUGGER_MHMAP_ACTIVE \
    ((g_debugger.mhmap_mode == DEBUGGER_MHMAP_MODE_ALWAYS) || \
     ((g_debugger.mhmap_mode == DEBUGGER_MHMAP_MODE_WITH_WINDOW) && TEST_DEBUGGER_ACTIVE))

    /**
     * @brief Test, zda by mělo CPU jít přes pomalé debug callbacky.
     *
     * Pomalou cestu (memory_*_with_logging_cb, port_*_with_logging_cb) používáme,
     * pokud je aktivní CPU Instruction History NEBO Memory Heatmap NEBO
     * libovolný subsystém trace-suite (iorqlog/intlog/hwlog) NEBO
     * I/O Ports panel tracking (= activity counters + history ring).
     * Vně callbacku pak rozlišení podle individuálních flagů.
     *
     * @note cputrack hookuje až po z80_step v mzarch_main_emulator_run smyčce,
     *       logovací čtecí callback ale potřebuje kvůli bajtům instrukce
     *       načteným při fetch (g_debugger_insn_fetch): po z80_step už paměť
     *       může obsahovat jiné bajty (samomodifikace, přepnutí bankingu).
     *       Totéž platí pro pokrytí kódu v úrovni detail (g_coverage_need_fetch).
     * @note g_io_window_tracking_active je gating flag pro io_activity +
     *       io_history hooky v port_*_with_logging_cb (V1.5 Sprint 1+2).
     *       Bez něj v tomto makru by se default callbacks nepřepnuly na
     *       logging variantu a hooky by nikdy neběžely.
     */
    /* Feature/Fix C: callback-dispatchované BP typy (MEM_R/W, IORQ_R/W) fajrují
     * jen z memory/port logging callbacků. Existence takového BP proto musí
     * logging cestu vynutit i bez otevřeného okna / recordingu - jinak BP tiše
     * nestřílí. per_type_active flagy jsou v g_bptmap (bptmap.h). */
#define TEST_DEBUGGER_NEED_DEBUG_CALLBACKS (TEST_DEBUGGER_CPUHIST_ACTIVE || TEST_DEBUGGER_MHMAP_ACTIVE \
    || (g_iorqlog_active != 0) \
    || (g_cputrack_active != 0) \
    || (g_io_window_tracking_active != 0) \
    || (g_eventlog_active != 0) \
    || (g_coverage_need_fetch != 0) \
    || (g_bptmap.per_type_active[ BPTMAP_IDX_MEM_W ] != 0) \
    || (g_bptmap.per_type_active[ BPTMAP_IDX_MEM_R ] != 0) \
    || (g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_R ] != 0) \
    || (g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_W ] != 0))

extern int g_iorqlog_active; /* fwd decl pro TEST_DEBUGGER_NEED_DEBUG_CALLBACKS */
extern int g_cputrack_active; /* fwd decl pro TEST_DEBUGGER_NEED_DEBUG_CALLBACKS (bajty fetch pro cputrack) */
extern uint8_t g_io_window_tracking_active; /* fwd decl pro TEST_DEBUGGER_NEED_DEBUG_CALLBACKS */
extern int g_eventlog_active; /* fwd decl pro TEST_DEBUGGER_NEED_DEBUG_CALLBACKS (event-viewer V1) */
extern int g_coverage_need_fetch; /* fwd decl pro TEST_DEBUGGER_NEED_DEBUG_CALLBACKS (pokrytí kódu detail: bajty fetch) */

#define DEBUGGER_HISTORY_POSMASK 0x1f
#define DEBUGGER_HISTORY_LENGTH (DEBUGGER_HISTORY_POSMASK + 1)
#define DEBUGGER_MAX_INSTR_BYTES 4

/** @brief Příznak IFF1 v st_DEBUGGER_HISTORY_REGS.flags. */
#define DEBUGGER_HISTORY_F_IFF1     0x01u
/** @brief Příznak IFF2. */
#define DEBUGGER_HISTORY_F_IFF2     0x02u
/** @brief Posun režimu přerušení IM (0-2) v bajtu příznaků. */
#define DEBUGGER_HISTORY_F_IM_SHIFT 2u
/** @brief Maska režimu přerušení IM v bajtu příznaků. */
#define DEBUGGER_HISTORY_F_IM_MASK  0x0Cu

    /**
     * @brief Registry CPU před instrukcí řádku historie (20 B).
     *
     * Zachytí je logovací čtecí callback při M1 prvního bajtu instrukce:
     * v té chvíli instrukce ještě nic nezměnila a R se za tento M1 ještě
     * nezvýšil (jádro v z80_step() přičte R až po čtení). PC řádku je
     * st_DEBUGGER_HISTORY_ROW.addr.
     *
     * @invariant Platí jen pro řádky zapsané s cpuhist; předpokládá krokové
     *            jádro z80_step(), kde registry jsou přímo pole z80_t
     *            (dávková varianta je drží v lokálních kopiích).
     */
    typedef struct st_DEBUGGER_HISTORY_REGS
    {
        uint16_t af;   /**< AF */
        uint16_t bc;   /**< BC */
        uint16_t de;   /**< DE */
        uint16_t hl;   /**< HL */
        uint16_t ix;   /**< IX */
        uint16_t iy;   /**< IY */
        uint16_t sp;   /**< SP */
        uint16_t wz;   /**< WZ (MEMPTR) */
        uint8_t  i;    /**< I */
        uint8_t  r;    /**< R (celý bajt) */
        uint8_t  flags;/**< IFF1, IFF2, IM (DEBUGGER_HISTORY_F_*) */
        uint8_t  pad;  /**< Výplň, vždy 0. */
    } st_DEBUGGER_HISTORY_REGS;

    /**
     * @brief Řádek historie instrukcí (cpuhist).
     *
     * @invariant byte[0] je opkód (prefix) z M1 na @c addr; další bajty
     *            platí jen do délky instrukce (zbytek může být z dřívější
     *            instrukce téhož slotu).
     */
    typedef struct st_DEBUGGER_HISTORY_ROW
    {
        uint16_t addr;                           /**< Adresa instrukce (PC před ní). */
        uint8_t byte[DEBUGGER_MAX_INSTR_BYTES];  /**< Bajty instrukce z fetch. */
        st_DEBUGGER_HISTORY_REGS regs;           /**< Registry před instrukcí. */
    } st_DEBUGGER_HISTORY_ROW;

    typedef struct st_DEBUGGER_HISTORY
    {
        unsigned position;
        unsigned byte_position;
        st_DEBUGGER_HISTORY_ROW row[DEBUGGER_HISTORY_LENGTH];
    } st_DEBUGGER_HISTORY;

    extern st_DEBUGGER_HISTORY g_debugger_history;

    /**
     * @brief Bajty instrukce, které CPU skutečně načetlo při fetch (poslední instrukce).
     *
     * Plní logovací čtecí callback (memory_read_with_logging_cb, všechny
     * platformy) nezávisle na cpuhist: při M1 prvního bajtu instrukce
     * (addr == instruction_addr) nastaví @c addr a @c byte[0], další bajty
     * téže instrukce (prefix M1, operandy) ukládá podle byte_position.
     * Cputrack z nich bere bajty právě vykonané instrukce, protože po
     * z80_step už paměť může obsahovat jiné bajty (samomodifikace,
     * přepnutí bankingu instrukcí samotnou).
     *
     * @invariant count <= DEBUGGER_MAX_INSTR_BYTES; byte[0..count-1] platí
     *            pro instrukci začínající na @c addr.
     * @note Platí jen při aktivním logovacím callbacku
     *       (TEST_DEBUGGER_NEED_DEBUG_CALLBACKS); emu vlákno.
     */
    typedef struct st_DEBUGGER_INSN_FETCH
    {
        uint16_t addr;                            /**< Adresa prvního bajtu instrukce. */
        uint8_t count;                            /**< Počet načtených bajtů (0 = nic). */
        uint8_t byte[DEBUGGER_MAX_INSTR_BYTES];   /**< Načtené bajty v pořadí fetch. */
    } st_DEBUGGER_INSN_FETCH;

    extern st_DEBUGGER_INSN_FETCH g_debugger_insn_fetch;

    /**
     * @brief Zapíše bajt fetch instrukce do g_debugger_insn_fetch (logovací callback).
     *
     * @param is_m1_start true = první bajt instrukce (M1 na instruction_addr).
     * @param slot        Index bajtu v instrukci (byte_position - 1); mimo
     *                    rozsah se ignoruje.
     * @param addr        Adresa čteného bajtu.
     * @param value       Načtený bajt.
     */
    static inline void debugger_insn_fetch_record(int is_m1_start, unsigned slot, uint16_t addr, uint8_t value)
    {
        if (is_m1_start)
        {
            g_debugger_insn_fetch.addr = addr;
            g_debugger_insn_fetch.byte[0] = value;
            g_debugger_insn_fetch.count = 1;
        }
        else if (slot < DEBUGGER_MAX_INSTR_BYTES)
        {
            g_debugger_insn_fetch.byte[slot] = value;
            g_debugger_insn_fetch.count = (uint8_t) (slot + 1);
        };
    }

#define debugger_history_position(i) ((i) & DEBUGGER_HISTORY_POSMASK)

    /**
     * @brief Zachytit registry před instrukcí do řádku historie.
     *
     * Volá logovací čtecí callback (všechny platformy) při M1 prvního bajtu
     * instrukce s aktivním cpuhist, tedy ve studené diagnostické cestě.
     *
     * @param o    Cíl (řádek historie).
     * @param cpu  Jádro v okamžiku M1 (krokové z80_step()).
     */
    static inline void debugger_history_capture_regs(st_DEBUGGER_HISTORY_REGS *o, const z80_t *cpu)
    {
        o->af = cpu->af.w;
        o->bc = cpu->bc.w;
        o->de = cpu->de.w;
        o->hl = cpu->hl.w;
        o->ix = cpu->ix.w;
        o->iy = cpu->iy.w;
        o->sp = cpu->sp;
        o->wz = cpu->wz.w;
        o->i = cpu->i;
        o->r = cpu->r;
        o->flags = (uint8_t) ((cpu->iff1 ? DEBUGGER_HISTORY_F_IFF1 : 0u)
                              | (cpu->iff2 ? DEBUGGER_HISTORY_F_IFF2 : 0u)
                              | (((unsigned) cpu->im & 3u) << DEBUGGER_HISTORY_F_IM_SHIFT));
        o->pad = 0;
    }

    extern void debugger_step_call(unsigned value);
    extern void debugger_reset_history(void);
    extern void debugger_init(void);
    extern void debugger_exit(void);

    /**
     * @brief Zjistí, zda je stav debuggeru inicializovaný pro UI vlákno.
     *
     * debugger_init() běží v emu vlákně až po inicializaci video rozhraní,
     * UI vlákno už v tu chvíli vykresluje. Okna debuggeru (Breakpoints,
     * Watch, ...) proto smí číst stav jádra (g_breakpoints, ...) až když
     * funkce vrátí nenulu.
     *
     * @return Nenula po dokončení debugger_init(), 0 před ním a od začátku
     *         debugger_exit().
     * @note Lze volat z libovolného vlákna (GLib atomic). Nechrání souběh
     *       úprav stavu za běhu - ty řeší fronta dbgapi.
     */
    extern int debugger_is_initialized(void);
    /*
     * Show/hide hlavního okna debuggeru - DVĚ sady podle vlákna:
     *
     *  - přímé varianty (bez _request): swap CPU callbacků provádí
     *    synchronně na aktuálním vlákně. Volat POUZE z EMU vlákna
     *    (BP hit v bptmap/breakpoints, dbgapi handlery).
     *
     *  - *_request varianty: pro UI vlákno (menu, Alt+D, vkbd,
     *    Focus-to v Disassembled). Nastaví g_debugger.active a swap
     *    callbacků delegují na EMU vlákno přes
     *    DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE - přímé volání z UI by
     *    souběžně s běžící instrukční smyčkou bylo data race.
     *    Z EMU vlákna *_request NEvolat (sync submit na vlastní frontu
     *    = deadlock do timeoutu).
     */
    extern void debugger_show_main_window(void);
    extern void debugger_hide_main_window(void);
    extern void debugger_show_hide_main_window(void);
    extern void debugger_show_main_window_request(void);
    extern void debugger_hide_main_window_request(void);
    extern void debugger_show_hide_main_window_request(void);
    extern void debugger_update_all(void);
    extern void debugger_animation(void);
    /**
     * @brief Čtení bajtu z aktuálně namapované paměti CPU pro debugger.
     *
     * Používá ho disassembler (GUI i MCP), MCP `mem_read`, zásobník,
     * vektory přerušení, dekódování instrukce u step over a cputrack
     * (makro debugger_memory_read_byte()). Čte přes
     * memory_read_byte_no_se(): respektuje banking, nespouští MEM_R
     * breakpointy (na rozdíl od memory_read_byte()).
     *
     * @param addr      Adresa v prostoru CPU (0x0000-0xFFFF).
     * @param user_data Nepoužito (signatura callbacku z80_dasm).
     * @return Bajt z aktuálně namapované paměti.
     *
     * Side effects: po dobu čtení nastaví g_debugger.memop_call = 1.
     */
    extern uint8_t debugger_dasm_read_cb(uint16_t addr, void *user_data);
    extern uint8_t debugger_dasm_pure_ram_read_cb(uint16_t addr, void *user_data);
    extern uint8_t debugger_dasm_history_read_cb(uint16_t addr, void *user_data);
    /**
     * @brief Zápis bajtu do aktuálně mapované paměti CPU debuggerem.
     *
     * MCP `mem_write`, úpravy paměti v GUI. Zapisuje přes
     * memory_write_byte_no_bp(): respektuje banking a vedlejší efekty zápisu
     * (VRAM, mapované porty), nespouští MEM_W breakpointy.
     *
     * @param addr  Adresa v prostoru CPU (0x0000-0xFFFF).
     * @param value Zapisovaný bajt.
     *
     * Side effects: po dobu zápisu g_debugger.memop_call = 1; při zápisu do
     * VRAM/CGRAM obnoví obraz (debugger_screen_refresh_if_enabled).
     * @pre Emu vlákno (handlery dbgapi).
     */
    extern void debugger_memory_write_byte(uint16_t addr, uint8_t value);
    extern void debugger_change_z80_flagbit(unsigned flagbit, unsigned value);
    extern void debugger_change_z80_register(z80_reg_t reg, uint16_t value);
    extern void debugger_change_gdg_reg_border(uint8_t value);
    extern void debugger_change_gdg_reg_palgrp(uint8_t value);
    extern void debugger_change_gdg_reg_pal(uint8_t pal, uint8_t value);
    extern void debugger_change_gdg_wfr(uint8_t value);
    extern void debugger_change_gdg_rfr(uint8_t value);
    extern uint32_t debuger_hextext_to_uint32(const char *txt);
    /**
     * @brief Vynucený plný refresh obrazovky (tenký wrapper nad
     *        mzarch_forced_full_screen_refresh()).
     *
     * Přegeneruje framebuffer z VRAM a dokončí snímek (výměna bufferů
     * g_framebuffer), takže se obraz překreslí i v pauze.
     *
     * @pre Volat z emu vlákna (krok debuggeru se screen_refresh_at_step,
     *      dbgapi handlery) nebo před startem emu vlákna. Framebuffer plní
     *      emu vlákno; UI (Ctrl+R, menu) posílá DBGAPI_CMD_SCREEN_REFRESH
     *      přes helper dbg_ui_screen_refresh().
     */
    extern void debugger_forced_screen_update(void);

    /**
     * @brief Helper: vyvolá forced screen update pokud je zapnutý
     *        Settings -> Screen -> "Auto refresh on edit".
     *
     * Volá se z míst, kde uživatel přes UI debuggeru může způsobit změnu
     * vizuální podoby obrazovky bez běhu emulace - tj.:
     *   - debugger_memory_write_byte() po zápisu do VRAM/CGRAM (gated
     *     flagem g_debugger.memop_vram_touched),
     *   - DMD mode a banking změna z okna Memory Map (handler
     *     DBGAPI_CMD_MEMMAP_SET na emu vlákně),
     *   - (budoucí) GDG chip inspect editace.
     *
     * Bez tohoto helperu by změna byla viditelná až po dalším frame nebo
     * step CPU.
     *
     * @pre Volat z emu vlákna (všichni současní volající jsou handlery
     *      dbgapi a debugger_memory_write_byte() volaná z nich). Dřívější
     *      poznámka "bezpečné volat z UI vlákna v pauze" neplatila: ani
     *      v pauze se framebuffer nesmí plnit souběžně s příkazy fronty,
     *      které refresh volají také. UI posílá DBGAPI_CMD_SCREEN_REFRESH.
     */
    extern void debugger_screen_refresh_if_enabled(void);

#define debugger_memory_read_byte(addr) debugger_dasm_read_cb(addr, NULL)
#define debugger_pure_ram_read_byte(addr) debugger_dasm_pure_ram_read_cb(addr, NULL)

#ifdef __cplusplus
}
#endif

#endif /* DEBUGGER_H */
