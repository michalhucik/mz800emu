/**
 * @file mz800_bootstrap.c
 * @brief Platformní část bootstrapu `--run-mzf` pro MZ-800 (IPL 9Z-504M).
 *
 * Replikuje inicializaci IPL (E813h-E8B6h), zavedení z pásky (IPLCMT
 * E945h, ]GOCMT E99Dh) a předání řízení programu (]GOPGM ECFCh) včetně
 * přepnutí do MZ-800 módu podle zadního přepínače.
 */

#include "main.h"

#include <string.h>

#include "mzarch/mzarch_config.h"
#include "mzarch/mzarch.h"
#include "mzarch/bootstrap.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/psg/psg.h"
#include "memory/mz800_memory.h"
#include "gdg/mz800_gdg.h"
#include "libs/cpu-z80/z80.h"

/** @brief Předvolba CTC1 v TIMST MZ-800 (033Eh: LD (HL),0FBh / LD (HL),3Ch). */
#define MZ800_BOOTSTRAP_TIMST_CTC1  0x3cfb

/** @brief Offset textové VRAM režimu MZ-700 (D000h-DFFFh) v rovině I. */
#define MZ800_BOOTSTRAP_TEXT_VRAM_OFFSET 0x1000

/** @brief Předvolba CTC0 tónu A z BEEP (MZ-800 ROM, tabulka not 027Bh: 'A', 04ECh). */
#define MZ800_BOOTSTRAP_BEEP_COUNT 0x04ec

void mzarch_platform_bootstrap_apply_load_map(void)
{
    /* Load-time map: ROM na 0x0000-0x0FFF a 0xE000+, ale RAM na
     * 0x1000-0x1FFF (= ROM_1000/CG-ROM odmapováno), aby header buffer
     * 0x10F0 byl RAM a hlavička MZF se korektně zapsala. */
    g_memory.map = MEMORY_MZ800_MAP_FLAG_ROM_0000 | MEMORY_MZ800_MAP_FLAG_ROM_E000;
#ifdef MZ800EMU_CFG_RAM_FASTPATH
    mz800_ram_fastpath_rebuild ();
#endif
}

void mzarch_platform_bootstrap_init(void)
{
    mzarch_platform_bootstrap_apply_load_map ();

    /* E82Ch: TIMST s A = 0, DE = 0 (RTC = půlnoc). */
    mzarch_bootstrap_rom_timst ( MZ800_BOOTSTRAP_TIMST_CTC1 );

    /* E862h-E86Ah: ztišení PSG - OUT (0F2h) 9Fh, 0BFh, 0DFh, 0FFh
     * (útlum 0Fh na tónových kanálech 0-2 a šumu). */
    for ( uint8_t v = 0x9f; ; v += 0x20 )
    {
        psg_write_byte ( PSG_CH_RIGHT | PSG_CH_LEFT, v );
        if ( v == 0xff ) break;
    };

    /* E86Ch: E003h <- 01h (BSR PC0 = 1). PC0 je blokování tónového
     * výstupu 8253, aktivní v 0 (PC0 = 0 blokuje, PC0 = 1 propouští;
     * Michal 2026-10-03),
     * E871h: E003h <- 05h (BSR PC2 = 1, už nastaveno v @INI55). */
    pio8255_write ( 3, 0x01 );
    pio8255_write ( 3, 0x05 );

    /* E876h-E8A6h: pracovní oblast monitoru, CLS, atributy, 1038h, TEMPO,
     * MSTP, BEEP (E8A1h), BPFLG. Textová VRAM režimu MZ-700 je v rovině I
     * na 1000h. */
    mzarch_bootstrap_rom_monitor_init ( &g_memoryVRAM_I[ MZ800_BOOTSTRAP_TEXT_VRAM_OFFSET ],
                                        MZ800_BOOTSTRAP_BEEP_COUNT );

    /* E8A9h-E8B6h: monitor ROM kopíruje obsah CG-ROM (4 KB) do CG-RAM
     * (= prvních 4 KB VRAM Plane I, mapovaných v MZ-700 modu na
     * 0xC000-0xCFFF). Bez toho je CG-RAM po bootu prázdná a programy
     * v MZ-700 modu vidí prázdné znaky.
     *
     * CG-ROM v paměti: g_memory.ROM[0x1000-0x1FFF] (= addr & 0x3fff pro
     * bus 0x1000-0x1FFF, viz MEMORY_ROM_READ_BYTE makro). */
    memcpy ( g_memoryVRAM_I, &g_memory.ROM[ 0x1000 ], 0x1000 );
}

