/*
 * callstack.h - shadow stack zásobníku volání (Callstack subsystem V1).
 *
 * Subsystém callstack udržuje shadow stack volání nezávislý na CPU
 * stacku - push při CALL / RST / IRQ accept / NMI, pop při RET /
 * RETI / RETN. Slouží debugger UI okno "Callstack" pro vizualizaci
 * hloubky a hierarchie volání a v navazujícím Profiler mutantu V1.5
 * jako event source per-function timingu (listener API).
 *
 * Aktivace přes g_callstack_active = 0/1 (default 0, registrace
 * Z80 hooků odložená do callstack_set_active(true)). Vypnutý subsystém
 * = nulový hot-path overhead (CALL/RET callback slot v z80_t = NULL).
 *
 * Threading: shadow stack se updatuje výhradně z EMU vlákna (přes
 * Z80 hooky + mzarch fan-out). UI vlákno čte přes callstack_snapshot_get
 * pod dbgapi sync handlerem (= safe-point, atomický kopie). Listener
 * callbacky volané z EMU vlákna - konzument je zodpovědný za vlastní
 * synchronizaci pokud chce data předat do UI vlákna.
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

#ifndef CALLSTACK_H
#define CALLSTACK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Maximální hloubka shadow stacku.
 *
 * Pevně 256 podle briefingu V1 (rozhodnutí #2). Při překročení se
 * push tichá no-op, počítadlo overflow_count se inkrementuje, ale
 * emulátor není nijak impactnut. Pop chování zůstává - shadow se
 * postupně dotáhne k reálné hloubce, jakmile sekvence RET vrátí
 * counter pod limit.
 *
 * Paměťová stopa: 256 * sizeof(st_CALLSTACK_ENTRY) (= ~5 KB statické
 * paměti v BSS).
 */
#define CALLSTACK_MAX_DEPTH 256


/**
 * @brief Typ záznamu v shadow stacku.
 *
 * Vyjadřuje původ frame (= co ho na stack pushlo). UI ho zobrazí v
 * sloupci "Kind", IRQ záznamy ve V1 sdílí push cestu s NMI (= mzarch
 * post-IRQ-accept hook), CALL a RST mají oddělené Z80 callbacky.
 *
 * SYNTHETIC frame vzniká při divergenci shadow stacku - např. když
 * RET pop najde prázdný shadow (= PUSH+RET trampoline pattern).
 * V1 logika: emit SYNTHETIC s flagem @ref CS_FLAG_DIVERGENT a
 * inkrement statistiky divergence_count.
 */
typedef enum en_CALLSTACK_ENTRY_KIND {
    CS_KIND_CALL = 0,    /**< CALL nn / CALL cc,nn (= 3-byte instrukce) */
    CS_KIND_RST,         /**< RST 00h..38h (= 1-byte instrukce) */
    CS_KIND_IRQ_IM0,     /**< IRQ accept v IM 0 (= RST z bus, typ. RST 38) */
    CS_KIND_IRQ_IM1,     /**< IRQ accept v IM 1 (= RST 38h) */
    CS_KIND_IRQ_IM2,     /**< IRQ accept v IM 2 (= vector dispatch přes I:vec) */
    CS_KIND_NMI,         /**< NMI accept (= CALL na 0066h) */
    CS_KIND_SYNTHETIC    /**< Synthetic frame z divergence handling */
} en_CALLSTACK_ENTRY_KIND;


/** @name Flagy v st_CALLSTACK_ENTRY.flags
 *
 * Single-bit flagy popisující dodatečné vlastnosti frame.
 * @{ */

/**
 * @brief Frame vznikl z divergence (= RET bez odpovídajícího CALL).
 *
 * V1 znamená "byl emitnut SYNTHETIC frame" - shadow stack byl prázdný
 * při RET. Hint pro UI "tady byla PUSH+RET trampoline nebo manual
 * stack manipulace, shadow se synchronizoval". Stat divergence_count
 * se zároveň inkrementuje.
 */
#define CS_FLAG_DIVERGENT  (1u << 0)

