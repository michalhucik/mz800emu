/**
 * @file   automation.h
 * @brief  Běh ukončený podmínkou: návratové kódy, `result.json`, časové podmínky.
 *
 * Automatizace je pro CI a skripty bez MCP klienta: emulátor v `--headless`
 * skončí sám, když nastane podmínka, zapíše `result.json` a vrátí kód.
 *
 * Podmínky:
 * - časové v bodě konce snímku (automation_on_screen_done(), i v buildu bez
 *   debuggeru): `--exit-after-frames N` (kód 0), `--timeout-frames N`
 *   a `--timeout-seconds S` (kód 2, emulovaný čas zaokrouhlený nahoru na
 *   celé snímky);
 * - breakpointy s akcí `exit` (Action DSL), včetně breakpointů, které
 *   automatizace založí z voleb `--exit-on-pc`, `--fail-on-pc`,
 *   `--exit-on-write`, `--exit-on-out`, a sady z `--bpt FILE` (jen build
 *   s debuggerem).
 * Vyhrává první splněná podmínka; ve stejném snímku má přednost
 * `--exit-after-frames` před timeoutem.
 *
 * Průběh konce: automation_request_exit() jen zapamatuje žádost, nastaví
 * důvod pauzy @c EMU_PAUSE_REASON_EXIT a pozastaví emulaci (může běžet
 * uprostřed instrukce, např. z MEM_W breakpointu). Skutečný konec udělá
 * paused smyčka přes automation_on_paused(): screenshot a snapshot
 * (`--exit-screenshot`, `--exit-snapshot`), `result.json` a řízený konec
 * emulator_quit_with_code() s úplným úklidem.
 *
 * Kdy proces skončí (rozhodnutí M4 č. 9, R5): jen v `--headless`, a to bez
 * MCP serveru, nebo s MCP TCP serverem, když je zadaná některá
 * automatizační volba. V GUI, v `--mcp-pipe` a v headless s MCP TCP bez
 * automatizační volby zůstane emulace jen v pauze s hláškou (stderr,
 * MCP událost `paused` s `reason: "exit"`); `result.json` se zapíše, je-li
 * požadovaný.
 *
 * Návratové kódy (rozhodnutí M4 č. 1):
 *
 * | Kód | Význam |
 * |-----|--------|
 * | 0 | úspěch, doběhl `--exit-after-frames`, nebo `exit 0` |
 * | 1 | selhání testu (`--fail-on-pc`, `exit 1`) |
 * | 2 | timeout |
 * | 3 | chyba emulátoru nebo voleb (jen při zadané automatizační volbě; jinak neplatná volba dál vrací 1) |
 * | 4 | rozjetí přehrávaného záznamu (`--exit-at-replay-end`, replay.h) |
 * | 5-63 | rezerva |
 * | 64-255 | volné pro `exit` z programu |
 *
 * Vlákna: automation_cli_*() volá hlavní vlákno v main() před startem emu
 * vlákna; všechno ostatní jen emu vlákno.
 */

