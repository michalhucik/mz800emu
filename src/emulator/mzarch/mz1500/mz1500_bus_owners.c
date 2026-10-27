/**
 * @file   mz1500_bus_owners.c
 * @brief  Statická tabulka vlastníků portů a mapovaných adres MZ-1500 (kontrakt v mz1500_bus_owners.h).
 *
 * Řádky mají stejné podmínky překladu jako větve `case` v mz1500_iorq.c.
 * Výstupní stupeň jen z doloženého zdroje (source_note, makra BUS_SRC_*
 * v bus_owners.h); kde v bázi mz800-knowledge není schéma ani datasheet
 * budiče, je DEVBUS_DRIVE_UNKNOWN. Báze popisuje MZ-1500 jen okrajově
 * (00-platform-comparison.md, hw/13-sound-psg.md, hw/15-quick-disk.md);
 * u registrů GDG a PSG je zdrojem model emulátoru a poznámka to říká.
 *
 * Zrcadla adres (mirror_note): emulátor dekóduje port jen dolním bajtem
 * adresy (switch port_lsb v mz1500_iorq.c); báze zrcadla na skutečném
 * MZ-1500 nedokládá, poznámka u všech řádků říká jen to.
 *
 * @par Licence: GPLv3
 */

#include "mz1500_bus_owners.h"

/* Podmínky překladu shodné s mz1500_iorq.c; přímo mz1500_config.h, aby šel
 * soubor přeložit i mimo build MZ-1500 (STANDALONE test). */
#include "mz1500_config.h"


/** @brief Zrcadla: model emulátoru, na HW neměřeno (společné všem řádkům). */
#define MIRROR_1500 "emulator decodes the low address byte only (mirrored over A8-A15); not measured on real hardware"

/** @brief Zdroj: dva PSG MZ-1500 (porty podle modelu emulátoru). */
#define SRC_PSG_1500 "two SN76489AN of the MZ-1500 (hw/13-sound-psg.md); the port assignment 0E9h both, 0F2h left, 0F3h right is the emulator model (mz1500_iorq.c), the data bus of the chip is input only (SN76489_techinfo.md)"
/** @brief Zdroj: registry GDG MZ-1500 na 0F0h / 0F1h. */
#define SRC_GDG_1500 "emulator model (mz1500_iorq.c, gdg_write_byte): 0F0h display mode, 0F1h palette, OUT only; the knowledge base has no MZ-1500 GDG register description"
/** @brief Zdroj: Quick Disk na MZ-1500 (rozpor s bází). */
#define SRC_QDISK_1500 "MZ-1E19 parts list names an SN74LS244 buffer (QuickDisk_kompendium.md), direction on the data bus unclear. The emulator decodes 0F4h-0F7h as on the MZ-800; hw/15-quick-disk.md says the built-in QD of the MZ-1500 uses 74h-77h (not verified, see plan D6 report)"
/** @brief Zdroj: stav GDG 0E008h na MZ-1500. */
#define SRC_E008_1500 "emulator model (gdg_read_dmd_status_memop, mz1500_memory.c): status read / GATE0 write at 0E008h; the driving chip of the MZ-1500 is not documented in the knowledge base"


/**
 * @brief Tabulka MZ-1500 (porty podle mz1500_iorq.c, MMIO podle mz1500_memory.c).
 *
 * Pořadí: podle adresy. Čip s různým rozsahem pro čtení a zápis má dva
 * řádky (FDC: data RW, ostatní jen W; RAM disk: 0EAh RW, 0EBh W, 0F8h-0F9h
 * R, 0FAh W). Mapované porty 0E000h-0E008h platí vždy, když je horní ROM
 * mapovaná se SPEC = 0 (mz1500_memory.c), nezávisle na režimu DMD.
 */
