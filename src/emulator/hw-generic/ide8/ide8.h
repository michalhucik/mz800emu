/*
 * File:   ide8.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 13. července 2018, 20:37
 *
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef IDE8_H
#define IDE8_H

#include <stdint.h>

#include "mzarch/mzarch.h"

typedef enum en_IDE8_ADDR
{
    IDE8_ADDR_DATA = 0,

    IDE8_ADDR_ERROR = 1,    // read
    IDE8_ADDR_FEATURES = 1, // write

    IDE8_ADDR_SCOUNT = 2, // sector count
    IDE8_ADDR_SECTOR,     // sector number
    IDE8_ADDR_LCYL,
    IDE8_ADDR_HCYL,
    IDE8_ADDR_HEAD, // drive/head register

    IDE8_ADDR_STATUS = 7, // read
    IDE8_ADDR_COMMAND = 7 // write
} en_IDE8_ADDR;

typedef enum en_IDE8_STATE
{
    IDE8_STATE_DISCONNECTED = 0,
    IDE8_STATE_CONNECTED = 1
} en_IDE8_STATE;

typedef enum en_IDE8_DRIVE
{
    IDE8_DRIVE_MASTER = 0,
    IDE8_DRIVE_SLAVE = 1,
    IDE8_DRIVES_COUNT
} en_IDE8_DRIVE;

typedef enum en_IDE8_CMD
{
    IDE8_CMD_NONE = 0x00,
    IDE8_CMD_RESET = 0x10,
    IDE8_CMD_SECTOR_READ = 0x20,
    IDE8_CMD_SECTOR_WRITE = 0x30,
    IDE8_CMD_SET_FEATURES = 0xef,
} en_IDE8_CMD;

typedef enum en_IDE8_ADDRESSING
{
    IDE8_ADDRESSING_CHS = 0,
    IDE8_ADDRESSING_LBA = 1
} en_IDE8_ADDRESSING;

typedef enum en_IDE8_FEATURE
{
    IDE8_FEATURE_8BITON = 0x01,
    IDE8_FEATURE_8BITOFF = 0x81,
} en_IDE8_FEATURE;

typedef enum en_IDE8_BUSMODE
{
    IDE8_BUSMODE_16 = 0,
    IDE8_BUSMODE_8 = 1,
} en_IDE8_BUSMODE;

#define IDE8_STS_BUSY (0x01 << 7)
#define IDE8_STS_READY (0x01 << 6)
#define IDE8_STS_WRERROR (0x01 << 5)
#define IDE8_STS_SEEKOK (0x01 << 4)
#define IDE8_STS_DRQ (0x01 << 3)
#define IDE8_STS_CORRECTED (0x01 << 2)
#define IDE8_STS_INDEX (0x01 << 1)
#define IDE8_STS_ERROR (0x01 << 0)

#define IDE8_SECTOR_SIZE 512

/* max velikost img je 500 MB */
#define IDE8_DEFAULT_NIPOS_GEO_C 1000
#define IDE8_DEFAULT_NIPOS_GEO_H 16
#define IDE8_DEFAULT_NIPOS_GEO_S 64
#define IDE8_MAX_IMG_BLOCKS (IDE8_DEFAULT_NIPOS_GEO_C * IDE8_DEFAULT_NIPOS_GEO_H * IDE8_DEFAULT_NIPOS_GEO_S)

/* Default filenames pro IDE8 disky jsou per-arch ("hdd0-mz800.img", ...)
 * - aby MZ-800/MZ-700/MZ-1500 měl každý vlastní výchozí HDD obrazy a
 * neházely se přes sebe v cfg_dir. Od mzhal kroku 7 se skládají RUNTIME
 * z g_mzhal.arch_name v ide8_cfg registraci (ide8.c), compile-time
 * makra zrušena.
 *
 * Když config vrátí relativní cestu, ide8_drive_open_image() ji resolvuje
 * proti cfg_dir. Absolutní cesty (uživatelem vybrané ve file dialogu) se
 * použijí přímo. */

typedef struct st_IDE8_DRIVE
{
    en_IDE8_STATE connected;
    char *filepath;
    en_IDE8_DRIVE drive_id;
    int geo_c;
    int geo_h;
    int geo_s;
    FILE *fp;
    uint32_t total_blocks;
    uint8_t cache[IDE8_SECTOR_SIZE];
    int data_pos;
    int sector_count;
    en_IDE8_CMD cmd;
    en_IDE8_ADDRESSING addressing;
    en_IDE8_BUSMODE busmode;
    uint8_t status;
    uint32_t block;
} st_IDE8_DRIVE;

typedef struct st_IDE8
{
    st_IDE8_DRIVE drive[IDE8_DRIVES_COUNT];
    en_IDE8_DRIVE selected;
    uint8_t regSECTOR_COUNT;
    uint8_t regSECTOR;
    uint16_t regCYLINDER;
    uint8_t regHEAD;
    uint8_t regFEATURES;
} st_IDE8;

extern st_IDE8 g_ide8;

#define IDE8_TEST_MASTER_CONNECTED (g_ide8.drive[IDE8_DRIVE_MASTER].connected == IDE8_STATE_CONNECTED)
#define IDE8_TEST_SLAVE_CONNECTED (g_ide8.drive[IDE8_DRIVE_SLAVE].connected == IDE8_STATE_CONNECTED)
#define IDE8_TEST_CONNECTED (IDE8_TEST_MASTER_CONNECTED || IDE8_TEST_SLAVE_CONNECTED)

#define IDE8_HEAD_BUSMODEMASK (0x01 << 6)
#define IDE8_HEAD_MSMASK (0x01 << 4)

#define ide8_drive_get_filepath(drive_id) (g_ide8.drive[drive_id].filepath)
#define ide8_drive_get_connected(drive_id) (g_ide8.drive[drive_id].connected)

#ifdef __cplusplus
extern "C"
{
#endif

    void ide8_init(void);
    void ide8_exit(void);
    void ide8_reset(void);

    uint8_t ide8_read_byte(en_IDE8_ADDR addr);
    void ide8_write_byte(en_IDE8_ADDR addr, uint8_t value);

    void ide8_drive_set_connected(en_IDE8_DRIVE drive_id, en_IDE8_STATE connected);
    void ide8_drive_close_image(st_IDE8_DRIVE *drive);
    int ide8_drive_open_image(st_IDE8_DRIVE *drive, char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* IDE8_H */
