/*
 * File:   bp_event.h
 *
 * Modul bp_event - vocabulary HW eventů + dispatch infrastruktura pro
 * smart BP typu BPT_TYPE_HW_EVENT.
 *
 * V1.5 HWE redesign:
 *   - 28 finálních eventů ve 4 kategoriích (signal / change / point / CPU).
 *   - Trigger condition pro signal eventy (low/high/rising/falling/changed).
 *   - State cache (g_bp_event_state[]) pro edge detection.
 *
 * V1.6+ TODO 4.6 vocabulary expand (+3 eventy = 31 total):
 *   - BP_EVENT_CPU_IFF1_CHANGE / IFF2_CHANGE (IFF1 a IFF2 separate)
 *   - BP_EVENT_CMT_STATE_CHANGE (CMT lifecycle: STOP/PLAY/RECORD/PAUSED)
 *   - BP_EVENT_CPU_IFF_CHANGE zachovan (= BC pro V1.5 .bpt files), ale
 *     deprecated ve prospech IFF1/IFF2 split.
 *   - psg:int_pa5 NEPRIDAN - service manual chyba (PA5 = VBLN, ne PSG INT,
 *     viz public/REVIEW_psg_int_pa5.md v mz800-knowledge).
 *
 * Účel:
 *   - Definuje stabilní enum HW eventů (vsync, ctc:zc0, irq:fdc, ...).
 *   - Konverzní funkce enum <-> string (= JSON serializace BP, UI dropdown).
 *   - bp_event_fire() = dispatch z emu hook sites do BP enforcement vrstvy
 *     (= breakpoints_enforce_hw_event v breakpoints.c).
 *   - g_bp_event_active[] = bitmap pro fast-skip v hot path (jeden array
 *     load + branch v default OFF stavu).
 *   - g_bp_event_state[] = předchozí signal level pro edge detection.
 *
 * Design:
 *   - Hooky v emu kódu volají paralelně s existujícími trace-suite v1
 *     call sites (HWLOG, INTLOG, IORQLOG rodiny) - žádná duplicita HW
 *     logiky, jen druhý observer.
 *   - Pro events s parametrem (raster s N) předává volající param
 *     (= row number) a BP filtruje podle bp->event_param.
 *   - Pro signal eventy předává volající aktuální signal level (0/1)
 *     v parametru value; BP s trigger condition rozhodne, zda fire.
 *
 * Threading: dispatch jen z EMU vlákna (čte g_bp_event_active z hot
 * path). Registrace per-event se synchronizuje přes breakpoints.c při
 * mutacích BP (UI vlákno volá set_event_name -> sync_bptmap).
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef BP_EVENT_H
#define BP_EVENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>


/**
 * @brief Enum HW event vocabulary pro BPT_TYPE_HW_EVENT.
 *
 * Hodnoty stabilní - serializace přes řetězcový název (bp_event_to_string).
 * Číselná hodnota se v .bpt souboru nevyskytuje.
 *
 * Kategorizace (viz bp_event_get_kind):
 *   SIGNAL       - signál se selectorem trigger condition (vsync, ctc:zc0,
 *                  irq:fdc, cmt:in, ...). Předává se 0/1 jako value.
 *   CHANGE       - implicit "happened" pri kazdem fire (mode_change,
 *                  palette_change, palgrp_change, border_change,
 *                  cksw_change). Value
 *                  nese novou hodnotu (= dostupna v condition jako Value).
 *   POINT_PARAM  - point event s parametrem (raster:N). Value nese row.
 *   POINT_NOPARAM- point event bez parametru (cpu:nmi, cpu:halt, ...).
 *
 * Speciální hodnoty:
 *   - BP_EVENT_NONE: výchozí pro BP bez nastaveného event_name.
 */
