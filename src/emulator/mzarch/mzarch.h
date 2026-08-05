#ifndef MZARCH_H
#define MZARCH_H

#include "main.h"

#include <stdint.h>
#include <stdbool.h>
#include "app/app_thread.h"

#include "mzcommon_config.h" /* mzhal 11c-2b: mzarch.h sám žádné per-arch makro nepoužívá */
#include "libs/cpu-z80/z80.h"
#include "hw-generic/gdg/gdg_state.h" /* jen g_gdg + superset (mzhal 11c-2b);
                                         per-arch gdg makra sem nepatří */
#include "mzarch/mzhal.h"
#include "mzarch/mzevent.h"

typedef enum en_SWITCH700
{
    SWITCH700_OFF = 0,
    SWITCH700_ON,
} en_SWITCH700;

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

    /* Nepodmíněně (mzhal 9e): layout st_mzarch_main je superset společný
     * všem architekturám. Pole plní/čte jen kód příslušné platformy,
     * jinde zůstávají v nule. */
    en_MZ800_HWCOMPAT_ALLOW_PSG1 mz800_hwcompat_allow_psg1; /* jen MZ-800 (HW experiment) */
    en_SWITCH700 switch700; /* zadní DIP MZ-700 mode; MZ-700 nativní ho nemá */

    app_mutex_t *reset_request_mutex;
    bool reset_request;
    unsigned reset_count;       /**< Inkrementuje se při každém dokončeném resetu. UI vlákno detekuje reset přes porovnání s předchozí hodnotou - umožňuje sync focus_addr na nové PC i při resetu z paused stavu (= bez normálního pause→run→pause přechodu). */

    /* D.4 - SP threshold BP polling. Drží předchozí hodnotu SP pro edge
     * detekci crossing přes threshold. Inicializuje se při resetu na
     * aktuální cpu->sp (= žádný falešný edge po resetu). Aktualizuje se
     * v hot loop pouze pokud per_type_active[BPTMAP_IDX_SP_THRESHOLD].
     * Nepodmíněně (mzhal 9c): layout st_mzarch_main nesmí záviset na
     * build volbě NO_DEBUGGER (pravidlo z kroku 5); pole používá jen
     * debugger kód. */
    uint16_t bp_sp_prev;
} st_mzarch_main;

extern struct st_mzarch_main g_mzarch_main;

/**
 * @brief Per-arch hook po dokončení snapshot loadu (mzhal 11e).
 *
 * Volá snapshot_mgr po obnově kompletního stavu. Implementace je
 * v per-EXE mzarch.c: MZ-800 zde rebuiduje RAM fast-path page-table
 * (snap_gdg obnovuje regDMD mimo banking-switch cestu), ostatní
 * architektury no-op. Compile-once snapshot_mgr tak nepotřebuje
 * per-arch symboly.
 */
extern void mzarch_snapshot_post_load(void);

/* Runtime z g_mzhal (mzhal 11c-2b): jediný konzument je cold UI menu
 * menu_rear_dip_switch.cpp; na MZ-700 nativním je predikát vždy 0. */
#define MZARCH_TEST_REAR_DIP_SWITCH700 (g_mzhal.has_rear_dip_switch700 && (g_mzarch_main.switch700 == 1))

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
    extern void mzarch_rear_dip_switch_mz700_compat(unsigned value);

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
     * @pre Volá se v safe-pointu (emulace v pauze nebo z emu vlákna mezi
     *      instrukcemi) - stejný kontrakt jako debugger_forced_screen_update.
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
