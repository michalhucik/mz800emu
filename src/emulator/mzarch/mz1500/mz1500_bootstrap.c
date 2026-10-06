/**
 * @file mz1500_bootstrap.c
 * @brief Platformní část bootstrapu `--run-mzf` pro MZ-1500 (IPL 9Z-502M).
 *
 * Replikuje inicializaci IPL (E810h-E89Ah) a zavedení z pásky (volba C,
 * E974h-E9ECh). Oproti MZ-800 IPL navíc nastavuje paletu (E881h) a
 * prioritu (E87Dh) a nekopíruje CG-ROM (MZ-1500 nemá CG-RAM).
 */

#include "main.h"

#include "mzarch/mzarch_config.h"
#include "mzarch/mzarch.h"
#include "mzarch/bootstrap.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/psg/psg.h"
#include "memory/mz1500_memory.h"
#include "gdg/mz1500_gdg.h"
#include "libs/cpu-z80/z80.h"

/** @brief Předvolba CTC1 v TIMST MZ-1500 (033Eh: LD (HL),54h / LD (HL),3Dh). */
#define MZ1500_BOOTSTRAP_TIMST_CTC1  0x3d54

/** @brief Předvolba CTC0 tónu A z BEEP (MZ-1500 ROM, tabulka not 027Bh: 'A', 03F8h). */
#define MZ1500_BOOTSTRAP_BEEP_COUNT 0x03f8

void mzarch_platform_bootstrap_apply_load_map(void)
{
    /* Load-time map: ROM 0000 + horni ROM. 0x1000-0x1FFF je na MZ-1500
     * RAM, takze header buffer 0x10F0 je zapisovatelny. */
    g_memory.map = MEMORY_MZ1500_MAP_FLAG_ROM_0000 | MEMORY_MZ1500_MAP_FLAG_ROM_UPPER;
}

void mzarch_platform_bootstrap_init(void)
{
    mzarch_platform_bootstrap_apply_load_map ();

    /* E819h-E81Ch: TIMST s A = 0, DE = 0 (RTC = půlnoc). */
    mzarch_bootstrap_rom_timst ( MZ1500_BOOTSTRAP_TIMST_CTC1 );

    /* E82Fh-E837h: ztišení obou PSG - OUT (0E9h) 9Fh, 0BFh, 0DFh, 0FFh. */
    for ( uint8_t v = 0x9f; ; v += 0x20 )
    {
        psg_write_byte ( PSG_CH_RIGHT | PSG_CH_LEFT, v );
        if ( v == 0xff ) break;
    };

    /* E83Ah: E003h <- 01h (BSR PC0 = 1). PC0 je blokování tónového
     * výstupu 8253, aktivní v 0 (PC0 = 0 blokuje, PC0 = 1 propouští;
     * Michal 2026-10-03). Bez tohoto zápisu CTC_AUDIO_MASK (pio8255.h)
     * ztlumí zvuk CTC0. */
    pio8255_write ( 3, 0x01 );

    /* E83Fh: OUT (0E8h),80h - port 0E8h údajně patří volitelné rozšiřující
     * zvukové kartě MZ-1M08 Voice Board (hlasová deska) a bit 7 je reset
     * tohoto zařízení (Michal 2026-10-03, [neověřeno] v dokumentaci).
     * Emulátor MZ-1M08 neemuluje a port 0E8h na MZ-1500 neobsluhuje,
     * proto se zápis nereplikuje. Až bude deska emulovaná, má bootstrap
     * tento reset provést také. */

    /* E843h-E873h: pracovní oblast monitoru, CLS, atributy, 1038h, TEMPO,
     * MSTP, BEEP (E86Eh), BPFLG. Textová VRAM: g_memory.VRAM (D000h znaky,
     * D800h atributy). */
    mzarch_bootstrap_rom_monitor_init ( g_memory.VRAM, MZ1500_BOOTSTRAP_BEEP_COUNT );

    /* E881h-E88Ch: paleta OUT (0F1h) 00h, 11h, ..., 77h (barva i -> i).
     * Po resetu má emulátor všechny položky palety 0 (černá), takže bez
     * tohoto kroku je PCG grafika neviditelná. */
    for ( uint8_t i = 0; i < 8; i++ )
    {
        gdg_write_byte ( 0x00f1, (uint8_t) ( i * 0x11 ) );
    };

    /* E87Ch-E87Dh: OUT (0F0h),0 (priorita / DMD MZ-1500 = 0). */
    gdg_write_byte ( 0x00f0, 0x00 );

    /* E88Dh-E89Ah + stub na C000h: nulování PCG RAM (banky 1-3 přes
     * OUT (0E5h)) - PCG RAM je po resetu emulátoru už nulová (ověřeno
     * měřením), krok se nereplikuje. */
}

void mzarch_platform_load_prepare_body_map(uint16_t fstrt)
{
    /* Pokud cilova adresa programu je v rozsahu prvnich 4 KB, odmapujeme
     * ROM 0x0000-0x0FFF aby se telo MZF korektne ulozilo do RAM. Pozn.:
     * realna ROM toto chovani dela jen kdyz fstrt == 0x0000, ale nase
     * bootstrap je vic vanilkove (= mapping musi odpovidat skutecne
     * cilove RAM). */
    if (fstrt < 0x1000)
    {
        g_memory.map &= ~MEMORY_MZ1500_MAP_FLAG_ROM_0000;
    };
}

void mzarch_platform_bootstrap_post_header(uint16_t fstrt)
{
    mzarch_platform_load_prepare_body_map ( fstrt );
}

void mzarch_platform_bootstrap_entry_regs(uint16_t fexec)
{
    /* E9E6h: LD BC,0100h (zařízení = CMT), E9E9h: LD HL,(1106h), E9ECh: JP (HL). */
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_BC, 0x0100 );
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, fexec );
}
