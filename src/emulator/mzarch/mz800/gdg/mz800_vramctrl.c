/*
 * File:   mz800_vramctrl.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 18:49
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

/*
 * 	Radic pameti VRAM
 *
 *
 *
 *	V rezimu MZ700 je pouzita jen interni pametova banka VRAM1 (8kB), ktera se mapuje:
 *
 *	MZ ADDR		VRAM ADDR
 *	============================================
 *	0x1000:		0x0000 - 0x07ff: CG-RAM1
 *	                0x0800 - 0x0fff: CG-RAM2
 *	0xd000:		0x1000 - 0x17ff: Character VRAM
 *		        0x1800 - 0x1fff: Attribute VRAM
 *
 *
 *
 *	V rezimu MZ800 jsou mapovany pametove banky podle toho jak je nastaven 1. a 2. bit DMD registu:
 *
 *	Rezim		Roviny		VRAM		Mapovani
 *	========================================================
 *	320x200@4/A	I., II.		1, 2		0x8000 - 0x9fff
 *	320x200@4/B	III., IV.	3, 4		0x8000 - 0x9fff
 *	320x200@16	I. - IV.	1, 2, 3, 4	0x8000 - 0x9fff
 *	640x200@2/A	I.		    1, 2		0x8000 - 0xbfff	(sudy bajt ulozen do VRAM1, lichy do VRAM2)
 *	640x200@2/B	III.		3, 4		0x8000 - 0xbfff	(sudy bajt ulozen do VRAM3, lichy do VRAM4)
 *	640x200@4	I., III.	1, 2, 3, 4	0x8000 - 0xbfff	(sudy bajt ulozen do VRAM1 a VRAM3, lichy do VRAM2 a VRAM4)
 *
 *
 *	reg. RF (0xcd):
 *			- 7. bit, rezim cteni: 0 => SINGLE, 1 => SEARCH
 *			- 6. bit, NC
 *			- 5. bit, NC
 *			- 4. bit, pouzita banka: 0 => A, 1 => B
 *			- 3. bit, rovina IV.
 *			- 2. bit, rovina III.
 *			- 1. bit, rovina II.
 *			- 0. bit, rovina I.
 *
 *	* bit 4 RF registru je propojen s bitem 4. WF registru (TODO: neoveroval jsem - tvrdi to v SM800)
 * 	* bit 4 je pouzit pouze v rezimech 320x200/4 (A/B) a 640x200/2 (A/B)
 *
 *	Pokud neni nastaven search, tak cteme tu rovinu, ktera je vybrana ( bit ma nastaveno 1).
 *      	Cteni z vice rovin neni podporovano a vraci 0x00.
 *		Pokud neni vybrana zadna rovina, tak cteme 0xff.
 *		Pokud neni pripojena EXVRAM, tak z ni cteme 0xff.
 *
 *	V rezimu SEARCH cteme automaticky ze vsech dostupnych rovin soucasne a vracime 1 tam, kde vysledna hodnota barevne
 *	kombinace pixelu odpovida hledane kombinaci, ktera je urcena nastavenim bitu 0. - 3. RF registru.
 *
 *
 *
 *	reg. WF (0xcc):
 *			- 7. bit, rezim zapisu
 *			- 6. bit, rezim zapisu
 *			- 5. bit, rezim zapisu
 *			- 4. bit, pouzita banka: 0 => A, 1 => B
 *			- 3. bit, rovina IV.
 *			- 2. bit, rovina III.
 *			- 1. bit, rovina II.
 *			- 0. bit, rovina I.
 *
 *	Rezimy zapisu:
 *			000	- SINGLE
 *			001	- EXOR
 *			010	- OR
 *			011	- RESET
 *			10x	- REPLACE
 *			11x	- PSET
 *
 *
 *	Hodnota 4. bitu je vyznamna pouze v rezimech REPLACE a PSET, jinak je ignorovana.
 *	V rezimu SINGLE jsou data primo zapsany do zvolenych rovin.
 *	V rezimu EXOR se provede XOR zapisovanych dat se soucasnym obsahem VRAM.
 *	V rezimu OR se provede OR zapisovanych dat se soucasnym obsahem VRAM.
 *	V rezimu RESET se provede vynulovani zapisovanych dat v soucasnem obsahu VRAM (NOT wr_data AND data).
 *	V rezimu REPLACE se zapisou data do zvolenych rovin a do ostatnich dostupnych rovin se zapise 0x00.
 *	V rezimu PSET se provede OR zapisovanych dat se soucasnym obsahem zvolenych rovin a u ostatnich dostupnych
 *      rovin se provede RESET (NOT wr_data AND data).
 *
 *
 */
