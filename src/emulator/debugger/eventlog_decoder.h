/**
 * @file   eventlog_decoder.h
 * @brief  Rich Detail decoder pro Event Viewer (Vlna 3 Commit 21).
 *
 * Formátuje per-kategorie human-readable text z @ref st_EVENTLOG_EVENT
 * pro UI "Detail" sloupec v Events okně (Log tab) a hover tooltip ve
 * Strip tabu. Decoder je pure C, žádné GUI dependencies - aby mohl být
 * unit-tested bez ImGui linkování.
 *
 * Závisí na:
 *  - @c debugger/trace/eventlog.h (st_EVENTLOG_EVENT, kategorie enumy)
 *  - @c debugger/trace/hwlog.h    (per-chip sub-type enumy)
 *  - @c debugger/trace/intlog.h   (CPU_INT bitmask, PIN_EDGE source/edge)
 *  - @c debugger/io_catalog.h     (port name lookup, jen IORQ kategorie)
 *  - @c debugger/trace/marklog.h  (marker name lookup, USER_MARK)
 *
 * Decoder NEPŘISTUPUJE k živému emu state (@c g_gdg, @c g_psg, ...) -
 * pracuje výhradně s informacemi zakódovanými v 24 B eventu (= ringové
 * záznamy jsou self-contained pro účely detailu).
 *
 * @section format Formát výstupu per kategorie
 *
 * | Kategorie         | Příklad výstupu                                    |
 * |-------------------|----------------------------------------------------|
 * | CPU_INT           | @c IM=2 IFF1=1 IFF2=1 RETI                         |
 * | CPU_PIN_EDGE      | @c PIOZ80@PA4 rising                               |
 * | IRQ_ACK_IM2       | @c PIOZ80_PA vec=0x40 isr=0x4042                   |
 * | IORQ_IN/OUT       | @c port=0xCE (GDG DMD) val=0x08                    |
 * | MMIO_R / MMIO_W   | @c addr=0xE003 val=0x80                            |
 * | GDG_MODE          | @c DMD=0x02 (320x200x16)                           |
 * | GDG_BANKING       | @c port 0xE0 (ROM bottom OFF)                      |
 * | GDG_HWSCROLL      | @c SOF=0x4000 / @c WID=80 / ...                    |
 * | GDG_COLORS        | @c BORDER=0xE0 / @c PAL[2]=0x9F                    |
 * | GDG_VIDEO         | @c VBLN start (= jen subtype label)                |
 * | GDG_WFRF          | @c WF=0x08 / @c RF=0xFF                            |
 * | PIO8255           | @c Port A=0x42 / @c CW=0x80 (mode 0)               |
 * | CTC8253           | @c CW: counter 2 mode 3 BIN / @c CTC1=0x12         |
 * | PIOZ80            | @c A MODE 2 (bidir) / @c A ICW 0xB7 ...            |
 * | PSG               | @c 0x80 (latch ch A period lo)                     |
 * | FDC               | @c reg=0 (CMD)=0x80 (Restore)                      |
 * | MEMEXT            | @c page=4 bank=0x12 (Luftner)                      |
 * | QD                | @c reg=2 (ctrl A)=0x18                             |
 * | RD                | @c port=0xFA val=0x42                              |
 * | BP_FIRE           | @c BP #5 reason=2 MARK                             |
 * | USER_MARK         | @c "isr_entry"                                     |
 * | CPU_CTRL          | @c HALT enter / @c RST 0x38                        |
 *
 * @author Michal Hucik <hucik@ordoz.com>
 *
 * Licence: GPLv3
 */

#ifndef EVENTLOG_DECODER_H
#define EVENTLOG_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "debugger/trace/eventlog.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Zformátuje rich Detail text pro daný event.
 *
 * Dispatch dle @c e->category na per-kategorie decoder helper. Pro neznámé
 * / nedekódované kategorie vrátí fallback @c "payload=0x..." formát.
 *
 * Funkce je side-effect free a thread-safe (nepřistupuje k mutable
 * globálům emu state). Lookup @c io_catalog g_io_ports[] je read-only
 * statické pole a tedy thread-safe. @c marklog_get_name() vrací pointer
 * na vlastněný string z internal registry - lifetime do dalšího
 * @c marklog_register / @c marklog_clear (= mimo render frame stable).
 *
 * @param e        Pointer na event (vlastněný ringem).
 * @param buf      Output buffer.
 * @param buf_len  Velikost bufferu (doporučeno 128 B).
 *
 * @pre @c e != NULL, @c buf != NULL, @c buf_len > 0
 * @post @c buf obsahuje NUL-terminated string délky < @c buf_len.
 */
