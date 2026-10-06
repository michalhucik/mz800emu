/**
 * @file bootstrap.c
 * @brief Společná část bootstrapu `--run-mzf` (zkratka za zavedení MZF přes ROM).
 *
 * Kontrakt: `--run-mzf` je zkratka, která musí nahrát MZF do počítače ve stavu,
 * v jakém by ho spustila ROM (reset -> ROM/IPL -> načtení z CMT -> skok na
 * exec adresu). Bootstrap proto neprovádí ROM kód, ale replikuje jeho trvalé
 * účinky na stav stroje (registry CPU, 8255, 8253, Z80 PIO, PSG, GDG, VRAM,
 * pracovní oblast monitoru v RAM).
 *
 * Rozdělení:
 *   - zde: kroky shodné na všech platformách (IM 1, @INI55, inicializace
 *     pracovní oblasti monitoru, MSTP) a sdílené pomocné rutiny (TIMST),
 *   - `mz<arch>/mz<arch>_bootstrap.c`: platformní kroky v pořadí podle ROM
 *     dané platformy (IPL MZ-800 / MZ-1500, monitor 1Z-013A MZ-700).
 *
 * Adresy ROM v komentářích odkazují na ROM v `mz<arch>/memory/ROM/` (MZ-700
 * 1Z-013A, MZ-800 a MZ-1500 dolní ROM 0000h-0FFFh a horní ROM E000h-FFFFh).
 * Stav po ROM cestě byl změřen v emulátoru (exec breakpoint na exec adrese
 * MZF po zavedení z pásky s vypnutým CMT hackem) a porovnán se stavem po
 * bootstrapu.
 *
 * Zápisy přes gdg_write_byte() (E008h, paleta, border) a psg_write_byte()
 * jdou stejnou cestou jako OUT instrukce, takže mohou spustit HW event
 * breakpointy GDG (změna módu, palety, borderu) ještě před první instrukcí
 * programu. Na chování emulace to vliv nemá.
 *
 * Vědomě nereplikované zbytky ROM cesty (nemají vliv na chování programu):
 * hodnoty CPU registrů mimo předávací rozhraní, R registr, obsah zásobníku
 * pod 10F0h, pracovní proměnné páskového loaderu a klávesnice (mimo jiné
 * 11A0h po BEEP, význam [neověřeno]), text "IPL is loading" / "LOADING" na
 * obrazovce, poloha kurzoru po tomto textu, čítače CTC (běh času), kopie
 * programu na 1200h po relokaci v IPL MZ-800.
 */

#include "main.h"

#include "mzarch/mzarch_config.h"
#include "mzarch/bootstrap.h"
#include "libs/mzf/mzf_tools.h"
#include "hw-generic/gdg/gdg.h"
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

#include <string.h>


/** @brief Adresa RAM trampolíny přerušení (INTSRQ, RST 38h skáče na 1038h). */
#define BOOTSTRAP_RAM_INTSRQ        0x1038
/** @brief Cíl trampolíny INTSRQ - obsluha přerušení monitoru (hodiny, 038Dh). */
#define BOOTSTRAP_ROM_CLOCK_ISR     0x038d
/** @brief Začátek řídicího bloku CMT, který ROM nuluje (FNAME, 10F1h). */
#define BOOTSTRAP_RAM_CMT_BLOCK     0x10f1
/** @brief Počet nulovaných bajtů řídicího bloku CMT (rutina 0FD8h s B = 0FFh). */
#define BOOTSTRAP_RAM_CMT_BLOCK_LEN 0xff
/** @brief Proměnná AMPM monitoru (TIMST ukládá A). */
#define BOOTSTRAP_RAM_AMPM          0x119b
/** @brief Proměnná monitoru, do které TIMST ukládá 0F0h. */
#define BOOTSTRAP_RAM_119C          0x119c
/** @brief Proměnná BPFLG monitoru (1 = nepípat po stisku klávesy). */
#define BOOTSTRAP_RAM_BPFLG         0x119d
/** @brief Proměnná TEMPO monitoru (tempo melodií, ROM nastavuje 4). */
#define BOOTSTRAP_RAM_TEMPO         0x119e
/** @brief Předvolba CTC0 aktuálního tónu (11A1h-11A2h), kterou čte 02ABh. */
#define BOOTSTRAP_RAM_RATIO         0x11a1
/** @brief Atribut, kterým ROM vyplní atributovou VRAM (bílé znaky na modré). */
#define BOOTSTRAP_IMPATB            0x71
/** @brief Velikost znakové i atributové části textové VRAM (D000h a D800h). */
#define BOOTSTRAP_TEXT_VRAM_PART    0x800


/**
 * @brief Zapíše jeden bajt do RAM bez ohledu na aktuální mapování paměti.
 *
 * @param addr  Adresa v RAM.
 * @param value Zapisovaná hodnota.
 *
 * Vedlejší efekty: změna RAM (přes `memory_load_block()` s MEMORY_LOAD_RAMONLY).
 */
