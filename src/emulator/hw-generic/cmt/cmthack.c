/*
 * File:   cmthack.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 2. července 2015, 20:49
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <strings.h>

// Lokalizace
#include "i18n.h"

#include "mzarch/mzcommon_config.h"
#include "cmthack.h"
#include "cmtext.h"

#include "libs/cpu-z80/z80.h"
#include "memory/memory.h"
#include "mzarch/mzarch.h"
#include "cmt.h"

#include "iface/iface_audio.h"
#include "hw-generic/gdg/framebuffer.h"
#include "emulator_measuring.h"

#include "cfgmain.h"

#include "baseui/baseui.h"
#include "baseui/baseui_filechooser.h"
#include "libs/generic_driver/generic_driver.h"
#include "generic_driver/memory_driver.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/eventlog.h"
#endif

st_CMTHACK g_cmthack;

static st_DRIVER *g_driver = &g_memory_driver_static;
#define CMT_HACK_DEFAULT_HANDLER_TYPE HANDLER_TYPE_MEMORY

typedef enum en_LOADRET
{
    LOADRET_OK = 0,
    LOADRET_ERROR,
    LOADRET_BREAK
} en_LOADRET;

void cmthack_reinstall_rom_patch(void)
{
    cmthack_mzarch_reinstall_rom_patch();
}

void cmthack_load_rom_patch(unsigned enabled)
{
    cmthack_mzarch_load_rom_patch(enabled);
}

void cmthack_reset(void)
{
    /* pokud jsme rozecteny MZF soubor, tak ho zavrem */
    generic_driver_close(&g_cmthack.mzf_handler);
}

void cmthack_exit(void)
{
    /* pokud jsme rozecteny MZF soubor, tak ho zavrem */
    generic_driver_close(&g_cmthack.mzf_handler);
    if (g_cmthack.last_filename)
    {
        g_free(g_cmthack.last_filename);
    }
}

#define DEFAULT_CMT_HACK_ENABLE 1
#define DEFAULT_CMT_HACK_FIX_TERMINATOR 1

void cmthack_propagatecfg_load_rom_patch(void *e, void *data)
{
    (void)data;
    cmthack_load_rom_patch(cfgelement_get_bool_value((CFGELM *)e));
}

void cmthack_init(void)
{
    generic_driver_register_handler(&g_cmthack.mzf_handler, CMT_HACK_DEFAULT_HANDLER_TYPE);
    generic_driver_set_handler_readonly_status(&g_cmthack.mzf_handler, 1);
    g_cmthack.last_filename = g_malloc0(1);
    g_cmthack.load_patch_installed = 0;
    g_cmthack.fix_fname_terminator = 0;

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "CMTHACK");

    CFGELM *elm;
    elm = cfgmodule_register_new_element(cmod, "enable", CFGENTYPE_BOOL, DEFAULT_CMT_HACK_ENABLE);
    cfgelement_set_propagate_cb(elm, cmthack_propagatecfg_load_rom_patch, NULL);
    cfgelement_set_handlers(elm, NULL, (void *)&g_cmthack.load_patch_installed);

    elm = cfgmodule_register_new_element(cmod, "fix_fname_terminator", CFGENTYPE_BOOL, DEFAULT_CMT_HACK_FIX_TERMINATOR);
    cfgelement_set_handlers(elm, (void *)&g_cmthack.fix_fname_terminator, (void *)&g_cmthack.fix_fname_terminator);

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}

void cmthack_result(en_LOADRET result)
{

    uint16_t reg_af = z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);

    if (result == LOADRET_OK)
    {
        reg_af &= ~0x01; /* reset CARRY flag */
    }
    else
    {
        reg_af |= 0x01;                    /* set CARRY flag */
        reg_af |= ((uint16_t)result << 8); /* result do regA */
        reg_af &= ((uint16_t)result << 8) | 0xff;
    };
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, reg_af);
}

/*
 *
 * Pozadavek na precteni headeru z CMT
 *
 */