void eventlog_decode_detail ( const st_EVENTLOG_EVENT *e,
                              char *buf, size_t buf_len );

/**
 * @brief Krátký subtype label per (kategorie, subtype) - Sub sloupec
 *        Log tabu / Strip tooltip (Vlna 3 Commit 22).
 *
 * Vrací kompaktní zkratku (max 8 viditelných znaků) pokrývající všechny
 * kategorie z @ref en_EVENTLOG_CATEGORY. Pro kategorie bez smysluplného
 * subtype (např. @c GDG_MODE, @c MMIO_R/W, @c GDG_WFRF) vrací @c "-".
 * Pro neznámé hodnoty vrací decimální zápis subtype.
 *
 * Funkce je pure C, side-effect free, thread-safe. UI vrstva volá v
 * každé řádce Log tabu - žádný heap, jen snprintf do callerova bufferu.
 *
 * @param category  @ref en_EVENTLOG_CATEGORY hodnota.
 * @param subtype   Per-kategorie subtype hodnota.
 * @param buf       Output buffer (doporučeno >=16 B).
 * @param buf_len   Velikost bufferu (musí být > 0).
 *
 * @pre @c buf != NULL, @c buf_len > 0
 * @post @c buf obsahuje NUL-terminated string délky < @c buf_len.
 */
void eventlog_decode_subtype_short ( uint8_t category, uint8_t subtype,
                                      char *buf, size_t buf_len );

/**
 * @brief Plný popis (kategorie, subtype) pro hover tooltip nad Sub
 *        buňkou (Vlna 3 Commit 22).
 *
 * Doplnění k @ref eventlog_decode_subtype_short(). Vrací čitelný popis
 * (typicky 10-40 znaků) pro tooltip nad Sub sloupcem Log tabu a v
 * Strip tooltipu. Pro neznámou kombinaci vrací @c "short (subtype N)"
 * fallback.
 *
 * Pure C, side-effect free, thread-safe.
 *
 * @param category  @ref en_EVENTLOG_CATEGORY hodnota.
 * @param subtype   Per-kategorie subtype hodnota.
 * @param buf       Output buffer (doporučeno >=64 B).
 * @param buf_len   Velikost bufferu (musí být > 0).
 *
 * @pre @c buf != NULL, @c buf_len > 0
 * @post @c buf obsahuje NUL-terminated string délky < @c buf_len.
 */
void eventlog_decode_subtype_full ( uint8_t category, uint8_t subtype,
                                     char *buf, size_t buf_len );

/**
 * @brief Formátuje ambient state bity do human-readable textu (Vlna 4
 *        Commit 24).
 *
 * Vstupem je hodnota @c st_EVENTLOG_EVENT::ambient (16 bitů bit-packed
 * snapshot HW state z okamžiku emit). Výstupní formát kompaktní pro
 * tooltip nad Log řádkem / Strip event tooltipem:
 *
 * @code
 * "IFF1=1 IM=2 reason=NMI_ACK bank=DEFAULT"
 * @endcode
 *
 * Reason field se vynechá, pokud je @c EVENTLOG_AMBIENT_REASON_NONE
 * (= default mimo BP fire stack). Banking summary se interpretuje
 * per-arch (= MZ-800 / MZ-700 / MZ-1500 každá vlastní string).
 *
 * Funkce je pure C, side-effect free, thread-safe.
 *
 * @param ambient   Hodnota @c st_EVENTLOG_EVENT::ambient (16 bitů).
 * @param buf       Output buffer (doporučeno >=48 B).
 * @param buf_len   Velikost bufferu (musí být > 0).
 *
 * @pre @c buf != NULL, @c buf_len > 0
 * @post @c buf obsahuje NUL-terminated string délky < @c buf_len.
 */
void eventlog_decode_ambient ( uint16_t ambient, char *buf, size_t buf_len );

#ifdef __cplusplus
}
#endif

#endif /* EVENTLOG_DECODER_H */