/**
 * @brief Tail-call placeholder (V1 nepoužito, rezervováno pro V2+).
 *
 * Pozdější fáze může detekovat "JP cíl po nedávném CALL" jako
 * tail-call optimalizaci a místo push synthetic markeru zachovat
 * předchozí frame s tímto flagem. V1 nikdy nenastavuje.
 */
#define CS_FLAG_TAILCALL   (1u << 1)

/**
 * @brief Detekován SP swap (= LD SP,... nebo EX (SP),HL mimo PUSH/POP).
 *
 * Placeholder pro Fázi 1C (mzarch.c hook). V1B (= toto API) nikdy
 * nenastavuje, statistika sp_swap_count zůstává 0. Hint pro UI
 * "shadow stack od této hloubky níže může být inkonzistentní".
 */
#define CS_FLAG_SP_SWAP    (1u << 2)
/** @} */


/**
 * @brief Jeden záznam shadow stacku.
 *
 * Layout odpovídá shared API contractu mezi Callstack producer
 * a budoucím Profiler consumer (= shape se nemění bez aktualizace
 * contract dokumentu). Velikost 24 B (= rozumný cache footprint).
 *
 * @invariant kind je jedna z @ref en_CALLSTACK_ENTRY_KIND hodnot.
 * @invariant Pro kind == CS_KIND_IRQ_IM* je im_mode shodné s
 *            IM mode v okamžiku accept (0/1/2). Pro ostatní kindy
 *            je im_mode = 0 a im2_vector_addr = 0.
 * @invariant Pro kind == CS_KIND_NMI je target_addr = 0x0066,
 *            im_mode = 0, im2_vector_addr = 0.
 */
typedef struct st_CALLSTACK_ENTRY {
    uint16_t return_addr;       /**< Návratová adresa pushnutá na CPU stack (= sem se RET vrátí). */
    uint16_t call_site_addr;    /**< Adresa CALL/RST/IRQ-source instrukce. */
    uint16_t target_addr;       /**< Cílová adresa volání (= entry rutiny). */
    uint16_t sp_at_entry;       /**< Hodnota SP PO pushi return_addr (= cpu->sp - 2 v okamžiku push). */
    uint64_t cycles_at_entry;   /**< Snapshot cpu->total_cycles při push (pro inclusive cycles výpočet). */
    uint64_t pxclk_total;       /**< Snapshot tlog_common_get_pxclk_total() při push (= pixel clock counter pro raster korelaci). */
    uint32_t screens_total;     /**< Snapshot tlog_common_get_screens_total() při push (= frame number). */
    uint32_t pxclk_in_screen;   /**< Snapshot tlog_common_get_pxclk_in_screen() při push (= pixel pozice v rámci snímku, lze rozdělit přes VIDEO_GET_SCREEN_ROW/COL na scanline + col). */
    uint8_t  bank_id;           /**< Banking state hash při push (= banks snapshot CRC8). */
    uint8_t  kind;              /**< @ref en_CALLSTACK_ENTRY_KIND cast na uint8_t. */
    uint8_t  flags;             /**< Bitmaska @ref CS_FLAG_* flagů. */
    uint8_t  im_mode;           /**< IM mode (0/1/2) - relevantní pro CS_KIND_IRQ_IM*. */
    uint16_t im2_vector_addr;   /**< Vector adresa (I<<8)|vec - jen pro CS_KIND_IRQ_IM2. */
    uint8_t  _pad[6];           /**< Padding pro alignment + rezerva. */
} st_CALLSTACK_ENTRY;


/**
 * @brief Statistiky callstack subsystému pro UI status panel.
 *
 * Plně placené aktuálním stavem shadow stacku + persistent counters
 * (= drží se přes životnost subsystému, reset jen explicitně přes
 * @ref callstack_reset_stats nebo @ref callstack_reset).
 */
