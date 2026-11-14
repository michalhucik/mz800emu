/**
 * @file cmt_cli.h
 * @brief CLI volby virtuální kazety: --cmt, --cmt-play, --cmt-play-pause, --cmt-boot.
 *
 * - `--cmt <soubor>` vloží při startu pásku do virtuálního CMT (přípony
 *   přehrávatelných pásek cmtext_is_playable_filename(): tmz, tzx, mzf,
 *   m12, mzt, tap, wav, wave). Zapnutý CMT hack vypne jen pro běh
 *   (cmthack_disable_for_session) a v GUI otevře okno Virtual CMT.
 * - `--cmt-play` po vložení stiskne PLAY. Při bootu z ROM ji inicializace
 *   ROM (hrana PC3) přepne do PLAY + PAUSE; páska běží rovnou jen
 *   s --run-mzf.
 * - `--cmt-play-pause` připraví PLAY + PAUSE; pauzu zruší ROM, až zapne
 *   motor (náběžná hrana PC3 8255, pio8255.c). ROM MZ-700, MZ-800
 *   i MZ-1500 mají na 069Fh stejnou rutinu zapnutí motoru (BSR 06h/07h
 *   na 0E003h + test PC4). Bez pauzy uživatele je motor v emulátoru
 *   totéž co páska bez pauzy: hrana PC3 přepne pohyb pásky
 *   (cmt_motor_toggle(); samostatně se motor pamatuje jen při pauze
 *   uživatele, st_CMT::motor_off). Hranu PC3 vyrobí i inicializace ROM
 *   (0741h-0744h), proto se při bootu z ROM vkládá PLAY a do PLAY + PAUSE
 *   ji přepne až ROM; s --run-mzf (ROM se přeskočí) rovnou PLAY + PAUSE.
 * - `--cmt-boot` (jen MZ-800) = --cmt-play-pause a navíc drží od resetu
 *   klávesu C, dokud ROM nezapne motor: IPL v nabídce zvolí
 *   "C:cassette tape" a nahraje program z pásky.
 *
 * Průběh: main() volá cmt_cli_validate() před inicializací (chyba = konec),
 * EMU vlákno volá cmt_cli_apply() v mzarch_main() po resetu, každý snímek
 * cmt_cli_on_screen_done() a při resetu cmt_cli_on_reset().
 */

#ifndef CMT_CLI_H
#define CMT_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

    /**
     * @brief Počet snímků, po který --cmt-boot nejdéle drží klávesu C.
     *
     * Pojistka pro případ, že ROM motor nezapne (např. IPL dřív zavede
     * systém z diskety nebo QD). 500 snímků = 10 s při 50 Hz (MZ-800 PAL).
     */
#define CMT_CLI_BOOT_HOLD_MAX_FRAMES 500

    /**
     * @brief Ověří kombinaci CLI voleb virtuální kazety.
     *
     * Chyba (anglicky na stderr): soubor z --cmt neexistuje nebo nemá
     * příponu přehrávatelné pásky; --cmt-play, --cmt-play-pause nebo
     * --cmt-boot bez --cmt; --cmt-play spolu s --cmt-play-pause;
     * --cmt-boot spolu s --cmt-play nebo s --run-mzf; --cmt-boot mimo
     * MZ-800 build.
     *
     * @return true = volby jsou v pořádku (nebo žádná není zadaná);
     *         false = chyba, volající má skončit s EXIT_FAILURE.
     * @pre sdlapp_options_init() a sdlapp_options_validate() už proběhly.
     * @note Bez vedlejších efektů kromě výpisu na stderr; nezávisí na
     *       inicializaci emulátoru.
     */
    extern bool cmt_cli_validate ( void );

    /**
     * @brief Provede CLI volby virtuální kazety po resetu stroje.
     *
     * Bez --cmt nic nedělá. Jinak vypne CMT hack jen pro běh, vloží pásku
     * (cmt_open_file_by_extension), podle voleb stiskne PLAY nebo připraví
     * PLAY + PAUSE (při bootu z ROM přes PLAY, viz popis souboru) a u
     * --cmt-boot začne držet klávesu C. Chyba při otevření
     * souboru se vypíše na stderr a emulátor běží dál bez pásky.
     *
     * @pre EMU vlákno, mzarch_main() po mzarch_main_reset(), před hlavní
     *      smyčkou; cmt_init() a cmthack_init() proběhly; volby ověřilo
     *      cmt_cli_validate(); volající nedrží device_state_lock() (funkce
     *      ho bere kolem vložení pásky, UI vlákno už může kreslit).
     * @post Při --cmt-boot je C stisknutá ve virtuální matici klávesnice
     *       (g_pio8255.vkbd_matrix), dokud ji neuvolní cmt_cli_on_screen_done()
     *       nebo cmt_cli_on_reset().
     */
    extern void cmt_cli_apply ( void );

    /**
     * @brief Per-frame krok --cmt-boot: uvolní C, jakmile ROM zapne motor.
     *
     * Motor je zapnutý, když páska hraje bez pauzy (CMT_TEST_PLAY
     * && !CMT_TEST_PAUSED). Klávesa se pustí až při zapnutí motoru poté,
     * co byl motor aspoň jednou vypnutý (PLAY + PAUSE po inicializaci ROM),
     * takže nezáleží na tom, jestli první konec snímku přijde před
     * inicializací ROM, nebo po ní. Po CMT_CLI_BOOT_HOLD_MAX_FRAMES snímcích bez
     * zapnutí motoru klávesu uvolní a vypíše varování na stderr. Bez držené
     * klávesy jen jedno čtení příznaku.
     *
     * @pre EMU vlákno; volá se z callbacku konce snímku (mzarch.c). Mění jen
     *      bit virtuální matice klávesnice - žádné port I/O ani paměť.
     */
    extern void cmt_cli_on_screen_done ( void );

    /**
     * @brief Při resetu stroje uvolní klávesu C drženou volbou --cmt-boot.
     *
     * @pre EMU vlákno (mzarch_main_reset).
     * @post Klávesa C není volbou --cmt-boot držená.
     */
    extern void cmt_cli_on_reset ( void );

#ifdef __cplusplus
}
#endif

#endif /* CMT_CLI_H */
