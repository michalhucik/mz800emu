/**
 * @file   mz800_bus_owners.c
 * @brief  Statická tabulka vlastníků portů a mapovaných adres MZ-800 (kontrakt v mz800_bus_owners.h).
 *
 * Řádky mají stejné podmínky překladu jako větve `case` v mz800_iorq.c.
 * Výstupní stupeň jen z doloženého zdroje (source_note, makra BUS_SRC_*
 * v bus_owners.h); kde v bázi mz800-knowledge není schéma ani datasheet
 * budiče (GDG, FDC, RAM disky, IDE8, Quick Disk), je DEVBUS_DRIVE_UNKNOWN.
 *
 * Zrcadla adres (mirror_note): emulátor dekóduje port jen dolním bajtem
 * adresy, tedy zrcadlí každý port přes všech 256 hodnot A8-A15 (switch
 * port_lsb v mz800_iorq.c). Báze mz800-knowledge zrcadla přes A8-A15 na
 * skutečném MZ-800 nedokládá ani nevyvrací (měření M5 plánu D6), proto je
 * poznámka u všech řádků stejná a říká jen to.
 *
 * @par Licence: GPLv3
 */

#include "mz800_bus_owners.h"

/* Podmínky překladu shodné s mz800_iorq.c; přímo mz800_config.h, aby šel
 * soubor přeložit i mimo build MZ-800 (STANDALONE test). */
#include "mz800_config.h"


/** @brief Zrcadla: model emulátoru, na HW neměřeno (společné všem řádkům). */
#define MIRROR_800 "emulator decodes the low address byte only (mirrored over A8-A15); not measured on real hardware"

/** @brief Zdroj: GDG registr 0CFh je 16bitový (registr volí horní bajt adresy). */
#define SRC_GDG_CF "GDG 16-bit registers: the high address byte selects SOF / SW / SSA / SEA / BCOL / CKSW at 0CF01h-0CF07h (hw/04-io-ports.md); write-only, custom LSI without a datasheet"
/** @brief Zdroj: mapper MZ-800 při IN 0E0h / 0E1h. */
#define SRC_MAPPER_IN "IN 0E0h / 0E1h selects VRAM / CG-ROM (hw/03-banking.md) and nothing drives the bus; the emulator returns the bus latch and marks the read unconnected"
/** @brief Zdroj: 8255 a 8253 na portech jen v režimu MZ-800. */
#define SRC_8255_MODE "NEC uPD8255A datasheet p. 8-70: three-state bidirectional data bus buffer; decoded on 0D0h-0D3h only with DMD in MZ-800 mode (hw/04-io-ports.md), in MZ-700 mode at 0E000h-0E003h"
/** @brief Zdroj: 8253 na portech jen v režimu MZ-800. */
#define SRC_8253_MODE "OKI MSM82C53-2 datasheet p. 7/19: D7-D0 three-state 8-bit bidirectional data bus (p. 6/19: high impedance outside /RD); decoded on 0D4h-0D7h only with DMD in MZ-800 mode"
/** @brief Zdroj: druhý PSG (volba HW kompatibility). */
#define SRC_PSG_STEREO "HW compatibility experiment allow_psg1 of the emulator (second SN76489 on 0F3h left and 0F9h right, 0F2h writes both); no second PSG on a real MZ-800 (hw/13-sound-psg.md)"
/** @brief Zdroj: standardní RAM disk na MZ-800 (zápis i 0E9h). */
#define SRC_RAMDISK_W "RAM disk MZ-1R18 (hypothesis, hw/22-ramdisk.md): 0EBh address, 0EAh data; the emulator also accepts a write to 0E9h; no schematic in the knowledge base, output stage not documented"
/** @brief Zdroj: Pezik na 0E8h sdílí porty se standardním RAM diskem. */
#define SRC_PEZIK_E8 "no schematic of the card in the knowledge base; output stage not documented. The emulator gives the Pezik priority over the standard RAM disk on 0E9h-0EBh"


/**
 * @brief Tabulka MZ-800 (porty podle mz800_iorq.c, MMIO podle mz800_memory.c).
 *
 * Pořadí: podle adresy. Čip s různým rozsahem nebo budičem pro čtení
 * a zápis má dva řádky (GDG: zápis 0CCh-0CFh / čtení stavu 0CEh; FDC:
 * data RW, ostatní jen W; mapper: IN 0E0h-0E1h dekódovaný bez budiče).
 */