void cmthack_load_file(void)
{
    iface_audio_pause_emulation(1);
    /* CMT-hack stall: file chooser dialog + IO stojí emulaci - vyloučit z benchmarku */
    emulator_measuring_maxspeed_stall_begin();
    framebuffer_flush_full_screen();

    char *filename = baseui_filechooser_open_file_wait(_("Select a MZF File"), ".mzf, .m12, .*", NULL, NULL, g_cmthack.last_filename, NULL);

    if (filename == NULL)
    {
        /* Zruseno: nastavit Err + Break */
        cmthack_result(LOADRET_BREAK);
        iface_audio_pause_emulation(0);
        emulator_measuring_frame_timing_reset();
        emulator_measuring_maxspeed_stall_end();
        return;
    };

    const char *file_ext = cmtext_get_filename_extension(filename);

    if ((0 == strcasecmp(file_ext, "mzf")) || (0 == strcasecmp(file_ext, "m12")))
    {
        cmthack_load_mzf_filename(filename);
    }
    else
    {
        baseui_show_message(true, "This file can't be open in CMTHACK.\nPlease, load this file by virtual CMT.");
        cmthack_result(LOADRET_BREAK);
    };

    g_free(filename);
    iface_audio_pause_emulation(0);
    emulator_measuring_frame_timing_reset();
    emulator_measuring_maxspeed_stall_end();
}

void cmthack_load_mzf_filename(const char *filename)
{

    unsigned reg_hl;

    /* pokud jsme rozecteny MZF soubor, tak ho zavrem */
    generic_driver_close(&g_cmthack.mzf_handler);

    if ((NULL == generic_driver_open_memory_from_file(&g_cmthack.mzf_handler, g_driver, filename)) || (g_cmthack.mzf_handler.err) || (g_driver->err))
    {
        /* Nejde otevrit soubor, nebo inicializovat handler: nastavit Err + Break */
        baseui_error("CMT HACK can't open MZF file '%s': %s, gdriver_err: %s\n", filename, strerror(errno), generic_driver_error_message(&g_cmthack.mzf_handler, g_cmthack.mzf_handler.driver));
        cmthack_result(LOADRET_BREAK);
        return;
    };

    g_cmthack.last_filename = g_realloc(g_cmthack.last_filename, strlen(filename) + 1);
    snprintf(g_cmthack.last_filename, strlen(filename) + 1, "%s", filename);

    /* precteme prvnich 128 bajtu z MZF souboru a ulozime je do RAM na adresu z regHL */

    reg_hl = z80_get_reg(g_mzarch_main.cpu, Z80_REG_HL);

    uint8_t buff[sizeof(st_MZF_HEADER)];
    if (EXIT_SUCCESS != generic_driver_read(&g_cmthack.mzf_handler, 0, &buff, sizeof(buff)))
    {
        /* Vadny soubor: nastavit Err + Checksum */
        baseui_error("CMT HACK can't load MZF header '%s': %s, gdriver_err: %s\n", filename, strerror(errno), generic_driver_error_message(&g_cmthack.mzf_handler, g_cmthack.mzf_handler.driver));
        generic_driver_close(&g_cmthack.mzf_handler);
        cmthack_result(LOADRET_ERROR);
        return;
    };
    memory_load_block(buff, reg_hl, sizeof(st_MZF_HEADER), MEMORY_LOAD_MAPED);

    int test_fname_term = mzf_header_test_fname_terminator(&g_cmthack.mzf_handler);

    st_MZF_HEADER mzfhdr;

    if (EXIT_SUCCESS == mzf_read_header(&g_cmthack.mzf_handler, &mzfhdr))
    {
        char ascii_filename[MZF_FNAME_FULL_LENGTH];
        mzf_tools_get_fname(&mzfhdr, ascii_filename);
        printf("\nCMT hack: load MZF header on 0x%04x.\n\n", reg_hl);
        printf("fname: %s", ascii_filename);
        if (test_fname_term == EXIT_FAILURE)
        {
            uint8_t buff = MZF_FNAME_TERMINATOR;
            if (g_cmthack.fix_fname_terminator)
            {
                memory_load_block(&buff, (reg_hl + sizeof(st_MZF_FILENAME) - 2), 1, MEMORY_LOAD_MAPED);
            }
            printf(" (!mising 0x0d%s)", (g_cmthack.fix_fname_terminator) ? " - fixed" : "");
        }
        printf("\n");
        printf("ftype: 0x%02x\n", mzfhdr.ftype);
        printf("fstrt: 0x%04x\n", mzfhdr.fstrt);
        printf("fsize: 0x%04x\n", mzfhdr.fsize);
        printf("fexec: 0x%04x\n", mzfhdr.fexec);

        if (CMT_TEST_MZFSIZE_CHECK_ENABLED)
        {
            unsigned mzf_size = mzfhdr.fsize + sizeof(st_MZF_HEADER);
            baseui_cmt_check_mzf_filesize(filename, mzf_size);
        };
    }
    else
    {
        baseui_error("CMT HACK can't read MZF header '%s': %s, gdriver_err: %s\n", filename, strerror(errno), mzf_error_message(&g_cmthack.mzf_handler, g_cmthack.mzf_handler.driver));
        generic_driver_close(&g_cmthack.mzf_handler);
        cmthack_result(LOADRET_ERROR);
        return;
    };

    /* Nacteno OK */
    cmthack_result(LOADRET_OK);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Vlna 5 Commit 31 - SYS lifecycle event do Event Viewer. Filename hash
     * (djb2 z basename) zaznamenáme v payloadu. */
    eventlog_sys_event ( EVENTLOG_SYS_MZF_INJECT, eventlog_filename_hash ( filename ) );
#endif
}

