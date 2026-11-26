/**
 * @file   intlog.h
 * @brief  Interrupt Log - INT pin transitions + IM/IFF/RETI events.
 *
 * Subsystém trace-suite. Loguje:
 *  1. rising/falling na CPU INT pinu - zdroj (CTC2, PIOZ80 obecně,
 *     PIOZ80@P[A,B][0..7] - 16 pinů rezervováno)
 *  2. změny CPU Z80 interrupt stavu (IM, IFF1, IFF2, RETI)
 *  3. stavy PIO-Z80 (připravený, alarmovaný, IM2-jump, RETI-aplikován)
 *  4. IM 2 IRQ acknowledge (vector_table_addr + isr_addr) - kritické
 *     pro debug ISR dispatchu
 *
 * Per-event záznam (24 B):
 *
 * | Offset | Velikost | Pole                                  |
 * |--------|----------|----------------------------------------|
 * | 0      | 8 B      | pxclk_total                            |
 * | 8      | 4 B      | screens_total                          |
 * | 12     | 4 B      | pxclk_in_screen                        |
 * | 16     | 1 B      | event_class (PIN_EDGE/CPU_INT_STATE/PIO_STATE/IRQ_ACK_IM2) |
 * | 17     | 1 B      | source_chip (CTC2, PIOZ80, ...)        |
 * | 18     | 1 B      | source_pin (0-15 pro PIOZ80@P[A,B][0..7]) |
 * | 19     | 1 B      | edge (RISING/FALLING/N/A)              |
 * | 20     | 4 B      | state_bitmask (IM/IFF1/IFF2/RETI/PIO bits  |
 * |        |          | nebo (vector_table_addr | isr_addr<<16) pro IRQ_ACK_IM2) |
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#ifndef INTLOG_H
#define INTLOG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "tlog_common.h"

typedef enum en_INTLOG_EVENT_CLASS
{
    INTLOG_EVENT_PIN_EDGE = 0,        /**< Změna na CPU INT pinu */
    INTLOG_EVENT_CPU_INT_STATE,       /**< Změna IM/IFF1/IFF2/RETI */
    INTLOG_EVENT_PIO_STATE,           /**< Změna PIOZ80 internal state */
    INTLOG_EVENT_IRQ_ACK_IM2,         /**< IM 2 IRQ acknowledge (vector + ISR addr) */
} en_INTLOG_EVENT_CLASS;

typedef enum en_INTLOG_SOURCE_CHIP
{
    INTLOG_CHIP_NONE = 0,
    INTLOG_CHIP_CTC2 = 1,             /**< CTC2 + PIO@PC2 maska */
    INTLOG_CHIP_PIOZ80 = 2,           /**< PIOZ80 obecně (ne specific pin) */
    INTLOG_CHIP_PIOZ80_PA0 = 3,       /**< PIOZ80 Port A bit 0 */
    INTLOG_CHIP_PIOZ80_PA1 = 4,
    INTLOG_CHIP_PIOZ80_PA2 = 5,
    INTLOG_CHIP_PIOZ80_PA3 = 6,
    INTLOG_CHIP_PIOZ80_PA4 = 7,       /**< CTC0 vstup (invertovaný) */
    INTLOG_CHIP_PIOZ80_PA5 = 8,       /**< VBLN signál */
    INTLOG_CHIP_PIOZ80_PA6 = 9,
    INTLOG_CHIP_PIOZ80_PA7 = 10,
    INTLOG_CHIP_PIOZ80_PB0 = 11,
    INTLOG_CHIP_PIOZ80_PB1 = 12,
    INTLOG_CHIP_PIOZ80_PB2 = 13,
    INTLOG_CHIP_PIOZ80_PB3 = 14,
    INTLOG_CHIP_PIOZ80_PB4 = 15,
    INTLOG_CHIP_PIOZ80_PB5 = 16,
    INTLOG_CHIP_PIOZ80_PB6 = 17,
    INTLOG_CHIP_PIOZ80_PB7 = 18,
    INTLOG_CHIP_FDC = 19,
    INTLOG_CHIP_CPU = 20,             /**< Pro CPU_INT_STATE eventy */
    INTLOG_CHIP_PIOZ80_PORT_A = 21,   /**< PIOZ80 Port A jako celek (pro IRQ ack source) */
    INTLOG_CHIP_PIOZ80_PORT_B = 22,   /**< PIOZ80 Port B jako celek (pro IRQ ack source) */
    INTLOG_CHIP_VECTOR_BUS_LATCH = 23, /**< IM 2 vector source = bus latch / žádný chip
                                            nedodal vector (CPU vzal záznam (I:duch sběrnice)).
                                            Typicky když IRQ patří chipu bez intread_cb. */
} en_INTLOG_SOURCE_CHIP;