#include "hw-generic/memory/memory_arch.h" /* per-arch memory - dříve tranzitivně přes memory.h (mzhal 11c-2c) */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "mzarch/mzarch_config.h"

#include "mz800_gdg.h"
#include "mz800_vramctrl.h"
#include "mz800_hwscroll.h"
#include "hw-generic/gdg/video.h"
#include "mz800_framebuffer.h"
#include "memory/memory.h"

#include "debugger/debugger.h"
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/mhmap.h"
#endif

/*******************************************************************************
 *
 * Obecne rutiny VRAM kontroleru
 *
 ******************************************************************************/

st_VRAMCTRL g_vramctrl;

void vramctrl_reset(void)
{
    g_vramctrl.regWF_PLANE = 0x01;
    g_vramctrl.regWF_MODE = 0x00;
    g_vramctrl.regRF_PLANE = 0x01;
    g_vramctrl.regRF_SEARCH = 0;
    g_vramctrl.regWFRF_VBANK = 0;
    g_vramctrl.mz700_wr_latch_is_used = 0;
}

/*******************************************************************************
 *
 * VRAM kontroler pro rezimy MZ-800
 *
 ******************************************************************************/

/**
 * Nastaveni hodnoty WFr a RFr
 *
 * @param addr
 * @param value
 */
void vramctrl_mz800_set_wf_rf_reg(int addr, uint8_t value)
{

    switch (addr)
    {

    case 0:
        /* nastaveni WF: 0xcc */
        g_vramctrl.regWF_PLANE = value & 0x0f;
        g_vramctrl.regWFRF_VBANK = (value >> 4) & 0x01;
        if (value & 0x80)
        {
            g_vramctrl.regWF_MODE = (value >> 5) & 0x06;
        }
        else
        {
            g_vramctrl.regWF_MODE = (value >> 5) & 0x07;
        };
        return;

    case 1:
        /* nastaveni RF: 0xcd */
        g_vramctrl.regRF_PLANE = value & 0x0f;
        g_vramctrl.regWFRF_VBANK = (value >> 4) & 0x01;
        g_vramctrl.regRF_SEARCH = (value >> 7) & 0x01;
        return;
    };
}

/* Mame pripojenu externi VRAM? */
#define DEF_USE_EXTVRAM 1

/* Bitove masky pro identifikaci jednotlivych rovin */
#define DEF_PLANE1 (1 << 0)
#define DEF_PLANE2 (1 << 1)
#define DEF_PLANE3 (1 << 2)
#define DEF_PLANE4 (1 << 3)

/* Pokud jsme v rozliseni 640x200, tak potrebujeme vedet, zda se saha na dudy^H^H^H^H ;) sudy, nebo lichy byte */
#define ADDR_IS_ODD (addr & 0x01)

/**
 * Proste cteni z VRAM v rezimu MZ-800
 *
 * @param addr
 * @return
 */