/*
 *
 * Pozadavek na precteni tela souboru
 *
 */
void cmthack_read_mzf_body(void)
{
    unsigned reg_hl;
    unsigned reg_bc;

    iface_audio_pause_emulation(1);
    /* CMT-hack stall: čtení MZF body z disku stojí emulaci - vyloučit z benchmarku */
    emulator_measuring_maxspeed_stall_begin();

    /* Mame otevren nejaky MZF? */
    if (!(g_cmthack.mzf_handler.status & HANDLER_STATUS_READY))
    {
        /* Zruseno: nastavit Err + Break */
        cmthack_result(LOADRET_BREAK);
        iface_audio_pause_emulation(0);
        emulator_measuring_frame_timing_reset();
        emulator_measuring_maxspeed_stall_end();
        return;
    };

    /* precteme prvnich regBC bajtu z MZF souboru a ulozime je do RAM na adresu z regHL */
    reg_hl = z80_get_reg(g_mzarch_main.cpu, Z80_REG_HL);
    reg_bc = z80_get_reg(g_mzarch_main.cpu, Z80_REG_BC);

    /* precteme prvnich regBC bajtu z MZF body a ulozime je do RAM na adresu z regHL */
    uint8_t data[0x10000];
    if (EXIT_SUCCESS != mzf_read_body(&g_cmthack.mzf_handler, data, reg_bc))
    {
        /* Vadny soubor: nastavit Err + Checksum */
        baseui_error("CMT HACK can't load MZF body '%s': %s, gdriver_err: %s\n", g_cmthack.last_filename, strerror(errno), mzf_error_message(&g_cmthack.mzf_handler, g_cmthack.mzf_handler.driver));
        generic_driver_close(&g_cmthack.mzf_handler);
        cmthack_result(LOADRET_ERROR);
        iface_audio_pause_emulation(0);
        emulator_measuring_frame_timing_reset();
        emulator_measuring_maxspeed_stall_end();
        return;
    };

    memory_load_block(data, reg_hl, reg_bc, MEMORY_LOAD_MAPED);

    printf("\nCMT hack: load 0x%04x bytes from MZF body to addr 0x%04x.\n", reg_bc, reg_hl);

    /* Nacteno OK */
    generic_driver_close(&g_cmthack.mzf_handler);
    cmthack_result(LOADRET_OK);
    iface_audio_pause_emulation(0);
    emulator_measuring_frame_timing_reset();
    emulator_measuring_maxspeed_stall_end();
}
