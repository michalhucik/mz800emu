#ifndef MZARCH_H
#define MZARCH_H

#include "main.h"

#include <stdint.h>
#include <stdbool.h>
#include "app/app_thread.h"

#include "mzarch_config.h"
#include "libs/cpu-z80/z80.h"
#include "hw-generic/gdg/gdg.h"
#include "mzarch/mzevent.h"

/**
 * @brief Poloha zadního přepínače SW1 MZ-800 (volba MZ-700 / MZ-800 módu).
 *
 * Hodnota je přímo bit 1 Status registru GDG (IN 0CEh). ROM 9Z-504M při
 * bitu 1 = 0 ponechá MZ-700 mód, při bitu 1 = 1 zapíše DMD = 0 (MZ-800
 * mód) a zhasne paletu - viz ]GOPGM (0ECFCh) a CTRL při resetu (0E853h).
 * Shodně T1-1 ("Stav přepínače MZ-700 ON ... ON = 0"). Poloha přepínače
 * nemění mód sama, čte ji jen ROM (a program).
 *
 * Číselné hodnoty jsou kompatibilní s elementem switch700 ve snapshotu.
 */
typedef enum en_MZ800_MODE_SW
{
    MZ800_MODE_SW_MZ700 = 0, /**< SW1 = ON: MZ-700 mód (výchozí, v praxi obvyklé nastavení) */
    MZ800_MODE_SW_MZ800 = 1, /**< SW1 = OFF: MZ-800 mód */
} en_MZ800_MODE_SW;

typedef enum en_MZ800_HWCOMPAT_ALLOW_PSG1
{
    MZ800_HWCOMPAT_ALLOW_PSG1_NO = 0,
    MZ800_HWCOMPAT_ALLOW_PSG1_YES,
} en_MZ800_HWCOMPAT_ALLOW_PSG1;

typedef struct st_mzarch_main
{
    z80_t *cpu; /* Model cpu-z80 */

    unsigned cursor_timer;

    uint16_t instruction_addr; /* posledni adresa na ktere se nabirala instrukce */

    int instruction_tstates;             /* citac tstates vykonanych v instrukcnim cyklu */
    int instruction_insideop_sync_ticks; /* citac GDG ticks vykonanych v instrukcnim cyklu, ktere uz jsme v ramci sync vykonali */

    /* trace-suite: akumulator extra WAIT T-states pridanych behem aktualne
     * vykonavane instrukce (z volani z80_add_wait_states uvnitr insideop
     * sync paths). Cputrack hook tuto hodnotu po dokonceni instrukce precte
     * jako wait_extra_tstates a vynuluje spolu s instruction_tstates.
     * Saturace na 0xFFFFFFFF pri preteceni (~20 min HALT pri 3.5 MHz). */
    uint32_t instruction_wait_tstates;

    uint8_t regDBUS_latch; /* Obsahuje esoterickeho ducha hodnoty posledniho bajtu, ktery byl precten na datove sbernici */

    int pio8255_ct53g7; /* rizeni CTC0 v rezimu MZ700 */

    st_EMUEVENT event;

    unsigned interrupt;

#if MZARCH == 800 /* HW experimenty pro MY-800 */
    en_MZ800_HWCOMPAT_ALLOW_PSG1 mz800_hwcompat_allow_psg1;

    /** Příznak M1 právě obsluhovaného čtení paměti (nenulový = instrukční
     *  výběr). Nastavuje ho memory_read_cb před čtením; model WAIT u VRAM
     *  podle něj určí T1r (M1 má 4 T, ostatní čtení 3 T). */
    int mreq_m1;
#endif /* MZARCH == 800 */

#if MZARCH != 700
    en_MZ800_MODE_SW mode_sw; /**< Poloha zadního přepínače SW1 (= bit 1 Status registru GDG) */
#endif /* MZARCH != 700 */

    app_mutex_t *reset_request_mutex;
    bool reset_request;
    unsigned reset_count;       /**< Inkrementuje se při každém dokončeném resetu. UI vlákno detekuje reset přes porovnání s předchozí hodnotou - umožňuje sync focus_addr na nové PC i při resetu z paused stavu (= bez normálního pause→run→pause přechodu). */

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* D.4 - SP threshold BP polling. Drží předchozí hodnotu SP pro edge
     * detekci crossing přes threshold. Inicializuje se při resetu na
     * aktuální cpu->sp (= žádný falešný edge po resetu). Aktualizuje se
     * v hot loop pouze pokud per_type_active[BPTMAP_IDX_SP_THRESHOLD]. */
    uint16_t bp_sp_prev;
#endif
} st_mzarch_main;

