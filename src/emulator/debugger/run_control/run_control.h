/**
 * @file run_control.h
 * @brief Řízení běhu z pohledu emu vlákna: čísla běhů, záznamy zastavení, deadline.
 *
 * Jediné místo, kde emulace zapíše "běh skončil, proč a kde" a vyšle MCP
 * události `paused` a `resumed`:
 *
 *   - run_control_on_stop() na vstupu do pauzy (mzzarch_main_do_emulator_paused()
 *     v mzarch.c, i po každém kroku): sestaví záznam ze `g_emulator.pause_reason`
 *     a detailů, které zdroj pauzy předal (run_control_note_*()), uloží ho
 *     (run_log.h) a vyšle právě jednu událost `paused`.
 *   - run_control_on_resume() při opuštění pauzy (rozběh nebo krok): zvedne
 *     číslo běhu, vynuluje důvod pauzy a u rozběhu (ne u kroku) vyšle
 *     událost `resumed`.
 *   - run_control_after_cmd() po každém příkazu fronty dbgapi: zapamatuje
 *     si, který příkaz a odkud emulaci rozběhl nebo pozastavil.
 *
 * **Deadline** omezuje běh v emulovaném čase. Kontroluje se v per-event
 * bloku hlavní smyčky (ne na instrukci), stejně jako dřívější frame-bounded
 * run: emulace se zastaví mezi instrukcemi v prvním bodě událostí na nebo
 * za deadlinem, nejpozději na konci snímku. `frames` se zastaví přesně na
 * hranici snímku. Deadline platí do nejbližší pauzy (run_control_on_stop()
 * ho zruší). Čas deadlinu běží i přes reset stroje (čítač času GDG reset
 * nenuluje, viz run_control_mono_ticks()).
 *
 * Vlákna: všechny funkce volá jen emu vlákno (mezi instrukcemi nebo
 * v pauze). Z jiných vláken se výsledky čtou jen přes run_log.h.
 *
 * Modul je jen v buildu s debuggerem.
 */

#ifndef RUN_CONTROL_H
#define RUN_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

    /** @brief Druh hodnoty deadlinu pro run_control_deadline_arm(). */
    typedef enum en_RUN_DEADLINE_KIND
    {
        RUN_DEADLINE_FRAMES = 0,   /**< Počet hranic snímků od teď (zastaví přesně na hranici). */
        RUN_DEADLINE_EMULATED_MS,  /**< Milisekundy emulovaného času (takt GDG platformy). */
        RUN_DEADLINE_TICKS         /**< Takty GDG. */
    } en_RUN_DEADLINE_KIND;

    /** @brief Nejvyšší povolený deadline ve snímcích. */
#define RUN_DEADLINE_MAX_FRAMES 1000000u

    /** @brief Nejvyšší povolený deadline v ms emulovaného času (24 h). */