typedef struct st_CALLSTACK_STATS {
    int      current_depth;         /**< Aktuální hloubka shadow stacku (0..CALLSTACK_MAX_DEPTH). */
    int      max_depth_reached;     /**< Nejvyšší hloubka od posledního reset. */
    uint32_t divergence_count;      /**< Suma všech divergence events = trampoline + longjmp + mismatch. */
    uint32_t diverg_trampoline;     /**< RET/RETI nad prázdným shadow (= PUSH+RET trampolína, např. CP/M BDOS dispatch). */
    uint32_t diverg_longjmp;        /**< RET našel match hluboko v shadow → pop N entries (= longjmp pattern). */
    uint32_t diverg_mismatch;       /**< RET top mismatch + žádný deep match → konzervativní pop 1 (= self-modifying / manual stack manipulation). */
    uint32_t sp_swap_count;         /**< Počet detekovaných SP swap (V1 vždy 0). */
    uint32_t overflow_count;        /**< Počet push pokusů nad CALLSTACK_MAX_DEPTH. */
    uint32_t stack_discard_count;   /**< Heuristika: RET kde sp_before_pop > shadow[0].sp_at_entry (= SP vyletěl nad nejhlubší tracked frame, typicky CP/M warm boot / ROM reset / exception handler). Při detekci se shadow auto-clear. */
} st_CALLSTACK_STATS;


/**
 * @brief Hodnoty kind pro @ref st_CALLSTACK_DIVERG_EVENT.kind - třídění
 *        diverg eventu podle příčiny.
 */
typedef enum en_CALLSTACK_DIVERG_KIND {
    CS_DIVERG_TRAMPOLINE   = 0,  /**< Empty shadow + RET / SP-nested trampolína. */
    CS_DIVERG_LONGJMP      = 1,  /**< RET pop přes více framů (deep match). */
    CS_DIVERG_MISMATCH     = 2,  /**< Top mismatch + žádný deep match (= self-modifying RET addr nebo neznámý pattern). */
    CS_DIVERG_STACK_DISCARD = 3, /**< SP vystoupil nad nejhlubší tracked frame (= stack reset, např. CP/M warm boot, ROM reset, exception handler). Shadow auto-cleared. */
} en_CALLSTACK_DIVERG_KIND;


/**
 * @brief Záznam o nedávné divergenci pro post-mortem diagnostiku.
 *
 * Drží se v ring bufferu @ref CALLSTACK_DIVERG_RING_SIZE (= 32) entries.
 * Při overflow se přepisuje od začátku (= oldest první).
 *
 * @field ret_pc       Adresa RET/RETI/RETN opcode (= cpu->pc - 1 pro RET,
 *                     cpu->pc - 2 pro RETI/RETN - aproximace, RETI/RETN
 *                     mají pc už za POP). Pro RETI/RETN tedy ne vždy
 *                     spolehlivé - viz kind field.
 * @field pop_target   Adresa kam RET reálně skočí (= PEEK16 z aktuálního SP).
 *                     Pro RETI/RETN = 0 (= nemáme přístup k pop_target z hooku).
 * @field sp_before    SP před POP - pro RET hook. Pro RETI/RETN = 0.
 * @field top_return   top.return_addr v okamžiku diverg (= co shadow čekal).
 * @field top_sp       top.sp_at_entry v okamžiku diverg.
 * @field top_kind     top.kind v okamžiku diverg (= en_CALLSTACK_ENTRY_KIND).
 *                     Pro empty shadow = 0xFF.
 * @field shadow_depth Aktuální depth v okamžiku diverg (0 = empty).
 * @field kind         Klasifikace eventu (en_CALLSTACK_DIVERG_KIND).
 * @field cycles_at    cpu->total_cycles v okamžiku diverg.
 */
typedef struct st_CALLSTACK_DIVERG_EVENT {
    uint16_t ret_pc;
    uint16_t pop_target;
    uint16_t sp_before;
    uint16_t top_return;
    uint16_t top_sp;
    uint8_t  top_kind;
    uint8_t  shadow_depth;
    uint8_t  kind;
    uint8_t  _pad;
    uint64_t cycles_at;
} st_CALLSTACK_DIVERG_EVENT;

#define CALLSTACK_DIVERG_RING_SIZE 32