extern struct st_mzarch_main g_mzarch_main;

#if MZARCH != 700
/** @brief Nenulové, je-li zadní přepínač SW1 v poloze MZ-800 mód. */
#define MZARCH_TEST_MODE_SW_MZ800 (g_mzarch_main.mode_sw == MZ800_MODE_SW_MZ800)
#else
#define MZARCH_TEST_MODE_SW_MZ800 (0) /* MZ-700 nativní: přepínač MZ-700 / MZ-800 neexistuje */
#endif

typedef enum en_INSIDEOP
{
    // IORQ, MREQ a MREQ_E00x se od sebe nicim nelisi - jsou vsak rozdeleny jen pro lepsi debugovani
    INSIDEOP_IORQ = 0,  /* operace PREAD, PWRITE */
    INSIDEOP_MREQ,      /* obecna operace MREQ */
    INSIDEOP_MREQ_E00x, /* MREQ pro mapovane porty */
    // Synchronizace MZ700 VRAM MREQ pri neaktivnim VBLN
    INSIDEOP_MREQ_MZ700_VRAMCTRL,        /* MZ700 VRAM MREQ */
    INSIDEOP_MREQ_MZ800_VRAMCTRL_READ,   /* MZ-800 build: cteni VRAM pres GDG (model WAIT mz800_vramwait.h, rezim MZ-800 i MZ-700) */
    INSIDEOP_MREQ_MZ800_VRAMCTRL_WRITE,  /* MZ-800 build: zapis do VRAM pres GDG (model WAIT mz800_vramwait.h, rezim MZ-800 i MZ-700) */
    INSIDEOP_IORQ_PSG_WRITE,             /* IORQ wite byte do PSG */
} en_INSIDEOP;

