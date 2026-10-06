/**
 * @file   eventlog_trigger.h
 * @brief  Triggery Event Vieweru "Pause on match" a "Auto-mark on match".
 *
 * Trigger = filtr (@ref st_EVENTLOG_FILTER) vyhodnocovaný na každé nově
 * zapsané události v @ref eventlog_record(). Při shodě:
 *  - @ref EVENTLOG_TRIGGER_PAUSE požádá o pauzu emulace
 *    (@c emulator_pause(true), stejná cesta jako breakpointy) a zapamatuje
 *    si pozici shody pro indikátor "Last match" v okně Events,
 *  - @ref EVENTLOG_TRIGGER_AUTOMARK zapíše uživatelský marker
 *    (@c marklog_register() při první shodě pod daným jménem,
 *    pak @c marklog_record()).
 *
 * @section ownership Vlastnictví stavu
 *
 * Veškerý stav, který čtou callbacky (filtr, jméno markeru, ID markeru,
 * gate @ref g_eventlog_pause_trigger_active /
 * @ref g_eventlog_automark_trigger_active a ukazatele callbacků), vlastní
 * tento modul a mění ho výhradně emu vlákno: callbacky běží na emu vlákně
 * uvnitř @ref eventlog_record(), změny přicházejí z okna Events příkazy
 * fronty dbgapi (@c DBGAPI_CMD_EVENTLOG_TRIGGER_SET,
 * @c DBGAPI_CMD_EVENTLOG_TRIGGER_CLEAR_MATCHES), které vykoná emu vlákno
 * v drainu mezi instrukcemi. Výměna filtru tedy nikdy neproběhne uprostřed
 * vyhodnocení a starý filtr se uvolní až po návratu příkazu.
 *
 * UI vlákno si drží jen editační buffery (text filtru, text jména, stav
 * checkboxu) a stav zobrazení; počítadla shod čte přes
 * @ref eventlog_trigger_get_status() (jen pro zobrazení, viz tam).
 *
 * Dřívější stav (do ui-thread-writes): stav triggerů žil ve statice
 * okna Events, UI vlákno uvolňovalo filtr, který callback na emu vlákně
 * mohl právě vyhodnocovat (use-after-free), a ImGui editovalo jméno
 * markeru přímo v bufferu, který callback četl.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 *
 * Licence: GPLv3
 */

#ifndef EVENTLOG_TRIGGER_H
#define EVENTLOG_TRIGGER_H

#include <stdint.h>
#include <stdbool.h>

#include "eventlog.h"
#include "marklog.h"
#include "../eventlog_filter.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Druh triggeru.
 */
typedef enum en_EVENTLOG_TRIGGER_KIND {
    EVENTLOG_TRIGGER_PAUSE    = 0, /**< Pause on match - shoda pozastaví emulaci. */
    EVENTLOG_TRIGGER_AUTOMARK = 1, /**< Auto-mark on match - shoda zapíše uživatelský marker. */
    EVENTLOG_TRIGGER_COUNT    = 2, /**< Počet druhů (hranice validace, není druh). */
} en_EVENTLOG_TRIGGER_KIND;


/**
 * @brief Maximální délka jména markeru auto-mark triggeru včetně NUL.
 *
 * Shodné s @ref MARKLOG_NAME_MAX - delší jméno by marklog při registraci
 * stejně zkrátil.
 */
#define EVENTLOG_TRIGGER_NAME_MAX MARKLOG_NAME_MAX


/**
 * @brief Kopie stavu triggeru pro zobrazení v UI.
 *
 * Plní @ref eventlog_trigger_get_status(). Hodnoty jsou čtené bez zámku,
 * mohou být navzájem nekonzistentní (např. @c last_match_frame z jiné
 * shody než @c last_match_pxclk) - slouží jen k zobrazení.
 *
 * @field armed                 Trigger je aktivní (gate v eventlog_record
 *                              zapnutý, filtr nastavený).
 * @field has_match             Od posledního vymazání proběhla aspoň jedna
 *                              shoda (pause: zastavení, automark: marker).
 * @field last_match_frame      PAUSE: @c screens_total poslední shody.
 * @field last_match_pxclk      PAUSE: @c pxclk_in_screen poslední shody.
 * @field last_match_event_idx  PAUSE: logický index shody v ringu v okamžiku
 *                              shody (@c -1 = neplatný). Pozdější zápisy
 *                              a přetečení ringu ho mohou zneplatnit.
 * @field marker_id             AUTOMARK: ID markeru z marklog_register()
 *                              (@ref MARKLOG_INVALID_ID = zatím
 *                              neregistrováno nebo registrace selhala).
 * @field total_marks           AUTOMARK: počet zapsaných markerů od
 *                              posledního vymazání.
 */
typedef struct st_EVENTLOG_TRIGGER_STATUS {
    bool     armed;
    bool     has_match;
    uint32_t last_match_frame;
    uint32_t last_match_pxclk;
    int64_t  last_match_event_idx;
    uint16_t marker_id;
    uint64_t total_marks;
} st_EVENTLOG_TRIGGER_STATUS;


/**
 * @brief Ověří, zda filtr smí sloužit jako trigger.
 *
 * Trigger nepřijme filtr s chybou syntaxe ani filtr s temporálním uzlem
 * (vyhodnocení temporálních uzlů potřebuje kontext ringu, který callback
 * v eventlog_record nemá). Prázdný výraz ("match all") tato funkce
 * nerozpozná - to musí ověřit volající podle textu výrazu.
 *
 * @param f  Filtr z eventlog_filter_parse() nebo @c NULL.
 * @return @c true pokud je @p f nenulový, bez chyby a bez temporálního uzlu.
 *
 * Bez vedlejších efektů, filtr jen čte; smí volat libovolné vlákno, které
 * @p f vlastní.
 */
