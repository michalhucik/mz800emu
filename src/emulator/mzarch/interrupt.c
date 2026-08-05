#include "main.h"

#include "interrupt.h"
#include "hw-generic/pioz80/pioz80.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/fdc/fdc.h"
#include "mzarch/mzhal.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/intlog.h"
#include "debugger/trace/eventlog.h"
#include "debugger/bptmap.h"
#include "debugger/breakpoints.h"
#include "debugger/bp_event.h"
#include "debugger/callstack.h"
#endif

/*******************************************************************************
 *
 *
 * Obsluha preruseni
 *
 *
 ******************************************************************************/

/**
 * Kontrola preruseni od CTC2 - je maskovano z i8255 PC02
 */
static inline void mzarch_interrupt_check_ctc2(void)
{
    /* k interruptu dojde jen pokud CTC2: 1 a PC02: 1 */
    if (CTC8253_OUT(2) & g_pio8255.signal_pc02)
    {
        g_mzarch_main.interrupt |= MZARCH_INTERRUPT_CTC2;
    }
    else
    {
        g_mzarch_main.interrupt &= ~MZARCH_INTERRUPT_CTC2;
    };
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* HWE - HW event BP hook (irq:ctc2). Posíláme aktuální INT level
     * (= 0/1) - enforce vrstva edge detection udělá přes trigger condition. */
    if ( g_bp_event_active[ BP_EVENT_IRQ_CTC2 ] ) {
        bp_event_fire ( BP_EVENT_IRQ_CTC2,
                         ( g_mzarch_main.interrupt & MZARCH_INTERRUPT_CTC2 ) ? 1 : 0 );
    }
#endif
}

/**
 * Kontrola preruseni od PIOZ80
 */
static inline void mzarch_interrupt_check_pioz80(void)
{
    if (g_pioz80.interrupt & PIOZ80_INTERRUPT_INT_BIT)
    {
        g_mzarch_main.interrupt |= MZARCH_INTERRUPT_PIOZ80;
    }
    else
    {
        g_mzarch_main.interrupt &= ~MZARCH_INTERRUPT_PIOZ80;
    };
}

// #include "hw-generic/gdg/gdg.h"

/**
 * Kontrola preruseni od FDC
 */
static inline void mzarch_interrupt_check_fdc(void)
{
    if (fdc_get_interrupt_state())
    {
        g_mzarch_main.interrupt |= MZARCH_INTERRUPT_FDC;
        // printf("%s() - FDC interrupt: 1, gdg_screens: %u, gdg_px: %u\n", __func__, g_gdg.total_elapsed.screens, g_gdg.total_elapsed.ticks);
    }
    else
    {
        g_mzarch_main.interrupt &= ~MZARCH_INTERRUPT_FDC;
    };
}

/**
 * Provede kontrolu vsech zarizeni, ktere jsou schopny vygenerovat interrupt.
 *
 * @param event_ticks
 */
void mzarch_interrupt_manager(void)
{
    /* Runtime capability check - volano event-driven (CTC/FDC/PIO zmeny),
     * ne per-instrukce. */
    if (g_mzhal.have_fdc)
        mzarch_interrupt_check_fdc();
    mzarch_interrupt_check_ctc2();
    if (g_mzhal.have_pioz80)
        mzarch_interrupt_check_pioz80();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite intlog: emit pin_edge eventy pro každou změnu interrupt
     * sběrnice. Polling po update - cheaper než hookování každého
     * mzarch_interrupt_check_* (= 3 funkce vs 1 polling). */
    if ( TEST_TRACE_INTLOG_DISPATCH ) {
        intlog_poll_interrupt_state ( g_mzarch_main.interrupt );
    }

    /* V1.5.A8: IRQ BP hook se NEvolá zde (= pre-dispatch). Místo toho
     * je volán z mzarch.c po z80_process_interrupt (POST-dispatch),
     * kde jsou dostupné vector_addr a isr_addr pro IM2 filter. */

    /* V1.5.A8.5: IRQ_SIG BP hook se volá ZDE (pre-dispatch). Detekuje
     * edge raise (prev=0, curr=1) na INT bus a fire BPT_TYPE_IRQ_SIG s
     * matching source mask. Static prev_irq drží snapshot z minulého
     * volání mzarch_interrupt_manager.
     *
     * per_type_active guard = zero overhead pokud žádný IRQ_SIG BP. */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_IRQ_SIG ] ) {
        static uint8_t s_prev_irq_for_sig = 0;
        uint8_t curr_irq = g_mzarch_main.interrupt;
        if ( curr_irq != s_prev_irq_for_sig ) {
            breakpoints_enforce_irq_sig ( curr_irq, s_prev_irq_for_sig );
        };
        s_prev_irq_for_sig = curr_irq;
    };
