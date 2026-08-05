/*
 * File:   ide8.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 13. července 2018, 20:36
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

// http://www.elektronika.kvalitne.cz/ATMEL/necoteorie/IDEtesty/rizeni_CF_HDD.html

/*
 *
 * Podporovane funkce:
 *
 *  RESET
 *  SECTOR_READ
 *  SECTOR_WRITE
 *  SET_FEATURES
 *      - busmode 16 / 8
 *
 *  Podle toho jak se chova NIPOS a ZA-emulator to vypada, ze zapis do registru
 *  nerozlisuje to, zda byl drive vybran, ci nikoliv.
 *  Nyni jsou tedy pri zapisu vsechny registry spolecne a selection se zohlednuje
 *  jen pri zapisu do COMMAND a pro adddressing CHS/LBA (regHEAD).
 *
 *  NIPOS - https://www.ordoz.com/mz800emu/files/nipos/
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <glib.h>
#include "mzarch/mzhal.h"

#include "fs_layer.h"
#include "cfgmain.h"
#include "ide8.h"
#include "baseui/baseui.h"

st_IDE8 g_ide8;

void ide8_drive_close_image(st_IDE8_DRIVE *drive)
{
    if (!drive->connected)
        return;
    FS_LAYER_FCLOSE(drive->fp);
    drive->connected = IDE8_STATE_DISCONNECTED;
}

void ide8_exit(void)
{
    int i;
    for (i = 0; i < IDE8_DRIVES_COUNT; i++)
    {
        if (g_ide8.drive[i].connected)
        {
            ide8_drive_close_image(&g_ide8.drive[i]);
            g_free(g_ide8.drive[i].filepath);
        };
    };
}

static void ide8_drive_reset(st_IDE8_DRIVE *drive)
{
    if (drive->connected)
    {
        drive->status = IDE8_STS_READY | IDE8_STS_SEEKOK;
        drive->addressing = IDE8_ADDRESSING_CHS;
        drive->busmode = IDE8_BUSMODE_16;
    }
    else
    {
        drive->status = IDE8_STS_BUSY;
    };
}

void ide8_reset(void)
{
    int i;
    for (i = 0; i < IDE8_DRIVES_COUNT; i++)
    {
        ide8_drive_reset(&g_ide8.drive[i]);
    };
    g_ide8.selected = IDE8_DRIVE_MASTER;
    g_ide8.regSECTOR_COUNT = 1;
    g_ide8.regSECTOR = 1;
    g_ide8.regCYLINDER = 0;
    g_ide8.regHEAD = 0;
    g_ide8.regFEATURES = IDE8_FEATURE_8BITOFF; // jen berlicka
    return;
}

static inline uint32_t ide8_drive_get_total_blocks(st_IDE8_DRIVE *drive)
{
    FS_LAYER_FSEEK_END(drive->fp, 0);
    uint32_t size = FS_LAYER_FTELL(drive->fp);
    return (size / IDE8_SECTOR_SIZE);
}

/**
 * Na 0. bloku si NIPOS ulozi geometrii (busmode je v 16 bitovem rezimu):
 *
 * 'M', 0, 'Z', 0, cc, 0, cc, 0, hh, 0, 0, 0, ss, 0, 0, 0
 *
 */
static inline int ide8_check_nipos_geometry(st_IDE8_DRIVE *drive, uint8_t *buff)
{

    uint8_t zeros = buff[1] | buff[3] | buff[5] | buff[7];
    zeros |= buff[9] | buff[10] | buff[11];
    zeros |= buff[13] | buff[14] | buff[15];

    if ((buff[0] == 'M') && (buff[2] == 'Z') && (zeros == 0x00))
    {
        uint16_t c = buff[4] | (buff[6] << 8);
        uint8_t h = buff[8];
        uint8_t s = buff[12];

        if ((c) && (h) && (s) && (h <= 0x10))
        {
            drive->geo_c = c;
            drive->geo_h = h;
            drive->geo_s = s;
            return EXIT_SUCCESS;
        };
    };
    return EXIT_FAILURE;
}

