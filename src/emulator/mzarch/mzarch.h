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
    INSIDEOP_MREQ_MZ800_VRAMCTRL_READ,   /* READ z MZ-800 grafickeho VRAM (HDL WAIT model) */
    INSIDEOP_MREQ_MZ800_VRAMCTRL_WRITE,  /* WRITE do MZ-800 grafickeho VRAM (HDL WAIT model + start horke faze) */
    INSIDEOP_IORQ_PSG_WRITE,             /* IORQ wite byte do PSG */
} en_INSIDEOP;

#ifdef __cplusplus
extern "C"
{
#endif

    extern void mzarch_main_init(void);
    extern void mzarch_main(void);
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
     * @note Volá se z UI vlákna bez zámku (dosavadní praxe, zápis jedné
     *       proměnné čtené emulačním vláknem).
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