bool eventlog_trigger_filter_is_armable ( const st_EVENTLOG_FILTER *f );


/**
 * @brief Nastaví nebo vypne trigger (výměna filtru, jména a gate).
 *
 * Při @p filter != @c NULL převezme filtr do vlastnictví modulu, u
 * @ref EVENTLOG_TRIGGER_AUTOMARK zkopíruje @p name (zkrátí na
 * @ref EVENTLOG_TRIGGER_NAME_MAX - 1 znaků) a při změně jména zneplatní
 * ID markeru (další shoda zaregistruje marker pod novým jménem). Pak
 * zaregistruje callback modulu do eventlog vrstvy
 * (@ref g_eventlog_pause_callback / @ref g_eventlog_automark_callback)
 * a zapne gate. Při @p filter == @c NULL trigger vypne (gate 0, jméno
 * a ID markeru beze změny). Počítadla shod se nemění (maže je
 * @ref eventlog_trigger_clear_matches()).
 *
 * @param kind        Druh triggeru.
 * @param filter      Nový filtr nebo @c NULL (= vypnout). Musí splňovat
 *                    @ref eventlog_trigger_filter_is_armable().
 * @param name        AUTOMARK: neprázdné jméno markeru (při @p filter !=
 *                    @c NULL povinné). PAUSE: ignoruje se, smí být @c NULL.
 *                    Řetězec se kopíruje, vlastnictví zůstává volajícímu.
 * @param[out] out_old Při úspěchu dostane předchozí filtr (nebo @c NULL);
 *                    vlastnictví přechází na volajícího, který ho uvolní
 *                    eventlog_filter_free(). Smí být @c NULL jen pokud
 *                    volající ví, že předchozí filtr nebyl (jinak únik).
 * @return @c true při úspěchu. @c false pro neplatný @p kind, filtr
 *         nesplňující @ref eventlog_trigger_filter_is_armable() nebo
 *         chybějící / prázdné jméno u AUTOMARK; pak se nic nemění,
 *         @p filter zůstává volajícímu a @p *out_old se nemění.
 *
 * @pre Volat výhradně z emu vlákna (handler DBGAPI_CMD_EVENTLOG_TRIGGER_SET,
 *      testy se simulovaným emu vláknem), nikdy uvnitř callbacku triggeru.
 * @post Callback triggeru vidí od příští události nový filtr a jméno;
 *       předchozí filtr už modul nepoužívá.
 */
bool eventlog_trigger_set ( en_EVENTLOG_TRIGGER_KIND kind,
                            st_EVENTLOG_FILTER *filter,
                            const char *name,
                            st_EVENTLOG_FILTER **out_old );


/**
 * @brief Vymaže počítadla shod triggeru (tlačítko Clear v okně Events).
 *
 * PAUSE: @c has_match = false, @c last_match_event_idx = -1.
 * AUTOMARK: @c has_match = false, @c total_marks = 0. ID markeru zůstává
 * (marklog_register je pro stejné jméno idempotentní).
 *
 * @param kind  Druh triggeru.
 * @return @c false pro neplatný @p kind, jinak @c true.
 *
 * @pre Volat výhradně z emu vlákna (handler
 *      DBGAPI_CMD_EVENTLOG_TRIGGER_CLEAR_MATCHES); počítadla zapisuje
 *      callback na emu vlákně.
 */
bool eventlog_trigger_clear_matches ( en_EVENTLOG_TRIGGER_KIND kind );


/**
 * @brief Zkopíruje stav triggeru pro zobrazení.
 *
 * Čte bez zámku pole, která zapisuje emu vlákno (callback, set, clear).
 * Na podporovaných platformách (x86-64, zarovnaná pole) se jednotlivá pole
 * netrhají, ale kopie jako celek nemusí odpovídat jednomu okamžiku -
 * výsledek je jen informativní a nesmí řídit mutaci stavu emulátoru.
 * Nečte ukazatel filtru ani jméno markeru.
 *
 * @param kind      Druh triggeru.
 * @param[out] out  Cíl kopie; při neplatném @p kind nebo @p out == @c NULL
 *                  se nic nezapíše (při neplatném @p kind a nenulovém
 *                  @p out se vynuluje a @c marker_id = MARKLOG_INVALID_ID).
 *
 * Smí volat libovolné vlákno.
 */
void eventlog_trigger_get_status ( en_EVENTLOG_TRIGGER_KIND kind,
                                   st_EVENTLOG_TRIGGER_STATUS *out );


/**
 * @brief Vypne oba triggery a uvolní jejich filtry (ukončení emulátoru).
 *
 * Volá @c debugger_exit() před @ref eventlog_destroy(). Po návratu jsou
 * gate vypnuté a modul nedrží žádný filtr; počítadla a ID markerů se
 * vynulují. Opakované volání je no-op.
 *
 * @pre Volat z emu vlákna (debugger_exit běží v emulator_quit, které smí
 *      volat jen emu vlákno), mimo callback triggeru.
 */
void eventlog_trigger_shutdown ( void );


#ifdef __cplusplus
}
#endif

#endif /* EVENTLOG_TRIGGER_H */