/**
 * @brief Snapshot ring bufferu nedávných divergence events.
 *
 * Vyplní @c out polem ordered chronologicky (oldest → newest). Vrací
 * počet valid entries (0..CALLSTACK_DIVERG_RING_SIZE).
 *
 * @param out_events  Pole o velikosti @ref CALLSTACK_DIVERG_RING_SIZE.
 * @return Počet valid entries (0..CALLSTACK_DIVERG_RING_SIZE).
 */
int callstack_get_recent_diverg ( st_CALLSTACK_DIVERG_EVENT *out_events );


/**
 * @brief Globální gate flag - non-zero = subsystém aktivní.
 *
 * Default 0 (= callbacky nezaregistrovány v z80_t, listener není
 * volán). Měněn výhradně přes @ref callstack_set_active: z hlavního
 * vlákna před startem emu vlákna (cfgmain propagate / CLI parsování),
 * jinak na emu vlákně (profiler, DBGAPI_CMD_CALLSTACK_SET_ACTIVE z panelu
 * Callstack). Hot path subsystém čte přes vlastní static state -
 * tento flag slouží pro UI / diagnostiku.
 *
 * @note Měnit hodnotu přímo bez @ref callstack_set_active je
 *       zakázáno - registrace Z80 hooků by nezůstala konzistentní.
 */
extern uint8_t g_callstack_active;


/* ============================================================================
 * Lifecycle (volá debugger_init / mzarch_main_init)
 * ============================================================================ */

/**
 * @brief Inicializace callstack subsystému (cfgmain registrace).
 *
 * Idempotentní. Zaregistruje sekci [CALLSTACK] s klíčem `active` (bool).
 * Načte hodnotu z INI, propaguje přes @ref callstack_set_active.
 *
 * Volá se z debugger_init() na hlavním vlákně PŘED startem emu vlákna.
 * Předpokládá že CPU instance g_mzarch_main.cpu už existuje (= mzarch
 * platform init proběhl).
 */
void callstack_init ( void );

/**
 * @brief Aplikace CLI options (--callstack).
 *
 * Pokud byl předán --callstack flag, vynutí g_callstack_active = 1
 * bez ohledu na INI hodnotu. Volá se po @ref callstack_init.
 */
void callstack_apply_cli_options ( void );

/**
 * @brief Přepnutí subsystému ON/OFF.
 *
 * Při ON: registruje Z80 CALL/RET hooky (z80_set_call/z80_set_ret),
 * vynuluje shadow + stats. Mzarch fan-out (cpu_ctrl_event, iff_change)
 * se aktivuje automaticky - příslušné mzarch_*_cb forwardují do
 * callstack interních funkcí jen pokud g_callstack_active.
 *
 * Při OFF: odregistruje Z80 CALL/RET hooky (NULL). Shadow stack ani
 * statistiky nenuluje (= viewable v UI po stop).
 *
 * Idempotentní (= opakované volání se stejnou hodnotou = no-op).
 *
 * @param enable true = aktivovat, false = deaktivovat.
 *
 * @note Musí být voláno z hlavního vlákna před spuštěním emu vlákna
 *       nebo z emu vlákna v safe-point (= dbgapi sync handler).
 *       Z80 callback slot manipulace není atomická. UI vlákno posílá
 *       DBGAPI_CMD_CALLSTACK_SET_ACTIVE (helper dbg_ui_callstack_set_active).
 */
void callstack_set_active ( bool enable );

/**
 * @brief Vynulování shadow stacku + statistik (volá emu reset path).
 *
 * Idempotentní. Volá se z mzarch_platform_fn_emu_reset hooku +
 * z @ref callstack_set_active při zapnutí.
 *
 * @pre Volat z emu vlákna (nebo z jednovláknového kontextu). Nulování
 *      g_depth souběžně s push/pop v CALL/RET hoocích by mohlo g_depth
 *      snížit pod nulu. UI vlákno posílá DBGAPI_CMD_CALLSTACK_RESET
 *      (helper dbg_ui_callstack_reset).
 */