static inline uint8_t vramctrl_mz800_memop_read_byte_internal(uint16_t addr)
{

    uint8_t search = 0xff;
    uint8_t notsearch = 0x00;
    uint8_t retval;

    /* Skutecna adresa ve VRAM */
    uint16_t phy_vram_addr = addr;

    /* v rozliseni 640x200 vydelime adresu 2 */
    if (GDG_MZ800_DMD_TEST_SCRW640)
    {
        phy_vram_addr = phy_vram_addr >> 1;
    };

    phy_vram_addr = hwscroll_shift_addr(phy_vram_addr);

    if (g_vramctrl.regRF_SEARCH)
    {

        if (GDG_MZ800_DMD_TEST_HICOLOR)
        {

            if (GDG_MZ800_DMD_TEST_SCRW640)
            {

                /* 640x200 @ 4 */

                if (g_vramctrl.regRF_PLANE & DEF_PLANE1)
                {
                    if (ADDR_IS_ODD)
                    {
                        search = g_memoryVRAM_II[phy_vram_addr];
                    }
                    else
                    {
                        search = g_memoryVRAM_I[phy_vram_addr];
                    };
                }
                else
                {
                    if (ADDR_IS_ODD)
                    {
                        notsearch = g_memoryVRAM_II[phy_vram_addr];
                    }
                    else
                    {
                        notsearch = g_memoryVRAM_I[phy_vram_addr];
                    };
                };

                if (g_vramctrl.regRF_PLANE & DEF_PLANE3)
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        if (ADDR_IS_ODD)
                        {
                            search &= g_memoryVRAM_IV[phy_vram_addr];
                        }
                        else
                        {
                            search &= g_memoryVRAM_III[phy_vram_addr];
                        };
                    }
                    else
                    {
                        /* search &= 0xff; */
                    };
                }
                else
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        if (ADDR_IS_ODD)
                        {
                            notsearch |= g_memoryVRAM_IV[phy_vram_addr];
                        }
                        else
                        {
                            notsearch |= g_memoryVRAM_III[phy_vram_addr];
                        };
                    }
                    else
                    {
                        notsearch |= 0xff;
                    };
                };
            }
            else
            {

                /* 320x200 @ 16 */

                if (g_vramctrl.regRF_PLANE & DEF_PLANE1)
                {
                    search = g_memoryVRAM_I[phy_vram_addr];
                }
                else
                {
                    notsearch = g_memoryVRAM_I[phy_vram_addr];
                };

                if (g_vramctrl.regRF_PLANE & DEF_PLANE2)
                {
                    search &= g_memoryVRAM_II[phy_vram_addr];
                }
                else
                {
                    notsearch |= g_memoryVRAM_II[phy_vram_addr];
                };

                if (g_vramctrl.regRF_PLANE & DEF_PLANE3)
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        search &= g_memoryVRAM_III[phy_vram_addr];
                    }
                    else
                    {
                        /* search &= 0xff; */
                    };
                }
                else
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        notsearch |= g_memoryVRAM_III[phy_vram_addr];
                    }
                    else
                    {
                        notsearch |= 0xff;
                    };
                };

                if (g_vramctrl.regRF_PLANE & DEF_PLANE4)
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        search &= g_memoryVRAM_IV[phy_vram_addr];
                    }
                    else
                    {
                        /* search &= 0xff; */
                    };
                }
                else
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        notsearch |= g_memoryVRAM_IV[phy_vram_addr];
                    }
                    else
                    {
                        notsearch |= 0xff;
                    };
                };
            };
        }
        else if (GDG_MZ800_DMD_TEST_SCRW640)
        {

            if (g_vramctrl.regWFRF_VBANK)
            {

                /* 640x200 @ 2 B */
                if (g_vramctrl.regRF_PLANE & DEF_PLANE3)
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        if (ADDR_IS_ODD)
                        {
                            search = g_memoryVRAM_IV[phy_vram_addr];
                        }
                        else
                        {
                            search = g_memoryVRAM_III[phy_vram_addr];
                        };
                    }
                    else
                    {
                        /* search = 0xff */
                    };
                }
                else
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        if (ADDR_IS_ODD)
                        {
                            notsearch = g_memoryVRAM_IV[phy_vram_addr];
                        }
                        else
                        {
                            notsearch = g_memoryVRAM_III[phy_vram_addr];
                        };
                    }
                    else
                    {
                        notsearch = 0xff;
                    };
                };
            }
            else
            {

                /* 640x200 @ 2 A */
                if (g_vramctrl.regRF_PLANE & DEF_PLANE1)
                {
                    if (ADDR_IS_ODD)
                    {
                        search = g_memoryVRAM_II[phy_vram_addr];
                    }
                    else
                    {
                        search = g_memoryVRAM_I[phy_vram_addr];
                    };
                }
                else
                {
                    if (ADDR_IS_ODD)
                    {
                        notsearch = g_memoryVRAM_II[phy_vram_addr];
                    }
                    else
                    {
                        notsearch = g_memoryVRAM_I[phy_vram_addr];
                    };
                };
            };
        }
        else
        {

            if (g_vramctrl.regWFRF_VBANK)
            {

                /* 320x200 @ 4 B */
                if (g_vramctrl.regRF_PLANE & DEF_PLANE3)
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        search = g_memoryVRAM_III[phy_vram_addr];
                    }
                    else
                    {
                        /* search = 0xff */
                    };
                }
                else
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        notsearch = g_memoryVRAM_III[phy_vram_addr];
                    }
                    else
                    {
                        notsearch = 0xff;
                    };
                };

                if (g_vramctrl.regRF_PLANE & DEF_PLANE4)
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        search &= g_memoryVRAM_IV[phy_vram_addr];
                    }
                    else
                    {
                        /* search = 0xff */
                    };
                }
                else
                {
                    if (DEF_USE_EXTVRAM)
                    {
                        notsearch |= g_memoryVRAM_IV[phy_vram_addr];
                    }
                    else
                    {
                        notsearch |= 0xff;
                    };
                };
            }
            else
            {

                /* 320x200 @ 4 A */
                if (g_vramctrl.regRF_PLANE & DEF_PLANE1)
                {
                    search = g_memoryVRAM_I[phy_vram_addr];
                }
                else
                {
                    notsearch = g_memoryVRAM_I[phy_vram_addr];
                };

                if (g_vramctrl.regRF_PLANE & DEF_PLANE2)
                {
                    search &= g_memoryVRAM_II[phy_vram_addr];
                }
                else
                {
                    notsearch |= g_memoryVRAM_II[phy_vram_addr];
                };
            };
        };

        return (search & (~notsearch));
    }
    else
    {

        /* proste cteni rovin */

        if (0x00 == g_vramctrl.regRF_PLANE)
        {
            return 0xff;
        };

        retval = 0xff;

        if (g_vramctrl.regRF_PLANE & DEF_PLANE1)
        {
            if (GDG_MZ800_DMD_TEST_SCRW640 && ADDR_IS_ODD)
            {
                retval &= g_memoryVRAM_II[phy_vram_addr];
            }
            else
            {
                retval &= g_memoryVRAM_I[phy_vram_addr];
            };
        };

        if (g_vramctrl.regRF_PLANE & DEF_PLANE2)
        {
            if (!GDG_MZ800_DMD_TEST_SCRW640)
            {
                retval &= g_memoryVRAM_II[phy_vram_addr];
            };
        };

        if (g_vramctrl.regRF_PLANE & DEF_PLANE3)
        {
            if (DEF_USE_EXTVRAM)
            {
                if (GDG_MZ800_DMD_TEST_SCRW640 && ADDR_IS_ODD)
                {
                    retval &= g_memoryVRAM_IV[phy_vram_addr];
                }
                else
                {
                    retval &= g_memoryVRAM_III[phy_vram_addr];
                };
            };
        };

        if (g_vramctrl.regRF_PLANE & DEF_PLANE4)
        {
            if (!GDG_MZ800_DMD_TEST_SCRW640)
            {
                if (DEF_USE_EXTVRAM)
                {
                    retval &= g_memoryVRAM_IV[phy_vram_addr];
                };
            };
        };
    };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /*
     * CDL recording - jen při aktivním debuggeru. VRAM access je sám o sobě
     * pomalá cesta (planar logika, search mode, switch per plane), takže
     * jeden if test tu výkon nezhoršuje. Hot path běžné instrukce jde přes
     * RAM, ne přes vramctrl.
     *
     * Zaznamenáváme do všech plane indikovaných regRF_PLANE - to je SW-určená
     * sada plane, ze kterých se aktuálně čte. Access kind (X / R) předává
     * memory callback přes g_mhmap_pending_read_kind - VRAM exec je na MZ-800
     * běžný (programy mívají kód v neviditelné části VRAM), takže rozlišení
     * X vs R má praktický význam.
     */
    if ( TEST_DEBUGGER_MHMAP_ACTIVE && g_dbg_in_cpu_path )
    {
        /* Recording: fyzická vram.cdl + mode-specific (320@16, 320@4A/B, 640@4, 640@2A/B). */
        mhmap_record_mz800_vram_read ( g_vramctrl.regRF_PLANE, addr, phy_vram_addr,
                                       g_mhmap_pending_read_kind );
    };
