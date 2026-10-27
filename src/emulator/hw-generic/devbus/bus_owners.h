/**
 * @file   bus_owners.h
 * @brief  Statická tabulka vlastníků adres sběrnice (porty IORQ, MMIO) - arch-neutrální typ.
 *
 * Každá platforma popíše čipy a periferie základní desky, které obsluhuje
 * její statický switch (`*_iorq.c`, mapované porty 0E000h+), jako pole
 * řádků st_BUS_STATIC_OWNER. Tabulka je jediná pravda pro:
 *
 * - validaci portů zařízení devbus (st_DEVBUS_ARCH_OPS::port_is_static),
 * - okno Bus Map a MCP get_bus_map (vlastník, směr, výstupní stupeň,
 *   zdroj deklarace).
 *
 * Výstupní stupeň (drive) se plní jen z doloženého zdroje (datasheet,
 * schéma, VHDL); kde doklad chybí, je DEVBUS_DRIVE_UNKNOWN a source_note
 * říká, co chybí. Přítomnost zařízení (odpojený FDC, Unicard ...) se
 * nerozhoduje v tabulce: řádek nese jen druh (present_kind), který
 * vyhodnotí lepidlo platformy (bus_present_fn). Tabulka sama nezávisí
 * na emulátoru a jde přeložit STANDALONE.
 *
 * Tabulky: mz700_bus_owners.c, mz800_bus_owners.c, mz1500_bus_owners.c.
 * Poznámky o zdroji společné více platformám jsou makra BUS_SRC_* níže,
 * aby tentýž čip měl na všech platformách stejný text.
 *
 * @par Licence: GPLv3
 */

#ifndef BUS_OWNERS_H
#define BUS_OWNERS_H

#include <stdint.h>
#include <stdbool.h>

#include "devbus_resolve.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Směr: vlastník obsluhuje čtení (IN / čtení MMIO). */
#define BUS_DIR_R 1u
/** @brief Směr: vlastník obsluhuje zápis (OUT / zápis MMIO). */
#define BUS_DIR_W 2u
/** @brief Oba směry. */
#define BUS_DIR_RW (BUS_DIR_R | BUS_DIR_W)

/** @brief Druh přítomnosti: vlastník je na desce vždy (bus_present_fn se nevolá). */
#define BUS_PRESENT_ALWAYS 0u

/*
 * Poznámky o zdroji deklarace budiče (source_note) společné platformám.
 * Anglicky (UI je zobrazuje beze změny). Nový text jen s odkazem na
 * datasheet, schéma, VHDL nebo kód emulátoru; nikdy odhad.
 */
/** @brief Zdroj: datasheet NEC uPD8255A. */
#define BUS_SRC_8255 "NEC uPD8255A datasheet p. 8-70: three-state bidirectional data bus buffer"
/** @brief Zdroj: datasheet OKI MSM82C53 (jen blokové schéma). */
#define BUS_SRC_8253 "OKI MSM82C53 datasheet: data bus buffer in the block diagram; an explicit three-state statement was not found"
/** @brief Zdroj: řídicí slovo 8253 nelze číst. */
#define BUS_SRC_8253_CW "8253 control word register is write-only (datasheet); the emulator returns the bus latch on read"
/** @brief Zdroj: Unicard VHDL. */
#define BUS_SRC_UNICARD "Unicard VHDL (hw-study/unicard/06-hw-svn-fakta.md): 74LVX245 bus path in Hi-Z unless /RD"
/** @brief Zdroj: bez schématu v bázi. */
#define BUS_SRC_NO_SCHEMATIC "no schematic of the card in the knowledge base; output stage not documented"
/** @brief Zdroj: FDC. */
#define BUS_SRC_FDC "MB8876A has an inverted data bus (hw/16-floppy.md); the bus buffer of the MZ-1E05 card is not documented"
/** @brief Zdroj: Quick Disk interface. */
#define BUS_SRC_QDISK "MZ-1E19 parts list names an SN74LS244 buffer (QuickDisk_kompendium.md), direction on the data bus unclear"
/** @brief Zdroj: mapper (jen zápis). */
#define BUS_SRC_MAPPER "write-only memory mapper (hw/03-banking.md); the emulator returns the bus latch on IN"
/** @brief Zdroj: MemExt. */
#define BUS_SRC_MEMEXT "write-only mapper of the memory extension (hw/23-memext.md)"
/** @brief Zdroj: CMT hack. */
#define BUS_SRC_CMTHACK "software trap of the emulator (CMT hack); no hardware on the real machine"
/** @brief Zdroj: Z80 PIO (technický manuál Zilog). */
#define BUS_SRC_PIOZ80 "Zilog Z80 PIO Technical Manual (datasheet set manuals/chip/src/pio-z80/z80piomn.pdf): D7-D0 Z80-CPU data bus bidirectional, tri-state"
/** @brief Zdroj: PSG SN76489 (jen vstupní datová sběrnice). */
#define BUS_SRC_PSG "SN76489AN datasheet (SN76489_techinfo.md): D0-D7 is an input data bus; the chip never drives the bus (hw/13-sound-psg.md: write-only)"
/** @brief Zdroj: GDG registry jen pro zápis. */
#define BUS_SRC_GDG_W "GDG registers are write-only except Status (hw/04-io-ports.md); custom LSI without a datasheet"
/** @brief Zdroj: stav GDG (čtení). */
#define BUS_SRC_GDG_STATUS "GDG Status register (hw/04-io-ports.md); custom LSI without a datasheet, DT0-DT7 wired to pins 20-27 (REVIEW_gdg_custom_lsi.md), output stage not documented"
/** @brief Zdroj: joystick přes 74LS365. */
#define BUS_SRC_JOY "74LS365 buffer drives DT0-DT5 only (service manual p. 35, sheet 44; hw/17-joystick.md); bits 6-7 float on hardware, the emulator returns 0FFh without a joystick"