void callstack_reset ( void );


/* ============================================================================
 * Snapshot API (pro UI / dbgapi handler)
 * ============================================================================ */

/**
 * @brief Atomický snapshot shadow stacku.
 *
 * Alokuje pole st_CALLSTACK_ENTRY o velikosti current_depth, naplní
 * obsahem shadow (= entries[0] = nejstarší frame, entries[count-1] =
 * top stacku). Caller musí pole uvolnit přes @ref callstack_snapshot_free.
 *
 * Při current_depth == 0 vrací out_entries = NULL, out_count = 0
 * a návratovou hodnotu 0 (= success, prázdný stack).
 *
 * Threading: musí být voláno z emu vlákna (= bez vnitřní synchronizace,
 * stejný pattern jako stack_history_copy_recent). Dbgapi sync handler
 * to zajistí.
 *
 * @param out_entries Cílový pointer na pole (alokovaný callee přes g_malloc).
 *                    Nesmí být NULL.
 * @param out_count   Cílový počet entries. Nesmí být NULL.
 * @return 0 OK, -1 error (= alokace selhala).
 *
 * @post Pokud return == 0 a *out_count > 0, caller vlastní paměť a musí
 *       ji uvolnit přes @ref callstack_snapshot_free.
 */
int callstack_snapshot_get ( st_CALLSTACK_ENTRY **out_entries, int *out_count );

/**
 * @brief Uvolnění snapshot bufferu vráceného @ref callstack_snapshot_get.
 *
 * Bezpečné pro NULL (= no-op). Po volání entries pointer není platný.
 *
 * @param entries Pole získané z @ref callstack_snapshot_get.
 */
void callstack_snapshot_free ( st_CALLSTACK_ENTRY *entries );


/* ============================================================================
 * Statistiky (pro UI status panel)
 * ============================================================================ */

/**
 * @brief Načtení snapshot statistik.
 *
 * @param out Cílová struktura. Nesmí být NULL.
 */
void callstack_get_stats ( st_CALLSTACK_STATS *out );

/**
 * @brief Vynulování statistik (UI button "Reset stats").
 *
 * Nezasahuje shadow stack - jen counters (divergence_count,
 * sp_swap_count, overflow_count, max_depth_reached).
 */
void callstack_reset_stats ( void );


/* ============================================================================
 * Listener API (shared-api-contract pro budoucí Profiler mutant)
 * ============================================================================ */

/**
 * @brief Callback při push frame (= CALL/RST/IRQ/NMI accept).
 *
 * Volaný z emu vlákna bezprostředně po vložení frame do shadow stacku.
 * Pointer entry zůstává platný JEN po dobu volání (= konzument si musí
 * data zkopírovat, ne držet pointer).
 *
 * @param entry     Právě pushnutá entry.
 * @param user      User data z @ref callstack_register_listener.
 */
typedef void (*callstack_on_enter_cb_t)(const st_CALLSTACK_ENTRY *entry, void *user);

/**
 * @brief Callback při pop frame (= RET/RETI/RETN/longjmp pop).
 *
 * Volaný z emu vlákna bezprostředně po odebrání frame ze shadow.
 * Pointer entry zůstává platný JEN po dobu volání (= zkopírovat).
 *
 * Pro divergence handling - pokud RET pop matchne entry hluboko v
 * stacku (longjmp pattern), fire on_exit jen pro TOP entry (= ne
 * pro každou popnutou). Pro prázdný shadow + RET fire on_exit se
 * synthetic entry (kind = CS_KIND_SYNTHETIC, flags = CS_FLAG_DIVERGENT).
 *
 * @param entry     Právě popnutá entry (snapshot před odstraněním).
 * @param user      User data z @ref callstack_register_listener.
 */
typedef void (*callstack_on_exit_cb_t)(const st_CALLSTACK_ENTRY *entry, void *user);