int ide8_drive_open_image(st_IDE8_DRIVE *drive, char *filepath)
{
    if (drive->connected)
    {
        ide8_drive_close_image(drive);
    };

    /* Relativní filepath resolveme proti cfg_dir (typicky default
     * "hdd0-{mzarch}.img" nebo "hdd1-{mzarch}.img"). Absolutní cesty
     * (z file dialogu) pass through beze změny. */
    char *resolved = sdlapp_paths_resolve_cfg(g_sdlapp->paths, filepath);

    g_print("IDE8 HDD%d - Open image '%s'\n", drive->drive_id, resolved);

    if (baseui_tools_file_access(resolved, F_OK) != -1)
    {
        FS_LAYER_FOPEN(drive->fp, resolved, FS_LAYER_FMODE_RW);
    }
    else
    {
        FS_LAYER_FOPEN(drive->fp, resolved, FS_LAYER_FMODE_W);
    };
    if (!drive->fp)
    {
        char *filepath_locale = baseui_tools_file_name_locale_from_utf8(resolved);
        fprintf(stderr, "%s():%d - Can't open file '%s'\n", __func__, __LINE__, filepath_locale);
        g_free(filepath_locale);
        g_free(resolved);
        return EXIT_FAILURE;
    };
    g_free(resolved);
    drive->total_blocks = ide8_drive_get_total_blocks(drive);
    g_print("IDE8 HDD%d - total blocks assigned in image: %d\n", drive->drive_id, drive->total_blocks);

    int nipos_geo_found = EXIT_FAILURE;
    if (drive->total_blocks)
    {
        uint32_t readlen = 0;
        uint32_t offset = 0;
        FS_LAYER_FSEEK(drive->fp, offset);
        if (FS_LAYER_FR_OK != FS_LAYER_FREAD(drive->fp, drive->cache, IDE8_SECTOR_SIZE, &readlen))
        {
            fprintf(stderr, "%s():%d - Read error on offset '%d'\n", __func__, __LINE__, offset);
            FS_LAYER_FCLOSE(drive->fp);
            return EXIT_FAILURE;
        };
        nipos_geo_found = ide8_check_nipos_geometry(drive, drive->cache);
    };

    if (nipos_geo_found == EXIT_SUCCESS)
    {
        g_print("\tNIPOS HDD geometry found\n");
    }
    else
    {
        drive->geo_c = IDE8_DEFAULT_NIPOS_GEO_C;
        drive->geo_h = IDE8_DEFAULT_NIPOS_GEO_H;
        drive->geo_s = IDE8_DEFAULT_NIPOS_GEO_S;
        g_print("\tNIPOS HDD geometry NOT found, use default\n");
    };
    g_print("\tc: %d, h: %d, s: %d = blocks: %d\n", drive->geo_c, drive->geo_h, drive->geo_s, (drive->geo_c * drive->geo_h * drive->geo_s));

    drive->connected = IDE8_STATE_CONNECTED;
    return EXIT_SUCCESS;
}

