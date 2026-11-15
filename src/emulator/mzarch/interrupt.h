#ifndef MZARCH_INTERRUPT_H
#define MZARCH_INTERRUPT_H

#ifdef __cplusplus
extern "C"
{
#endif

#define MZARCH_INTERRUPT_CTC2 (1 << 0)
#define MZARCH_INTERRUPT_PIOZ80 (1 << 1)
#define MZARCH_INTERRUPT_FDC (1 << 2)

/**
 * Předstih žádosti o přerušení před koncem instrukce, v ticích pixel clocku.
 * Z80 vzorkuje /INT náběžnou hranou hodin posledního taktu instrukce, takže
 * žádost, která přijde těsně před koncem instrukce, se přijme až po další.
 * Změřeno na MZ-800 (emu-experiments/ctc-meas, test P, 2026-11-15, procesor
 * v HALT, IM 1): první zápis obsluhy přichází 81,5 až 84,7 T po náběžné hraně
 * OUT2 (pět hodnot po 0,8 T), cesta od přijetí je 79,5 T. Žádost na pinu
 * OUT2 tedy musí předcházet konec instrukce o víc než 1,2 T (6 tiků)
 * a stačí 2,0 T (10 tiků); 8 tiků je střed. Platí pro všechny zdroje
 * přerušení (pro jiné než CTC2 neměřeno [neověřeno]).
 */
#define MZARCH_INTERRUPT_SETUP_TICKS 8

/**
 * Zpoždění náběžné hrany OUT2 za sestupnou hranou CLK1 (HSYNC), v ticích
 * pixel clocku. Změřeno na MZ-800 (ctc-meas, záznam CTC-2-R): OUT1 se mění
 * 125 až 145 ns po sestupné hraně CLK1 a OUT2 dalších 145 až 150 ns po
 * sestupné hraně OUT1, celkem 285 až 290 ns = 5 tiků. Emulátor obě hrany
 * zpracuje v čase hrany CLK1; zpoždění se započte jen do času žádosti
 * o přerušení.
 */
#define MZARCH_INTERRUPT_CTC2_DELAY_TICKS 5

#include "libs/cpu-z80/z80.h"

    extern void mzarch_interrupt_manager(void);
    extern void mzarch_ei_cb(z80_t *cpu, void *user_data);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* D.3 - HW event BP wrapper callbacks pro z80 lib events. */
    extern void mzarch_di_cb(z80_t *cpu, void *user_data);
    extern void mzarch_im_cb(z80_t *cpu, uint8_t new_im, void *user_data);
    extern void mzarch_halt_cb(z80_t *cpu, void *user_data);
    extern void mzarch_nmi_cb(z80_t *cpu, void *user_data);
    /* V1.7+ 1.5 - unified IFF1/IFF2 change callback (všech 7 reasonů). */
    extern void mzarch_iff_change_cb(z80_t *cpu, uint8_t new_iff1,
                                     uint8_t new_iff2, uint8_t reason,
                                     void *user_data);
    /**
     * @brief Konzument @c z80_cpu_ctrl_event_cb - emit do eventlog
     *        kategorie @c EVENTLOG_CAT_CPU_CTRL.
     *
     * Volá se z z80 lib při HALT entry (cpu->halted 0->1), HALT exit
     * (1->0 po IRQ/NMI ack) a při dispatch každého RST opcode (00..38).
     * Funkce castuje @c event (= @c z80_cpu_ctrl_event_t) na
     * @c en_EVENTLOG_CPU_CTRL_SUB (1:1 mapping, chráněno regression
     * testem @c test_eventlog_cpu_ctrl_subtype_enum_values) a zapíše
     * 24 B event do ringu pokud je kategorie @c CPU_CTRL aktivní.
     *
     * Payload = 0 (= veškerá informace je v subtype + PC). RST adresa
     * je dohledatelná ze subtype.
     *
     * Hot path: NULL gate check + active mask test. V OFF stavu
     * ~3 instrukce overhead.
     *
     * Registrace v @c mzarch_main_init() přes
     * @c z80_set_cpu_ctrl_event(). Nezávislé na existujícím
     * @c mzarch_halt_cb (= oba se volají paralelně při HALT entry).
     *
     * @param cpu        z80 instance (nepoužito v body, BC s callback typedef).
     * @param event      Typ control eventu (@c z80_cpu_ctrl_event_t cast).
     * @param pc         PC v okamžiku eventu (= adresa za RST/HALT instrukci).
     * @param user_data  Nepoužito.
     *
     * @threadsafety Volat jen z emu vlákna (= z80 lib hot path).
     */
    extern void mzarch_cpu_ctrl_event_cb(z80_t *cpu, uint8_t event,
                                         uint16_t pc, void *user_data);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MZARCH_INTERRUPT_H */