#endif

    return retval;
}

/**
 * Cteni z VRAM v rezimu MZ-800 - s ohledem na synchronizaci a HDL-presnym WAIT.
 *
 * Sync je dvouchodovy:
 *  1. Standardni mreq() sync - posune g_gdg.total_elapsed.ticks na konec
 *     aktualniho M-cyklu (T3f).
 *  2. HDL-presny WAIT (INSIDEOP_MREQ_MZ800_VRAMCTRL_READ) - pokud jsme
 *     uvnitr horke faze po predchozim WRITE, pripocte 6-9 TW WAIT podle
 *     tw_wr[]. READ neaktualizuje stav horke faze.
 *
 * @param addr Mz800 VRAM offset (0x0000 - 0x3FFF, addr & 0x3fff aplikovano volajicim).
 * @return Bajt prectreny z VRAM (s aplikaci RF/SEARCH logiky).
 */
inline uint8_t vramctrl_mz800_memop_read_byte_sync(uint16_t addr)
{
    mzarch_main_insideop_mreq();
    mzarch_main_insideop_mreq_mz800_vramctrl_read();
    uint8_t byte = vramctrl_mz800_memop_read_byte_internal(addr);
    return byte;
}

/**
 * Zapis do VRAM v rezimu MZ-800 - s ohledem na synchronizaci a HDL-presnym WAIT.
 *
 * Sync je dvouchodovy:
 *  1. Standardni mreq() sync - posune g_gdg.total_elapsed.ticks na konec
 *     aktualniho M-cyklu (T3f).
 *  2. HDL-presny WAIT (INSIDEOP_MREQ_MZ800_VRAMCTRL_WRITE) - pokud jsme
 *     uvnitr predchozi horke faze, pripocte 3-6 TW WAIT podle tw_ww[].
 *     Vzdy startuje novou horkou fazi (32 CLK0 v 320x200, 17 CLK0
 *     v 640x200) od T3f tohoto WRITE.
 *
 * @param addr  Mz800 VRAM offset (0x0000 - 0x3FFF).
 * @param value Bajt k zapisu (interpretace dle WF_MODE).
 */