void ide8_init(void)
{

    int i;
    for (i = 0; i < IDE8_DRIVES_COUNT; i++)
    {
        st_IDE8_DRIVE *drive = &g_ide8.drive[i];
        drive->drive_id = i;
        drive->connected = IDE8_STATE_DISCONNECTED;
        drive->filepath = g_new0(char, 1);
    };
    g_ide8.selected = IDE8_DRIVE_MASTER;

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "IDE8");
    CFGELM *elm;

    CFGELM *elm_conn_hdd0 = cfgmodule_register_new_element(cmod, "hdd0_connected", CFGENTYPE_BOOL, IDE8_STATE_DISCONNECTED);
    cfgelement_set_handlers(elm_conn_hdd0, NULL, (void *)&g_ide8.drive[IDE8_DRIVE_MASTER].connected);

    /* Defaultní jména obrazů runtime z g_mzhal (mzhal krok 7) -
     * identická s dřívějšími compile-time "hdd<N>-" MZARCH_NAME ".img". */
    static char default_hdd0[32];
    static char default_hdd1[32];
    snprintf(default_hdd0, sizeof(default_hdd0), "hdd0-%s.img", g_mzhal.arch_name);
    snprintf(default_hdd1, sizeof(default_hdd1), "hdd1-%s.img", g_mzhal.arch_name);

    elm = cfgmodule_register_new_element(cmod, "hdd0_filepath", CFGENTYPE_TEXT, default_hdd0);
    cfgelement_set_pointers(elm, (void *)&g_ide8.drive[IDE8_DRIVE_MASTER].filepath, (void *)&g_ide8.drive[IDE8_DRIVE_MASTER].filepath);

    CFGELM *elm_conn_hdd1 = cfgmodule_register_new_element(cmod, "hdd1_connected", CFGENTYPE_BOOL, IDE8_STATE_DISCONNECTED);
    cfgelement_set_handlers(elm_conn_hdd1, NULL, (void *)&g_ide8.drive[IDE8_DRIVE_SLAVE].connected);

    elm = cfgmodule_register_new_element(cmod, "hdd1_filepath", CFGENTYPE_TEXT, default_hdd1);
    cfgelement_set_pointers(elm, (void *)&g_ide8.drive[IDE8_DRIVE_SLAVE].filepath, (void *)&g_ide8.drive[IDE8_DRIVE_SLAVE].filepath);

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);

    if (cfgelement_get_bool_value(elm_conn_hdd0))
    {
        st_IDE8_DRIVE *drive = &g_ide8.drive[IDE8_DRIVE_MASTER];
        ide8_drive_open_image(drive, drive->filepath);
    };

    if (cfgelement_get_bool_value(elm_conn_hdd1))
    {
        st_IDE8_DRIVE *drive = &g_ide8.drive[IDE8_DRIVE_SLAVE];
        ide8_drive_open_image(drive, drive->filepath);
    };

    ide8_reset();
}

void ide8_drive_set_connected(en_IDE8_DRIVE drive_id, en_IDE8_STATE connected)
{
    st_IDE8_DRIVE *drive = &g_ide8.drive[drive_id];
    if (connected == drive->connected)
        return;

    if (connected == IDE8_STATE_CONNECTED)
    {
        ide8_drive_open_image(drive, drive->filepath);
    }
    else
    {
        ide8_drive_close_image(drive);
        g_print("IDE8 HDD%d - Disconnected\n", drive_id);
    };
}

static int ide8_drive_image_expand(st_IDE8_DRIVE *drive)
{

    if (drive->block >= IDE8_MAX_IMG_BLOCKS)
    {
        fprintf(stderr, "%s():%d - Can't seek to block '%d' - max alowed blocks excededmax alowed count of total blocks exceded (%d)\n", __func__, __LINE__, drive->block, IDE8_MAX_IMG_BLOCKS);
        // TODO: nastavit err reg
        drive->status = IDE8_STS_ERROR | IDE8_STS_WRERROR;
        return EXIT_FAILURE;
    };

    uint32_t i;
    uint8_t buff[IDE8_SECTOR_SIZE];
    memset(buff, 0x00, sizeof(buff));
    for (i = drive->total_blocks; i <= drive->block; i++)
    {
        uint32_t offset = i * IDE8_SECTOR_SIZE;
        // g_print ( "expand block: %d (%d)\n", i, offset );
        FS_LAYER_FSEEK(drive->fp, offset);
        uint32_t writelen = 0;
        if (FS_LAYER_FR_OK != FS_LAYER_FWRITE(drive->fp, buff, IDE8_SECTOR_SIZE, &writelen))
        {
            fprintf(stderr, "%s():%d - Write error on offset '%d'\n", __func__, __LINE__, offset);
            // TODO: nastavit err reg
            drive->status = IDE8_STS_ERROR | IDE8_STS_WRERROR;
            return EXIT_FAILURE;
        };
        drive->total_blocks++;
    };

    return EXIT_SUCCESS;
}