void mzarch_platform_load_prepare_body_map(uint16_t fstrt)
{
    /* Pokud cílová adresa programu je v rozsahu prvních 4 KB, odmapujeme
     * ROM 0x0000-0x0FFF aby se tělo MZF korektně uložilo do RAM. Pozn.:
     * reálná ROM toto chování dělá jen když fstrt == 0x0000, ale naše
     * bootstrap je víc vanilkové (= mapping musí odpovídat skutečné
     * cílové RAM). */
    if (fstrt < 0x1000)
    {
        g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_ROM_0000;
#ifdef MZ800EMU_CFG_RAM_FASTPATH
        mz800_ram_fastpath_rebuild ();
#endif
    };
}

void mzarch_platform_bootstrap_post_header(uint16_t fstrt)
{
    /* Bod 1 - společné pro všechny platformy: odmapování dolní ROM. */
    mzarch_platform_load_prepare_body_map ( fstrt );

    /* Bod 2 - MZ-800 specific: nastavit startovní mode podle zadního
     * přepínače SW1 (en_MZ800_MODE_SW) v g_mzarch_main.mode_sw.
     *
     * Společné napříč oběma stavy switche:
     *   - PROHIBITED = 0 (= banking mode není aktivní; pro jistotu clear)
     *   - ROM E000 mapped (= horní ROM dostupná; default už je v _init)
     */
    g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_E000;

    if (g_mzarch_main.mode_sw == MZ800_MODE_SW_MZ700)
    {
        printf ( "Bootstrap: mode switch = MZ-700 mode\n" );
        /* SW1 = MZ-700 mód: ]GOPGM (ECFCh) při DMD status
         * bit 1 = 0 mód nemění; DMD zůstává 08h z IPL (E816h). Po
         * resetu (gdg_reset) už DMD = 08h, zápis přes gdg_write_byte()
         * (cesta OUT CEh) je pak bez účinku; jinak proběhne se všemi
         * vedlejšími efekty GDG (CTC0 GATE0, framebuffer). */
        gdg_write_byte ( 0x00ce, 0x08 );
    }
    else
    {
        printf ( "Bootstrap: mode switch = MZ-800 mode\n" );
        /* SW1 = MZ-800 mód, ]GOPGM (ECFCh):
         *   ED02h: OUT (0CEh),0     - DMD = 0 (320x200@4A),
         *   ED05h: CALL @BLACK      - E8E1h: OUT (0F0h) 00h, 10h, 20h, 30h, 40h
         *                             (PAL0-3 = 0, PALGRP = 0) a E8EEh
         *                             OUT (06CFh),0 (border = 0).
         * DMD, paleta i border jdou přes gdg_write_byte() stejně jako
         * OUT instrukce. U DMD to znamená i vedlejší efekty GDG, které
         * přímý zápis g_gdg.regDMD vynechával: GATE0 CTC0 = 1 (800 mód,
         * ctc82530_on_regDMD_changed), vynulování latche zápisu MZ-700
         * (g_vramctrl.mz700_wr_latch_is_used) a aktualizace framebufferu.
         * VRAM/CG-RAM odpojené (clear CGRAM_VRAM + ROM_1000 flagy). */
        gdg_write_byte ( 0x00ce, 0x00 );
        static const uint8_t c_black[] = { 0x00, 0x10, 0x20, 0x30, 0x40 };
        for ( unsigned i = 0; i < sizeof ( c_black ); i++ )
        {
            gdg_write_byte ( 0x00f0, c_black[ i ] );
        };
        gdg_write_byte ( 0x06cf, 0x00 );
        g_memory.map &= ~( MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM |
                           MEMORY_MZ800_MAP_FLAG_ROM_1000 );
    }
#ifdef MZ800EMU_CFG_RAM_FASTPATH
    /* Bootstrap měnil g_memory.map přímo (CGRAM_VRAM / ROM_1000 až po
     * zápisu DMD, který fast-path přepočítal ještě se starou mapou)
     * -> přepočti fast-path. */
    mz800_ram_fastpath_rebuild ();
#endif
    /* SW1 = MZ-700 mód: ponechat default z _init.
     *   - DMD zůstává v MZ-700 mode (= bit 3 = 1, default po gdg_init).
     *   - VRAM se v MZ-700 mode připojuje automaticky s horní ROM
     *     (= ROM E000 implikuje VRAM D000-DFFF mapping, viz
     *     MEMORY_MZ700_MAP_TEST_VRAM_D000 makro). CGRAM_VRAM flag tu
     *     není relevantní - v 700 modu se 0xC000-0xCFFF mapuje
     *     automaticky s ROM E000 přes memory_internal_read_c000_cfff. */
}

void mzarch_platform_bootstrap_entry_regs(uint16_t fexec)
{
    /* ]GOCMT E99Dh: LD BC,100h (zařízení = magnetofon) -> přes EXX v BC
     * při skoku; ]GOPGM ED19h-ED41h: IX = start adresa, JP (IX). */
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_BC, 0x0100 );
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_IX, fexec );
}