typedef enum en_BP_EVENT {
    BP_EVENT_NONE = 0,

    /* === Signal events (17) - selector 0/1/up/down/changed === */
    BP_EVENT_GDG_VSYNC,             /**< vertikalni sync (= 50 Hz) */
    BP_EVENT_GDG_HSYNC,             /**< horizontalni sync */
    BP_EVENT_GDG_VBLN,              /**< vertikalni blanking */
    BP_EVENT_GDG_HBLN,              /**< horizontalni blanking */
    BP_EVENT_CTC_ZC0,               /**< CTC8253 ZC vystup kanal 0 */
    BP_EVENT_CTC_ZC1,               /**< CTC8253 ZC vystup kanal 1 */
    BP_EVENT_CTC_ZC2,               /**< CTC8253 ZC vystup kanal 2 */
    BP_EVENT_IRQ_CTC2,              /**< INT linka CTC kanalu 2 */
    BP_EVENT_IRQ_PIOZ80_A,          /**< Z80 PIO port A interrupt */
    BP_EVENT_IRQ_PIOZ80_B,          /**< Z80 PIO port B interrupt */
    BP_EVENT_IRQ_FDC,               /**< WD279x FDC IRQ linka */
    BP_EVENT_TEMPO,                 /**< GDG TEMPO signal (~32 Hz) */
    BP_EVENT_CURSOR,                /**< NE556 cursor timer (~2 Hz) */
    BP_EVENT_CMT_IN,                /**< CMT vstupni signal (cteni z pasky) */
    BP_EVENT_CMT_OUT,               /**< CMT vystupni signal (zapis na pasku) */
    BP_EVENT_CMT_MSTATE,            /**< CMT motor sense (= simulovany) */
    BP_EVENT_CMT_MOTOR,             /**< CMT motor on/off (PC3 bit 8255) */

    /* === Change events (5) - implicit "changed" === */
    BP_EVENT_GDG_MODE_CHANGE,       /**< zapis do DMD (display mode descriptor) */
    BP_EVENT_GDG_PALETTE_CHANGE,    /**< zmena konkretni barvy v palete */
    BP_EVENT_GDG_PALGRP_CHANGE,     /**< zmena palette group selectoru */
    BP_EVENT_GDG_BORDER_CHANGE,     /**< zmena border barvy */
    BP_EVENT_GDG_CKSW_CHANGE,       /**< zápis bitu CKSW (port 0x07CF), value = bit 0/1; jen MZ-800 */
    BP_EVENT_CMT_STATE_CHANGE,      /**< CMT lifecycle change (V1.6+ TODO 4.6) - value = en_BP_CMT_STATE */

    /* === Point events (1) - parametr filter === */
    BP_EVENT_GDG_RASTER,            /**< raster:N (row N), parametr 0..311 */

    /* === CPU events (8) - point/state, bez selectoru === */
    BP_EVENT_CPU_NMI,               /**< NMI assertion */
    BP_EVENT_CPU_DI,                /**< DI instrukce vykonana */
    BP_EVENT_CPU_IM_CHANGE,         /**< IM 0/1/2 zmena (param = new IM) */
    BP_EVENT_CPU_IFF_CHANGE,        /**< IFF1/IFF2 zmena (param = new IFF, agregovany) - DEPRECATED v V1.6+ */
    BP_EVENT_CPU_IFF1_CHANGE,       /**< IFF1 zmena (V1.6+ TODO 4.6) - value = new IFF1 (0/1) */
    BP_EVENT_CPU_IFF2_CHANGE,       /**< IFF2 zmena (V1.6+ TODO 4.6) - value = new IFF2 (0/1) */
    BP_EVENT_CPU_HALT,              /**< HALT instrukce */
    BP_EVENT_CPU_RESET,             /**< CPU reset */

    BP_EVENT_COUNT                  /**< sentinel - pocet platnych hodnot */
} en_BP_EVENT;


/**
 * @brief Composite CMT state values pro BP_EVENT_CMT_STATE_CHANGE event.
 *
 * Composes en_CMT_STATE (STOP/PLAY/RECORD) s g_cmt.paused flag. PAUSED je
 * "override" - pokud je tape paused, value = PAUSED bez ohledu na underlying
 * state (PLAY/RECORD).
 *
 * Hodnoty stabilní (V1.6+ TODO 4.6 vocabulary expand).
 */