static int ide8_drive_sector_read(st_IDE8_DRIVE *drive)
{
    uint32_t offset = drive->block * IDE8_SECTOR_SIZE;
    // g_print ( "read block: %d (%d)\n", drive->block, offset );

    if (drive->block >= drive->total_blocks)
    {
        ide8_drive_image_expand(drive);
    };

    FS_LAYER_FSEEK(drive->fp, offset);

    uint32_t readlen = 0;
    if (FS_LAYER_FR_OK != FS_LAYER_FREAD(drive->fp, drive->cache, IDE8_SECTOR_SIZE, &readlen))
    {
        fprintf(stderr, "%s():%d - Read error on offset '%d'\n", __func__, __LINE__, offset);
        // TODO: nastavit err reg
        drive->status = IDE8_STS_ERROR;
        return EXIT_FAILURE;
    };

    drive->status = IDE8_STS_READY | IDE8_STS_DRQ;
    drive->sector_count--;
    drive->data_pos = 0;
    return EXIT_SUCCESS;
}

static int ide8_drive_sector_write(st_IDE8_DRIVE *drive)
{
    uint32_t offset = drive->block * IDE8_SECTOR_SIZE;
    // g_print ( "write block: %d (%d)\n", drive->block, offset );

    if (drive->block >= drive->total_blocks)
    {
        ide8_drive_image_expand(drive);
    };

    FS_LAYER_FSEEK(drive->fp, offset);

    uint32_t writelen = 0;
    if (FS_LAYER_FR_OK != FS_LAYER_FWRITE(drive->fp, drive->cache, IDE8_SECTOR_SIZE, &writelen))
    {
        fprintf(stderr, "%s():%d - Write error on offset '%d'\n", __func__, __LINE__, offset);
        // TODO: nastavit err reg
        drive->status = IDE8_STS_ERROR | IDE8_STS_WRERROR;
        return EXIT_FAILURE;
    };

    if (drive->block == 0)
    {
        uint16_t c = drive->geo_c;
        uint8_t h = drive->geo_h;
        uint8_t s = drive->geo_s;
        if (EXIT_SUCCESS == ide8_check_nipos_geometry(drive, drive->cache))
        {
            if ((c != drive->geo_c) || (h != drive->geo_h) || (s != drive->geo_s))
            {
                g_print("IDE8 HDD%d NIPOS HDD geometry changed:\n", drive->drive_id);
                g_print("c: %d, h: %d, s: %d = blocks: %d\n", drive->geo_c, drive->geo_h, drive->geo_s, (drive->geo_c * drive->geo_h * drive->geo_s));
            };
        };
    };

    drive->status = IDE8_STS_READY | IDE8_STS_DRQ;
    drive->sector_count--;
    drive->data_pos = 0;
    return EXIT_SUCCESS;
}

static uint8_t ide8_drive_read_data(st_IDE8_DRIVE *drive)
{
    if (drive->cmd != IDE8_CMD_SECTOR_READ)
        return 0x00;
    uint8_t value = drive->cache[drive->data_pos];
    if (drive->busmode == IDE8_BUSMODE_16)
    {
        drive->data_pos += 2;
    }
    else
    {
        drive->data_pos += 1;
    };
    if (drive->data_pos >= IDE8_SECTOR_SIZE)
    {
        if (drive->sector_count)
        {
            drive->block++;
            if (EXIT_SUCCESS != ide8_drive_sector_read(drive))
            {
                drive->cmd = IDE8_CMD_NONE;
            };
        }
        else
        {
            drive->status = IDE8_STS_READY;
            drive->cmd = IDE8_CMD_NONE;
        };
    };
    return value;
}