#endif
}

/**
 * CPU preslo do rezimu, kdy je opet povoleno ruseni - zkontrolujeme, zda tu neceka nejaky interrupt
 *
 * @param cpu
 * @param user_data
 */
void mzarch_ei_cb(z80_t *cpu, void *user_data)
{
    (void)cpu;
    (void)user_data;
    // printf("mz800_ei_cb()\n");
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite intlog: EI vykonáno (CPU právě povolilo přerušení). */
    if ( TEST_TRACE_INTLOG_DISPATCH ) {
        uint32_t bits = INTLOG_STATE_BIT_EI
            | ( cpu->iff1 ? INTLOG_STATE_BIT_IFF1 : 0 )
            | ( cpu->iff2 ? INTLOG_STATE_BIT_IFF2 : 0 )
            | ( cpu->im == 0 ? INTLOG_STATE_BIT_IM0 :
                cpu->im == 1 ? INTLOG_STATE_BIT_IM1 : INTLOG_STATE_BIT_IM2 );
        intlog_record_cpu_int_state ( bits );
    }
#endif
    /* IFF*_CHANGE BP eventy (V1.5.A8 a starší) zde fíruly přímo, ale V1.7+
     * 1.5 přesouvá jejich generaci do unified mzarch_iff_change_cb()
     * (= napojené přes z80_set_iff_change). To pokrývá i ostatní reasony
     * mimo EI/DI (INT_ACK / NMI_ACK / RETI / RETN / RESET), které
     * předtím neměly BP event vyvolaný vůbec. */
    if (g_mzarch_main.interrupt)
    {
        MZ800_MAIN_SET_EVENT(MZEVENT_BREAK_MZARCH_INTERRUPT, 0);
    };
}


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

/**
 * @brief D.3 - z80 lib DI callback. Fire BP event di + iff_change.
 *
 * Volá se z z80 lib po vykonání DI instrukce. Iff1/iff2 už jsou 0.
 */
void mzarch_di_cb ( z80_t *cpu, void *user_data ) {
    (void) cpu;
    (void) user_data;

    /* trace-suite intlog: DI vykonáno (CPU právě zakázalo přerušení).
     * Symetrie s mzarch_ei_cb - bez tohoto fan-outu by DI v okně Events
     * (eventlog kategorie CPU_INT) chybělo, zatímco EI by se logovalo.
     * IFF1/IFF2 jsou v tomto okamžiku už 0 (DI je provedeno). */
    if ( TEST_TRACE_INTLOG_DISPATCH ) {
        uint32_t bits = INTLOG_STATE_BIT_DI
            | ( cpu->iff1 ? INTLOG_STATE_BIT_IFF1 : 0 )
            | ( cpu->iff2 ? INTLOG_STATE_BIT_IFF2 : 0 )
            | ( cpu->im == 0 ? INTLOG_STATE_BIT_IM0 :
                cpu->im == 1 ? INTLOG_STATE_BIT_IM1 : INTLOG_STATE_BIT_IM2 );
        intlog_record_cpu_int_state ( bits );
    }

    if ( g_bp_event_active[ BP_EVENT_CPU_DI ] ) {
        bp_event_fire ( BP_EVENT_CPU_DI, 0 );
    }
    /* IFF*_CHANGE BP eventy přesunuty do mzarch_iff_change_cb (V1.7+ 1.5),
     * viz mzarch_ei_cb komentář. */
}


/**
 * @brief D.3 - z80 lib IM change callback. Fire BP event im_change.
 *
 * @param new_im Nová hodnota IM (0/1/2). Předává jako event_param.
 */
void mzarch_im_cb ( z80_t *cpu, uint8_t new_im, void *user_data ) {
    (void) cpu;
    (void) user_data;
    if ( g_bp_event_active[ BP_EVENT_CPU_IM_CHANGE ] ) {
        bp_event_fire ( BP_EVENT_CPU_IM_CHANGE, (int32_t) new_im );
    }
}


