/**
 * @file   mz700_bus_owners.c
 * @brief  Statická tabulka vlastníků portů a mapovaných adres MZ-700 (kontrakt v mz700_bus_owners.h).
 *
 * Řádky mají stejné podmínky překladu jako větve `case` v mz700_iorq.c.
 * Výstupní stupeň jen z doloženého zdroje (source_note); kde v bázi
 * mz800-knowledge není schéma ani datasheet budiče, je DEVBUS_DRIVE_UNKNOWN.
 * Zrcadla adres (mirror_note) báze pro MZ-700 nedokládá, proto NULL.
 *
 * @par Licence: GPLv3
 */

#include "mz700_bus_owners.h"

/* Podmínky překladu shodné s mz700_iorq.c; přímo mz700_config.h, aby šel
 * soubor přeložit i mimo build MZ-700 (STANDALONE test). */
#include "mz700_config.h"


/* Zdroje společné platformám jsou BUS_SRC_* v bus_owners.h; zde jen MZ-700. */
/** @brief Zdroj: GDG registry MZ-700 (0F0h, 0F1h). */
#define SRC_GDG_W_700 "emulator model (mz700_iorq.c, gdg_write_byte): OUT only; the decode on a real MZ-700 is not documented in the knowledge base"
/** @brief Zdroj: stav 0E008h na MZ-700. */
#define SRC_E008_R_700 "emulator model (gdg_read_dmd_status_memop): HBLNK, TEMPO, joystick bits; the driving chip on a real MZ-700 is not documented"


/**
 * @brief Tabulka MZ-700 (porty podle mz700_iorq.c, MMIO podle mz700_memory.c).
 *
 * Pořadí: podle adresy. Čip s různým rozsahem pro čtení a zápis má dva
 * řádky (FDC: data 58h-5Bh / 0D8h-0DBh RW, ostatní jen W).
 */
