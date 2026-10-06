/**
 * @file bootstrap.h
 * @brief Bootstrap `--run-mzf`: zavedení MZF ve stavu, v jakém ho spouští ROM.
 *
 * Společná část je v bootstrap.c, platformní kroky v
 * `mz<arch>/mz<arch>_bootstrap.c`. Kontrakt viz popis bootstrap.c.
 */

#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

#include <stdint.h>

/**
 * @brief Zavede MZF soubor a připraví CPU na jeho spuštění (CLI `--run-mzf`).
 *
 * Replikuje trvalé účinky ROM (reset -> IPL/monitor -> načtení z CMT) na
 * stav stroje, nahraje hlavičku na 10F0h a tělo na fstrt přes CMT hack a
 * nastaví registry tak, jak je ROM předává programu (SP = 10F0h, IM 1,
 * DI, platformní registry viz mzarch_platform_bootstrap_entry_regs()).
 *
 * @param filename Cesta k MZF souboru (UTF-8).
 *
 * @pre Volá se po mzarch_main_reset(), před první vykonanou instrukcí,
 *      z emulátorového vlákna.
 * @post PC = fexec; CPU zastavené před první instrukcí programu.
 *
 * Chování při chybě: v buildu bez debuggeru při nečitelném souboru zobrazí
 * chybu a ukončí emulátor (EXIT_FAILURE). S debuggerem chybu ohlásí CMT hack
 * (baseui_error), hlavička se nenahraje a bootstrap čte 10F0h-116Fh: 10F1h a
 * výš je vynulované inicializací monitoru, takže fsize = fstrt = fexec = 0.
 * Dolní ROM se odmapuje a CPU startuje na 0000h v RAM (ověřeno s
 * neexistujícím souborem: emulace běží dál z RAM, žádný program).
 */
extern void mzarch_bootstrap_run_mzf(const char *filename);

/**
 * @brief Replikuje ROM rutinu TIMST (0308h) volanou z IPL s A = 0, DE = 0.
 *
 * Naprogramuje CTC1 (mode 2, dělič na 1 s) a CTC2 (mode 0, 0A8C0h = 12 h)
 * stejnou sekvencí zápisů jako ROM, přečte E006h (LSB, MSB) jako ROM na
 * 0331h, čímž spotřebuje latch CTC2, a nastaví proměnné monitoru
 * 119Bh (AMPM) = 0 a 119Ch = 0F0h.
 *
 * @param ctc1_count Konečná předvolba CTC1 z ROM na 033Eh (MZ-800 3CFBh,
 *                   MZ-1500 3D54h; liší se podle frekvence vstupu CTC1).
 *
 * @pre Volá se jen na platformách, jejichž ROM TIMST při startu volá
 *      (MZ-800 a MZ-1500 IPL); monitor MZ-700 1Z-013A ji nevolá.
 *
 * Vedlejší efekty: zápisy do 8253 a čtení CTC2 (posun čtecího
 * přepínače LSB/MSB, zrušení latche), zápisy do RAM 119Bh, 119Ch.
 */
extern void mzarch_bootstrap_rom_timst(uint16_t ctc1_count);

/**
 * @brief Replikuje inicializaci pracovní oblasti monitoru a obrazovky z ROM.
 *
 * Společné pro MZ-700 (monitor 0070h-00A6h), MZ-800 (IPL E876h-E8A6h) a
 * MZ-1500 (IPL E843h-E873h): vynuluje 10F1h-11EFh, smaže znakovou VRAM
 * (00h), vyplní atributovou VRAM hodnotou 71h, zapíše trampolínu
 * 1038h = JP 038Dh, TEMPO (119Eh) = 4, provede MSTP (CTC0 mode 3, GATE0 = 0),
 * replikuje BEEP (0577h: RATIO 11A1h-11A2h, předvolba CTC0, GATE0 1 -> 0,
 * MSTP) a nastaví BPFLG (119Dh) = 1.
 *
 * @param text_vram  Ukazatel na textovou VRAM platformy: 2 KB znaků
 *                   (D000h) následovaných 2 KB atributů (D800h). Vlastnictví
 *                   zůstává volajícímu (globální pole paměti).
 * @param beep_count Předvolba CTC0 tónu A z tabulky not ROM (027Ch):
 *                   MZ-700 a MZ-800 04ECh, MZ-1500 03F8h.
 *
 * @pre Volá se z bootstrapu před nahráním hlavičky MZF. RAM se zapisuje přes
 *      memory_load_block() s MEMORY_LOAD_RAMONLY, tedy nezávisle na
 *      aktuálním mapování; VRAM přímo do pole `text_vram`.
 * @post 10F1h-11EFh je vynulované kromě proměnných monitoru uvedených výše;
 *       hlavička a tělo MZF nahrané potom je přepíšou.
 *
 * Vedlejší efekty: RAM, VRAM, 8253 (CTC0), GDG registr E008h.
 */
extern void mzarch_bootstrap_rom_monitor_init(uint8_t *text_vram, uint16_t beep_count);