/**
 * @brief D.3 - z80 lib HALT callback. Fire BP event halt.
 */
void mzarch_halt_cb ( z80_t *cpu, void *user_data ) {
    (void) cpu;
    (void) user_data;
    if ( g_bp_event_active[ BP_EVENT_CPU_HALT ] ) {
        bp_event_fire ( BP_EVENT_CPU_HALT, 0 );
    }
}


/**
 * @brief D.3 - z80 lib NMI callback. Fire BP event nmi.
 */
void mzarch_nmi_cb ( z80_t *cpu, void *user_data ) {
    (void) cpu;
    (void) user_data;
    if ( g_bp_event_active[ BP_EVENT_CPU_NMI ] ) {
        bp_event_fire ( BP_EVENT_CPU_NMI, 0 );
    }
}


/**
 * @brief V1.7+ 1.5 - z80 lib IFF change callback. Fire BP eventy
 *        IFF_CHANGE / IFF1_CHANGE / IFF2_CHANGE podle decision matrix.
 *
 * Volá se z z80 lib po každé změně IFF1 nebo IFF2 (sedm důvodů viz
 * `z80_iff_change_reason_t` - RESET, EI, DI, INT_ACK, NMI_ACK, RETI,
 * RETN). Pro každý důvod určuje matice (viz `iff_fire_matrix` níže),
 * které ze tří BP eventů se mají vyvolat:
 *
 *  - EI / DI / INT_ACK / RESET / NMI_ACK: oba IFF se mění (nebo se
 *    chovají jako by se měnily kvůli forward-compat pro alternativní
 *    Z80 architektury), fíruje všechny 3 eventy.
 *  - RETI: na originálním Z80 IFF beze změny (= signal pro PIO),
 *    nefíruje nic.
 *  - RETN: IFF1 obnoven z IFF2 (mění se vždy, i kdy by IFF1==IFF2
 *    před, kvůli jednoduché sémantice - vnořená ISR detekce přes
 *    BP condition expression).
 *
 * Reason se předá BP enforcement vrstvě přes ambient global
 * `g_bp_fire_reason`. DSL filter v BP condition: `Reason == nmi_ack`.
 *
 * @param cpu      Z80 CPU instance.
 * @param new_iff1 Nová hodnota IFF1 (0/1).
 * @param new_iff2 Nová hodnota IFF2 (0/1).
 * @param reason   z80_iff_change_reason_t (0..6).
 * @param user_data Nepoužito.
 */