#ifdef __cplusplus
extern "C"
{
#endif

    extern void mzarch_main_init(void);
    extern void mzarch_main(void);

    /**
     * @brief Resetuje stroj a zavede MZF stejně jako CLI `--run-mzf`.
     *
     * Provede úplný reset (mzarch_main_reset(): GDG, paměť, CPU, periferie,
     * stav debuggeru) a hned za ním mzarch_bootstrap_run_mzf(). Výsledný stav
     * nezávisí na tom, co v emulátoru běželo předtím, a je shodný se stavem
     * po startu emulátoru s `--run-mzf`.
     *
     * @param filename Cesta k MZF souboru (UTF-8).
     *
     * @pre Volá se z emulátorového vlákna v bodě mezi instrukcemi (drain
     *      fronty dbgapi). Soubor ověřil volající: při nečitelném souboru
     *      se chová jako mzarch_bootstrap_run_mzf() (viz bootstrap.h).
     * @post PC = exec adresa MZF, CPU stojí před první instrukcí programu.
     *       Stav pauzy emulace se nemění.
     *
     * Vedlejší efekty: stejné jako tlačítko Reset (BP událost reset, záznam
     * COLD_RESET do Event Vieweru, zrušení frame-bounded běhu) plus zápisy
     * bootstrapu do periferií a RAM.
     */
    extern void mzarch_main_reset_and_run_mzf(const char *filename);

    /**
     * @brief Okamžitý reset stroje (stejný jako tlačítko Reset, bez fronty žádostí).
     *
     * Volá ho záznam vstupů (replay.c), aby reset proběhl přesně v bodě
     * hranice snímku, kde ho záznam uložil, a ne až v příští iteraci
     * hlavní smyčky jako mzarch_platform_fn_reset_request().
     *
     * @pre Emulátorové vlákno, bod mezi instrukcemi (per-event blok nebo
     *      paused smyčka).
     * @post Stav jako po mzarch_main_reset(); stav pauzy se nemění.
     */
    extern void mzarch_main_reset_now(void);
    extern void mzarch_main_insideop_iorq(void);
    extern void mzarch_main_insideop_mreq(void);
    extern void mzarch_main_insideop_mreq_e00x(void);
    extern void mzarch_main_insideop_mreq_mz700_vramctrl(void);
    extern void mzarch_main_insideop_mreq_mz800_vramctrl_read(void);
    extern void mzarch_main_insideop_mreq_mz800_vramctrl_write(void);
    extern void mzarch_main_insideop_iorq_psg_write(void);

    /**
     * @brief Nahlásí podtečení tiků při uzavření snímku (pojistka).
     *
     * Volá ji jen makro gdg_on_screen_done_event(), když
     * g_gdg.total_elapsed.ticks < VIDEO_SCREEN_TICKS. To za správného běhu
     * nenastane: znamená to, že se konec snímku zpracoval dvakrát. Funkce
     * vypíše varování na stderr (prvních 8 výskytů, u osmého navíc oznámí, že
     * další potlačí); srovnání
     * tiků na 0 dělá volající makro.
     *
     * @param ticks Hodnota g_gdg.total_elapsed.ticks před odečtem.
     *
     * @pre Volá se z EMU vlákna. Mimo hot path (nejvýš jednou za snímek).
     * @par Side effects Výpis na stderr, interní čítač výskytů.
     */
    extern void mzarch_main_report_screen_done_underflow(unsigned ticks);

    /**
     * @brief Nastaví polohu zadního přepínače SW1 (MZ-700 / MZ-800 mód).
     *
     * Mění jen stav přepínače, ne aktuální mód GDG - ten přepíná ROM
     * nebo program podle bitu 1 Status registru (typicky po resetu).
     *
     * @param mode Nová poloha přepínače.
     *
     * @note Na MZ-700 (MZARCH == 700) nic nedělá.
     * @pre Emu vlákno (menu zásah zařadí přes mzarch_request_mode_sw_set())
     *      nebo inicializace před startem emulace (--mode-switch).
     */
    extern void mzarch_mode_sw_set(en_MZ800_MODE_SW mode);

    /**
     * @brief Převede hodnotu volby --mode-switch na polohu přepínače SW1.
     *
     * @param text Hodnota volby: "700" (MZ-700 mód) nebo "800" (MZ-800 mód).
     * @param out Výstup: poloha přepínače; při chybě se nemění.
     * @return true při úspěchu; při neplatné nebo chybějící hodnotě vypíše
     *         chybu na stderr (anglicky) a vrátí false.
     */
    extern bool mzarch_mode_sw_parse_cli(const char *text, en_MZ800_MODE_SW *out);

    /**
     * @brief Hodnota CLI volby s cestou souboru převedená do UTF-8.
     *
     * Interní cesty emulátoru jsou v UTF-8 (GLib). Na Windows je ale `argv`
     * v ANSI kódové stránce procesu, takže se tam hodnota vezme z UTF-16
     * příkazové řádky (g_win32_get_command_line()), bezeztrátově i pro znaky
     * mimo ANSI kódovou stránku. Hledá se stejně jako v sdlapp_options: token
     * přesně `name` (hodnota v dalším tokenu), nebo `name=hodnota`. Jinde
     * (a když se na Windows volba nenajde) vrátí kopii sdlapp_option_value().
     *
     * @param name Jméno volby včetně `--` (např. "--record", "--cmt").
     * @return Nově alokovaná hodnota v UTF-8 (uvolnit g_free), nebo NULL,
     *         pokud volba nemá hodnotu.
     * @pre sdlapp_options_init() už proběhlo.
     * @note Bez vedlejších efektů, volatelné z libovolného vlákna.
     */
    extern char *mzarch_cli_option_value_utf8(const char *name);

    /**
     * @brief Vynutí kompletní překreslení obrazovky emulátoru z aktuálního
     *        stavu VRAM/GDG (border i screen), nezávisle na debuggeru.
     *
     * Přegeneruje celý framebuffer (MZ-700 i MZ-800 cesta dle aktuálního
     * DMD režimu), označí framebuffer state jako změněný a dokončí snímek,
     * takže SDL consumer obraz skutečně překreslí i v pauze. Core varianta
     * původní debugger-only @c debugger_forced_screen_update() - dostupná i
     * v buildu bez debuggeru (volá ji snapshot load po obnově stavu).
     *
     * @pre Volá se v safe-pointu: z emu vlákna mezi instrukcemi (dbgapi
     *      handlery vč. DBGAPI_CMD_SCREEN_REFRESH z Ctrl+R / menu debuggeru,
     *      krok debuggeru), nebo před startem emu vlákna.
     * @note Snapshot load z UI dialogu (snapshot_load_dialog.cpp, quickload)
     *      ji volá z UI vlákna v pauze; souběh s příkazy fronty, které
     *      framebuffer plní také, není vyloučen [neověřeno] - mimo rozsah
     *      ui-thread-writes T6 (okna mimo debugger).
     */
    extern void mzarch_forced_full_screen_refresh(void);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /**
     * @name Maska háčků po instrukci
     *
     * Hlavní smyčka po každém z80_step() čte jedinou proměnnou
     * g_mzarch_post_insn_hooks; nulová = žádný háček, žádná další práce.
     * Bity přepočítává mzarch_post_insn_hooks_recompute() na studených
     * cestách (start a stop diagnostiky), nikdy instrukční smyčka.
     * Nový konzument = nový bit + jeho test v recompute a v rozbočovači
     * mzarch_main_post_instruction_hooks() (mzarch.c).
     * @{
     */
#define MZARCH_POST_INSN_CPUTRACK    (1u << 0) /**< trace-suite cputrack (g_cputrack_active) */
#define MZARCH_POST_INSN_PROFILER_PC (1u << 1) /**< profiler podle PC (g_profiler_pc_active) */
#define MZARCH_POST_INSN_COVERAGE    (1u << 2) /**< pokrytí kódu (g_coverage_active, coverage_hook.h) */

    /**
     * @brief Informace o právě dokončené instrukci pro háčky po instrukci.
     *
     * Rozbočovač ji spočítá jednou po z80_step() a předá ji všem aktivním
     * konzumentům, aby si ji nepočítal každý zvlášť.
     *
     * @invariant `pc` je adresa začátku kroku (g_mzarch_main.instruction_addr),
     *            `next_pc` je cpu->pc po kroku, před případným přijetím
     *            přerušení (přerušení se přijímá až v bloku událostí).
     * @note Platí jen po dobu volání háčků; konzument si ukazatel neukládá.
     */
    typedef struct st_MZARCH_INSN_INFO
    {
        uint16_t pc;      /**< Adresa začátku vykonaného kroku. */
        uint16_t next_pc; /**< PC po kroku (u HALT stejné jako pc). */
        uint32_t tstates; /**< T-stavy kroku včetně WAIT. */
        uint32_t wait;    /**< Čekací stavy kroku (g_mzarch_main.instruction_wait_tstates). */
    } st_MZARCH_INSN_INFO;

    /** @brief Aktivní háčky po instrukci (bity MZARCH_POST_INSN_*); 0 = žádný. */
    extern uint32_t g_mzarch_post_insn_hooks;

    /**
     * @brief Přepočítá g_mzarch_post_insn_hooks z příznaků konzumentů.
     *
     * Volají ji cesty, které mění příznak aktivity konzumenta
     * (cputrack_recompute_active(), profiler_pc_set_active()).
     *
     * @pre Volá se mimo instrukční smyčku (studená cesta).
     * @post Bit konzumenta je nastaven právě tehdy, když je konzument aktivní.
     * @note Zápis jedné proměnné bez zámku, stejně jako dosavadní příznaky
     *       konzumentů; emu vlákno ho uvidí nejpozději v další instrukci.
     */
    extern void mzarch_post_insn_hooks_recompute(void);
    /** @} */
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#define mz800_main_get_instruction_start_ticks() (g_gdg.total_elapsed.ticks - g_mzarch_main.instruction_insideop_sync_ticks) /* muze legalne vracet i zaporne cislo! */

#define mz800_main_cursor_timer_reset() \
    {                                   \
        g_mzarch_main.cursor_timer = 0; \
    }
#define mz800_main_get_cursor_timer_state() ((g_mzarch_main.cursor_timer / 25) & 1)

#define MZ800_MAIN_SET_EVENT(e, t)          \
    {                                       \
        g_mzarch_main.event.event_name = e; \
        g_mzarch_main.event.ticks = t;      \
    }

#ifdef __cplusplus
}
#endif

#endif /* MZARCH_H */