typedef enum en_BP_CMT_STATE {
    BP_CMT_STATE_STOP    = 0,   /**< g_cmt.state == STOP, paused == 0 (= ejected nebo stopped) */
    BP_CMT_STATE_PLAY    = 1,   /**< g_cmt.state == PLAY, paused == 0 */
    BP_CMT_STATE_RECORD  = 2,   /**< g_cmt.state == RECORD, paused == 0 */
    BP_CMT_STATE_PAUSED  = 3,   /**< g_cmt.paused == 1 (override jakekoli underlying state) */
    BP_CMT_STATE_COUNT          /**< sentinel */
} en_BP_CMT_STATE;


/**
 * @brief Kategorie eventu pro UI/enforce dispatch logiku.
 *
 * Hodnoty se neseralizuji - odvozeny z en_BP_EVENT pres bp_event_get_kind.
 */
typedef enum en_BP_EVENT_KIND {
    BP_EVT_KIND_SIGNAL,         /**< signal s trigger condition (0/1/up/down/changed) */
    BP_EVT_KIND_CHANGE,         /**< implicit changed, bez selectoru */
    BP_EVT_KIND_POINT_PARAM,    /**< point s parametrem (raster) */
    BP_EVT_KIND_POINT_NOPARAM   /**< point bez parametru (CPU NMI, RESET, ...) */
} en_BP_EVENT_KIND;


/**
 * @brief Trigger condition pro signal HW eventy (vsync, hsync, IRQ lines, ...).
 *
 * Aplikuje se jen na BP_EVT_KIND_SIGNAL eventy. Pro CHANGE / POINT
 * eventy je trigger ignorován (= implicit "happened").
 *
 * Hodnoty stabilní - serializace přes string (low/high/rising/falling/changed).
 */
typedef enum en_BP_EVENT_TRIGGER {
    BP_EVT_TRIG_RISING = 0,     /**< default - prev=0, curr=1 (legacy "happened") */
    BP_EVT_TRIG_FALLING,        /**< prev=1, curr=0 */
    BP_EVT_TRIG_CHANGED,        /**< prev != curr */
    BP_EVT_TRIG_LOW,            /**< curr == 0 (level) */
    BP_EVT_TRIG_HIGH,           /**< curr == 1 (level) */
    BP_EVT_TRIG_COUNT           /**< sentinel */
} en_BP_EVENT_TRIGGER;


/**
 * @brief Bitmap aktivních eventů (= alespoň 1 BP s daným eventem registrován).
 *
 * Hot path test:
 *   if (g_bp_event_active[BP_EVENT_X]) bp_event_fire(BP_EVENT_X, value);
 *
 * V default OFF stavu = false všude → branch predictor naučí "nikdy",
 * ~zero impact. Spravuje breakpoints.c při set_event_name / sync_bptmap.
 */
extern bool g_bp_event_active [ BP_EVENT_COUNT ];


/**
 * @brief Per-event prev signal value pro edge detection.
 *
 * Updates v breakpoints_enforce_hw_event() PO iteraci. Hodnoty platí jen
 * pro BP_EVT_KIND_SIGNAL eventy (jinde nepoužito).
 */
extern uint8_t g_bp_event_state [ BP_EVENT_COUNT ];


/**
 * @brief Vrátí kategorii eventu.
 *
 * @param event  Hodnota enum (BP_EVENT_NONE / out-of-range vratí
 *               BP_EVT_KIND_POINT_NOPARAM jako bezpečný default).
 */
extern en_BP_EVENT_KIND bp_event_get_kind ( en_BP_EVENT event );


/**
 * @brief Konverze enum -> string (stabilní název pro JSON / UI).
 *
 * Příklad: BP_EVENT_GDG_VSYNC = "vsync", BP_EVENT_GDG_RASTER = "raster"
 * (= bez ":N" suffixu - parametr se serializuje samostatně do
 * event_param). BP_EVENT_NONE -> "" (prázdný řetězec).
 *
 * @param event Hodnota enum.
 * @return Konstantní string (statická tabulka). Fallback "" pro neznámou hodnotu.
 */
extern const char* bp_event_to_string ( en_BP_EVENT event );


