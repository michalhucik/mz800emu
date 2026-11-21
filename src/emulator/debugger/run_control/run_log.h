/**
 * @file run_log.h
 * @brief Čísla běhů a kruh záznamů zastavení (čekání na zastavení přes MCP).
 *
 * Model:
 *   - **Běh** je úsek emulace od opuštění pauzy (rozběh nebo krok jedné
 *     instrukce) do dalšího vstupu do pauzy. Každý běh má číslo `run_seq`
 *     (monotónní od 0, 0 = běh od startu emulátoru). Číslo zvedá emu
 *     vlákno při opuštění pauzy (run_log_begin()).
 *   - **Záznam zastavení** (st_RUN_LOG_RECORD) zapíše emu vlákno na vstupu
 *     do pauzy (run_log_end()): důvod, PC, čas v taktech GDG, kdo a čím
 *     běh spustil. Kruh drží posledních RUN_LOG_CAPACITY záznamů.
 *   - **Čekání** (run_log_wait()) volá jiné vlákno (dispatch MCP): vrátí
 *     záznam, jakmile běh s daným číslem skončí, nebo po timeoutu.
 *
 * Modul je čisté C nad GLib bez globálního stavu emulátoru. Záznamy plní
 * run_control.c z hlavní smyčky emulace, testy je plní přímo.
 *
 * Vlákna: všechny funkce jsou thread-safe (jeden statický GMutex a GCond).
 * run_log_begin() a run_log_end() volá jen emu vlákno, ostatní libovolné.
 * Řetězce v záznamu (`reason`, `started_by`, ...) jsou statické literály,
 * kopie záznamu je proto bezpečná i po přepsání kruhu.
 */

#ifndef RUN_LOG_H
#define RUN_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

    /** @brief Počet záznamů zastavení v kruhu (nejstarší se přepisují). */
#define RUN_LOG_CAPACITY 32

    /** @brief Délka textového detailu záznamu včetně koncové nuly. */
#define RUN_LOG_DETAIL_SIZE 128

    /** @brief Nejdelší povolené čekání run_log_wait() [ms] (pod 30 s limitem wrapperu MCP). */