/**
 * @brief Jeden řádek statické tabulky vlastníků.
 *
 * @invariant first <= last; u mmio == 0 jsou first, last <= 0FFh (dolní
 *            bajt adresy portu); dirs != 0; id odpovídá [a-z][a-z0-9_]*
 *            a je společné všem řádkům téhož čipu; abbrev má 1-2 znaky;
 *            drive != DEVBUS_DRIVE_NONE jen s BUS_DIR_R (řádek se čtením
 *            a DEVBUS_DRIVE_NONE = čip čtení dekóduje, ale sběrnici nebudí:
 *            mapper MZ-800 při IN 0E0h / 0E1h); source_note není NULL
 *            (i u UNKNOWN říká, co chybí).
 * @note Vlastnictví: statická data platformy (žijí do konce procesu).
 */
typedef struct st_BUS_STATIC_OWNER {
    uint16_t first;           /**< První adresa (včetně). */
    uint16_t last;            /**< Poslední adresa (včetně). */
    uint8_t  mmio;            /**< 0 = port IORQ (dolní bajt), 1 = MMIO adresa. */
    uint8_t  dirs;            /**< BUS_DIR_R | BUS_DIR_W. */
    const char *id;           /**< Stabilní id vlastníka pro MCP ("unicard", "fdc0", "ppi8255"). */
    const char *name;         /**< Anglický název pro UI (msgid; UI překládá). */
    const char *abbrev;       /**< 1-2 znaky do buňky mřížky ("U", "F0"). */
    en_DEVBUS_DRIVE drive;    /**< Výstupní stupeň při čtení (UNKNOWN = v bázi nedoloženo). */
    uint8_t  drive_mask;      /**< Jen PARTIAL: buzené bity. */
    unsigned present_kind;    /**< BUS_PRESENT_ALWAYS, nebo druh pro bus_present_fn platformy. */
    const char *source_note;  /**< Anglicky: zdroj deklarace budiče, nebo co chybí. */
    const char *mirror_note;  /**< Anglicky: zrcadla adresy podle báze; NULL = neověřeno. */
} st_BUS_STATIC_OWNER;

/**
 * @brief Je vlastník druhu @p present_kind právě přítomný (připojený, osazený)?
 *
 * Dodává lepidlo platformy (čte globální stav periferií v emu vlákně nebo
 * pod zámkem stavu zařízení). Pro BUS_PRESENT_ALWAYS se nevolá.
 */
typedef bool (*bus_present_fn)(unsigned present_kind);

/**
 * @brief Statická tabulka platformy předávaná vrstvě bus_map.
 * @invariant rows != NULL při count > 0; present smí být NULL, pak jsou
 *            všechny řádky přítomné.
 */
typedef struct st_BUS_STATIC_TABLE {
    const st_BUS_STATIC_OWNER *rows; /**< Řádky. */
    unsigned count;                  /**< Počet řádků. */
    bus_present_fn present;          /**< Vyhodnocení přítomnosti. */
} st_BUS_STATIC_TABLE;

/**
 * @brief Obsluhuje některý řádek tabulky port @p port_lsb (v libovolném směru)?
 * @param t        Tabulka.
 * @param port_lsb Dolní bajt adresy portu.
 * @return true = port patří statickému switchi platformy.
 */
bool bus_owners_port_is_static(const st_BUS_STATIC_TABLE *t, uint8_t port_lsb);

#ifdef __cplusplus
}
#endif

#endif /* BUS_OWNERS_H */
