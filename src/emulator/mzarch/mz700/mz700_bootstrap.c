/**
 * @file mz700_bootstrap.c
 * @brief Platformní část bootstrapu `--run-mzf` pro MZ-700 (monitor 1Z-013A).
 *
 * Replikuje studený start monitoru 1Z-013A (004Ah-00A6h) a příkaz L
 * (0111h-012Eh). Monitor MZ-700 na rozdíl od IPL MZ-800/MZ-1500 nevolá
 * TIMST (CTC1/CTC2 zůstávají neprogramované), nenastavuje PC0 a nemá PSG
 * ani Z80 PIO.
 */

#include "main.h"

#include "mzarch/mzarch_config.h"
#include "mzarch/mzarch.h"
#include "mzarch/bootstrap.h"
#include "hw-generic/memory/memory.h"
#include "memory/mz700_memory.h"
#include "libs/cpu-z80/z80.h"

/** @brief Předvolba CTC0 tónu A z BEEP (MZ-700 ROM, tabulka not 027Bh: 'A', 04ECh). */
#define MZ700_BOOTSTRAP_BEEP_COUNT 0x04ec

void mzarch_platform_bootstrap_apply_load_map(void)
{
    /* Default banking: ROM 0000 + ROM E000 mapped, Prohibited NEAKTIVNI
     * (= ROM E800/code a F000 monitor viditelne). 0x1000-0x1FFF je na
     * MZ-700 RAM, takze header buffer 0x10F0 je zapisovatelny. */
    g_memory.map = MEMORY_MZ700_MAP_FLAG_ROM_0000 | MEMORY_MZ700_MAP_FLAG_ROM_E000;
}

void mzarch_platform_bootstrap_init(void)
{
    mzarch_platform_bootstrap_apply_load_map ();

    /* Monitor 1Z-013A: 0070h vynulování 10F1h-11EFh, 0078h CLS, 007Dh
     * atributy 71h, 0085h trampolína 1038h, 0090h TEMPO, 0095h MSTP, 009Fh BEEP,
     * 00A2h BPFLG. Textová VRAM MZ-700: g_memory.VRAM (D000h znaky,
     * D800h atributy). */
    mzarch_bootstrap_rom_monitor_init ( g_memory.VRAM, MZ700_BOOTSTRAP_BEEP_COUNT );
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
        g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_ROM_0000;
    };
}

void mzarch_platform_bootstrap_post_header(uint16_t fstrt)
{
    mzarch_platform_load_prepare_body_map ( fstrt );
}

void mzarch_platform_bootstrap_entry_regs(uint16_t fexec)
{
    /* Příkaz L monitoru: 0126h LD HL,(1106h) ... 012Eh JP (HL). */
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, fexec );
}