uint8_t ide8_read_byte(en_IDE8_ADDR addr)
{
    uint8_t value = 0xff;
    switch (addr)
    {

    case IDE8_ADDR_DATA:
        value = ide8_drive_read_data(&g_ide8.drive[g_ide8.selected]);
        break;

    case IDE8_ADDR_STATUS:
        value = g_ide8.drive[g_ide8.selected].status;
        if (g_ide8.drive[g_ide8.selected].status & IDE8_STS_ERROR)
        {
            g_ide8.drive[g_ide8.selected].status = IDE8_STS_READY;
        };
        break;

    default:
        fprintf(stderr, "%s():%d - Read unsupported addr 0x%02x\n", __func__, __LINE__, addr);
    };

    // g_print ( "read port: 0x%02x - 0x%02x\n", ( 0x78 | addr ), value );
    return value;
}

static int ide8_drive_assign_registers(st_IDE8_DRIVE *drive)
{
    drive->sector_count = g_ide8.regSECTOR_COUNT;
    if (drive->addressing == IDE8_ADDRESSING_LBA)
    {
        drive->block = (g_ide8.regSECTOR);
        drive->block |= g_ide8.regCYLINDER << 8;
        drive->block |= g_ide8.regHEAD << 24;
    }
    else
    {
        if ((!g_ide8.regSECTOR) || (g_ide8.regSECTOR > drive->geo_s) || (g_ide8.regCYLINDER >= drive->geo_c) || (g_ide8.regHEAD >= drive->geo_h))
        {
            drive->status = IDE8_STS_READY | IDE8_STS_ERROR;
            return EXIT_FAILURE;
        };
        drive->block = (g_ide8.regCYLINDER * drive->geo_h * drive->geo_s);
        drive->block += (g_ide8.regHEAD * drive->geo_s);
        drive->block += g_ide8.regSECTOR;
    };
    drive->block--;
    return EXIT_SUCCESS;
}

static int ide8_drive_cmd_read(st_IDE8_DRIVE *drive)
{
    if (EXIT_SUCCESS != ide8_drive_assign_registers(drive))
        return EXIT_FAILURE;
    return ide8_drive_sector_read(drive);
}

static void ide8_drive_cmd_write(st_IDE8_DRIVE *drive)
{
    if (EXIT_SUCCESS != ide8_drive_assign_registers(drive))
        return;
    drive->data_pos = 0;
    drive->status = IDE8_STS_READY | IDE8_STS_DRQ;
}

static void ide8_drive_cmd_set_features(st_IDE8_DRIVE *drive)
{

    switch (g_ide8.regFEATURES)
    {

    case IDE8_FEATURE_8BITON:
        // g_print ( "set feature - busmode 8\n" );
        drive->busmode = IDE8_BUSMODE_8;
        break;

    case IDE8_FEATURE_8BITOFF:
        // g_print ( "set feature - busmode 16\n" );
        drive->busmode = IDE8_BUSMODE_16;
        break;

    default:
        fprintf(stderr, "%s():%d - Unsupported feature: 0x%02x\n", __func__, __LINE__, g_ide8.regFEATURES);
    };
}

