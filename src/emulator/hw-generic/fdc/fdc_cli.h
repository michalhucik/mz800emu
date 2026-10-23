/**
 * @file fdc_cli.h
 * @brief CLI volby disketových mechanik: --fdd, --fdd0 .. --fdd7, --fdd-readonly.
 *
 * Mechaniky se číslují přes oba řadiče: 0-3 = mechaniky 0-3 primárního
 * řadiče FDC0 (porty 0D8h-0DFh), 4-7 = mechaniky 0-3 sekundárního řadiče
 * FDC1 (porty 58h-5Fh).
 *
 * - `--fdd <N>=<soubor>` (opakovatelná) a `--fdd<N> <soubor>` vloží při
 *   startu DSK obraz do mechaniky N. Obě formy jsou rovnocenné. Vložení
 *   jde stejnou cestou jako z menu (fdc_mount_dskfile()): cesta se zapíše
 *   do INI (`<prefix>_fddX_dskpath`), pokud se INI při konci ukládá
 *   (`--no-save-ini` zápis vypne). Disk z INI v téže mechanice nahradí.
 * - `--fdd-readonly <N>` (opakovatelná) chrání mechaniku N proti zápisu
 *   jen pro tento běh (do INI se nezapíše); platí pro disk vložený z CLI
 *   i z INI a pro další disky vložené do téže mechaniky za běhu.
 *
 * Průběh: main() volá fdc_cli_validate() před inicializací emulátoru
 * (syntaxe, rozsah N, duplicitní mechanika, existence souboru; chyba =
 * konec). fdc_init() převezme cesty a příznaky přes fdc_cli_dsk_path()
 * a fdc_cli_readonly(); chyby, které jdou poznat až po načtení INI
 * a otevření obrazu (odpojený FDC1, obraz nejde otevřít), si zapamatuje
 * fdc_cli_note_failure() a emu vlákno je ohlásí přes fdc_cli_report()
 * v mzarch_main() žádostí o konec s chybou (automation_request_exit()).
 *
 * Výpisy pro uživatele jsou anglicky (jádro emulátoru bez lokalizace).
 */

#ifndef FDC_CLI_H
#define FDC_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/** @brief Počet mechanik adresovatelných z CLI (2 řadiče x 4 mechaniky). */
#define FDC_CLI_DRIVES 8

    /**
     * @brief Ověří CLI volby disketových mechanik a zapamatuje si je.
     *
     * Chyba (anglicky na stderr): `--fdd` bez `=` nebo s prázdnou cestou,
     * číslo mechaniky mimo 0-7, mechanika zadaná víckrát (i kombinací
     * `--fdd` a `--fdd<N>`), soubor neexistuje nebo není obyčejný soubor,
     * `--fdd-readonly` s číslem mimo 0-7.
     *
     * @return true = volby jsou v pořádku (nebo žádná není zadaná);
     *         false = chyba, volající má skončit (automation_cli_error()).
     * @pre sdlapp_options_init() a sdlapp_options_validate() už proběhly.
     * @post Při true drží modul cesty v UTF-8 a příznaky read-only pro
     *       fdc_cli_dsk_path() a fdc_cli_readonly() až do konce procesu.
     * @note Hlavní vlákno před startem emu vlákna; volat nejvýš jednou.
     */
    extern bool fdc_cli_validate ( void );

    /**
     * @brief Cesta k DSK obrazu z CLI pro mechaniku řadiče.
     *
     * @param fdc_index Index řadiče (FDC0 = 0, FDC1 = 1).
     * @param drive_id Mechanika řadiče 0-3.
     * @return Cesta v UTF-8 (vlastní modul), nebo NULL, když CLI pro
     *         mechaniku nic nezadal nebo jsou argumenty mimo rozsah.
     * @pre fdc_cli_validate() vrátilo true (jinak vždy NULL).
     */
    extern const char *fdc_cli_dsk_path ( unsigned fdc_index, unsigned drive_id );

    /**
     * @brief Je mechanika řadiče z CLI chráněná proti zápisu (--fdd-readonly)?
     *
     * @param fdc_index Index řadiče (FDC0 = 0, FDC1 = 1).
     * @param drive_id Mechanika řadiče 0-3.
     * @return true = chránit jen pro tento běh; false = bez volby nebo mimo rozsah.
     * @pre fdc_cli_validate() vrátilo true (jinak vždy false).
     */
    extern bool fdc_cli_readonly ( unsigned fdc_index, unsigned drive_id );

    /**
     * @brief Zapamatuje chybu CLI vložení disku zjištěnou při inicializaci.
     *
     * Zpráva se hned vypíše na stderr; konec běhu vyžádá až fdc_cli_report()
     * v emu vlákně. Uchová se první zpráva, další se jen vypíšou.
     *
     * @param message Anglický popis chyby (zkopíruje se).
     * @note Volá fdc_init() (před hlavní smyčkou emu vlákna).
     */
    extern void fdc_cli_note_failure ( const char *message );

    /**
     * @brief Ohlásí chybu CLI vložení disku žádostí o konec s chybou.
     *
     * Bez zapamatované chyby nedělá nic. Jinak zavolá
     * automation_request_exit() s kódem AUTOMATION_EXIT_ERROR a důvodem
     * `error`: v `--headless` bez MCP proces skončí, jinde emulace zůstane
     * v pauze se zprávou (viz automation.h).
     *
     * @return true = žádná chyba; false = konec vyžádán.
     * @pre Emu vlákno, mzarch_main() po resetu, před hlavní smyčkou.
     */
    extern bool fdc_cli_report ( void );

#ifdef __cplusplus
}
#endif

#endif /* FDC_CLI_H */