#ifndef AUTOMATION_H
#define AUTOMATION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @name Návratové kódy automatizace
 *  @{ */
#define AUTOMATION_EXIT_SUCCESS 0       /**< Úspěch. */
#define AUTOMATION_EXIT_FAILURE 1       /**< Selhání testu. */
#define AUTOMATION_EXIT_TIMEOUT 2       /**< Vypršel `--timeout-*`. */
#define AUTOMATION_EXIT_ERROR 3         /**< Chyba emulátoru nebo voleb. */
#define AUTOMATION_EXIT_REPLAY_DESYNC 4 /**< Přehrávání záznamu se rozjelo (`--exit-at-replay-end`, replay.h). */
/** @} */

    /**
     * @brief Důvod konce zapisovaný do `result.json` (pole `reason`).
     */
    typedef enum en_AUTOMATION_REASON
    {
        AUTOMATION_REASON_NONE = 0,    /**< Zatím žádný konec. */
        AUTOMATION_REASON_FRAMES,      /**< `frames`: doběhl `--exit-after-frames`. */
        AUTOMATION_REASON_TIMEOUT,     /**< `timeout`: vypršel `--timeout-*`. */
        AUTOMATION_REASON_EXIT_ACTION, /**< `exit_action`: akce `exit` breakpointu. */
        AUTOMATION_REASON_ERROR,       /**< `error`: chyba voleb, souboru nebo havárie. */
        AUTOMATION_REASON_QUIT,        /**< `quit`: konec zvenku (SIGINT, okno, MCP) nebo po --record-frames. */
        AUTOMATION_REASON_REPLAY_END,  /**< `replay_end`: přehrávání záznamu došlo na konec (`--exit-at-replay-end`). */
        AUTOMATION_REASON_REPLAY_DESYNC /**< `replay_desync`: přehrávání se rozjelo (kód AUTOMATION_EXIT_REPLAY_DESYNC). */
    } en_AUTOMATION_REASON;

    /**
     * @brief Co konec spustilo (pole `trigger` v `result.json`).
     *
     * Řetězce vlastní volající, automation_request_exit() si je zkopíruje.
     */
    typedef struct st_AUTOMATION_TRIGGER
    {
        int bp_id;           /**< ID breakpointu, -1 = konec nespustil breakpoint. */
        const char *bp_name; /**< Jméno breakpointu nebo NULL. */
        const char *bp_type; /**< Typ breakpointu (`PC_EXEC`, `MEM_W`, ...) nebo NULL. */
        uint64_t hits;       /**< Počet zásahů breakpointu včetně tohoto. */
    } st_AUTOMATION_TRIGGER;

    /**
     * @brief Příznak pro háček konce snímku: automatizace počítá snímky.
     *
     * Zapne ho automation_start(), když je zadaná některá automatizační
     * volba. Háček v mzarch.c ho testuje jednou za snímek.
     */
    extern bool g_automation_frame_hook;

    /**
     * @brief Je na příkazové řádce některá automatizační volba?
     *
     * Testuje jen přítomnost voleb (`--result-json`, `--automation-dir`,
     * `--exit-after-frames`, `--timeout-frames`, `--timeout-seconds`,
     * `--exit-on-pc`, `--fail-on-pc`, `--exit-on-write`, `--exit-on-out`,
     * `--bpt`, `--exit-screenshot`, `--exit-snapshot`), ne jejich platnost.
     *
     * @return true = automatizační režim (chyby voleb vracejí kód 3).
     * @pre sdlapp_options_init() už proběhl.
     */
    bool automation_cli_requested(void);

    /**
     * @brief Zkontroluje a zapamatuje automatizační volby (hlavní vlákno).
     *
     * Kontroluje formát čísel a specifikací (`ADDR[,skip=N][,code=C]` ...),
     * rozsahy, existenci `--bpt` a `--run-mzf` a že build má debugger pro
     * volby, které ho potřebují. Relativní cesty převede na absolutní vůči
     * aktuálnímu adresáři a založí `--automation-dir`. Chyby vypisuje
     * anglicky na stderr.
     *
     * @return true = volby jsou v pořádku (nebo žádné nejsou); false = chyba,
     *         volající má skončit přes automation_cli_error().
     * @pre sdlapp_options_init() a sdlapp_options_validate() proběhly.
     */
    bool automation_cli_validate(void);

    /**
     * @brief Běží automatický běh (zadaná automatizační volba)?
     *
     * Platí od automation_cli_validate() (hlavní vlákno před startem emu
     * vlákna) do konce procesu. V automatickém běhu se nenačítá ani
     * neukládá výchozí sada breakpointů (`.bpt`) a proměnných (`.vars`):
     * výsledek nesmí záviset na zbytcích z jiné relace, breakpointy jsou
     * jen z `--bpt` a `--exit-on-*` / `--fail-on-pc`.
     *
     * @return true = automatický běh.
     */
    bool automation_is_active(void);

    /**
     * @brief Konec procesu z main() kvůli chybě před startem emulace.
     *
     * V automatizačním režimu zapíše `result.json` s důvodem `error`
     * (jen základní pole, bez stavu stroje), pokud jde zjistit jeho cestu,
     * a vrátí @c AUTOMATION_EXIT_ERROR. Mimo automatizaci vrátí
     * @p default_code beze změny (zpětná kompatibilita).
     *
     * @param default_code Kód, který by main() vrátil bez automatizace.
     * @param message Anglický popis chyby do `result.json`.
     * @return Návratový kód pro main().
     */
    int automation_cli_error(int default_code, const char *message);

    /**
     * @brief Spustí automatizaci (emu vlákno, po aplikaci voleb v mzarch_main()).
     *
     * Vynuluje čítač snímků a čas, v buildu s debuggerem načte `--bpt`
     * a založí breakpointy z `--exit-on-*` / `--fail-on-pc` a vypne
     * automatické ukládání breakpointů a proměnných při konci (sada pro
     * test nesmí přepsat uživatelův soubor). Když založení selže, požádá
     * o konec s kódem 3.
     *
     * Bez automatizačních voleb nedělá nic.
     */
    void automation_start(void);

    /**
     * @brief Zapamatuje si kód zařízení, se kterým `--run-mzf` spustil program.
     *
     * Hodnoty jdou do `result.json` jako `inputs.run_mzf_device` (zápis
     * volby: `--run-mzf-device` normalizovaný, jinak nastavení
     * `[CMT] run_mzf_device`) a `inputs.run_mzf_bc` (BC programu). Bez
     * volání (bez `--run-mzf`) jsou obě pole null. Volá se i bez
     * automatizačních voleb (jen uloží hodnoty).
     *
     * @param text Zápis volby (zkopíruje se, NULL = nic).
     * @param bc   BC, se kterým program startuje.
     * @pre Emu vlákno (mzarch_main()), po mzarch_bootstrap_run_mzf().
     */
    void automation_note_run_mzf_device(const char *text, uint16_t bc);

    /**
     * @brief Háček konce snímku (emu vlákno, jen při g_automation_frame_hook).
     *
     * Volá se z mz800_main_event_callback_screen_done() po háčcích videa
     * a série screenshotů, takže poslední snímek je v záznamech. Počítá
     * snímky od automation_start() a vyhodnocuje časové podmínky.
     */
    void automation_on_screen_done(void);

    /**
     * @brief Žádost o konec běhu s kódem (emu vlákno, i uprostřed instrukce).
     *
     * Zapamatuje první žádost (další do vyřízení ignoruje), vypíše ji na
     * stderr, nastaví důvod pauzy @c EMU_PAUSE_REASON_EXIT, předá kód,
     * důvod a zprávu záznamu zastavení (run_control_note_exit()) a pozastaví
     * emulaci. Událost `paused` (`reason: "exit"`, `exit_code`,
     * `exit_reason`, `message`, `pc`) pošle v buildu s MCP záznam zastavení
     * na vstupu do pauzy. Vyřízení dělá automation_on_paused().
     *
     * Funguje i bez automatizačních voleb (akce `exit` breakpointu).
     *
     * @param code Návratový kód (0-255).
     * @param reason Důvod do `result.json`.
     * @param message Anglický popis nebo NULL.
     * @param trigger Co konec spustilo, nebo NULL.
     */
    void automation_request_exit(int code, en_AUTOMATION_REASON reason, const char *message,
                                 const st_AUTOMATION_TRIGGER *trigger);

    /**
     * @brief Vyřídí čekající žádost o konec (začátek paused smyčky, emu vlákno).
     *
     * Bez čekající žádosti nedělá nic. Jinak zapíše artefakty a `result.json`
     * a tam, kde smí proces skončit, zavolá emulator_quit_with_code() (pak
     * se nevrací). Jinde žádost uzavře a emulace zůstane v pauze.
     */
    void automation_on_paused(void);

    /**
     * @brief Běžný konec procesu (z emulator_quit(0), emu vlákno).
     *
     * V automatizačním režimu zapíše `result.json` s důvodem `quit`, pokud
     * už výsledek nezapsal řízený konec ani žádost v pauze.
     */
    void automation_on_quit(void);

    /**
     * @brief Havarijní konec (z emulator_quit(nenulový), emu vlákno).
     *
     * V automatizačním režimu zapíše `result.json` s důvodem `error`.
     *
     * @param exit_value Kód havarijního konce.
     * @return @c AUTOMATION_EXIT_ERROR v automatizačním režimu, jinak @p exit_value.
     */
    int automation_on_crash(int exit_value);

#ifdef __cplusplus
}
#endif

#endif /* AUTOMATION_H */