typedef enum en_INTLOG_EDGE
{
    INTLOG_EDGE_NONE = 0,
    INTLOG_EDGE_RISING,
    INTLOG_EDGE_FALLING,
} en_INTLOG_EDGE;

/* Bity v state_bitmask pro CPU_INT_STATE event class */
#define INTLOG_STATE_BIT_IM0       (1u << 0)
#define INTLOG_STATE_BIT_IM1       (1u << 1)
#define INTLOG_STATE_BIT_IM2       (1u << 2)
#define INTLOG_STATE_BIT_IFF1      (1u << 3)
#define INTLOG_STATE_BIT_IFF2      (1u << 4)
#define INTLOG_STATE_BIT_RETI      (1u << 5)  /**< Edge: RETI právě vykonán */
#define INTLOG_STATE_BIT_EI        (1u << 6)  /**< Edge: EI právě vykonán */
#define INTLOG_STATE_BIT_DI        (1u << 7)  /**< Edge: DI právě vykonán */
/* Bity pro PIO_STATE event class */
#define INTLOG_STATE_BIT_PIO_READY        (1u << 8)
#define INTLOG_STATE_BIT_PIO_ARMED        (1u << 9)
#define INTLOG_STATE_BIT_PIO_IM2_JUMP     (1u << 10)  /**< IM 2 IRQ ack proběhl, ISR jump nastavený */
#define INTLOG_STATE_BIT_PIO_RETI_APPLIED (1u << 11)  /**< RETI dispatched - PIOZ80 vrácen z RECEIVED do NONE */

typedef struct st_INTLOG_CONFIG
{
    en_TLOG_MODE mode;
    char *dir;
    char *name;
    unsigned chunk_mb;
    unsigned max_total_mb;
    unsigned save_on_exit;
} st_INTLOG_CONFIG;

extern st_INTLOG_CONFIG g_intlog_config;
extern int g_intlog_active;

/* Forward decl pro fan-out gate - viz analogická konstrukce v hwlog.h. */
extern int g_eventlog_active;

/**
 * @brief Gate pro čistě intlog disk recording.
 *
 * Pravdivá pouze pokud běží trace-suite intlog disk persistence
 * (= @c tlog_writer_append() do souboru). Použít na call-sites, které
 * testují vlastní intlog tlog writer stav. NEPOUŽÍVAT jako gate před
 * voláním @c intlog_record_* z chip driverů - tam patří
 * @ref TEST_TRACE_INTLOG_DISPATCH.
 */
#define TEST_TRACE_INTLOG_ACTIVE (g_intlog_active)

/**
 * @brief Gate pro DISPATCH volání @c intlog_record_*() z chip driverů.
 *
 * Pravdivá pokud alespoň jeden ze sinků konzumuje intlog event:
 *
 *  - @c g_intlog_active   = trace-suite intlog disk persistence
 *  - @c g_eventlog_active = in-memory ring pro Event Viewer UI
 *
 * Sémantika: "má smysl vůbec volat @c intlog_record_*(), protože
 * alespoň jeden sink jej využije". Vnitřek @c intlog_record_* /
 * @c intlog_poll_interrupt_state pak nezávisle routuje na příslušné
 * sinky.
 *
 * Toto je makro, které patří na všechna call-sites v chip driverech /
 * @c mzarch.c / @c interrupt.c, která chystají payload a volají
 * @c intlog_record_pin_edge / @c intlog_record_cpu_int_state /
 * @c intlog_record_pio_state / @c intlog_record_irq_ack_im2.
 */
#define TEST_TRACE_INTLOG_DISPATCH (g_intlog_active || g_eventlog_active)

