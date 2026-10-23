/**
 * @file device_request.h
 * @brief Fronta zásahů GUI do zařízení, provádí je emu vlákno.
 *
 * Menu a okna zařízení (FDC, QDisk, RAM disk, MemExt, IDE8, Unicard,
 * tiskárna, plotter, ROM, zadní DIP přepínač, Allow PSG1, joystick,
 * kazeta) dřív měnila stav stroje přímo z UI vlákna souběžně s emulací:
 * uvolňovala a realokovala buffery, zavírala soubory, četla data do paměti
 * rozšíření, přepisovala ROM. Emu vlákno přitom mohlo stejná data právě
 * používat.
 *
 * Nově UI zásah jen zařadí (device_request_submit(): funkce a kopie
 * argumentů) a provede ho emu vlákno v device_request_poll(): v per-event
 * bloku hlavní smyčky (CPU stojí mezi instrukcemi) a v obou paused
 * smyčkách v mzarch.c, tedy i v buildu bez debuggeru. UI na provedení
 * nečeká; nový stav ukáže nejpozději za jeden snímek.
 *
 * Při záznamu nebo přehrávání vstupů (replay_is_active()) emu vlákno zásah
 * neprovede a vrátí DEVICE_REQ_REJECTED_REPLAY: záznam by změnu nezopakoval.
 * Kontrola běží v emu vlákně v okamžiku provedení, takže ji nemůže obejít
 * start záznamu mezi zařazením a provedením.
 *
 * Zámek stavu zařízení (device_state_lock()): emu vlákno drží zámek po
 * dobu provádění zásahů, UI ho drží při kreslení menu a oken zařízení,
 * která čtou řetězce a buffery zařízení (cesty k obrazům, připojení).
 * Emu vlákno zámek jen zkouší (g_mutex_trylock): když ho UI právě drží,
 * zásahy počkají do dalšího volání poll. Emu vlákno tak na UI nikdy
 * nečeká; UI čeká nejvýš po dobu zásahů provedených v jednom pollu (zápis
 * souboru RAM disku, homing plotteru apod.).
 *
 * Modul je bez lokalizačních maker (jádro); hlášení pro uživatele skládá
 * UI z výsledků (device_request_take_result()).
 */

#ifndef DEVICE_REQUEST_H
#define DEVICE_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Délka textového argumentu zásahu (včetně nuly), stačí na cestu. */
#define DEVICE_REQUEST_STR_MAX 1024

/** @brief Počet celočíselných argumentů zásahu. */
#define DEVICE_REQUEST_INT_ARGS 4

/** @brief Nejvyšší počet nevyzvednutých výsledků; starší se zahodí. */
#define DEVICE_REQUEST_RESULTS_MAX 32

/**
 * @brief Argumenty zásahu, kopírují se při zařazení.
 *
 * Význam položek určuje funkce zásahu. Struktura nevlastní žádnou paměť,
 * takže ji lze kopírovat přiřazením.
 *
 * @invariant `str` je vždy ukončený nulou (device_request_args_set_str()).
 */
typedef struct st_DEVICE_REQ_ARGS {
    int  i[DEVICE_REQUEST_INT_ARGS];   /**< Celočíselné argumenty (index zařízení, mechanika, volba). */
    char str[DEVICE_REQUEST_STR_MAX];  /**< Textový argument (cesta, režim), "" = žádný. */
} st_DEVICE_REQ_ARGS;

/**
 * @brief Funkce zásahu, volá ji emu vlákno.
 *
 * @param args Kopie argumentů ze zařazení (platná jen během volání).
 * @return 0 = provedeno; záporná hodnota = chyba zásahu (význam podle
 *         funkce, hlášení pro uživatele obvykle ukáže zařízení samo).
 * @pre Emu vlákno drží zámek stavu zařízení; replay neběží.
 */
typedef int (*device_request_fn_t)(const st_DEVICE_REQ_ARGS *args);

/**
 * @brief Stav vyřízeného zásahu.
 */
typedef enum en_DEVICE_REQ_STATUS {
    DEVICE_REQ_DONE = 0,         /**< Funkce zásahu proběhla, `rc` je její návrat. */
    DEVICE_REQ_REJECTED_REPLAY,  /**< Neprovedeno: běží záznam nebo přehrávání vstupů. */
} en_DEVICE_REQ_STATUS;