static const st_BUS_STATIC_OWNER s_rows[] = {
    { 0x01, 0x02, 0, BUS_DIR_W,  "cmthack", "CMT hack (emulator trap)", "CH",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_CMTHACK, BUS_SRC_CMTHACK, MIRROR_1500 },
    { 0x50, 0x51, 0, BUS_DIR_RW, "unicard", "Unicard", "U",
      DEVBUS_DRIVE_TRISTATE, 0, MZ1500_BUS_PRESENT_UNICARD, BUS_SRC_UNICARD, MIRROR_1500 },
#if CFG_HWEXT_HAVE_FDC
    { 0x58, 0x5b, 0, BUS_DIR_RW, "fdc1", "FDC 1 (WD279x data registers)", "F1",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ1500_BUS_PRESENT_FDC1, BUS_SRC_FDC, MIRROR_1500 },
    { 0x5c, 0x5f, 0, BUS_DIR_W,  "fdc1", "FDC 1 (drive select, motor, side)", "F1",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_FDC1, BUS_SRC_FDC, MIRROR_1500 },
#endif
#if CFG_HWEXT_HAVE_RAMDISK
    { 0x68, 0x6f, 0, BUS_DIR_RW, "pezik68", "RAM disk Pezik (68h)", "PZ",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ1500_BUS_PRESENT_PEZIK68, BUS_SRC_NO_SCHEMATIC, MIRROR_1500 },
#endif
#if CFG_HWEXT_HAVE_IDE8
    { 0x78, 0x7f, 0, BUS_DIR_RW, "ide8", "IDE8 interface", "ID",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ1500_BUS_PRESENT_IDE8, BUS_SRC_NO_SCHEMATIC, MIRROR_1500 },
#endif
#if CFG_HWEXT_HAVE_FDC
    { 0xd8, 0xdb, 0, BUS_DIR_RW, "fdc0", "FDC 0 (WD279x data registers)", "F0",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ1500_BUS_PRESENT_FDC0, BUS_SRC_FDC, MIRROR_1500 },
    { 0xdc, 0xdf, 0, BUS_DIR_W,  "fdc0", "FDC 0 (drive select, motor, side)", "F0",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_FDC0, BUS_SRC_FDC, MIRROR_1500 },
#endif
    { 0xe0, 0xe6, 0, BUS_DIR_W,  "mapper", "Memory mapper (banking)", "M",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_ALWAYS, BUS_SRC_MAPPER, MIRROR_1500 },
    { 0xe7, 0xe7, 0, BUS_DIR_W,  "memext", "Memory extension mapper", "ME",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_MEMEXT, BUS_SRC_MEMEXT,
      "the bank number is the high address nibble A12-A15 (hw/23-memext.md), not a mirror" },
    { 0xe9, 0xe9, 0, BUS_DIR_W,  "psg", "PSG SN76489 x 2 (0E9h both, 0F2h left, 0F3h right)", "S",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_ALWAYS, SRC_PSG_1500, MIRROR_1500 },
#if CFG_HWEXT_HAVE_RAMDISK
    { 0xea, 0xea, 0, BUS_DIR_RW, "ramdisk", "RAM disk (standard, data)", "RD",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ1500_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, MIRROR_1500 },
    { 0xeb, 0xeb, 0, BUS_DIR_W,  "ramdisk", "RAM disk (standard, address)", "RD",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, MIRROR_1500 },
#endif
    { 0xf0, 0xf1, 0, BUS_DIR_W,  "gdg", "Display mode / palette (GDG)", "G",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_ALWAYS, SRC_GDG_1500, MIRROR_1500 },
    { 0xf2, 0xf3, 0, BUS_DIR_W,  "psg", "PSG SN76489 x 2 (0E9h both, 0F2h left, 0F3h right)", "S",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_ALWAYS, SRC_PSG_1500, MIRROR_1500 },
#if CFG_HWEXT_HAVE_QDISK
    { 0xf4, 0xf7, 0, BUS_DIR_RW, "qdisk", "Quick Disk interface", "Q",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ1500_BUS_PRESENT_QDISK, SRC_QDISK_1500, MIRROR_1500 },
#endif
#if CFG_HWEXT_HAVE_RAMDISK
    { 0xf8, 0xf9, 0, BUS_DIR_R,  "ramdisk", "RAM disk (standard, reset / read)", "RD",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ1500_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, MIRROR_1500 },
    { 0xfa, 0xfa, 0, BUS_DIR_W,  "ramdisk", "RAM disk (standard, control)", "RD",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, MIRROR_1500 },
#endif
#if HAVE_PIOZ80
    { 0xfc, 0xff, 0, BUS_DIR_RW, "pioz80", "Z80 PIO (printer)", "Z",
      DEVBUS_DRIVE_TRISTATE, 0, MZ1500_BUS_PRESENT_ALWAYS, BUS_SRC_PIOZ80, MIRROR_1500 },
#endif
    /* Mapované porty 0E000h-0E008h (mz1500_memory.c, horní ROM se SPEC = 0). */
    { 0xE000, 0xE003, 1, BUS_DIR_RW, "ppi8255", "8255 PPI", "P",
      DEVBUS_DRIVE_TRISTATE, 0, MZ1500_BUS_PRESENT_ALWAYS, BUS_SRC_8255, NULL },
    { 0xE004, 0xE006, 1, BUS_DIR_RW, "ctc8253", "8253 CTC (counters)", "T",
      DEVBUS_DRIVE_TRISTATE, 0, MZ1500_BUS_PRESENT_ALWAYS, BUS_SRC_8253, NULL },
    { 0xE007, 0xE007, 1, BUS_DIR_W,  "ctc8253", "8253 CTC (control word)", "T",
      DEVBUS_DRIVE_NONE, 0, MZ1500_BUS_PRESENT_ALWAYS, BUS_SRC_8253_CW, NULL },
    { 0xE008, 0xE008, 1, BUS_DIR_RW, "gdg", "GDG Status / GATE0 (0E008h)", "G",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ1500_BUS_PRESENT_ALWAYS, SRC_E008_1500, NULL },
};


const st_BUS_STATIC_OWNER *mz1500_bus_owners(unsigned *count)
{
    *count = (unsigned) (sizeof(s_rows) / sizeof(s_rows[0]));
    return s_rows;
}


bool mz1500_bus_port_is_static(uint8_t port_lsb)
{
    st_BUS_STATIC_TABLE t;
    t.rows = s_rows;
    t.count = (unsigned) (sizeof(s_rows) / sizeof(s_rows[0]));
    t.present = NULL;
    return bus_owners_port_is_static(&t, port_lsb);
}
