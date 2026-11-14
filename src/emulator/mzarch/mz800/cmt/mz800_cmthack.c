/*
 * File:   mz800_cmthack.c
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

#include "main.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/rom.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmthack.h"

static void mz800_cmthack_install_rom_patch(void)
{

    /* ROM hack */

    /*
     * Precteni hlavicky z CMT
     *
     *	RHEAD:
     *
     *	Vystup: CY = 1  doslo k chybe
     *			A = 1  chyba kontrolniho souctu
     *			A = 2  detekovano BREAK
     *		CY = 0  O.K.
     *
     */
    g_memory.ROM[0x04d8] = 0xe5; /* push HL */
    g_memory.ROM[0x04d9] = 0x21; /* ld HL, 0x10f0 */
    g_memory.ROM[0x04da] = 0xf0;
    g_memory.ROM[0x04db] = 0x10;
    g_memory.ROM[0x04dc] = 0xd3; /* out (0x01), a */
    g_memory.ROM[0x04dd] = 0x01;
    g_memory.ROM[0x04de] = 0xe1; /* pop HL */
    g_memory.ROM[0x04df] = 0xc9; /* ret */

    /*
     *	RDATA:
     *
     * Precte program z CMT podle informaci
     *
     *	vystup:  CY s vyznamem jako u RHEAD
     *
     */
    g_memory.ROM[0x04f8] = 0xe5; /* push HL */
    g_memory.ROM[0x04f9] = 0xc5; /* push BC */
    g_memory.ROM[0x04fa] = 0x2a; /* ld HL, (0x1104) */
    g_memory.ROM[0x04fb] = 0x04;
    g_memory.ROM[0x04fc] = 0x11;
    g_memory.ROM[0x04fd] = 0xed; /* ld BC, (0x1102) */
    g_memory.ROM[0x04fe] = 0x4b;
    g_memory.ROM[0x04ff] = 0x02;
    g_memory.ROM[0x0500] = 0x11;
    g_memory.ROM[0x0501] = 0xd3; /* out (0x02), A */
    g_memory.ROM[0x0502] = 0x02;
    g_memory.ROM[0x0503] = 0xc1; /* pop BC */
    g_memory.ROM[0x0504] = 0xe1; /* pop HL */
    g_memory.ROM[0x0505] = 0xc9; /* ret */

    /*
     *
     * JSS 1.3 a 1.6A na adrese 0xf3bb (0xf3b8) provadi kontrolu horni a dolni ROM:
     *
     * xor a, nasledne xor 0x0000 - 0x0xfff, 0xe010 - 0xffff
     *
     * obsah porovna s tim co je na adrese 0xe840 ( == 0xed)
     *
     * Bohuzel nikdo uz asi nikdy nezjisti na ktere adrese dela JSS korekci, aby
     * byl vysledny XOR = 0xed
     *
     */

    g_memory.ROM[0x0506] = 0x59; /* pokud zde dame 0x00, tak XOR vychazi 0xb4, proto: 0xb4 ^ 0xed = 0x59 */

    cmt_stop();
}

void cmthack_mzarch_reinstall_rom_patch(void)
{
    if (!TEST_ROM_CMTHACK_INCOMP)
    {
        if ((!TEST_ROM_USER_DEFINED) || (g_rom.user_defined_cmthack_type == ROM_CMTHACK_DEFAULT))
        {
            if (g_cmthack.load_patch_installed)
            {
                mz800_cmthack_install_rom_patch();
                /* patch zápisu (Direct Save) podle nastavení a stavu decku */
                cmthack_save_patch_sync();
            };
            return;
        };
    };
    /* ROM bez výchozího patche (INCOMP, vlastní ROM hacku): patch zápisu v ROM není */
    cmthack_save_patch_sync();
}

void cmthack_mzarch_load_rom_patch(unsigned enabled)
{
    if (!TEST_ROM_CMTHACK_INCOMP)
    {
        if ((!TEST_ROM_USER_DEFINED) || (g_rom.user_defined_cmthack_type == ROM_CMTHACK_DEFAULT))
        {
            if (enabled)
            {
                mz800_cmthack_install_rom_patch();
            }
            else
            {
                memcpy(&g_memory.ROM[0x04d8], &g_rom.mz700rom[0x04d8], 8);
                memcpy(&g_memory.ROM[0x04f8], &g_rom.mz700rom[0x04f8], 15);
            };
            g_cmthack.load_patch_installed = enabled & 1;
            /* patch zápisu jen s patchem čtení (vypnutí vrátí i jeho bajty) */
            cmthack_save_patch_sync();
        }
        else if (g_rom.user_defined_cmthack_type == ROM_CMTHACK_CUSTOM)
        {
            g_cmthack.load_patch_installed = enabled & 1;
            /* vlastní ROM hacku uživatele: výchozí patch zápisu se nedává */
            cmthack_save_patch_sync();
            rom_reinstall(g_rom.rom_cfg);
        };
    };
}