static void ide8_write_command(en_IDE8_CMD cmd)
{
    int ret = EXIT_SUCCESS;
    switch (cmd)
    {

    case IDE8_CMD_RESET:
        // g_print ( "port write command RESET\n" );
        ide8_drive_reset(&g_ide8.drive[g_ide8.selected]);
        break;

    case IDE8_CMD_SECTOR_READ:
        // g_print ( "port write command READ\n" );
        ret = ide8_drive_cmd_read(&g_ide8.drive[g_ide8.selected]);
        break;

    case IDE8_CMD_SECTOR_WRITE:
        // g_print ( "port write command WRITE\n" );
        ide8_drive_cmd_write(&g_ide8.drive[g_ide8.selected]);
        break;

    case IDE8_CMD_SET_FEATURES:
        // g_print ( "port write command SET_FEATURES\n" );
        ide8_drive_cmd_set_features(&g_ide8.drive[g_ide8.selected]);
        break;

    default:
        fprintf(stderr, "%s():%d - Unknown command: 0x%02x\n", __func__, __LINE__, cmd);
        return;
    };
    if (ret == EXIT_SUCCESS)
    {
        g_ide8.drive[g_ide8.selected].cmd = cmd;
    }
    else
    {
        g_ide8.drive[g_ide8.selected].cmd = IDE8_CMD_NONE;
    };
}

static void ide8_drive_write_data(st_IDE8_DRIVE *drive, uint8_t value)
{
    if (drive->cmd != IDE8_CMD_SECTOR_WRITE)
        return;

    drive->cache[drive->data_pos] = value;

    if (drive->busmode == IDE8_BUSMODE_16)
    {
        drive->data_pos += 2;
    }
    else
    {
        drive->data_pos += 1;
    };

    if (drive->data_pos < IDE8_SECTOR_SIZE)
        return;

    if (EXIT_SUCCESS != ide8_drive_sector_write(drive))
    {
        drive->cmd = IDE8_CMD_NONE;
    };
    drive->block++;

    if (!drive->sector_count)
    {
        drive->status = IDE8_STS_READY;
        drive->cmd = IDE8_CMD_NONE;
    };
}

void ide8_write_byte(en_IDE8_ADDR addr, uint8_t value)
{

    switch (addr)
    {
    case IDE8_ADDR_DATA:
        ide8_drive_write_data(&g_ide8.drive[g_ide8.selected], value);
        break;

    case IDE8_ADDR_FEATURES:
        // g_print ( "port 0x%02x write feature 0x%02x\n", ( 0x78 | addr ), value );
        g_ide8.regFEATURES = value;
        break;

    case IDE8_ADDR_SCOUNT:
        // g_print ( "port 0x%02x write sector count 0x%02x\n", ( 0x78 | addr ), value );
        g_ide8.regSECTOR_COUNT = value;
        break;

    case IDE8_ADDR_SECTOR:
        // g_print ( "port 0x%02x write sector 0x%02x\n", ( 0x78 | addr ), value );
        g_ide8.regSECTOR = value;
        break;

    case IDE8_ADDR_LCYL:
        // g_print ( "port 0x%02x write Lcyl 0x%02x\n", ( 0x78 | addr ), value );
        g_ide8.regCYLINDER &= 0xff00;
        g_ide8.regCYLINDER |= value;
        break;

    case IDE8_ADDR_HCYL:
        // g_print ( "port 0x%02x write Hcyl 0x%02x\n", ( 0x78 | addr ), value );
        g_ide8.regCYLINDER &= 0x00ff;
        g_ide8.regCYLINDER |= (value << 8);
        break;

    case IDE8_ADDR_HEAD:
        // g_print ( "port 0x%02x write head 0x%02x\n", ( 0x78 | addr ), value );
        g_ide8.regHEAD = value & 0x0f;
        g_ide8.selected = (value & IDE8_HEAD_MSMASK) ? IDE8_DRIVE_SLAVE : IDE8_DRIVE_MASTER;
        g_ide8.drive[g_ide8.selected].addressing = (value & IDE8_HEAD_BUSMODEMASK) ? IDE8_ADDRESSING_LBA : IDE8_ADDRESSING_CHS;
        break;

    case IDE8_ADDR_COMMAND:
        ide8_write_command(value);
        break;

    default:
        fprintf(stderr, "%s():%d - Write to unsupported addr 0x%02x, value: 0x%02x\n", __func__, __LINE__, addr, value);
    };
}