void intlog_init ( void );
void intlog_apply_cli_options ( void );
void intlog_recompute_active ( int debugger_active );
int  intlog_start ( void );
void intlog_stop ( void );
void intlog_finalize ( void );

/**
 * @brief Uzavřít (uložit) aktuální segment, volitelně přesměrovat na @p path.
 *
 * Runtime finalizace streamovaného intlog segmentu (analogie
 * @ref cputrack_save_segment). Při zadaném @p path přesměruje @c dir/name na
 * cestu pro NÁSLEDNÝ segment; již zapsané chunk soubory nepřejmenovává.
 *
 * @param path  Cílová cesta dalšího segmentu, nebo NULL (jen flush+restart).
 * @return 0 OK, -1 chyba.
 */
int intlog_save_segment ( const char *path );

/**
 * @brief Test, zda byl recording zastaven kvůli max_total_mb limitu.
 *
 * Public wrapper nad @ref tlog_writer_is_truncated() pro interní
 * @c s_writer instanci tohoto subsystému. Použití typ. v UI status
 * indicators (např. titulek SDL okna).
 *
 * @return 1 = truncated (recording stopnut), 0 = stále aktivní recording
 *         nebo neaktivní subsystém.
 */
int intlog_is_truncated ( void );

/**
 * @brief Hook pro pin edge event.
 */
void intlog_record_pin_edge ( en_INTLOG_SOURCE_CHIP src, uint8_t pin,
                              en_INTLOG_EDGE edge );

/**
 * @brief Hook pro CPU interrupt state change.
 *
 * Volat z mzarch_ei_cb / pioz80_reti_cb / míst kde se mění IM (LD A,I/R,
 * IM 0/1/2 instr - hooky v Z80 emulátoru, viz cpu-z80/z80.h).
 */
void intlog_record_cpu_int_state ( uint32_t state_bits );

/**
 * @brief Hook pro PIOZ80 state change.
 */
void intlog_record_pio_state ( uint32_t state_bits );

/**
 * @brief Hook pro IM 2 IRQ acknowledgement event.
 *
 * Volat z @ref pioz80_interrupt_ack_im2_cb po určení vector + ISR adresy.
 * Reuse existujícího 24 B layoutu - source_chip identifikuje port (PA / PB
 * jako celek), state_bitmask kóduje 32-bit value:
 *  - bity 0..15  = vector_table_addr (entry adresa v IM 2 tabulce)
 *  - bity 16..31 = isr_addr (16-bit hodnota přečtená z paměti, nahrávaná do PC)
 *
 * Tj. encoded = (uint32_t) vector_table_addr | ((uint32_t) isr_addr << 16).
 *
 * @param src                 source_chip (PIOZ80_PORT_A / PIOZ80_PORT_B / CTC2 / ...)
 * @param vector_table_addr   Adresa entry v IM 2 tabulce v paměti
 *                            = (cpu->I << 8) | vector (bit 0 se nemaskuje)
 * @param isr_addr            ISR adresa přečtená z paměti na adrese
 *                            vector_table_addr (16-bit LE), tj. hodnota
 *                            nahrávaná do PC.
 */
void intlog_record_irq_ack_im2 ( en_INTLOG_SOURCE_CHIP src,
                                 uint16_t vector_table_addr,
                                 uint16_t isr_addr );

/**
 * @brief Polling hook - zavolat z interrupt_manager() po update.
 *
 * Detekuje rising/falling edges na pinech porovnáním současného stavu
 * `g_mzarch_main.interrupt` proti uloženému `s_prev_interrupt`. Emit
 * pin_edge event pro každou změnu.
 */
void intlog_poll_interrupt_state ( uint8_t current_interrupt );

/**
 * @brief Writer kanálu intlog pro čtení proudu záznamů (trace_read).
 *
 * Vrací writer i po stopu (popis posledního uzavřeného segmentu), viz
 * tlog_writer_read_snapshot() a devdoc/trace/stream-read.md.
 *
 * @return Ukazatel na statický writer modulu (nikdy NULL).
 * @pre Snímek writeru pořizovat jen z emu vlákna; tlog_writer_segment()
 *      z libovolného vlákna.
 */
const st_TLOG_WRITER *intlog_get_writer ( void );

#ifdef __cplusplus
}
#endif

#endif /* INTLOG_H */