#define RUN_LOG_WAIT_MAX_MS 25000

    /**
     * @brief Jeden záznam zastavení běhu.
     *
     * Invarianty: `stop_ticks` a `start_ticks` jsou takty GDG od startu
     * emulátoru (gdg_get_total_ticks(), stejná hodnota jako `Cycle` ve
     * výrazech breakpointů; reset stroje je nenuluje, nahrání snapshotu je
     * nastaví na uloženou hodnotu); `elapsed_ticks` = `stop_ticks` -
     * `start_ticks`, 0 když čas během běhu klesl. Řetězcové členy nejsou
     * nikdy NULL (statické literály).
     */
    typedef struct st_RUN_LOG_RECORD
    {
        uint64_t run_seq;          /**< Číslo běhu, který skončil. */
        const char *reason;        /**< Důvod: "step", "run_to", "breakpoint", "frames",
                                        "deadline", "manual", "exit", "bp_action_saturation",
                                        "event_trigger", "replay_goto" (zastavení na cíli goto),
                                        "replay_goto_out_of_range" (goto zadané snímkem a takty,
                                        takt ležel za koncem snímku; zastavení v dalším snímku). */
        const char *started_by;    /**< Původ příkazu, který běh spustil: "user", "mcp",
                                        "test", "internal"; "user" i u rozběhu mimo frontu
                                        dbgapi (klávesa Pause v GUI). */
        const char *start_cmd;     /**< Příkaz, který běh spustil ("run", "run_to",
                                        "step_into", ...), "external" mimo frontu dbgapi,
                                        "startup" u běhu 0. */
        const char *stopped_by;    /**< U důvodu "manual" původ pauzy ("user", "mcp", ...),
                                        jinak "emulator". */
        bool step;                 /**< true = běh byl krok jedné instrukce. */
        uint16_t pc;               /**< PC v okamžiku zastavení (další instrukce). */
        int32_t bp_id;             /**< ID breakpointu u "breakpoint" a "bp_action_saturation", jinak -1. */
        uint64_t start_ticks;      /**< Takty GDG od startu emulátoru při spuštění běhu. */
        uint64_t stop_ticks;       /**< Takty GDG od startu emulátoru při zastavení. */
        uint64_t elapsed_ticks;    /**< Délka běhu v taktech GDG (stop_ticks - start_ticks). */
        uint32_t stop_frame;       /**< Počet dokončených snímků od startu emulátoru při zastavení. */
        int32_t exit_code;         /**< Návratový kód u důvodu "exit", jinak -1. */
        const char *exit_reason;   /**< Důvod žádosti o konec u "exit" ("frames", "timeout",
                                        "exit_action", ...), jinak "". */
        uint64_t bytes;            /**< Objem zápisů akcí u "bp_action_saturation", jinak 0. */
        char detail[RUN_LOG_DETAIL_SIZE]; /**< Doplňující text (zpráva akce exit, detail saturace), jinak "". */
        bool goto_out_of_range;    /**< Běh prováděl replay goto zadané snímkem a takty a cílový takt ležel na konci snímku nebo za ním (cíl neexistuje). Nezávisí na důvodu zastavení. */
    } st_RUN_LOG_RECORD;

    /** @brief Výsledek run_log_wait(). */
    typedef enum en_RUN_LOG_WAIT
    {
        RUN_LOG_WAIT_DONE = 0,     /**< Běh skončil (záznam je ve výstupu, nebo už vypadl z kruhu). */
        RUN_LOG_WAIT_TIMEOUT,      /**< Běh do timeoutu neskončil. */
        RUN_LOG_WAIT_SHUTDOWN      /**< Emulátor končí (run_log_shutdown()), čekání přerušeno. */
    } en_RUN_LOG_WAIT;

    /**
     * @brief Vyplní záznam výchozími hodnotami (prázdné řetězce, bp_id a exit_code -1).
     *
     * @param[out] rec Záznam k vyplnění (nesmí být NULL).
     */
    void run_log_record_clear(st_RUN_LOG_RECORD *rec);

    /**
     * @brief Začátek běhu: zvedne číslo běhu a označí emulaci jako běžící.
     *
     * @return Číslo nového běhu.
     * @pre Volá jen emu vlákno při opuštění pauzy.
     * @post run_log_current() hlásí běžící běh s vráceným číslem.
     */
    uint64_t run_log_begin(void);

    /**
     * @brief Konec běhu: uloží záznam do kruhu a probudí čekající.
     *
     * @param rec Záznam; @c rec->run_seq se přepíše číslem aktuálního běhu.
     * @pre Volá jen emu vlákno na vstupu do pauzy.
     * @post Běh je ukončený, run_log_wait() na jeho číslo vrátí DONE.
     */
    void run_log_end(const st_RUN_LOG_RECORD *rec);

    /**
     * @brief Číslo aktuálního (nebo posledního) běhu.
     *
     * @param[out] running Volitelně: true = běh právě probíhá (NULL = nezjišťovat).
     * @return Číslo běžícího běhu, v pauze číslo posledního skončeného.
     */
    uint64_t run_log_current(bool *running);

    /**
     * @brief Počká, až skončí běh s daným číslem.
     *
     * Vrátí hned, pokud běh @p run_seq už skončil. Číslo větší než aktuální
     * běh čeká na budoucí běh (do timeoutu).
     *
     * @param run_seq Číslo běhu, na jehož konec se čeká.
     * @param timeout_ms Nejdelší čekání, oříznuté na 0..RUN_LOG_WAIT_MAX_MS.
     * @param[out] out Záznam zastavení (jen při DONE a @p found true).
     * @param[out] found Volitelně: true = záznam byl v kruhu; false = běh
     *             skončil, ale záznam už kruh přepsal.
     * @return RUN_LOG_WAIT_DONE, RUN_LOG_WAIT_TIMEOUT nebo RUN_LOG_WAIT_SHUTDOWN.
     * @note Blokuje volající vlákno nejvýš @p timeout_ms. Nevolat z emu vlákna.
     */
    en_RUN_LOG_WAIT run_log_wait(uint64_t run_seq, int timeout_ms,
                                 st_RUN_LOG_RECORD *out, bool *found);

    /**
     * @brief Kopie posledních záznamů zastavení, od nejstaršího po nejnovější.
     *
     * @param[out] out Pole pro nejvýš @p max záznamů.
     * @param max Kapacita pole.
     * @return Počet zkopírovaných záznamů (nejnovější @p max ze záznamů v kruhu).
     */
    unsigned run_log_history(st_RUN_LOG_RECORD *out, unsigned max);

    /**
     * @brief Ukončení: probudí všechna čekání, další čekání vrací hned SHUTDOWN.
     */
    void run_log_shutdown(void);

    /**
     * @brief Vrátí modul do počátečního stavu (jen testy).
     *
     * @post Číslo běhu 0, běží, kruh prázdný, příznak ukončení shozený.
     */
    void run_log_reset_for_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* RUN_LOG_H */