#define RUN_DEADLINE_MAX_MS 86400000u

    /**
     * @brief Vstup do pauzy: zapíše záznam zastavení a vyšle událost `paused`.
     *
     * Důvod bere z `g_emulator.pause_reason`; bez nastaveného důvodu je to
     * "step" (dokončený krok) nebo "manual". Zruší deadline a frame-bounded
     * run (oba platí jen do pauzy) a zahodí detaily předané run_control_note_*().
     *
     * @pre Emu vlákno, vstup do mzzarch_main_do_emulator_paused() před
     *      debugger_step_call(0) (příznak kroku ještě platí).
     * @post run_log hlásí běh jako skončený, čekající run_log_wait() se probudí.
     */
    void run_control_on_stop(void);

    /**
     * @brief Opuštění pauzy: nové číslo běhu, vynulovaný důvod pauzy, událost `resumed`.
     *
     * Událost `resumed` se vysílá jen u rozběhu, ne u kroku jedné instrukce.
     *
     * @pre Emu vlákno, konec paused smyčky (rozběh nebo vyžádaný krok).
     * @post `g_emulator.pause_reason` == EMU_PAUSE_REASON_NONE.
     */
    void run_control_on_resume(void);

    /**
     * @brief Zaznamená, který příkaz fronty dbgapi rozběhl nebo pozastavil emulaci.
     *
     * Rozběh = emulace byla v pauze a po příkazu běží nebo má vyžádaný krok.
     * Pozastavení = běžela a po příkazu stojí; bez důvodu od příkazu se doplní
     * EMU_PAUSE_REASON_MANUAL a původ pauzy.
     *
     * @param cmd Příkaz bez příznaku BLOCKING (en_DBGAPI_CMD).
     * @param origin Původ příkazu (en_DBGAPI_CMD_ORIGIN).
     * @param was_paused Emulace byla v pauze před příkazem.
     * @param was_step Příznak kroku byl nastavený před příkazem.
     * @pre Emu vlákno, hned po dbgapi_emu_dispatch() příkazu.
     */
    void run_control_after_cmd(unsigned cmd, int origin, bool was_paused, bool was_step);

    /**
     * @brief Předá ID breakpointu, který právě zastavuje emulaci (důvod "breakpoint").
     *
     * @param bp_id ID breakpointu.
     * @pre Emu vlákno, před vstupem do pauzy.
     */
    void run_control_note_breakpoint(int bp_id);

    /**
     * @brief Předá detail auto-pauzy po saturaci zápisů akcí breakpointů.
     *
     * @param bp_id ID breakpointu, jehož akce překročila limit.
     * @param bytes Celkový objem zapsaných bajtů.
     * @param detail Text pro záznam a událost (zkopíruje se, NULL = bez textu).
     * @pre Emu vlákno, před vstupem do pauzy.
     */
    void run_control_note_saturation(int bp_id, uint64_t bytes, const char *detail);

    /**
     * @brief Předá detail žádosti o konec běhu (důvod "exit").
     *
     * @param code Návratový kód.
     * @param exit_reason Statický řetězec důvodu ("frames", "timeout", ...).
     * @param message Zpráva (zkopíruje se, NULL = bez zprávy).
     * @pre Emu vlákno, před vstupem do pauzy.
     */
    void run_control_note_exit(int code, const char *exit_reason, const char *message);

    /**
     * @brief Emulovaný čas v taktech GDG (gdg_get_total_ticks()).
     *
     * @return Takty GDG od startu emulátoru. Reset stroje čítač nenuluje
     *         (nuluje ho jen gdg_init()); nahrání snapshotu ho nastaví na
     *         uloženou hodnotu (snapshot nese čas stroje).
     * @pre Emu vlákno.
     */
    uint64_t run_control_mono_ticks(void);

    /**
     * @brief Nastaví deadline běhu relativně k teď.
     *
     * @param kind Druh hodnoty.
     * @param value Hodnota (1..RUN_DEADLINE_MAX_FRAMES snímků, 1..RUN_DEADLINE_MAX_MS ms,
     *        ticks >= 1).
     * @param[out] out_ticks Volitelně: délka deadlinu v taktech GDG od teď.
     * @return false = neplatná hodnota, deadline se nezměnil.
     * @pre Emu vlákno (příkaz fronty dbgapi). Platí do nejbližší pauzy.
     */
    bool run_control_deadline_arm(en_RUN_DEADLINE_KIND kind, uint64_t value, uint64_t *out_ticks);

    /**
     * @brief Zruší deadline (např. když příkaz, pro který byl nastavený, selhal).
     *
     * @pre Emu vlákno.
     */
    void run_control_deadline_clear(void);

    /**
     * @brief Nastaví zastavení na čase: běh se zastaví na začátku první
     *        instrukce, jejíž čas GDG je >= @p ticks.
     *
     * Kontrolu provádí breakpoints_enforce_global() (run_control_stop_at_check())
     * před každou instrukcí; bránu GLOBAL breakpointů v hlavní smyčce drží
     * zapnutou bptmap_set_global_hook(), takže horká cesta nedostane žádnou
     * novou podmínku. Po přijetí přerušení se zastaví až před první instrukcí
     * obsluhy (stav po přijetí).
     *
     * @param ticks  Cílový čas v taktech GDG (gdg_get_total_ticks()).
     * @param reason Důvod pauzy po dosažení (en_EMU_PAUSE_REASON, typicky
     *               EMU_PAUSE_REASON_RUN_UNTIL_TICKS).
     * @pre Emu vlákno (příkaz fronty dbgapi, replay). Platí do nejbližší
     *      pauzy: emulator_pause(true) ho zruší (run_control_stop_at_clear()),
     *      ať pauzu způsobilo dosažení cíle, breakpoint nebo uživatel.
     * @post g_debugger.stop_at_active == 1.
     */
    void run_control_stop_at_arm(uint64_t ticks, int reason);

    /**
     * @brief Zruší zastavení na čase (no-op, když není nastavené).
     *
     * @pre Emu vlákno.
     * @post g_debugger.stop_at_active == 0, brána GLOBAL odpovídá jen
     *       uživatelským GLOBAL breakpointům.
     */
    void run_control_stop_at_clear(void);

    /**
     * @brief Kontrola zastavení na čase před instrukcí.
     *
     * Při dosaženém cíli zruší zastavení, nastaví důvod pauzy a pozastaví
     * emulaci (instrukce na aktuálním PC se nevykoná).
     *
     * @return true = emulace se právě pozastavila.
     * @pre Emu vlákno, před vykonáním instrukce (breakpoints_enforce_global()).
     */
    bool run_control_stop_at_check(void);

    /**
     * @brief Kontrola frame-bounded runu a deadlinu v per-event bloku hlavní smyčky.
     *
     * Splněná podmínka nastaví důvod pauzy (FRAMES / DEADLINE) a pozastaví
     * emulaci; hlavní smyčka hned vstoupí do pauzy.
     *
     * @pre Emu vlákno, po mzarch_main_process_events(); volat jen když je
     *      aktivní frame-bounded run nebo deadline (g_debugger.run_frames_active
     *      | g_debugger.run_deadline_active).
     */
    void run_control_check_run_bounds(void);

    /**
     * @brief Číslo běhu, který spustí příkaz zpracovávaný právě teď.
     *
     * V pauze číslo příštího běhu (aktuální + 1), za běhu číslo běžícího.
     *
     * @return Číslo běhu.
     * @pre Emu vlákno (handler příkazu fronty dbgapi).
     */
    uint64_t run_control_seq_for_start(void);

#ifdef __cplusplus
}
#endif

#endif /* RUN_CONTROL_H */