/**
 * @brief Konverze string -> enum + parametr.
 *
 * Akceptované formáty:
 *   - "vsync", "ctc:zc0", "irq:fdc", ... = jednoduché názvy (param = 0)
 *   - "raster:N", kde N je dekadické číslo 0-65535 = parametrizovaný event
 *
 * BC aliasy pro V1.5.A8 a starší persistence:
 *   - "pio:porta_int" -> BP_EVENT_IRQ_PIOZ80_A
 *   - "pio:portb_int" -> BP_EVENT_IRQ_PIOZ80_B
 *   - "fdc:irq"       -> BP_EVENT_IRQ_FDC
 *   - "tape:edge"     -> BP_EVENT_CMT_IN
 *   - "nmi", "di", "im_change", ... -> BP_EVENT_CPU_*
 *
 * @param name        Vstupní string (NULL = neznámý).
 * @param out_event   Výstup enum hodnoty (povinný).
 * @param out_param   Výstup parametru (0 pokud event nepoužívá; nepovinný = NULL OK).
 * @return true pokud parsování uspělo (= name odpovídá známému eventu).
 *
 * Při neúspěchu *out_event a *out_param zůstávají nezměněné.
 */
extern bool bp_event_from_string ( const char *name,
                                    en_BP_EVENT *out_event,
                                    int32_t *out_param );


/**
 * @brief Konverze trigger enum -> string (stabilní pro JSON).
 *
 * Hodnoty: "low", "high", "rising", "falling", "changed". Fallback "rising".
 */
extern const char* bp_event_trigger_to_string ( en_BP_EVENT_TRIGGER trig );


/**
 * @brief Konverze string -> trigger enum.
 *
 * @return true pokud s odpovídá známé hodnotě, jinak false (out nezměněn).
 */
extern bool bp_event_trigger_from_string ( const char *s,
                                            en_BP_EVENT_TRIGGER *out );


/**
 * @brief Reason kódy pro detailnější filtering v BP DSL přes identifikátor
 *        `Reason`.
 *
 * Pro IFF events (BP_EVENT_CPU_IFF*_CHANGE) odpovídá `z80_iff_change_reason_t`
 * z `cpu-z80/z80.h` - hodnoty 0..6 (RESET/EI/DI/INT_ACK/NMI_ACK/RETI/RETN).
 * BP_REASON_NONE = 0xFF je sentinel default pro non-IFF eventy (= filter
 * `Reason == NMI_ACK` nikdy nematchne na non-IFF event).
 *
 * Stabilní - DSL symbolic names mapují přímo (= "EI" -> 1, "NMI_ACK" -> 4
 * apod.); JSON persist přes integer hodnotu.
 *
 * **Předávání hodnoty:** `bp_event_fire()` zachovává původní signaturu
 * (= 67 callsites bez migrace). Reason se předává přes ambient global
 * `g_bp_fire_reason` který callsite nastaví těsně před fire a po fire
 * reset na NONE. BP enforcement vrstva (`bp_expr.c` resolve_ident
 * "Reason") čte tuto hodnotu. Pattern je bezpečný v emu vlákně (= jediný
 * fire stack najednou, žádná rekurze v BP action která by fíryla další
 * event).
 */
typedef enum en_BP_FIRE_REASON {
    BP_REASON_IFF_RESET   = 0,  /**< IFF cleared v důsledku CPU reset (= odpovídá Z80_IFF_REASON_RESET). */
    BP_REASON_IFF_EI      = 1,  /**< IFF set v důsledku EI instrukce. */
    BP_REASON_IFF_DI      = 2,  /**< IFF cleared v důsledku DI instrukce. */
    BP_REASON_IFF_INT_ACK = 3,  /**< IFF cleared v důsledku INT acknowledgement (IM 0/1/2). */
    BP_REASON_IFF_NMI_ACK = 4,  /**< IFF1 cleared v důsledku NMI acknowledgement (IFF2 zachováno). */
    BP_REASON_IFF_RETI    = 5,  /**< IFF beze změny - RETI je signal pro Z80 PIO (no-op pro IFF). */
    BP_REASON_IFF_RETN    = 6,  /**< IFF1 obnovené z IFF2 (RETN). */
    BP_REASON_NONE        = 0xFF /**< Default pro non-IFF eventy (= reason nemá smysl). */
} en_BP_FIRE_REASON;