/**
 * @brief Platformní bootstrap inicializace podle ROM dané platformy.
 *
 * Volá se po společných krocích (IM 1, @INI55, Z80 PIO), před nahráním
 * MZF hlavičky a těla. Nastaví load-time mapování paměti a v pořadí podle
 * ROM provede platformní kroky (MZ-800/MZ-1500: TIMST, ztišení PSG,
 * PC0 = 1; MZ-800: kopie CG-ROM do CG-RAM; MZ-1500: paleta a priorita)
 * a pracovní oblast monitoru přes mzarch_bootstrap_rom_monitor_init().
 *
 * Vedlejší efekty: `g_memory.map`, periferie a paměť podle platformy.
 */
extern void mzarch_platform_bootstrap_init(void);

/**
 * @brief Nastaví registry, které ROM předává spouštěnému programu.
 *
 * Volá se po nahrání těla MZF, před nastavením PC. Platformy podle ROM:
 *   - MZ-700 (monitor, příkaz L, 012Eh `JP (HL)`): HL = fexec,
 *   - MZ-800 (IPL ]GOCMT E99Dh, ]GOPGM ED41h `JP (IX)`): BC = 0100h
 *     (zařízení = CMT), IX = fexec,
 *   - MZ-1500 (IPL E9E6h-E9ECh `JP (HL)`): BC = 0100h, HL = fexec.
 *
 * @param fexec Exec adresa z hlavičky MZF.
 *
 * Vedlejší efekty: registry CPU (z80_set_reg()).
 */
extern void mzarch_platform_bootstrap_entry_regs(uint16_t fexec);

/**
 * @brief Nastaví kanonickou load-time memory map (= RAM na header bufferu
 *        0x10F0 i v dolní RAM), bez resetu PIO/CTC a bez CGROM kopie.
 *
 * Vyčleněno z `mzarch_platform_bootstrap_init()` (= jen řádek s
 * `g_memory.map = ...`). Slouží pro mid-session load (media_load_mzf),
 * kde nechceme destruktivní machine reset, ale potřebujeme map ve které
 * `cmthack_load_mzf_filename()` korektně zapíše hlavičku do RAM na 0x10F0
 * (= na MZ-800 je po resetu na 0x1000-0x1FFF mapovaná CG-ROM, viz
 * `MEMORY_MZ800_MAP_FLAG_ROM_1000`, takže MAPED zápis hlavičky by se
 * ztratil).
 *
 * Volající si typicky uloží `g_memory.map` před voláním a po dokončení
 * loadu ho obnoví, aby load neměl trvalý side effect na banking.
 *
 * Side effecty: pouze `g_memory.map`. Žádný PIO/CTC/GDG/CGROM zásah.
 */
extern void mzarch_platform_bootstrap_apply_load_map(void);

/**
 * @brief Odmapuje dolní ROM (0x0000-0x0FFF) pokud `fstrt < 0x1000`.
 *
 * Identické s "Bodem 1" v `mzarch_platform_bootstrap_post_header()`, ale
 * BEZ platform-specific GDG/video zásahů (= mz800 Bod 2 přepnutí DMD).
 * Určeno pro mid-session load (media_load_mzf), kde nechceme měnit video
 * mód běžícího programu, ale tělo MZF s `fstrt < 0x1000` se musí zapsat
 * do RAM místo pod ROM.
 *
 * Volá se PŘED `cmthack_read_mzf_body()`, na již nastavené load-time mapě
 * (`mzarch_platform_bootstrap_apply_load_map()`).
 *
 * @param fstrt cílová adresa programu (mzf_header.fstrt)
 * Side effecty: pouze `g_memory.map`.
 */
extern void mzarch_platform_load_prepare_body_map(uint16_t fstrt);

/**
 * @brief Platform-specific bootstrap úpravy podle načteného MZF headeru.
 *
 * Volá se po načtení MZF headeru do `0x10F0` a po jeho přečtení do
 * `mzf_header` struktury, ale PŘED nahráním těla MZF přes
 * `cmthack_read_mzf_body()`. Každá platforma sama rozhodne co upravit
 * v memory map / GDG state na základě cílové adresy programu.
 *
 * Společné chování napříč platformami:
 *   - Pokud `fstrt < 0x1000`, odmapovat dolní ROM (`MEMORY_*_MAP_FLAG_ROM_0000`),
 *     jinak by `cmthack_read_mzf_body()` zapisoval pod ROM. Pozn.: reálná
 *     ROM toto dělá jen když `fstrt == 0x0000` - naše bootstrap je víc
 *     vanilla (= mapping musí odpovídat skutečné cílové RAM).
 *
 * Platform-specific (MZ-800):
 *   - Pokud je nastavený MZ-800 mode (= backside switch S1=OFF), replikovat
 *     ]GOPGM (ECFCh): GDG DMD = 0 (320x200@4A, přes gdg_write_byte
 *     jako OUT CEh vč. GATE0 CTC0) a @BLACK (E8E1h: paleta
 *     PAL0-3 = 0, PALGRP = 0, border = 0), a odmapovat CG-RAM/VRAM
 *     v rozsahu 0x1000-0x1FFF + 0xC000-0xCFFF (= reset CGRAM_VRAM flag).
 *     V pozici MZ-700 switche ]GOPGM mód nemění (zůstává MZ-700).
 *
 * @param fstrt   cílová adresa programu (mzf_header.fstrt)
 */
extern void mzarch_platform_bootstrap_post_header(uint16_t fstrt);

#endif // BOOTSTRAP_H