static const st_BUS_STATIC_OWNER s_rows[] = {
    { 0x01, 0x02, 0, BUS_DIR_W,  "cmthack", "CMT hack (emulator trap)", "CH",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_CMTHACK, BUS_SRC_CMTHACK, MIRROR_800 },
    { 0x50, 0x51, 0, BUS_DIR_RW, "unicard", "Unicard", "U",
      DEVBUS_DRIVE_TRISTATE, 0, MZ800_BUS_PRESENT_UNICARD, BUS_SRC_UNICARD, MIRROR_800 },
#if CFG_HWEXT_HAVE_FDC
    { 0x58, 0x5b, 0, BUS_DIR_RW, "fdc1", "FDC 1 (WD279x data registers)", "F1",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_FDC1, BUS_SRC_FDC, MIRROR_800 },
    { 0x5c, 0x5f, 0, BUS_DIR_W,  "fdc1", "FDC 1 (drive select, motor, side)", "F1",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_FDC1, BUS_SRC_FDC, MIRROR_800 },
#endif
#if CFG_HWEXT_HAVE_RAMDISK
    { 0x68, 0x6f, 0, BUS_DIR_RW, "pezik68", "RAM disk Pezik (68h)", "PZ",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_PEZIK68, BUS_SRC_NO_SCHEMATIC, MIRROR_800 },
#endif
#if CFG_HWEXT_HAVE_IDE8
    { 0x78, 0x7f, 0, BUS_DIR_RW, "ide8", "IDE8 interface", "ID",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_IDE8, BUS_SRC_NO_SCHEMATIC, MIRROR_800 },
#endif
    { 0xcc, 0xce, 0, BUS_DIR_W,  "gdg", "GDG (WF, RF, DMD)", "G",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_ALWAYS, BUS_SRC_GDG_W, MIRROR_800 },
    { 0xce, 0xce, 0, BUS_DIR_R,  "gdg", "GDG Status (0CEh)", "G",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_ALWAYS, BUS_SRC_GDG_STATUS, MIRROR_800 },
    { 0xcf, 0xcf, 0, BUS_DIR_W,  "gdg", "GDG 16-bit registers (scroll, BCOL, CKSW)", "G",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_ALWAYS, SRC_GDG_CF,
      "the high address byte is the register number 01h-07h, not a mirror (hw/04-io-ports.md)" },
    { 0xd0, 0xd3, 0, BUS_DIR_RW, "ppi8255", "8255 PPI", "P",
      DEVBUS_DRIVE_TRISTATE, 0, MZ800_BUS_PRESENT_MODE800, SRC_8255_MODE, MIRROR_800 },
    { 0xd4, 0xd6, 0, BUS_DIR_RW, "ctc8253", "8253 CTC (counters)", "T",
      DEVBUS_DRIVE_TRISTATE, 0, MZ800_BUS_PRESENT_MODE800, SRC_8253_MODE, MIRROR_800 },
    { 0xd7, 0xd7, 0, BUS_DIR_W,  "ctc8253", "8253 CTC (control word)", "T",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_MODE800, BUS_SRC_8253_CW, MIRROR_800 },
#if CFG_HWEXT_HAVE_FDC
    { 0xd8, 0xdb, 0, BUS_DIR_RW, "fdc0", "FDC 0 (WD279x data registers)", "F0",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_FDC0, BUS_SRC_FDC, MIRROR_800 },
    { 0xdc, 0xdf, 0, BUS_DIR_W,  "fdc0", "FDC 0 (drive select, motor, side)", "F0",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_FDC0, BUS_SRC_FDC, MIRROR_800 },
#endif
    { 0xe0, 0xe1, 0, BUS_DIR_RW, "mapper", "Memory mapper (banking; IN selects VRAM / CG-ROM)", "M",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_ALWAYS, SRC_MAPPER_IN, MIRROR_800 },
    { 0xe2, 0xe6, 0, BUS_DIR_W,  "mapper", "Memory mapper (banking)", "M",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_ALWAYS, BUS_SRC_MAPPER, MIRROR_800 },
    { 0xe7, 0xe7, 0, BUS_DIR_W,  "memext", "Memory extension mapper", "ME",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_MEMEXT, BUS_SRC_MEMEXT,
      "the bank number is the high address nibble A12-A15 (hw/23-memext.md), not a mirror" },
#if CFG_HWEXT_HAVE_RAMDISK
    { 0xe8, 0xef, 0, BUS_DIR_RW, "pezik_e8", "RAM disk Pezik (0E8h)", "PE",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_PEZIK_E8, SRC_PEZIK_E8, MIRROR_800 },
    { 0xe9, 0xeb, 0, BUS_DIR_W,  "ramdisk", "RAM disk (standard, address / data)", "RD",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_RAMDISK_STD, SRC_RAMDISK_W, MIRROR_800 },
    { 0xea, 0xea, 0, BUS_DIR_R,  "ramdisk", "RAM disk (standard, data)", "RD",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, MIRROR_800 },
#endif
    { 0xf0, 0xf0, 0, BUS_DIR_W,  "gdg", "GDG palette (0F0h)", "G",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_ALWAYS, BUS_SRC_GDG_W, MIRROR_800 },
#if HAVE_JOY
    { 0xf0, 0xf0, 0, BUS_DIR_R,  "joy0", "Joystick 1 (JOY0, 0F0h)", "J0",
      DEVBUS_DRIVE_PARTIAL, 0x3f, MZ800_BUS_PRESENT_ALWAYS, BUS_SRC_JOY, MIRROR_800 },
    { 0xf1, 0xf1, 0, BUS_DIR_R,  "joy1", "Joystick 2 (JOY1, 0F1h)", "J1",
      DEVBUS_DRIVE_PARTIAL, 0x3f, MZ800_BUS_PRESENT_ALWAYS, BUS_SRC_JOY, MIRROR_800 },
#else
    { 0xf0, 0xf1, 0, BUS_DIR_R,  "joy0", "Joystick ports (no joystick in this build)", "J0",
      DEVBUS_DRIVE_PARTIAL, 0x3f, MZ800_BUS_PRESENT_ALWAYS, BUS_SRC_JOY, MIRROR_800 },
#endif
    { 0xf2, 0xf2, 0, BUS_DIR_W,  "psg", "PSG SN76489 (0F2h)", "S",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_ALWAYS, BUS_SRC_PSG, MIRROR_800 },
    { 0xf3, 0xf3, 0, BUS_DIR_W,  "psg_stereo", "Second PSG (HW compatibility: 0F3h left, 0F9h right)", "S2",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_PSG_STEREO, SRC_PSG_STEREO, MIRROR_800 },
#if CFG_HWEXT_HAVE_QDISK
    { 0xf4, 0xf7, 0, BUS_DIR_RW, "qdisk", "Quick Disk interface", "Q",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_QDISK, BUS_SRC_QDISK, MIRROR_800 },
#endif
#if CFG_HWEXT_HAVE_RAMDISK
    { 0xf8, 0xf9, 0, BUS_DIR_R,  "ramdisk", "RAM disk (standard, reset / read)", "RD",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, MIRROR_800 },
#endif
    { 0xf9, 0xf9, 0, BUS_DIR_W,  "psg_stereo", "Second PSG (HW compatibility: 0F3h left, 0F9h right)", "S2",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_PSG_STEREO, SRC_PSG_STEREO, MIRROR_800 },
#if CFG_HWEXT_HAVE_RAMDISK
    { 0xfa, 0xfa, 0, BUS_DIR_W,  "ramdisk", "RAM disk (standard, control)", "RD",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, MIRROR_800 },
#endif
#if HAVE_PIOZ80
    { 0xfc, 0xff, 0, BUS_DIR_RW, "pioz80", "Z80 PIO (printer)", "Z",
      DEVBUS_DRIVE_TRISTATE, 0, MZ800_BUS_PRESENT_ALWAYS, BUS_SRC_PIOZ80, MIRROR_800 },
#endif
    /* Mapované porty 0E000h-0E008h, jen s DMD v režimu MZ-700 a horní ROM
     * mapovanou (mz800_memory.c); v režimu MZ-800 oblast čte 0FFh. */
    { 0xE000, 0xE003, 1, BUS_DIR_RW, "ppi8255", "8255 PPI", "P",
      DEVBUS_DRIVE_TRISTATE, 0, MZ800_BUS_PRESENT_MODE700, BUS_SRC_8255, NULL },
    { 0xE004, 0xE006, 1, BUS_DIR_RW, "ctc8253", "8253 CTC (counters)", "T",
      DEVBUS_DRIVE_TRISTATE, 0, MZ800_BUS_PRESENT_MODE700, BUS_SRC_8253, NULL },
    { 0xE007, 0xE007, 1, BUS_DIR_W,  "ctc8253", "8253 CTC (control word)", "T",
      DEVBUS_DRIVE_NONE, 0, MZ800_BUS_PRESENT_MODE700, BUS_SRC_8253_CW, NULL },
    { 0xE008, 0xE008, 1, BUS_DIR_RW, "gdg", "GDG Status / GATE0 (0E008h)", "G",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ800_BUS_PRESENT_MODE700, BUS_SRC_GDG_STATUS, NULL },
};


const st_BUS_STATIC_OWNER *mz800_bus_owners(unsigned *count)
{
    *count = (unsigned) (sizeof(s_rows) / sizeof(s_rows[0]));
    return s_rows;
}


bool mz800_bus_port_is_static(uint8_t port_lsb)
{
    st_BUS_STATIC_TABLE t;
    t.rows = s_rows;
    t.count = (unsigned) (sizeof(s_rows) / sizeof(s_rows[0]));
    t.present = NULL;
    return bus_owners_port_is_static(&t, port_lsb);
}