void vramctrl_mz800_memop_write_byte_sync(uint16_t addr, uint8_t value)
{
    mzarch_main_insideop_mreq();
    mzarch_main_insideop_mreq_mz800_vramctrl_write();
    vramctrl_mz800_memop_write_byte(addr, value);
}

/**
 * Cteni z VRAM v rezimu MZ-800 - bez synchronizace
 *
 * @param addr
 * @return
 */
inline uint8_t vramctrl_mz800_memop_read_byte(uint16_t addr)
{
    uint8_t byte = vramctrl_mz800_memop_read_byte_internal(addr);
    return byte;
}

/**
 * Zapis do VRAM v rezimu MZ-800
 *
 * TODO: ten write je 1:1 prepis z VHDL ... asi by jej slo zrychlit
 *
 * @param addr
 * @param value
 */
inline void vramctrl_mz800_memop_write_byte(uint16_t addr, uint8_t value)
{

    /* TODO: meli by jsme volat asi jen pokud se pise do viditelne VRAM */
    framebuffer_MZ800_screen_changed();

    /* Signál pro debugger_memory_write_byte: tento zápis v aktuálním banking
     * kontextu skončil ve VRAM. Při debuggerem-iniciovaném zápisu (memop_call)
     * pak na konci debugger_memory_write_byte() proběhne screen refresh,
     * pokud je v menu zapnutý "Auto refresh on edit". */
    VRAMCTRL_DBG_NOTIFY_VRAM_TOUCH();

    /* Obecna dostupnost jednotlivych rovin */
    int avlb_plane;

    /* Dostupnost rovin pro operace typu SINGLE */
    int avlb_plane_s;

    /* signal pro skutecny zapis do fyzickych VRAM */
    int WE;

    /* Pokud jsme v rozliseni 640x200, tak potrebujeme vedet, zda se saha na dudy^H^H^H^Hsudy, nebo lichy byte */
    int addr_is_odd = addr & 0x01;

    uint8_t wrvalue = 0;

    /* Skutecna adresa ve VRAM */
    int phy_vram_addr = addr;

    /* v rozliseni 640x200 vydelime adresu 2 */
    if (GDG_MZ800_DMD_TEST_SCRW640)
    {
        phy_vram_addr = phy_vram_addr >> 1;
    };

    phy_vram_addr = hwscroll_shift_addr(phy_vram_addr);

    addr_is_odd = addr & 0x01;

    /******************************************************************************/
    /*		Urceni obecne dostupnosti rovin                                   */
    /******************************************************************************/

    /* v HICOLOR rezimech pozuivame vsechny roviny */
    avlb_plane = GDG_MZ800_DMD_TEST_HICOLOR ? (DEF_PLANE1 | DEF_PLANE2 | DEF_PLANE3 | DEF_PLANE4) : 0x00;

    /* bude se pracovat jen s jednou bankou? */
    avlb_plane |= g_vramctrl.regWFRF_VBANK ? (DEF_PLANE3 | DEF_PLANE4) : (DEF_PLANE1 | DEF_PLANE2);

    /* zaverecne rozhodnuti udelime podle rozliseni */
    avlb_plane &= GDG_MZ800_DMD_TEST_SCRW640 ? (DEF_PLANE1 | DEF_PLANE3) : (DEF_PLANE1 | DEF_PLANE2 | DEF_PLANE3 | DEF_PLANE4);

    /* dostupnost rovin pro operace typu SINGLE */
    avlb_plane_s = GDG_MZ800_DMD_TEST_SCRW640 ? (DEF_PLANE1 | DEF_PLANE3) : (DEF_PLANE1 | DEF_PLANE2 | DEF_PLANE3 | DEF_PLANE4);

    /******************************************************************************/
    /*		Do kterych rovin se bude psat?                                    */
    /******************************************************************************/

    WE = 0;

    if (!(GDG_MZ800_DMD_TEST_SCRW640 && addr_is_odd))
    {

        if (!(g_vramctrl.regWF_MODE & (1 << 2)))
        {
            if (g_vramctrl.regWF_PLANE & DEF_PLANE1 & avlb_plane_s)
            {

                /* SINGLE operace - vybrano I. */
                WE |= DEF_PLANE1;
            };
        }
        else if (avlb_plane & DEF_PLANE1)
        {

            /* REPLACE, PSET - vybrano I. */
            if (g_vramctrl.regWF_PLANE & DEF_PLANE1)
            {
                WE |= DEF_PLANE1;

                /* REPLACE, PSET - NEvybrano I. */
            }
            else if (g_vramctrl.regWF_MODE & (GDG_WF_MODE_REPLACE | GDG_WF_MODE_PSET))
            {
                WE |= DEF_PLANE1;
            };
        };
    };

    if (GDG_MZ800_DMD_TEST_SCRW640)
    {
        /* 640x200, lichy bajt? */
        if (addr_is_odd)
        {

            if (!(g_vramctrl.regWF_MODE & (1 << 2)))
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE1 & avlb_plane_s)
                {

                    /* SINGLE operace - vybrano I. */
                    WE |= DEF_PLANE2;
                };
            }
            else if (avlb_plane & DEF_PLANE1)
            {

                /* REPLACE, PSET - vybrano I. */
                if (g_vramctrl.regWF_PLANE & DEF_PLANE1)
                {
                    WE |= DEF_PLANE2;

                    /* REPLACE, PSET - NEvybrano I. */
                }
                else if (g_vramctrl.regWF_MODE & (GDG_WF_MODE_REPLACE | GDG_WF_MODE_PSET))
                {
                    WE |= DEF_PLANE2;
                };
            };
        };
    }
    else
    {
        /* 320x200 */
        if (!(g_vramctrl.regWF_MODE & (1 << 2)))
        {
            if (g_vramctrl.regWF_PLANE & DEF_PLANE2 & avlb_plane_s)
            {

                /* SINGLE operace - vybrano II. */
                WE |= DEF_PLANE2;
            };
        }
        else if (avlb_plane & DEF_PLANE2)
        {

            /* REPLACE, PSET - vybrano II. */
            if (g_vramctrl.regWF_PLANE & DEF_PLANE2)
            {
                WE |= DEF_PLANE2;

                /* REPLACE, PSET - NEvybrano II. */
            }
            else if (g_vramctrl.regWF_MODE & (GDG_WF_MODE_REPLACE | GDG_WF_MODE_PSET))
            {
                WE |= DEF_PLANE2;

                /* 640x200, lichy bajt? */
            }
            else if (GDG_MZ800_DMD_TEST_SCRW640)
            {
                /* TODO: nejak si uz nevybavuju proc je to tady prazdne :(
                 *
                 *
                 *
                 *
                 *
                 *
                 */
            };
        };
    };

    if (DEF_USE_EXTVRAM)
    {
        if (!(GDG_MZ800_DMD_TEST_SCRW640 && addr_is_odd))
        {

            if (!(g_vramctrl.regWF_MODE & (1 << 2)))
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE3 & avlb_plane_s)
                {

                    /* SINGLE operace - vybrano III. */
                    WE |= DEF_PLANE3;
                };
            }
            else if (avlb_plane & DEF_PLANE3)
            {

                /* REPLACE, PSET - vybrano III. */
                if (g_vramctrl.regWF_PLANE & DEF_PLANE3)
                {
                    WE |= DEF_PLANE3;

                    /* REPLACE, PSET - NEvybrano III. */
                }
                else if (g_vramctrl.regWF_MODE & (GDG_WF_MODE_REPLACE | GDG_WF_MODE_PSET))
                {
                    WE |= DEF_PLANE3;
                };
            };
        };

        if (GDG_MZ800_DMD_TEST_SCRW640)
        {
            /* 640x200, lichy bajt? */
            if (addr_is_odd)
            {

                if (!(g_vramctrl.regWF_MODE & (1 << 2)))
                {
                    if (g_vramctrl.regWF_PLANE & DEF_PLANE3 & avlb_plane_s)
                    {

                        /* SINGLE operace - vybrano I. */
                        WE |= DEF_PLANE4;
                    };
                }
                else if (avlb_plane & DEF_PLANE3)
                {

                    /* REPLACE, PSET - vybrano III. */
                    if (g_vramctrl.regWF_PLANE & DEF_PLANE3)
                    {
                        WE |= DEF_PLANE4;

                        /* REPLACE, PSET - NEvybrano III. */
                    }
                    else if (g_vramctrl.regWF_MODE & (GDG_WF_MODE_REPLACE | GDG_WF_MODE_PSET))
                    {
                        WE |= DEF_PLANE4;
                    };
                };
            };
        }
        else
        {
            /* 320x200 */
            if (!(g_vramctrl.regWF_MODE & (1 << 2)))
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE4 & avlb_plane_s)
                {

                    /* SINGLE operace - vybrano IV. */
                    WE |= DEF_PLANE4;
                };
            }
            else if (avlb_plane & DEF_PLANE4)
            {

                /* REPLACE, PSET - vybrano IV. */
                if (g_vramctrl.regWF_PLANE & DEF_PLANE4)
                {
                    WE |= DEF_PLANE4;

                    /* REPLACE, PSET - NEvybrano IV. */
                }
                else if (g_vramctrl.regWF_MODE & (GDG_WF_MODE_REPLACE | GDG_WF_MODE_PSET))
                {
                    WE |= DEF_PLANE4;
                };
            };
        };
    };

    /******************************************************************************/
    /*		Nyni uz jsme pripraveni podle typu operace provest zapis do VRAM  */
    /******************************************************************************/

    if (WE & DEF_PLANE1)
    {

        switch (g_vramctrl.regWF_MODE)
        {

        case GDG_WF_MODE_SINGLE:
            wrvalue = value;
            break;

        case GDG_WF_MODE_EXOR:
            wrvalue = value ^ g_memoryVRAM_I[phy_vram_addr];
            break;

        case GDG_WF_MODE_OR:
            wrvalue = value | g_memoryVRAM_I[phy_vram_addr];
            break;

        case GDG_WF_MODE_RESET:
            wrvalue = (~value) & g_memoryVRAM_I[phy_vram_addr];
            break;

        case GDG_WF_MODE_REPLACE:
            if (g_vramctrl.regWF_PLANE & DEF_PLANE1)
            {
                wrvalue = value;
            }
            else
            {
                wrvalue = 0x00;
            };
            break;

        case GDG_WF_MODE_PSET:
            if (g_vramctrl.regWF_PLANE & DEF_PLANE1)
            {
                wrvalue = value | g_memoryVRAM_I[phy_vram_addr];
            }
            else
            {
                wrvalue = (~value) & g_memoryVRAM_I[phy_vram_addr];
            };
            break;
        };

        g_memoryVRAM_I[phy_vram_addr] = wrvalue;
    };

    if (WE & DEF_PLANE2)
    {

        switch (g_vramctrl.regWF_MODE)
        {

        case GDG_WF_MODE_SINGLE:
            wrvalue = value;
            break;

        case GDG_WF_MODE_EXOR:
            wrvalue = value ^ g_memoryVRAM_II[phy_vram_addr];
            break;

        case GDG_WF_MODE_OR:
            wrvalue = value | g_memoryVRAM_II[phy_vram_addr];
            break;

        case GDG_WF_MODE_RESET:
            wrvalue = (~value) & g_memoryVRAM_II[phy_vram_addr];
            break;

        case GDG_WF_MODE_REPLACE:
            if (GDG_MZ800_DMD_TEST_SCRW640)
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE1)
                {
                    wrvalue = value;
                }
                else
                {
                    wrvalue = 0x00;
                };
            }
            else
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE2)
                {
                    wrvalue = value;
                }
                else
                {
                    wrvalue = 0x00;
                };
            };
            break;

        case GDG_WF_MODE_PSET:
            if (GDG_MZ800_DMD_TEST_SCRW640)
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE1)
                {
                    wrvalue = value | g_memoryVRAM_II[phy_vram_addr];
                }
                else
                {
                    wrvalue = (~value) & g_memoryVRAM_II[phy_vram_addr];
                };
            }
            else
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE2)
                {
                    wrvalue = value | g_memoryVRAM_II[phy_vram_addr];
                }
                else
                {
                    wrvalue = (~value) & g_memoryVRAM_II[phy_vram_addr];
                };
            };
            break;
        };
        g_memoryVRAM_II[phy_vram_addr] = wrvalue;
    };

    if (WE & DEF_PLANE3)
    {

        switch (g_vramctrl.regWF_MODE)
        {

        case GDG_WF_MODE_SINGLE:
            wrvalue = value;
            break;

        case GDG_WF_MODE_EXOR:
            wrvalue = value ^ g_memoryVRAM_III[phy_vram_addr];
            break;

        case GDG_WF_MODE_OR:
            wrvalue = value | g_memoryVRAM_III[phy_vram_addr];
            break;

        case GDG_WF_MODE_RESET:
            wrvalue = (~value) & g_memoryVRAM_III[phy_vram_addr];
            break;

        case GDG_WF_MODE_REPLACE:
            if (g_vramctrl.regWF_PLANE & DEF_PLANE3)
            {
                wrvalue = value;
            }
            else
            {
                wrvalue = 0x00;
            };
            break;

        case GDG_WF_MODE_PSET:
            if (g_vramctrl.regWF_PLANE & DEF_PLANE3)
            {
                wrvalue = value | g_memoryVRAM_III[phy_vram_addr];
            }
            else
            {
                wrvalue = (~value) & g_memoryVRAM_III[phy_vram_addr];
            };
            break;
        };
        g_memoryVRAM_III[phy_vram_addr] = wrvalue;
    };

    if (WE & DEF_PLANE4)
    {

        switch (g_vramctrl.regWF_MODE)
        {

        case GDG_WF_MODE_SINGLE:
            wrvalue = value;
            break;

        case GDG_WF_MODE_EXOR:
            wrvalue = value ^ g_memoryVRAM_IV[phy_vram_addr];
            break;

        case GDG_WF_MODE_OR:
            wrvalue = value | g_memoryVRAM_IV[phy_vram_addr];
            break;

        case GDG_WF_MODE_RESET:
            wrvalue = (~value) & g_memoryVRAM_IV[phy_vram_addr];
            break;

        case GDG_WF_MODE_REPLACE:
            if (GDG_MZ800_DMD_TEST_SCRW640)
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE3)
                {
                    wrvalue = value;
                }
                else
                {
                    wrvalue = 0x00;
                };
            }
            else
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE4)
                {
                    wrvalue = value;
                }
                else
                {
                    wrvalue = 0x00;
                };
            };
            break;

        case GDG_WF_MODE_PSET:
            if (GDG_MZ800_DMD_TEST_SCRW640)
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE3)
                {
                    wrvalue = value | g_memoryVRAM_IV[phy_vram_addr];
                }
                else
                {
                    wrvalue = (~value) & g_memoryVRAM_IV[phy_vram_addr];
                };
            }
            else
            {
                if (g_vramctrl.regWF_PLANE & DEF_PLANE4)
                {
                    wrvalue = value | g_memoryVRAM_IV[phy_vram_addr];
                }
                else
                {
                    wrvalue = (~value) & g_memoryVRAM_IV[phy_vram_addr];
                };
            };
            break;
        };
        g_memoryVRAM_IV[phy_vram_addr] = wrvalue;
    };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /*
     * CDL recording - jen při aktivním debuggeru. WE bitmask přesně určuje,
     * do kterých plane byl zápis proveden (SINGLE/EXOR/OR/RESET/REPLACE/PSET
     * režimy + planar interleave v 640 modu jsou všechny zohledněny v WE).
     */
    if ( TEST_DEBUGGER_MHMAP_ACTIVE && g_dbg_in_cpu_path )
    {
        /* Recording: fyzická vram.cdl (dle WE = co se reálně zapsalo do VRAM bank)
         * + mode-specific (dle WF_PLANE = SW intent kam program chtěl psát). */
        mhmap_record_mz800_vram_write ( (uint8_t) WE, (uint8_t) g_vramctrl.regWF_PLANE,
                                        addr, phy_vram_addr );
    };
#endif

    return;
}