/**
 * @brief Registrace listener páru pro Profiler V1.5+ konzumenta.
 *
 * V1 podporuje single slot - opakovaná registrace přepíše předchozí.
 * Pokud bude potřeba multi-listener, vrstva nad call_cb si fan-out
 * řeší sama.
 *
 * @param on_enter Callback pro push frame (NULL = nezavěšen).
 * @param on_exit  Callback pro pop frame (NULL = nezavěšen).
 * @param user     User data předaná zpět do callbacků.
 */
void callstack_register_listener ( callstack_on_enter_cb_t on_enter,
                                    callstack_on_exit_cb_t on_exit,
                                    void *user );

/**
 * @brief Odregistrace listener (V1 = single slot, ignoruje user).
 *
 * V1 zruší aktuálně registrovaný listener bez porovnání user pointeru.
 * Parametr user je pro forward-compat s multi-listener verzí.
 *
 * @param user User pointer (V1 nepoužito).
 */
void callstack_unregister_listener ( void *user );


/* ============================================================================
 * Internal API (volá mzarch fan-out - není určeno pro UI / 3rd party)
 * ============================================================================ */

/**
 * @brief Fan-out hook z mzarch_cpu_ctrl_event_cb pro RST opcody.
 *
 * Volá se z mzarch_cpu_ctrl_event_cb po vlastním eventlog dispatchi,
 * jen pokud g_callstack_active != 0 a event je v rozsahu RST_00..RST_38.
 * Pushne CS_KIND_RST entry s target_addr = N*8 (= n z RST n*8), call_site
 * = pc - 1 (= RST je 1-byte opcode), return_addr = pc.
 *
 * No-op pro HALT_ENTER / HALT_EXIT events (callstack je neřeší).
 *
 * @param event z80_cpu_ctrl_event_t cast na uint8_t.
 * @param pc    PC v okamžiku eventu (= za RST opcode, dle z80.h doc).
 */
void callstack_on_cpu_ctrl_event ( uint8_t event, uint16_t pc );

/**
 * @brief Fan-out hook z mzarch_iff_change_cb pro RETI/RETN unwind.
 *
 * Volá se z mzarch_iff_change_cb po vlastním BP dispatchi, jen pokud
 * g_callstack_active != 0. Pro reason RETI/RETN provádí pop top entry
 * (= unwind IRQ/NMI frame). Pro ostatní reasony no-op (callstack
 * nezajímají EI/DI/INT_ACK/NMI_ACK/RESET cesty zde - INT_ACK push
 * řeší mzarch IRQ accept hook ve Fázi 1C).
 *
 * @param reason z80_iff_change_reason_t cast na uint8_t.
 */
void callstack_on_iff_change ( uint8_t reason );

/**
 * @brief Hook pro IRQ accept (volá se z mzarch.c Fáze 1C).
 *
 * Pushne entry s kind = CS_KIND_IRQ_IM{0,1,2} podle im_mode. Volání
 * z mzarch.c bezprostředně po z80_process_interrupt() pokud IRQ
 * byl skutečně přijat (= cpu->iff1 == 0 po volání).
 *
 * Vstupní g_callstack_active gate kontroluje funkce sama (= caller
 * nemusí testovat).
 *
 * @param pc_before_irq    PC instrukce přerušené IRQ accept (= co se pushne).
 * @param isr_addr         ISR vstupní adresa (target_addr).
 * @param im_mode          IM mode v okamžiku accept (0/1/2).
 * @param im2_vector_addr  Vector adresa (I<<8)|vec - jen pro IM 2 (jinak 0).
 */
void callstack_on_irq_accept ( uint16_t pc_before_irq, uint16_t isr_addr,
                                uint8_t im_mode, uint16_t im2_vector_addr );

/**
 * @brief Hook pro NMI accept (volá se z mzarch.c Fáze 1C).
 *
 * Pushne entry s kind = CS_KIND_NMI, target_addr = 0x0066.
 * Vstupní g_callstack_active gate kontroluje funkce sama.
 *
 * @param pc_before_nmi    PC instrukce přerušené NMI (= co se pushne).
 */
void callstack_on_nmi_accept ( uint16_t pc_before_nmi );


#ifdef __cplusplus
}
#endif

#endif /* CALLSTACK_H */
