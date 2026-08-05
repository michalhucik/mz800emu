#include "main.h"

#include "mzarch/mzhal.h"
#include "mzarch/bootstrap.h"
#include "libs/mzf/mzf_tools.h"
#include "hw-generic/gdg/gdg_state.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/pioz80/pioz80.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/cmt/cmthack.h"
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#else
#include "baseui/baseui.h"
#include "emulator.h"
#endif
#include "libs/cpu-z80/z80.h"


static void mzarch_bootstrap_init(void)
{
    // 8255 init (master_port)
    pio8255_write(3, 0x8a);
    pio8255_write(3, 0x07);
    pio8255_write(3, 0x05);

    // CTC init
    ctc8253_write_byte(3, 0x74);
    ctc8253_write_byte(3, 0xb0);
    ctc8253_write_byte(2, 0xc0);
    ctc8253_write_byte(2, 0xa8);
    ctc8253_write_byte(1, 0xa0);
    ctc8253_write_byte(1, 0x00);
    ctc8253_write_byte(3, 0x80);
    ctc8253_write_byte(1, 0xfb);
    ctc8253_write_byte(1, 0x3c);

    // PIO init (jen platformy s PIO-Z80)
    if (g_mzhal.have_pioz80)
    {
        pioz80_write_byte(0, 0x00);
        pioz80_write_byte(0, 0xcf);
        pioz80_write_byte(0, 0x3f);
        pioz80_write_byte(0, 0x07);
        pioz80_write_byte(1, 0x00);
        pioz80_write_byte(1, 0xcf);
        pioz80_write_byte(1, 0x00);
        pioz80_write_byte(1, 0x07);
    }

    // Zavolame platform-specific bootstrap init, ktery muze nastavit mapovani pameti a podobne
    mzarch_platform_bootstrap_init();
}

void mzarch_bootstrap_run_mzf(const char *filename)
{
    printf("Bootstrapping...\n");

    mzarch_bootstrap_init();

    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, 0x10f0);
    cmthack_load_mzf_filename(filename);

    st_MZF_HEADER mzf_header;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    for (size_t i = 0; i < sizeof(st_MZF_HEADER); i++)
    {
        uint8_t *p = (uint8_t *)&mzf_header + i;
        *p = debugger_memory_read_byte(0x10f0 + i);
    };
#else
    FILE *f = baseui_tools_file_open(filename, "rb");
    if (!f)
    {
        baseui_show_error_message("Cannot open file ''%s'' for reading.", filename);
        emulator_quit(EXIT_FAILURE);
    };
    if (baseui_tools_file_read(&mzf_header, sizeof(st_MZF_HEADER), 1, f) != 1)
    {
        baseui_show_error_message("Cannot read header from file ''%s''.", filename);
        emulator_quit(EXIT_FAILURE);
    };
    baseui_tools_file_close(f);
#endif

    /* Post-header platform-specific úpravy mapování paměti. Musí být
     * PŘED cmthack_read_mzf_body() - jinak by tělo MZF zapisovalo přes
     * špatně mapovanou ROM (např. fstrt < 0x1000 by se neuložilo do RAM).
     *
     * Společné chování (= odmapování dolní ROM pro fstrt < 0x1000) +
     * platform-specific (= MZ-800 mode přepnutí na 320x200@4A) viz
     * deklaraci v bootstrap.h. */
    mzarch_platform_bootstrap_post_header(mzf_header.fstrt);

    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, mzf_header.fstrt);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_BC, mzf_header.fsize);
    cmthack_read_mzf_body();
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_SP, 0x10f0);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_PC, mzf_header.fexec);

    g_print("Bootstrap done.\n");
}