static void bootstrap_ram_write(uint16_t addr, uint8_t value)
{
    memory_load_block(&value, addr, 1, MEMORY_LOAD_RAMONLY);
}


void mzarch_bootstrap_rom_timst(uint16_t ctc1_count)
{
    /* TIMST (0308h) s A = 0, DE = 0 (volání z IPL MZ-800 E82Ch, MZ-1500 E81Ch):
     *   0308h: DI
     *   030Ch: LD (AMPM),A           ; A = 0
     *   030Fh: LD (119Ch),0F0h
     *   031Dh: E007h <- 74h, 0B0h    ; CTC1 mode 2, CTC2 mode 0 (LSB+MSB)
     *   0325h: E006h <- A8C0h - DE   ; CTC2 = A8C0h (12 h v sekundách)
     *   0328h: E005h <- 000Ah        ; CTC1 krátký start
     *   032Eh: E007h <- 80h          ; latch CTC2, čekání na načtení
     *   033Eh: E005h <- ctc1_count   ; CTC1 = 1 s (MZ-800 3CFBh, MZ-1500 3D54h)
     *   0350h: EI
     * EI na konci TIMST se do exec adresy neprojeví (IPL i páskový loader
     * dělají DI), proto se IFF nemění. */
    bootstrap_ram_write(BOOTSTRAP_RAM_AMPM, 0x00);
    bootstrap_ram_write(BOOTSTRAP_RAM_119C, 0xf0);

    ctc8253_write_byte(3, 0x74);
    ctc8253_write_byte(3, 0xb0);
    ctc8253_write_byte(2, 0xc0);
    ctc8253_write_byte(2, 0xa8);
    ctc8253_write_byte(1, 0x0a);
    ctc8253_write_byte(1, 0x00);
    ctc8253_write_byte(3, 0x80);
    /* 0331h-0338h: LD C,(HL) / LD A,(HL) z E006h - čtení LSB a MSB CTC2
     * spotřebuje latch z příkazu 80h. Bez toho by první čtení E006h
     * programem vrátilo zastaralou zachycenou hodnotu. */
    (void) ctc8253_read_byte(2);
    (void) ctc8253_read_byte(2);
    ctc8253_write_byte(1, (uint8_t)(ctc1_count & 0xff));
    ctc8253_write_byte(1, (uint8_t)(ctc1_count >> 8));
}


void mzarch_bootstrap_rom_monitor_init(uint8_t *text_vram, uint16_t beep_count)
{
    /* 0FD8h s B = 0FFh, HL = 10F1h: vynulování řídicího bloku CMT a pracovní
     * oblasti monitoru 10F1h-11EFh (MZ-700 0070h, MZ-800 E876h, MZ-1500 E843h).
     * Hlavička MZF se do 10F0h-116Fh nahraje až potom. */
    uint8_t zero[BOOTSTRAP_RAM_CMT_BLOCK_LEN];
    memset(zero, 0x00, sizeof(zero));
    memory_load_block(zero, BOOTSTRAP_RAM_CMT_BLOCK, sizeof(zero), MEMORY_LOAD_RAMONLY);

    /* PRNT 16h (CLS) a FILLA 71h na D800h (MZ-700 0078h-0082h, MZ-800
     * E87Eh-E888h, MZ-1500 E84Bh-E855h): znaková VRAM = 00h (mezera),
     * atributová VRAM = 71h. Kurzor (1171h) je po vynulování výše 0,0. */
    memset(&text_vram[0], 0x00, BOOTSTRAP_TEXT_VRAM_PART);
    memset(&text_vram[BOOTSTRAP_TEXT_VRAM_PART], BOOTSTRAP_IMPATB, BOOTSTRAP_TEXT_VRAM_PART);

    /* INTSRQ: 1038h = JP 038Dh (MZ-700 0085h, MZ-800 E88Bh, MZ-1500 E858h). */
    bootstrap_ram_write(BOOTSTRAP_RAM_INTSRQ, 0xc3);
    bootstrap_ram_write(BOOTSTRAP_RAM_INTSRQ + 1, (uint8_t)(BOOTSTRAP_ROM_CLOCK_ISR & 0xff));
    bootstrap_ram_write(BOOTSTRAP_RAM_INTSRQ + 2, (uint8_t)(BOOTSTRAP_ROM_CLOCK_ISR >> 8));

    /* TEMPO = 4 (MZ-700 0090h, MZ-800 E896h, MZ-1500 E863h). */
    bootstrap_ram_write(BOOTSTRAP_RAM_TEMPO, 0x04);

    /* MSTP (02BEh): E007h <- 36h (CTC0 mode 3, LSB+MSB), E008h <- 0 (GATE0). */
    ctc8253_write_byte(3, 0x36);
    gdg_write_byte(0xe008, 0x00);

    /* BEEP (0577h: DE = 0352h, RST 30h = melodie "A" ze 0352h). Trvalý
     * účinek: tabulka not (tón A na 027Bh, předvolba na 027Ch) -> RATIO
     * 11A1h-11A2h, 02ABh zapíše předvolbu do E004h (LSB, MSB) a E008h <- 1,
     * konec melodie MSTP (02BEh): E007h <- 36h, E008h <- 0. CTC0 tak drží
     * předvolbu tónu jako na ROM cestě. */
    bootstrap_ram_write(BOOTSTRAP_RAM_RATIO, (uint8_t)(beep_count & 0xff));
    bootstrap_ram_write(BOOTSTRAP_RAM_RATIO + 1, (uint8_t)(beep_count >> 8));
    ctc8253_write_byte(0, (uint8_t)(beep_count & 0xff));
    ctc8253_write_byte(0, (uint8_t)(beep_count >> 8));
    gdg_write_byte(0xe008, 0x01);
    ctc8253_write_byte(3, 0x36);
    gdg_write_byte(0xe008, 0x00);

    /* BPFLG = 1 (MZ-700 00A2h, MZ-800 E8A4h, MZ-1500 E871h). */
    bootstrap_ram_write(BOOTSTRAP_RAM_BPFLG, 0x01);
}