/**
 * @brief Výsledek vyřízeného zásahu.
 *
 * @invariant `seq` odpovídá návratu device_request_submit() pro tentýž
 *            zásah; `name` je ukazatel ze zařazení (statický řetězec).
 */
typedef struct st_DEVICE_REQ_RESULT {
    uint32_t             seq;     /**< Pořadové číslo zásahu (od 1). */
    const char          *name;    /**< Název zásahu pro log a testy (statický). */
    en_DEVICE_REQ_STATUS status;  /**< Provedeno, nebo odmítnuto. */
    int                  rc;      /**< Návrat funkce zásahu (jen DEVICE_REQ_DONE, jinak 0). */
} st_DEVICE_REQ_RESULT;

/**
 * @brief Vynuluje argumenty a nastaví textový argument.
 *
 * @param args Argumenty (nesmí být NULL).
 * @param str  Text (smí být NULL = ""); delší se ořízne na
 *             DEVICE_REQUEST_STR_MAX - 1 bajtů.
 * @return false, když se text nevešel celý (argumenty jsou i tak platné).
 * @post `args->i` jsou nulové, `args->str` je ukončený nulou.
 */
bool device_request_args_set_str(st_DEVICE_REQ_ARGS *args, const char *str);

/**
 * @brief Zařadí zásah pro emu vlákno.
 *
 * @param name Název zásahu (statický řetězec, ukládá se jen ukazatel).
 * @param fn   Funkce zásahu (nesmí být NULL).
 * @param args Argumenty, zkopírují se; smí být NULL (= nulové argumenty).
 * @return Pořadové číslo zásahu (od 1); 0 při `fn` == NULL (nezařazeno).
 *
 * @note Libovolné vlákno (typicky UI). Nečeká na provedení.
 * @post Při nenulovém návratu zásah čeká ve frontě; výsledek přijde přes
 *       device_request_take_result().
 */
uint32_t device_request_submit(const char *name, device_request_fn_t fn,
                               const st_DEVICE_REQ_ARGS *args);

/**
 * @brief Provede čekající zásahy (emu vlákno).
 *
 * Bez čekajícího zásahu jedno atomické čtení. Jinak zkusí zámek stavu
 * zařízení; když ho drží UI, vrátí se a zásahy provede další volání.
 * Zásahy se provádějí v pořadí zařazení; při aktivním replay se
 * neprovedou (DEVICE_REQ_REJECTED_REPLAY). Výsledek každého zásahu se
 * zařadí do fronty výsledků.
 *
 * @pre Emu vlákno mezi instrukcemi (per-event blok nebo paused smyčka);
 *      volající nedrží zámek stavu zařízení ani zámek fronty.
 * @note Vedlejší efekty podle funkcí zásahů.
 */
void device_request_poll(void);

/**
 * @brief Vyzvedne nejstarší výsledek vyřízeného zásahu.
 *
 * @param out Výstup (nesmí být NULL).
 * @return true = výsledek vyzvednut; false = žádný (`out` beze změny).
 * @note Libovolné vlákno (typicky UI každý snímek).
 */
bool device_request_take_result(st_DEVICE_REQ_RESULT *out);

/**
 * @brief Počet zásahů, které emu vlákno ještě nedokončilo.
 * @return Počet čekajících a právě prováděných zásahů (atomické čtení).
 */
int device_request_pending(void);

/**
 * @brief Zahodí čekající zásahy i nevyzvednuté výsledky.
 *
 * Pro ukončení emulátoru a testy; pořadové číslo se nenuluje.
 * @note Libovolné vlákno.
 */
void device_request_clear(void);

/**
 * @brief Zamkne stav zařízení pro čtení z UI (blokující).
 *
 * UI ho drží při kreslení menu a oken zařízení, aby emu vlákno mezitím
 * neprovedlo zásah, který mění čtené řetězce a buffery. Zámek není
 * rekurzivní.
 *
 * @pre Volající zámek nedrží a během držení nečeká na emu vlákno
 *      (synchronní příkaz dbgapi by mohl čekat na emu vlákno, které čeká
 *      na tento zámek v jiné cestě než device_request_poll()).
 */
void device_state_lock(void);

/**
 * @brief Odemkne stav zařízení (pár k device_state_lock()).
 * @pre Volající zámek drží.
 */
void device_state_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_REQUEST_H */