/**
 * @brief Ambient reason hodnota čtená BP enforcement vrstvou při evaluaci
 *        DSL identifikátoru `Reason`.
 *
 * Default = BP_REASON_NONE. Callsite (typicky mzarch IFF callback) nastaví
 * konkrétní reason těsně před `bp_event_fire()` a po skončení fire stack
 * vrátí na NONE. Mimo aktivní fire je hodnota nedefinovaná - DSL filter
 * `Reason == X` má smysl pouze v rámci BP condition evaluovaného na hit
 * konkrétního eventu.
 *
 * Pattern je bezpečný v single-thread emu vlákně - DSL evaluation runs
 * synchronně v rámci `bp_event_fire()` -> `breakpoints_enforce_hw_event()`
 * call stacku.
 */
extern uint8_t g_bp_fire_reason;


/**
 * @brief Dispatch HW eventu do BP enforcement vrstvy.
 *
 * Volá se z hook sites v emu (paralelně s trace-suite v1 hwlog/intlog
 * volání). Výchozí path:
 *   1. Test g_bp_event_active[event] - pokud false, return (= žádný BP)
 *   2. breakpoints_enforce_hw_event(event, value) v breakpoints.c
 *
 * Sémantika `value` per kind:
 *   SIGNAL       - 0 nebo 1 (current signal level)
 *   CHANGE       - nová hodnota (mode/palette/palgrp/border value)
 *   POINT_PARAM  - parametr (= raster row 0..311)
 *   POINT_NOPARAM- 0 (ignorováno) nebo info (IM/IFF new value)
 *
 * Pro events s reason kontextem (IFF*_CHANGE) callsite nastaví
 * `g_bp_fire_reason` před fire a vrátí na BP_REASON_NONE po fire stacku
 * (= viz mzarch_iff_change_cb pattern v interrupt.c).
 *
 * Side effects: může pause emulátor (pokud BP action nevolá continue).
 *
 * @param event  Identifikace eventu.
 * @param value  Per-kind value (signal level / change value / param / 0).
 */
extern void bp_event_fire ( en_BP_EVENT event, int32_t value );


/**
 * @brief Inicializace bitmap (= clear all to false) + state cache.
 *
 * Volá se z bptmap_init() / breakpoints_init() jednou při startu.
 */
extern void bp_event_init ( void );


/**
 * @brief Reset bitmap do default OFF stavu.
 *
 * Volá breakpoints.c při sync_bptmap před re-registrací eventů.
 */
extern void bp_event_clear_all_active ( void );


/**
 * @brief Označí daný event jako aktivní (= alespoň 1 BP).
 */
extern void bp_event_set_active ( en_BP_EVENT event, bool active );


/**
 * @brief Test, zda je event smysluplny na aktualni MZARCH platforme.
 *
 * Per-arch matrix:
 *  - MZ-800  (HAVE_PIOZ80=1): vsechny eventy podporovane
 *  - MZ-1500 (HAVE_PIOZ80=0): bez irq:pioz80_a/b a bez GDG
 *                             change eventu (mode/palette/palgrp/border).
 *                             MZ-1500 sice fire mode_change a palette_change
 *                             ale jejich semantika je ruzna od MZ-800,
 *                             a UI je pro MZ-800 layout - skryvame.
 *  - MZ-700  (HAVE_PIOZ80=0): bez irq:pioz80_a/b, bez GDG change eventu.
 *
 * Pouzito v UI dropdown filteru (bpt_edit_panel.cpp) - eventy ktere
 * vraci false se v dropdown skryvaji. Existujici BP s nepodporovanym
 * eventem (load ze starsiho .bpt) se NEFILTRUJI - jsou viditelne v
 * preview, ale uzivatel je nemuze nove vybrat.
 *
 * @param event Hodnota enum.
 * @return true pokud event ma smysl na aktualni platforme.
 */
extern bool bp_event_is_supported_on_current_arch ( en_BP_EVENT event );


#ifdef __cplusplus
}
#endif

#endif /* BP_EVENT_H */