/**
 * @brief Inicializace stroje do stavu, ve kterém ROM spouští program z pásky.
 *
 * Kroky společné všem platformám (IM 1, @INI55, Z80 PIO u platforem s IPL),
 * potom platformní část `mzarch_platform_bootstrap_init()` (mapování paměti,
 * TIMST, PSG, PC0, paleta, CG-RAM, pracovní oblast monitoru, VRAM).
 *
 * @pre Emulátor je po resetu (mzarch_main_reset()), CPU ještě nevykonalo
 *      žádnou instrukci.
 * @post Stav odpovídá ROM těsně před načtením hlavičky z pásky (kromě
 *       zbytků vyjmenovaných v popisu souboru).
 *
 * Vedlejší efekty: zápisy do 8255, 8253, Z80 PIO, GDG, PSG, VRAM, RAM,
 * `g_memory.map`, IM CPU.
 */
static void mzarch_bootstrap_init(void)
{
    /* IM 1 (MZ-700 004Dh, MZ-800 E814h, MZ-1500 E813h). Po resetu Z80 je
     * IM 0; IFF1 = IFF2 = 0 zůstává (ROM skáčí na program s DI). Přímý zápis
     * jako při načtení snapshotu (snap_z80.c). */
    g_mzarch_main.cpu->im = 1;

    /* @INI55 (073Eh): E003h <- 8Ah (Mode Set), 07h (PC3 = 1), 05h (PC2 = 1). */
    pio8255_write(3, 0x8a);
    pio8255_write(3, 0x07);
    pio8255_write(3, 0x05);

#if HAVE_PIOZ80
    /* Z80 PIO (IPL MZ-800 E835h-E840h, MZ-1500 E8BEh-E8DFh): kanál A
     * bit mode, PA0-PA5 vstup, INT vypnuto; kanál B bit mode, vše výstup. */
    pioz80_write_byte(0, 0x00);
    pioz80_write_byte(0, 0xcf);
    pioz80_write_byte(0, 0x3f);
    pioz80_write_byte(0, 0x07);
    pioz80_write_byte(1, 0x00);
    pioz80_write_byte(1, 0xcf);
    pioz80_write_byte(1, 0x00);
    pioz80_write_byte(1, 0x07);
#endif

    /* Platformní kroky ROM (mapování, TIMST, PSG, PC0, paleta, CG-RAM,
     * pracovní oblast monitoru a VRAM přes mzarch_bootstrap_rom_monitor_init). */
    mzarch_platform_bootstrap_init();

    /* Zápisy přes psg_write_byte() a gdg_write_byte() synchronizují čas
     * jako I/O operace uvnitř instrukce (mzarch_main_insideop): posunou
     * g_gdg.total_elapsed.ticks, nastaví instruction_insideop_sync_ticks
     * a přidají CPU čekací stavy (PSG, wait_cycles i op_tstate). Bootstrap
     * běží mimo instrukci, takže
     * tyto rozpracované účty instrukce vynulujeme - jinak by první instrukce
     * programu dostala čekací stavy navíc. Posun času GDG zůstává (monotónní,
     * odpovídá několika µs před startem programu). */
    g_mzarch_main.instruction_insideop_sync_ticks = 0;
    g_mzarch_main.instruction_wait_tstates = 0;
    g_mzarch_main.cpu->wait_cycles = 0;
    g_mzarch_main.cpu->op_tstate = 0;
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

    /* Registry, které ROM předává programu (BC = zařízení u IPL, skokový
     * registr s exec adresou) - platformně podle ROM. */
    mzarch_platform_bootstrap_entry_regs(mzf_header.fexec);

    z80_set_reg(g_mzarch_main.cpu, Z80_REG_PC, mzf_header.fexec);

    g_print("Bootstrap done.\n");
}