void mzarch_iff_change_cb ( z80_t *cpu, uint8_t new_iff1, uint8_t new_iff2,
                            uint8_t reason, void *user_data ) {
    (void) cpu;
    (void) user_data;

    /* Decision matrix - per reason které ze 3 IFF BP eventů fíruje.
     * Index = z80_iff_change_reason_t (0=RESET, 1=EI, 2=DI, 3=INT_ACK,
     * 4=NMI_ACK, 5=RETI, 6=RETN). Bity: 0x1=IFF_CHANGE, 0x2=IFF1_CHANGE,
     * 0x4=IFF2_CHANGE. */
    static const uint8_t iff_fire_matrix [ 7 ] = {
        /* RESET   */ 0x1 | 0x2 | 0x4,  /* všechny tři */
        /* EI      */ 0x1 | 0x2 | 0x4,
        /* DI      */ 0x1 | 0x2 | 0x4,
        /* INT_ACK */ 0x1 | 0x2 | 0x4,
        /* NMI_ACK */ 0x1 | 0x2 | 0x4,  /* IFF2 forward-compat pro alt Z80 arch */
        /* RETI    */ 0,                /* IFF beze změny, nefíruje nic */
        /* RETN    */ 0x1 | 0x2         /* jen IFF1 + agregovany (IFF2 zachováno) */
    };

    if ( reason >= 7 ) return;  /* defensiva proti neznámému reasonu */
    uint8_t flags = iff_fire_matrix [ reason ];

    /* BP dispatch jen pokud má matrix non-zero flags. RETI má flags=0
     * (= IFF se nemění, žádný BP event); přesto musíme dál pokračovat
     * do callstack fan-outu, který RETI/RETN potřebuje pro pop top
     * frame. */
    if ( flags != 0 ) {
        /* Reason se předává přes ambient global - viz bp_event.h doc. */
        g_bp_fire_reason = reason;

        if ( ( flags & 0x1 ) && g_bp_event_active[ BP_EVENT_CPU_IFF_CHANGE ] ) {
            /* Agregovany event - value = new_iff1 (= zachováno z V1.5.A8 chování). */
            bp_event_fire ( BP_EVENT_CPU_IFF_CHANGE, (int32_t) new_iff1 );
        }
        if ( ( flags & 0x2 ) && g_bp_event_active[ BP_EVENT_CPU_IFF1_CHANGE ] ) {
            bp_event_fire ( BP_EVENT_CPU_IFF1_CHANGE, (int32_t) new_iff1 );
        }
        if ( ( flags & 0x4 ) && g_bp_event_active[ BP_EVENT_CPU_IFF2_CHANGE ] ) {
            bp_event_fire ( BP_EVENT_CPU_IFF2_CHANGE, (int32_t) new_iff2 );
        }

        g_bp_fire_reason = (uint8_t) BP_REASON_NONE;  /* cleanup */
    };

    /* Callstack fan-out (mutant callstack-experimental Fáze 1B). Gate
     * uvnitř callstack_on_iff_change kontroluje g_callstack_active = 0
     * pro nulový overhead. Volá se PO BP dispatchi - BP má prioritu pro
     * UI breakpoint, callstack je observer. Pro reason RETI/RETN provede
     * pop top frame; ostatní reasony jsou no-op. MUSÍ být VOLÁNO i když
     * flags == 0, protože RETI (matrix flags=0) potřebuje pop top. */
    callstack_on_iff_change ( reason );
}


/**
 * @brief Event Viewer Vlna 1 Commit 5 - konzument z80 CPU control
 *        eventů, fan-out do eventlog kategorie @c EVENTLOG_CAT_CPU_CTRL.
 *
 * Volá se z z80 lib při HALT entry, HALT exit (= probuzení z HALT
 * při IRQ/NMI ack) a při dispatch RST opcode (RST 00..38).
 *
 * Hodnoty @c event jsou @c z80_cpu_ctrl_event_t cast na @c uint8_t a
 * MUSÍ být 1:1 s @c en_EVENTLOG_CPU_CTRL_SUB (= žádná translace,
 * jen přímý cast do subtype). Pojistka proti driftu je
 * regression test @c test_eventlog_cpu_ctrl_subtype_enum_values
 * (a paralelní test @c test_cpu_ctrl_event_enum_values v z80 lib).
 *
 * Hot-path: dvojitá gate kontrola (subsystém aktivní + per-kategorie
 * bit v active_mask). Při vypnutém eventlog nebo zamaskované kategorii
 * = no-op (= jen čtení dvou globálů).
 *
 * @param cpu        Z80 CPU instance (nepoužito, signalizace eventu).
 * @param event      Typ control eventu (cast @c z80_cpu_ctrl_event_t).
 * @param pc         CPU PC v okamžiku eventu (viz z80.h pro semantiku
 *                   per event type).
 * @param user_data  Nepoužito.
 */
void mzarch_cpu_ctrl_event_cb ( z80_t *cpu, uint8_t event, uint16_t pc,
                                void *user_data ) {
    (void) cpu;
    (void) user_data;

    /* Callstack fan-out (mutant callstack-experimental Fáze 1B). Gate
     * uvnitř callstack_on_cpu_ctrl_event filtruje g_callstack_active == 0
     * + skipuje HALT_ENTER/HALT_EXIT. Pro RST_00..RST_38 pushne entry.
     * Volá se PŘED eventlog dispatchem - oba subsystémy jsou nezávislé
     * konzumenti, pořadí nemá funkční dopad. */
    callstack_on_cpu_ctrl_event ( event, pc );

    if ( !TEST_TRACE_EVENTLOG_ACTIVE ) return;
    if ( !( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_CPU_CTRL ) ) ) return;

    /* event z z80 lib je přímo subtype v eventlog vrstvě (1:1 enum). */
    eventlog_record ( (uint8_t) EVENTLOG_CAT_CPU_CTRL,
                      event,
                      pc,
                      0u );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