static const st_BUS_STATIC_OWNER s_rows[] = {
    { 0x01, 0x02, 0, BUS_DIR_W,  "cmthack", "CMT hack (emulator trap)", "CH",
      DEVBUS_DRIVE_NONE, 0, MZ700_BUS_PRESENT_CMTHACK, BUS_SRC_CMTHACK, NULL },
    { 0x50, 0x51, 0, BUS_DIR_RW, "unicard", "Unicard", "U",
      DEVBUS_DRIVE_TRISTATE, 0, MZ700_BUS_PRESENT_UNICARD, BUS_SRC_UNICARD, NULL },
#if CFG_HWEXT_HAVE_FDC
    { 0x58, 0x5b, 0, BUS_DIR_RW, "fdc1", "FDC 1 (WD279x data registers)", "F1",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ700_BUS_PRESENT_FDC1, BUS_SRC_FDC, NULL },
    { 0x5c, 0x5f, 0, BUS_DIR_W,  "fdc1", "FDC 1 (drive select, motor, side)", "F1",
      DEVBUS_DRIVE_NONE, 0, MZ700_BUS_PRESENT_FDC1, BUS_SRC_FDC, NULL },
#endif
#if CFG_HWEXT_HAVE_RAMDISK
    { 0x68, 0x6f, 0, BUS_DIR_RW, "pezik68", "RAM disk Pezik (68h)", "PZ",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ700_BUS_PRESENT_PEZIK68, BUS_SRC_NO_SCHEMATIC, NULL },
#endif
#if CFG_HWEXT_HAVE_IDE8
    { 0x78, 0x7f, 0, BUS_DIR_RW, "ide8", "IDE8 interface", "ID",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ700_BUS_PRESENT_IDE8, BUS_SRC_NO_SCHEMATIC, NULL },
#endif
#if CFG_HWEXT_HAVE_FDC
    { 0xd8, 0xdb, 0, BUS_DIR_RW, "fdc0", "FDC 0 (WD279x data registers)", "F0",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ700_BUS_PRESENT_FDC0, BUS_SRC_FDC, NULL },
    { 0xdc, 0xdf, 0, BUS_DIR_W,  "fdc0", "FDC 0 (drive select, motor, side)", "F0",
      DEVBUS_DRIVE_NONE, 0, MZ700_BUS_PRESENT_FDC0, BUS_SRC_FDC, NULL },
#endif
    { 0xe0, 0xe6, 0, BUS_DIR_W,  "mapper", "Memory mapper (banking)", "M",
      DEVBUS_DRIVE_NONE, 0, MZ700_BUS_PRESENT_ALWAYS, BUS_SRC_MAPPER, NULL },
    { 0xe7, 0xe7, 0, BUS_DIR_W,  "memext", "Memory extension mapper", "ME",
      DEVBUS_DRIVE_NONE, 0, MZ700_BUS_PRESENT_MEMEXT, BUS_SRC_MEMEXT, NULL },
#if CFG_HWEXT_HAVE_RAMDISK
    { 0xea, 0xea, 0, BUS_DIR_RW, "ramdisk", "RAM disk (standard, data)", "RD",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ700_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, NULL },
    { 0xeb, 0xeb, 0, BUS_DIR_W,  "ramdisk", "RAM disk (standard, address)", "RD",
      DEVBUS_DRIVE_NONE, 0, MZ700_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, NULL },
#endif
    { 0xf0, 0xf1, 0, BUS_DIR_W,  "gdg", "Display mode / palette (GDG)", "G",
      DEVBUS_DRIVE_NONE, 0, MZ700_BUS_PRESENT_ALWAYS, SRC_GDG_W_700, NULL },
#if CFG_HWEXT_HAVE_QDISK
    { 0xf4, 0xf7, 0, BUS_DIR_RW, "qdisk", "Quick Disk interface", "Q",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ700_BUS_PRESENT_QDISK, BUS_SRC_QDISK, NULL },
#endif
#if CFG_HWEXT_HAVE_RAMDISK
    { 0xf8, 0xf9, 0, BUS_DIR_R,  "ramdisk", "RAM disk (standard, reset / read)", "RD",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ700_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, NULL },
    { 0xfa, 0xfa, 0, BUS_DIR_W,  "ramdisk", "RAM disk (standard, control)", "RD",
      DEVBUS_DRIVE_NONE, 0, MZ700_BUS_PRESENT_RAMDISK_STD, BUS_SRC_NO_SCHEMATIC, NULL },
#endif
#if HAVE_PIOZ80
    { 0xfc, 0xff, 0, BUS_DIR_RW, "pioz80", "Z80 PIO", "Z",
      DEVBUS_DRIVE_TRISTATE, 0, MZ700_BUS_PRESENT_PIOZ80, BUS_SRC_PIOZ80, NULL },
#endif
    /* Mapované porty 0E000h-0E008h (mz700_memory.c). */
    { 0xE000, 0xE003, 1, BUS_DIR_RW, "ppi8255", "8255 PPI", "P",
      DEVBUS_DRIVE_TRISTATE, 0, MZ700_BUS_PRESENT_ALWAYS, BUS_SRC_8255, NULL },
    { 0xE004, 0xE006, 1, BUS_DIR_RW, "ctc8253", "8253 CTC (counters)", "T",
      DEVBUS_DRIVE_TRISTATE, 0, MZ700_BUS_PRESENT_ALWAYS, BUS_SRC_8253, NULL },
    { 0xE007, 0xE007, 1, BUS_DIR_W,  "ctc8253", "8253 CTC (control word)", "T",
      DEVBUS_DRIVE_NONE, 0, MZ700_BUS_PRESENT_ALWAYS, BUS_SRC_8253_CW, NULL },
    { 0xE008, 0xE008, 1, BUS_DIR_RW, "gdg", "Status / TEMPO (0E008h)", "G",
      DEVBUS_DRIVE_UNKNOWN, 0, MZ700_BUS_PRESENT_ALWAYS, SRC_E008_R_700, NULL },
};


const st_BUS_STATIC_OWNER *mz700_bus_owners(unsigned *count)
{
    *count = (unsigned) (sizeof(s_rows) / sizeof(s_rows[0]));
    return s_rows;
}


bool mz700_bus_port_is_static(uint8_t port_lsb)
{
    st_BUS_STATIC_TABLE t;
    t.rows = s_rows;
    t.count = (unsigned) (sizeof(s_rows) / sizeof(s_rows[0]));
    t.present = NULL;
    return bus_owners_port_is_static(&t, port_lsb);
}
