/**
 * @file cmt_request.h
 * @brief Zásahy GUI do kazety přes emu vlákno (device_request.h).
 *
 * Menu CMT, okno Virtual CMT, seznam bloků pásky a zadní DIP přepínač dřív
 * volaly přímo z UI vlákna funkce, které otevírají pásku (uvolní předchozí
 * obraz a jeho bloky), přegenerují stream bloku (rychlost, vlastní pulzy),
 * přepisují ROM patchem CMT hacku nebo mění polaritu streamu, zatímco
 * emu vlákno pásku čte. Tyto funkce zásah jen zařadí; provede ho emu
 * vlákno v device_request_poll() (při aktivním replay ho odmítne, záznam
 * by změnu nezopakoval; GUI tyto prvky při záznamu i tak zamyká).
 *
 * Transport (play, stop, pauza, výběr bloku, nahrávání, vysunutí) jde dál
 * přes replay_host_cmt(), který zásah zapíše do záznamu vstupů.
 * Režim načítání okna Virtual CMT (CMT hack a CPU Boost dohromady) jde
 * zásahem cmt_request_set_loading_mode(), položky hlavního menu Use CMT
 * Hack a Enable CPU Boost týmž zásahem (cmt_request_set_loading_hack(),
 * cmt_request_set_loading_boost()). Položka Enable MZF Size Check zůstává
 * přímá: mění jen jeden příznak a stav kazety nepřestavuje. Pravidla
 * režimu načítání a Load directly jsou jen v cmt_loading_mode_apply()
 * a cmt_load_directly_apply(); volá je emu vlákno přímo (zásahy této
 * fronty a dbgapi pro nástroje MCP emu_cmt_hack_set a emu_cmt_load_directly).
 *
 * Úpravy pásky (smazání a přesun bloků, import souboru jako bloků)
 * nesou čítač výměn pásky, kterou uživatel viděl (*_on_tape()), a výsledek
 * pro obnovu výběru v okně zapíšou do st_CMT_REQUEST_EDIT_RESULT
 * (cmt_request_get_edit_result()); export celé pásky do WAV pásku nemění.
 *
 * Nástroje nad páskou (kopie pulzů surového bloku pro analýzu,
 * uplatnění analýzy, zápis informací o pásce, převod profilu bloku) jdou
 * stejnou cestou a stejným kruhem výsledků úprav; kopii pulzů si okno
 * vyzvedne cmt_request_take_pulses(). Čtení informací o pásce a kontrolu
 * pásky okno nezařazuje: čte je pod device_state_lock() (cmt_view.h).
 *
 * Všechny funkce, které zásah zařazují: libovolné vlákno (typicky UI),
 * nečekají na provedení, vrací pořadové číslo zásahu
 * (device_request_submit()), 0 = nezařazeno. Výjimky (čtení výsledku
 * úpravy, kódování seznamu bloků, čítač zahozených operací okna) mají
 * kontrakt u sebe.
 */

#ifndef CMT_REQUEST_H
#define CMT_REQUEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_request.h"
#include "libs/mztape/mztape.h"
#include "cmt_tmz_analyze.h"
#include "cmt_tmz_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zařadí otevření pásky ze souboru (cmt_open_file_by_extension()).
 * @param path Cesta k souboru (nesmí být NULL ani prázdná; delší než
 *        DEVICE_REQUEST_STR_MAX - 1 bajtů = nezařadí se).
 * @param play true = po úspěšném otevření spustit přehrávání (cmt_play()).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 * @note Návrat zásahu (st_DEVICE_REQ_RESULT.rc): 0 = otevřeno, -1 =
 *       nepodařilo se (chybu hlásí kazeta sama).
 */
uint32_t cmt_request_open(const char *path, bool play);

/**
 * @brief Zařadí uložení vložené pásky (cmttmz_save()) bez kontroly pásky.
 *
 * Emu vlákno uloží pásku, která je vložená v době provedení; nekontroluje,
 * že je to táž páska, kterou uživatel viděl při zadání (mezitím ji mohlo
 * vyměnit MCP, záznam vstupů nebo návrat na checkpoint). Okno Virtual CMT
 * proto používá cmt_request_save_on_tape(); tato funkce nemá v GUI
 * volající a zůstává kvůli zpětné slučitelnosti.
 *
 * @param path Cíl (UTF-8); NULL nebo prázdný = dosavadní cesta pásky. Delší
 *        než DEVICE_REQUEST_STR_MAX - 1 bajtů = nezařadí se.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 * @note Návrat zásahu (st_DEVICE_REQ_RESULT.rc): 0 = uloženo, -1 = chyba
 *       (text v cmttmz_get_save_message()).
 */
uint32_t cmt_request_save(const char *path);

/** @brief Návrat zásahu s kontrolou pásky (*_on_tape()): vložená páska je jiná, nic se neprovedlo. */
#define CMT_REQUEST_RC_TAPE_CHANGED (-2)

/**
 * @brief Jako cmt_request_save(), ale jen nad páskou, kterou uživatel viděl.
 *
 * Okno Virtual CMT ukládá pásku z dialogu Save as (asynchronní výběr
 * souboru) nebo z dialogu neuložených změn; mezitím mohlo MCP, záznam
 * vstupů nebo návrat na checkpoint pásku vyměnit a do souboru zvoleného
 * pro pásku A by se uložila páska B. Emu vlákno porovná @p tape_serial
 * s cmttmz_get_tape_serial() a při neshodě nic neuloží (návrat
 * CMT_REQUEST_RC_TAPE_CHANGED; cesta ani příznak Modified pásky se nemění).
 *
 * @param tape_serial Čítač výměn pásky (cmttmz_get_tape_serial()) v době, kdy uživatel uložení zadal.
 * @param path Cíl (jako cmt_request_save()).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 * @note Návrat zásahu: 0 = uloženo, -1 = chyba (text v cmttmz_get_save_message()),
 *       CMT_REQUEST_RC_TAPE_CHANGED = jiná páska, neuloženo.
 */
uint32_t cmt_request_save_on_tape(unsigned tape_serial, const char *path);

/**
 * @brief Zařadí export bloku vložené pásky (cmttmz_export_block()).
 * @param block_id Index bloku (záporný = nezařadí se).
 * @param path Cíl (.mzf, .m12, .wav; nesmí být NULL ani prázdný, nejvýš
 *        DEVICE_REQUEST_STR_MAX - 1 bajtů).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 * @note Návrat zásahu: 0 = exportováno, -1 = chyba (text v cmttmz_get_save_message()).
 */
uint32_t cmt_request_export_block(int block_id, const char *path);

/**
 * @brief Jako cmt_request_export_block(), ale jen nad páskou, kterou uživatel viděl.
 *
 * Okno Virtual CMT si číslo bloku pamatuje přes asynchronní dialog souboru;
 * mezitím mohlo MCP, záznam vstupů nebo návrat na checkpoint pásku vyměnit.
 * Emu vlákno porovná @p tape_serial s cmttmz_get_tape_serial() a při
 * neshodě nic neprovede (návrat CMT_REQUEST_RC_TAPE_CHANGED).
 *
 * @param tape_serial Čítač výměn pásky (st_CMT_VIEW::tape_serial) v době volby bloku.
 * @param block_id Index bloku. @param path Cíl (jako cmt_request_export_block()).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 */
uint32_t cmt_request_export_block_on_tape(unsigned tape_serial, int block_id, const char *path);

/**
 * @brief Zařadí změnu výchozí rychlosti Virtual CMT (cmt_change_speed()).
 * @param cmtspeed Poměr en_CMTSPEED (ne CMTSPEED_CUSTOM, neplatná hodnota =
 *        nezařadí se); vlastní pulzy nastaví cmt_request_set_custom_pulses().
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t cmt_request_set_speed(int cmtspeed);

/**
 * @brief Zařadí výchozí vlastní pulzy Virtual CMT (cmt_change_custom_pulses()).
 * @param pulses Délky pulzů (nesmí být NULL); předají se přesně (textem
 *        v šestnáctkovém zápisu double).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (`pulses` je NULL).
 */
uint32_t cmt_request_set_custom_pulses(const st_MZTAPE_PULSES_LENGTH *pulses);

/**
 * @brief Zařadí změnu rychlosti jednoho bloku pásky (cmt_tape_change_block_speed()).
 * @param block_id Index bloku v kontejneru (záporný = nezařadí se).
 * @param cmtspeed CMTSPEED_NONE = výchozí rychlost, poměr, nebo
 *        CMTSPEED_CUSTOM (pak je @p pulses povinné).
 * @param is_mz true = blok MZ formátu (rychlost DEFAULT/SET), false = jiný
 *        formát (jen rychlost).
 * @param pulses Vlastní pulzy pro CMTSPEED_CUSTOM, jinak NULL.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t cmt_request_set_block_speed(int block_id, int cmtspeed, bool is_mz,
                                     const st_MZTAPE_PULSES_LENGTH *pulses);

/**
 * @brief Jako cmt_request_set_block_speed(), ale jen nad páskou, kterou uživatel viděl.
 *
 * Při jiném čítači výměn pásky (cmttmz_get_tape_serial()) emu vlákno nic
 * nezmění a vrátí CMT_REQUEST_RC_TAPE_CHANGED (editor vlastních pulzů je
 * otevřený libovolně dlouho).
 *
 * @param tape_serial Čítač výměn pásky v době volby bloku.
 * @param block_id Index bloku. @param cmtspeed Rychlost. @param is_mz Blok MZ. @param pulses Pulzy pro CMTSPEED_CUSTOM.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 */
uint32_t cmt_request_set_block_speed_on_tape(unsigned tape_serial, int block_id, int cmtspeed, bool is_mz,
                                             const st_MZTAPE_PULSES_LENGTH *pulses);

/**
 * @brief Zařadí zapnutí nebo vypnutí CMT hacku (cmthack_load_rom_patch()) bez kontroly STOP.
 *
 * GUI ji nepoužívá (hlavní menu jde přes cmt_request_set_loading_hack(),
 * který hack mění jen ve STOP); zůstává kvůli zpětné slučitelnosti.
 *
 * @param enabled true = patch ROM nainstalovat.
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t cmt_request_set_hack(bool enabled);

/**
 * @brief Zařadí volbu Fix FNAME Terminator CMT hacku.
 * @param enabled true = opravovat zakončení jména souboru.
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t cmt_request_set_fix_fname_terminator(bool enabled);

/** @brief Jméno zásahu Direct Save (cmt_request_set_direct_save()); podle něj UI pozná výsledek. */
#define CMT_REQUEST_DIRECT_SAVE_NAME "cmt_set_direct_save"

/**
 * @brief Zařadí volbu Direct Save CMT hacku (`[CMTHACK] direct_save`), jen ve STOP.
 *
 * Emu vlákno volbu změní jen s deckem ve STOP (jako přepínač CMT hacku,
 * cmt_loading_mode_apply()): cmthack_direct_save_set() nastaví volbu
 * a se zapnutým hackem hned nainstaluje nebo odebere patch zápisu.
 * Výsledek zásahu (rc): 1 = patch zápisu je teď v ROM, 0 = není,
 * -1 = odmítnuto (deck mimo STOP, nic se nezměnilo). Při záznamu
 * a přehrávání vstupů ho fronta odmítne (DEVICE_REQ_REJECTED_REPLAY).
 * Ohlásí ho okno Virtual CMT (imgui_cmt_device_request_result()).
 *
 * @param enabled true = ROM SAVE ukládat přímo na pásku.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 */
uint32_t cmt_request_set_direct_save(bool enabled);

/**
 * @brief Zařadí polaritu kazety ze zadního DIP přepínače
 *        (cmt_rear_dip_switch_cmt_inverted_polarity()).
 * @param inverted true = invertovaná polarita.
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t cmt_request_set_polarity(bool inverted);

/**
 * @brief Režim načítání okna Virtual CMT (přepínač Loading: Tape | Boost | Direct).
 *
 * Každý režim je dvojice voleb: CMT hack (ROM patch, přímé načtení MZF)
 * a CPU Boost (MAX SPEED při běžícím motoru). Kombinace hack + Boost
 * (z hlavního menu, INI, CLI, MCP) se ukazuje jako Direct.
 */
typedef enum en_CMT_LOADING_MODE {
    CMT_LOADING_MODE_TAPE = 0, /**< signál v reálném čase: hack vypnutý, CPU Boost vypnutý */
    CMT_LOADING_MODE_BOOST,    /**< signál při MAX SPEED během běhu motoru: hack vypnutý, CPU Boost zapnutý */
    CMT_LOADING_MODE_DIRECT,   /**< přímé načtení MZF do paměti: hack zapnutý, CPU Boost vypnutý (nepoužívá se) */
    CMT_LOADING_MODE_COUNT     /**< počet režimů (není režim) */
} en_CMT_LOADING_MODE;

/** @brief Název zásahu cmt_request_set_loading_mode() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_LOADING_MODE_NAME "cmt_set_loading_mode"

/**
 * @brief Zařadí změnu režimu načítání (CMT hack a CPU Boost jedním zásahem).
 *
 * Emu vlákno nastaví CPU Boost (cmt_cpu_boost_set()) a CMT hack
 * (cmthack_load_rom_patch()) podle en_CMT_LOADING_MODE. Změnu CMT hacku
 * (do Direct nebo z něj) provede jen ve STOP: přímé načtení by přehrávanou
 * pásku zastavilo a při nahrávání by hack pásku nepoužil (cmthack.c), takže
 * mimo STOP zásah nic nezmění a vrátí -1. Opuštění Direct zapomene stav
 * hacku nad páskou (cmthack_tape_state_clear(): hlavička načtená bez těla).
 * Přepnutí Tape <-> Boost jde i při PLAY a RECORD. Při záznamu
 * a přehrávání vstupů se zásah odmítne (device_request_poll()).
 *
 * @param mode Režim (mimo rozsah = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 * @note Návrat zásahu (st_DEVICE_REQ_RESULT.rc): výsledný režim
 *       (en_CMT_LOADING_MODE), -1 = odmítnuto mimo STOP (nic se nezměnilo).
 */
uint32_t cmt_request_set_loading_mode(int mode);

/** @brief Název zásahu cmt_request_load_directly_on_tape() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_LOAD_DIRECTLY_NAME "cmt_load_directly"

/**
 * @brief Zařadí Load directly (kontextové menu řádku okna Virtual CMT): výběr bloku a režim Direct jedním zásahem.
 *
 * Emu vlákno pod zámkem stavu zařízení provede vše, nebo nic: páska musí
 * být ta, kterou uživatel viděl (@p tape_serial proti
 * cmttmz_get_tape_serial()), deck ve STOP a blok @p block_id musí nést
 * MZF; pak se blok vybere (cmt_tape_select_block() bez forced) a po
 * úspěchu se režim načítání přepne do Direct (CMT hack zapnutý, CPU Boost
 * vypnutý; byl-li hack zapnutý, nemění se režim ani CPU Boost). Dřív to byly dva zásahy
 * ve dvou frontách (výběr bloku operací záznamu vstupů, režim zásahem
 * fronty zařízení) a mezi nimi mohla páska změnit stav.
 * Při záznamu a přehrávání vstupů se odmítne celý (device_request_poll(),
 * DEVICE_REQ_REJECTED_REPLAY); do záznamu vstupů se nezapisuje.
 *
 * @param tape_serial Čítač výměn pásky (st_CMT_VIEW::tape_serial) v době volby.
 * @param block_id Číslo bloku (záporné = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 * @note Návrat zásahu (st_DEVICE_REQ_RESULT.rc): 1 = blok vybraný a režim
 *       přepnutý do Direct, 0 = blok vybraný (Direct už byl), -1 = nic se
 *       nezměnilo (deck mimo STOP, ROM bez podpory CMT hacku, blok
 *       neexistuje, blok bez MZF podle typu indexu pásky, blok nejde
 *       otevřít),
 *       CMT_REQUEST_RC_TAPE_CHANGED = jiná páska, nic se nezměnilo.
 */
uint32_t cmt_request_load_directly_on_tape(uint32_t tape_serial, int block_id);

/**
 * @brief Provede změnu režimu načítání hned (tělo zásahu cmt_request_set_loading_mode()).
 *
 * Společná pravidla přepínače Loading okna Virtual CMT, položek hlavního
 * menu Use CMT Hack a Enable CPU Boost a nástroje MCP emu_cmt_hack_set:
 * zásahy fronty zařízení ji volají v device_request_poll(), dbgapi
 * (DBGAPI_CMD_CMT_HACK_SET) přímo. Navíc jen v okně: přepínač Loading
 * segment Direct zakáže, když aktuální blok pásky nenese MZF (UI,
 * cmt_window.cpp); položka menu ani MCP tuto kontrolu nemají. ROM bez
 * podpory hacku (cmthack_rom_supported()) tato funkce neodmítá (hack
 * zůstane vypnutý, viz @post): v UI je segment Direct i položka Use CMT
 * Hack s takovou ROM zakázaná s důvodem (dokud hack není zapnutý), v MCP
 * emu_cmt_hack_set(true) odmítne dbgapi chybou.
 *
 * Změnu CMT hacku (do Direct nebo z něj) provede jen ve STOP; mimo STOP
 * nezmění nic (ani CPU Boost) a vrátí -1. Vypnutí hacku zapomene stav
 * hacku nad páskou (cmthack_tape_state_clear(): hlavička načtená bez těla,
 * konec pásky). CPU Boost se mění v každém stavu transportu. Požadavek,
 * který stav nemění (hack už má požadovanou hodnotu), uspěje v každém
 * stavu.
 *
 * @param mode Režim (en_CMT_LOADING_MODE), nebo -1 = podle @p hack a @p boost.
 * @param hack Jen s @p mode -1: 0 = hack vypnout, 1 = zapnout, -1 = ponechat.
 * @param boost Jen s @p mode -1: 0 = CPU Boost vypnout, 1 = zapnout, -1 = ponechat.
 * @return Výsledný režim (en_CMT_LOADING_MODE: hack = Direct, jinak podle CPU
 *         Boost); -1 = odmítnuto mimo STOP, nic se nezměnilo.
 * @pre Emu vlákno pod device_state_lock() (UI čte režim z kopie stavu).
 * @post Při návratu >= 0 platí požadovaný CPU Boost a požadovaný hack,
 *       pokud ho vybraná ROM podporuje (cmthack_rom_supported()); jinak
 *       zůstane hack vypnutý a návrat ukazuje skutečný režim (Tape nebo
 *       Boost, ne Direct). Při -1 je vše beze změny.
 * @note Záznam ani přehrávání vstupů nekontroluje: zásah fronty odmítne
 *       device_request_poll(), příkaz MCP brána záznamu vstupů a dbgapi.
 */
int cmt_loading_mode_apply(int mode, int hack, int boost);

/**
 * @brief Důvod, proč Load directly nic neprovedl (cmt_load_directly_apply()).
 */
typedef enum en_CMT_LOAD_DIRECTLY_REFUSAL {
    CMT_LOAD_DIRECTLY_OK = 0,      /**< provedeno, nic nebylo odmítnuto */
    CMT_LOAD_DIRECTLY_NO_TAPE,     /**< v decku není páska */
    CMT_LOAD_DIRECTLY_NOT_STOPPED, /**< deck není ve STOP (PLAY, pauza, nahrávání) */
    CMT_LOAD_DIRECTLY_NO_BLOCK,    /**< blok s tímto číslem na pásce není */
    CMT_LOAD_DIRECTLY_NOT_MZF,     /**< blok nenese soubor MZF podle typu indexu (řídicí, surový, ZX, blok 0x46, ze kterého MZF nejde sestavit, ...) */
    CMT_LOAD_DIRECTLY_CANNOT_OPEN, /**< blok nese MZF, ale nejde vybrat (cmt_tape_select_block()) */
    CMT_LOAD_DIRECTLY_NO_HACK,     /**< vybraná ROM CMT hack nepodporuje (cmthack_rom_supported()), Direct nejde zapnout */
} en_CMT_LOAD_DIRECTLY_REFUSAL;

/**
 * @brief Provede Load directly hned: výběr bloku a režim Direct najednou (tělo zásahu cmt_request_load_directly_on_tape()).
 *
 * Jediné místo pravidel Load directly okna Virtual CMT a nástroje MCP
 * emu_cmt_load_directly. Vše nebo nic, v tomto pořadí kontrol: páska
 * vložená, deck ve STOP, vybraná ROM CMT hack podporuje (je-li hack
 * vypnutý; cmthack_rom_supported()), blok existuje, blok nese MZF (typ indexu pásky
 * CMTEXT_BLOCK_TYPE_MZF: soubor z bloku jde sestavit; totéž rozhoduje
 * nabídka okna a pole souboru resource MCP - blok 0x46, ze kterého MZF
 * sestavit nejde, se odmítne); pak výběr bloku (cmt_tape_select_block() bez
 * forced, ve STOP jen otevře blok) a teprve po jeho úspěchu přepnutí do
 * Direct (CMT hack zapnutý, CPU Boost vypnutý). Je-li hack už zapnutý,
 * nemění se nic dalšího, ani CPU Boost. Kontrolu, že jde o pásku, kterou
 * uživatel viděl, dělá jen zásah okna (čítač výměn pásky).
 *
 * @param block_id Číslo bloku (od 0).
 * @param[out] why Důvod odmítnutí (smí být NULL); při úspěchu CMT_LOAD_DIRECTLY_OK.
 * @return 1 = blok vybraný a režim přepnutý do Direct; 0 = blok vybraný,
 *         Direct už byl; -1 = nic se nezměnilo (důvod v @p why).
 * @pre Emu vlákno pod device_state_lock().
 * @post Při návratu >= 0 je blok @p block_id otevřený a CMT hack
 *       nainstalovaný; při -1 se nezměnil blok, CPU Boost ani hack.
 * @note Záznam ani přehrávání vstupů nekontroluje (viz cmt_loading_mode_apply()).
 */
int cmt_load_directly_apply(int block_id, en_CMT_LOAD_DIRECTLY_REFUSAL *why);

/**
 * @brief Zařadí změnu jen CMT hacku (hlavní menu Use CMT Hack), CPU Boost zůstane.
 *
 * Týž zásah jako cmt_request_set_loading_mode() (jméno
 * CMT_REQUEST_LOADING_MODE_NAME, kontrola STOP, odmítnutí při replay,
 * úklid stavu hacku při vypnutí). Kombinace hack + CPU Boost je platná
 * (okno ji ukazuje jako Direct).
 *
 * @param enabled true = hack zapnout.
 * @return Pořadové číslo zásahu (od 1).
 * @note Návrat zásahu: výsledný režim (en_CMT_LOADING_MODE), -1 = odmítnuto mimo STOP.
 */
uint32_t cmt_request_set_loading_hack(bool enabled);

/**
 * @brief Zařadí změnu jen CPU Boost (hlavní menu Enable CPU Boost), CMT hack zůstane.
 *
 * Týž zásah jako cmt_request_set_loading_mode(); CPU Boost se tak mění
 * jen na emu vlákně (cmt_cpu_boost_set() -> MAX SPEED), ne z UI vlákna.
 * Jde v každém stavu transportu, při záznamu a přehrávání vstupů se
 * odmítne.
 *
 * @param enabled true = CPU Boost zapnout.
 * @return Pořadové číslo zásahu (od 1).
 * @note Návrat zásahu: výsledný režim (en_CMT_LOADING_MODE).
 */
uint32_t cmt_request_set_loading_boost(bool enabled);

/**
 * @brief Zaznamená operaci okna Virtual CMT, kterou emu vlákno zahodilo, protože se mezitím změnil stav decku.
 *
 * Volají ji místa, která operaci okna rozhodnutou ze starší kopie stavu
 * zahodí: replay_ui_cmt_drain() při nesplněných podmínkách
 * replay_host_cmt_guarded() mimo přehrávání záznamu vstupů (při přehrávání
 * se operace z UI zahazují všechny a nehlásí se). Zvedne čítač
 * cmt_request_get_window_drop_serial(); okno podle něj ukáže varování
 * (cmt_view_poll_dropped()).
 *
 * @note Libovolné vlákno (typicky emu vlákno); atomické zvýšení čítače.
 */
void cmt_request_note_window_drop(void);

/**
 * @brief Čítač operací okna zahozených kvůli změně stavu decku (cmt_request_note_window_drop()).
 *
 * Roste o 1 s každou zahozenou operací (přetečení nevadí, UI hlídá jen
 * změnu). Jádro stav nijak jinak nemění.
 *
 * @return Hodnota čítače (0 = od startu nic nezahozeno).
 * @note Libovolné vlákno; atomické čtení, bez zámku.
 */
unsigned cmt_request_get_window_drop_serial(void);

/** @brief Název zásahu cmt_request_lengths() (st_DEVICE_REQ_RESULT.name; UI jeho výsledek nehlásí). */
#define CMT_REQUEST_LENGTHS_NAME "cmt_lengths"

/**
 * @brief Zařadí jeden krok výpočtu délek bloků vložené pásky pro okno Virtual CMT.
 *
 * Emu vlákno zavolá cmttmz_lengths_update_budget(CMTTMZ_LENGTHS_STEP_US):
 * spočítá délky bloků, které zatím známé nejsou (otevření pásky je
 * nepočítá), nejvýš na CMTTMZ_LENGTHS_STEP_US času hostitele (aspoň jeden
 * blok). Stav stroje se nemění (jen vedlejší pole údajů bloků pro UI),
 * proto jde o zásah device_request_submit_ui_data(): nepřepíše
 * last_user_action a nezvedne revizi stavu. Při záznamu nebo přehrávání
 * vstupů se zásah odmítne; UI ho tehdy nezařazuje a odmítnutý zásah
 * zopakuje. Okno ho zařazuje jen ve STOP a nejvýš jednou za
 * CMT_VIEW_LENGTHS_INTERVAL_FRAMES snímků (cmt_view_request_lengths()),
 * aby krok nezdržel přehrávání ani záznam.
 *
 * @return Pořadové číslo zásahu (od 1).
 * @note Návrat zásahu: vždy 0. Výsledek UI spolkne bez hlášení
 *       (imgui_cmt_device_request_result(), CMT_REQUEST_LENGTHS_NAME).
 */
uint32_t cmt_request_lengths(void);


/* ------------------------------------------------------------------ */
/* Úpravy pásky: mazání, přesun, import, export celé pásky            */
/* ------------------------------------------------------------------ */

/** @brief Název zásahu cmt_request_remove_blocks_on_tape() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_REMOVE_BLOCKS_NAME "cmt_remove_blocks"
/** @brief Název zásahu cmt_request_move_block_on_tape() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_MOVE_BLOCK_NAME "cmt_move_block"
/** @brief Název zásahu cmt_request_import_on_tape() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_IMPORT_NAME "cmt_import"
/** @brief Název zásahu cmt_request_export_tape_on_tape() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_EXPORT_TAPE_NAME "cmt_export_tape"

/** @brief Velikost textu výsledku úpravy (st_CMT_REQUEST_EDIT_RESULT::message, včetně nuly). */
#define CMT_REQUEST_EDIT_MESSAGE_SIZE 512

/** @brief Počet výsledků úprav, které si jádro pamatuje (stejně jako nevyzvednutých výsledků fronty). */
#define CMT_REQUEST_EDIT_RESULTS_MAX DEVICE_REQUEST_RESULTS_MAX

/**
 * @brief Druh úpravy pásky ve výsledku (st_CMT_REQUEST_EDIT_RESULT::op).
 */
typedef enum en_CMT_REQUEST_EDIT_OP {
    CMT_REQUEST_EDIT_NONE = 0, /**< položka nepoužitá (není výsledek) */
    CMT_REQUEST_EDIT_REMOVE,   /**< smazání bloků (cmt_request_remove_blocks_on_tape()) */
    CMT_REQUEST_EDIT_MOVE,     /**< přesun bloku (cmt_request_move_block_on_tape()) */
    CMT_REQUEST_EDIT_IMPORT,   /**< import souboru jako bloků (cmt_request_import_on_tape()) */
    CMT_REQUEST_EDIT_ANALYSIS_PULSES, /**< kopie pulzů surového bloku pro analýzu (cmt_request_analysis_pulses()); pásku nemění */
    CMT_REQUEST_EDIT_ANALYSIS_APPLY,  /**< uplatnění analýzy: Add / Replace (cmt_request_analysis_apply()) */
    CMT_REQUEST_EDIT_TAPE_INFO,       /**< zápis informací o pásce (cmt_request_tape_info_set()) */
    CMT_REQUEST_EDIT_CONVERT,         /**< převod bloku na profil zapisovače (cmt_request_convert_block_on_tape()) */
} en_CMT_REQUEST_EDIT_OP;

/**
 * @brief Výsledek zásahu úpravy pásky pro okno Virtual CMT (obnova výběru po úpravě).
 *
 * Každá úprava pásky zvedne čítač výměn pásky a okno tím ztratí výběr
 * řádků. Aby si ho po vlastní úpravě obnovilo, zapíše funkce zásahu
 * (emu vlákno, pod zámkem stavu zařízení) tento výsledek pod pořadovým
 * číslem zásahu a okno si ho v obsluze výsledku zásahu přečte
 * cmt_request_get_edit_result() pod týmž zámkem - stejně, jako dnes čte
 * text výsledku uložení (cmttmz_get_save_message()). Žádný další kanál
 * (callback) není.
 *
 * Výsledek zapíše každé provedení zásahu (úspěch, chyba,
 * CMT_REQUEST_RC_TAPE_CHANGED); zásah odmítnutý při záznamu
 * a přehrávání vstupů (DEVICE_REQ_REJECTED_REPLAY) se neprovede a výsledek
 * nemá.
 *
 * @invariant `seq` != 0 u zapsané položky; `rc` je týž jako
 *            st_DEVICE_REQ_RESULT.rc téhož zásahu; `message` je ukončená nulou.
 * @invariant Při `rc` 0: 0 <= `select_block` < `block_count`, nebo
 *            `select_block` -1 (prázdná páska po smazání, zápis informací
 *            o pásce, uplatnění analýzy bez vybraného řádku).
 *            Při `rc` != 0: `select_block` -1, `count` 0, `removed_block` -1.
 * @invariant `removed_block` >= 0 jen u úspěšného mazání (`op` CMT_REQUEST_EDIT_REMOVE, `rc` 0).
 * @invariant Kopie pulzů (CMT_REQUEST_EDIT_ANALYSIS_PULSES) pásku nemění:
 *            `select_block` je číslo bloku kopie, `count` počet pulzů.
 *            Uplatnění analýzy, zápis informací a převod profilu, které pásku
 *            nezměnily (nic vybraného, formulář beze změny, převod beze změny),
 *            mají `rc` 0 a `count` 0.
 */
typedef struct st_CMT_REQUEST_EDIT_RESULT {
    uint32_t seq;          /**< pořadové číslo zásahu (device_request_submit()), 0 = prázdná položka */
    int      op;           /**< druh úpravy (en_CMT_REQUEST_EDIT_OP) */
    int      rc;           /**< návrat zásahu: 0 = provedeno, -1 = chyba (text v message), CMT_REQUEST_RC_TAPE_CHANGED = jiná páska, nic se nezměnilo */
    int      select_block; /**< blok k výběru po úpravě: import = první importovaný blok, přesun = cílová pozice, mazání = blok na místě prvního smazaného (bez něj poslední blok), kopie pulzů = blok kopie, uplatnění analýzy = první přidaný blok souboru, převod profilu = převedený blok; -1 = žádný (také zápis informací o pásce) */
    int      removed_block; /**< mazání: číslo prvního smazaného bloku před smazáním (okno z něj píše "Deleted block %d." u jednoho bloku); jinak -1 */
    uint32_t count;        /**< import = počet importovaných bloků, mazání = počet smazaných bloků, přesun = 1, kopie pulzů = počet pulzů, uplatnění analýzy = počet přidaných bloků souborů, zápis informací a převod = 1 páska se změnila / 0 beze změny; 0 při chybě */
    uint32_t tape_serial;  /**< čítač výměn pásky po zásahu (cmttmz_get_tape_serial()): okno s ním může zařadit další úpravu téže pásky */
    uint32_t block_count;  /**< počet bloků pásky po zásahu (0 = bez pásky nebo prázdná páska) */
    bool     new_tape;     /**< import: páska vznikla importem do prázdného decku (předtím nebyla vložená) */
    char     message[CMT_REQUEST_EDIT_MESSAGE_SIZE]; /**< anglicky: chyba úpravy (cmttmz_get_edit_error()) při rc -1; při úspěchu varování: importu (cmttmz_get_import_warning()), změny taktu po mazání a přesunu, taktu nových bloků po uplatnění analýzy, zápisu informací a převodu profilu (cmttmz_get_edit_warning()); jinak "" */
} st_CMT_REQUEST_EDIT_RESULT;

/**
 * @brief Výsledek zásahu úpravy pásky podle pořadového čísla zásahu.
 *
 * Jádro si pamatuje posledních CMT_REQUEST_EDIT_RESULTS_MAX výsledků úprav
 * (kruh podle pořadového čísla), tedy aspoň tolik, kolik výsledků drží
 * fronta zásahů (DEVICE_REQUEST_RESULTS_MAX); okno, které výsledek zásahu
 * vyzvedne každý snímek, ho proto najde, i když emu vlákno mezitím
 * provedlo další úpravy.
 *
 * @param seq Pořadové číslo zásahu (návrat cmt_request_remove_blocks_on_tape(),
 *        cmt_request_move_block_on_tape(), cmt_request_import_on_tape(),
 *        cmt_request_analysis_pulses(), cmt_request_analysis_apply(),
 *        cmt_request_tape_info_set(), cmt_request_convert_block_on_tape();
 *        st_DEVICE_REQ_RESULT.seq).
 * @param[out] out Kopie výsledku (nesmí být NULL; při false beze změny).
 * @return true = výsledek nalezen; false = zásah neprovedený (odmítnutý při
 *         záznamu vstupů, dosud čekající), přepsaný novějším (položku
 *         přepíše úprava s pořadovým číslem o násobek
 *         CMT_REQUEST_EDIT_RESULTS_MAX většším; čísla počítají všechny zásahy
 *         všech zařízení, takže nejdřív po CMT_REQUEST_EDIT_RESULTS_MAX
 *         zásazích libovolného druhu) nebo @p seq 0.
 * @pre UI vlákno pod device_state_lock() (výsledky zapisuje emu vlákno
 *      v device_request_poll() pod týmž zámkem), nebo emu vlákno.
 */
bool cmt_request_get_edit_result(uint32_t seq, st_CMT_REQUEST_EDIT_RESULT *out);

/**
 * @brief Zapíše seznam čísel bloků jako rozsahy ("0-3,7,9-12").
 *
 * Souvislá řada aspoň dvou čísel je "a-b", samostatné číslo "a"; položky
 * odděluje čárka bez mezer. Tak se seznam bloků vejde do textového
 * argumentu zásahu (DEVICE_REQUEST_STR_MAX).
 *
 * @param block_ids Čísla bloků, vzestupně bez opakování, nezáporná.
 * @param count Počet (aspoň 1).
 * @param[out] out Buffer pro text (ukončený nulou).
 * @param size Velikost bufferu včetně nuly.
 * @return true = zapsáno celé; false = NULL, @p count 0, záporné číslo,
 *         pořadí není vzestupné bez opakování, nebo se text nevejde
 *         (obsah @p out pak není platný seznam).
 */
bool cmt_request_block_ranges_encode(const int *block_ids, uint32_t count, char *out, size_t size);

/**
 * @brief Přečte seznam rozsahů z cmt_request_block_ranges_encode() zpět na čísla bloků.
 *
 * Přijme jen tvar, který zapisuje kodér: nezáporná desítková čísla, "a-b"
 * s a < b, čárky bez mezer, vše vzestupně bez opakování a bez prázdných
 * položek.
 *
 * @param text Text (nesmí být NULL).
 * @param[out] block_ids Pole pro čísla bloků.
 * @param cap Kapacita pole.
 * @return Počet čísel (aspoň 1); -1 = text není platný seznam nebo se
 *         čísla nevejdou do @p cap (obsah @p block_ids pak nemá význam).
 */
int cmt_request_block_ranges_decode(const char *text, int *block_ids, uint32_t cap);

/**
 * @brief Zařadí smazání bloků pásky, kterou uživatel viděl (cmt_tape_remove_blocks()).
 *
 * Čísla se seřadí vzestupně a do textového argumentu zásahu se zapíšou
 * jako rozsahy (cmt_request_block_ranges_encode()). Když se seznam
 * nevejde (DEVICE_REQUEST_STR_MAX), zásah se nezařadí a funkce vrátí 0;
 * volající ohlásí chybu. Rozdělit smazání do více zásahů nejde: první
 * zásah čísla dalších bloků posune (a změní čítač výměn pásky).
 *
 * Emu vlákno porovná @p tape_serial s cmttmz_get_tape_serial() (neshoda =
 * CMT_REQUEST_RC_TAPE_CHANGED, nic se nezměnilo) a smaže bloky jako
 * cmt_tape_remove_blocks(): jen ve STOP, mimo nahrávání a záznam
 * a přehrávání vstupů (cmt_tape_edit_allowed()); kurzor a čítače nastaví
 * jádro. Při záznamu a přehrávání vstupů zásah odmítne už fronta
 * (DEVICE_REQ_REJECTED_REPLAY). Výsledek pro okno zapíše do
 * st_CMT_REQUEST_EDIT_RESULT (cmt_request_get_edit_result()).
 *
 * @param tape_serial Čítač výměn pásky (st_CMT_VIEW::tape_serial) v době volby bloků.
 * @param block_ids Čísla bloků (libovolné pořadí, bez opakování, nezáporná; pole jen čte).
 * @param count Počet (aspoň 1).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (NULL, @p count 0,
 *         záporné nebo opakované číslo, seznam rozsahů se nevejde).
 * @note Návrat zásahu (st_DEVICE_REQ_RESULT.rc): 0 = smazáno, -1 = chyba
 *       (páska beze změny, text v st_CMT_REQUEST_EDIT_RESULT::message, např.
 *       "Block 7 does not exist", "The tape can be edited only when the deck
 *       is stopped"), CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @note Libovolné vlákno (typicky UI); nečeká na provedení.
 */
uint32_t cmt_request_remove_blocks_on_tape(uint32_t tape_serial, const int *block_ids, uint32_t count);

/**
 * @brief Zařadí přesun bloku pásky, kterou uživatel viděl (cmt_tape_move_block()).
 *
 * Kontrola pásky, podmínky (STOP, nahrávání, záznam vstupů) a výsledek
 * jako cmt_request_remove_blocks_on_tape(). Přesun na vlastní místo je
 * úspěch beze změny.
 *
 * @param tape_serial Čítač výměn pásky v době volby bloku.
 * @param block_id Číslo přesouvaného bloku (záporné = nezařadí se).
 * @param to Pozice bloku po přesunu (0 .. počet - 1; záporná = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 * @note Návrat zásahu: 0 = přesunuto (st_CMT_REQUEST_EDIT_RESULT::select_block
 *       = @p to), -1 = chyba (páska beze změny, text v message),
 *       CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 */
uint32_t cmt_request_move_block_on_tape(uint32_t tape_serial, int block_id, int to);

/**
 * @brief Zařadí import souboru jako bloků pásky před blok @p at (cmt_tape_import_file()).
 *
 * Kontrola pásky: @p tape_serial se porovná s cmttmz_get_tape_serial()
 * jako u ostatních zásahů *_on_tape() (platí i bez vložené pásky - vysunutí
 * čítač zvedá, takže okno, které vidělo prázdný deck, pozná vložení pásky).
 * Navíc @p tape_serial 0 a deck v době provedení bez pásky = import do nové
 * pásky bez ohledu na čítač. Bez pásky vloží cmt_tape_import_file() novou
 * prázdnou pásku (při chybě ji zase vysune). Podmínky (STOP, nahrávání,
 * záznam vstupů) a výsledek jako cmt_request_remove_blocks_on_tape().
 * Import běží synchronně na emu vlákně (velký WAV zastaví emulaci i okno
 * jako otevření).
 *
 * @param tape_serial Čítač výměn pásky v době volby místa, nebo 0 = jen do prázdného decku.
 * @param path Soubor (MZF, M12, MZT, TAP, WAV, TMZ, TZX; UTF-8; nesmí být
 *        NULL ani prázdný, nejvýš DEVICE_REQUEST_STR_MAX - 1 bajtů).
 * @param at Pozice 0 .. počet bloků (počet = na konec); záporná = na konec
 *        pásky v době provedení. Bez pásky jen záporná (nová páska, blok 0);
 *        nezáporná bez pásky = chyba "no tape in the deck".
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (cesta chybí nebo je dlouhá).
 * @note Návrat zásahu: 0 = importováno (st_CMT_REQUEST_EDIT_RESULT: count =
 *       počet bloků, select_block = první importovaný, message = varování
 *       importu), -1 = chyba (páska beze změny, text "Can't import <cesta>:
 *       <důvod>" nebo odmítnutí v message), CMT_REQUEST_RC_TAPE_CHANGED =
 *       jiná páska.
 */
uint32_t cmt_request_import_on_tape(uint32_t tape_serial, const char *path, int at);

/**
 * @brief Zařadí export celé pásky, kterou uživatel viděl, do WAV
 *        (cmttmz_export_tape_wav()).
 *
 * Export pásku nemění (soubor pásky, otevřený blok, player ani Modified).
 * Emu vlákno nejdřív porovná @p tape_serial s cmttmz_get_tape_serial():
 * neshoda (jiná, vysunutá nebo mezitím upravená páska) =
 * CMT_REQUEST_RC_TAPE_CHANGED, nic se nezapíše a zpráva exportu
 * (cmttmz_get_export_message()) zůstane beze změny. Stav transportu
 * nekontroluje (okno ho ověří před zařazením). Běží synchronně na emu
 * vlákně; při záznamu a přehrávání vstupů ho odmítne fronta
 * (DEVICE_REQ_REJECTED_REPLAY), jako každý zásah. Varianta bez čítače
 * výměn pásky není.
 *
 * @param tape_serial Čítač výměn pásky (st_CMT_VIEW::tape_serial) v době volby.
 * @param path Cíl s příponou .wav (UTF-8; nesmí být NULL ani prázdný,
 *        nejvýš DEVICE_REQUEST_STR_MAX - 1 bajtů).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 * @note Návrat zásahu: 0 = exportováno, -1 = chyba; zpráva v obou případech
 *       v cmttmz_get_export_message() ("Tape exported: <cesta>, <s> s"
 *       s varováním v závorce, nebo text chyby); zprávu uložení
 *       (cmttmz_get_save_message()) export nepřepíše.
 *       CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @note Libovolné vlákno (typicky UI); nečeká na provedení.
 */
uint32_t cmt_request_export_tape_on_tape(uint32_t tape_serial, const char *path);


/* ------------------------------------------------------------------ */
/* Nástroje nad páskou: analýza, informace o pásce, převod             */
/* ------------------------------------------------------------------ */

/** @brief Název zásahu cmt_request_analysis_pulses() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_ANALYSIS_PULSES_NAME "cmt_analysis_pulses"
/** @brief Název zásahu cmt_request_analysis_apply() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_ANALYSIS_APPLY_NAME "cmt_analysis_apply"
/** @brief Název zásahu cmt_request_tape_info_set() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_TAPE_INFO_SET_NAME "cmt_tape_info_set"
/** @brief Název zásahu cmt_request_convert_block_on_tape() (st_DEVICE_REQ_RESULT.name). */
#define CMT_REQUEST_CONVERT_BLOCK_NAME "cmt_convert_block"

/**
 * @brief Počet čekajících zápisů informací o pásce, jejichž kopii jádro drží (cmt_request_tape_info_set()).
 *
 * Zápis, který se neprovedl (odmítnutý při záznamu vstupů, zahozený
 * device_request_clear()), kopii neuvolní sám; uvolní ji až zápis
 * o CMT_REQUEST_TAPE_INFO_SLOTS dál. Zápis, jehož kopii mezitím uvolnil
 * novější, skončí chybou (viz cmt_request_tape_info_set()).
 */
#define CMT_REQUEST_TAPE_INFO_SLOTS 4

/**
 * @brief Zařadí kopii pulzů surového bloku 0x18 pásky, kterou uživatel viděl (vstup analýzy).
 *
 * Emu vlákno porovná @p tape_serial s cmttmz_get_tape_serial() (neshoda =
 * CMT_REQUEST_RC_TAPE_CHANGED) a zavolá cmttmz_analyze_copy_pulses(). Kopii
 * si okno vyzvedne cmt_request_take_pulses() s pořadovým číslem tohoto
 * zásahu a analyzuje ji samo (cmttmz_analyze_pulses() smí běžet v jiném
 * vlákně). Pásku zásah nemění, takže nekontroluje stav transportu
 * (analýzu jde připravit i při PLAY; uplatnění pak STOP vyžaduje). Při
 * záznamu a přehrávání vstupů ho odmítne fronta (DEVICE_REQ_REJECTED_REPLAY).
 * Zařazuje se jako zásah jen pro data UI (device_request_submit_ui_data()):
 * stav stroje nemění, proto nezvedá revizi stavu (state_rev) ani
 * nepřepisuje last_user_action.
 *
 * @param tape_serial Čítač výměn pásky (st_CMT_VIEW::tape_serial) v době volby bloku.
 * @param block_id Číslo bloku (záporné = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 * @note Návrat zásahu: 0 = kopie připravená (st_CMT_REQUEST_EDIT_RESULT:
 *       op CMT_REQUEST_EDIT_ANALYSIS_PULSES, select_block = blok, count =
 *       počet pulzů), -1 = chyba (text v message, např. "Block 0 is not
 *       a raw signal block (CSW recording)"), CMT_REQUEST_RC_TAPE_CHANGED =
 *       jiná páska. Kopie existuje jen při 0.
 * @note Libovolné vlákno (typicky UI); nečeká na provedení.
 */
uint32_t cmt_request_analysis_pulses(uint32_t tape_serial, int block_id);

/**
 * @brief Vyzvedne kopii pulzů připravenou zásahem cmt_request_analysis_pulses().
 *
 * Jádro drží nejvýš jednu nevyzvednutou kopii: provedení dalšího zásahu
 * kopie pulzů ji uvolní (cmttmz_pulses_free()) a nahradí svou. Vyzvednutím
 * přechází vlastnictví na volajícího.
 *
 * @param seq Pořadové číslo zásahu (návrat cmt_request_analysis_pulses()).
 * @return Kopie (uvolní volající cmttmz_pulses_free(), až ji žádné vlákno
 *         nečte); NULL = zásah ještě neproběhl, skončil chybou, kopii už
 *         někdo vyzvedl, nebo ji nahradil novější zásah kopie pulzů.
 * @note Libovolné vlákno; vlastní zámek modulu (device_state_lock() není potřeba).
 */
st_CMTTMZ_PULSES* cmt_request_take_pulses(uint32_t seq);

/**
 * @brief Zařadí uplatnění výsledku analýzy na pásku (Add nebo Replace, cmt_tape_analysis_apply()).
 *
 * Zásah si data nekopíruje: ukazatele @p p, @p a a @p use uloží do registru
 * modulu a emu vlákno je přečte až v device_request_poll(). Vlastnictví
 * zůstává volajícímu. **Volající smí data uvolnit nebo změnit právě tehdy,
 * když platí jedno z:**
 * - funkce vrátila 0 (zásah se nezařadil, data nikdo nečte);
 * - vyzvedl výsledek tohoto zásahu (st_DEVICE_REQ_RESULT se stejným seq,
 *   stav DEVICE_REQ_DONE i DEVICE_REQ_REJECTED_REPLAY) - po výsledku zásah
 *   na data nesáhne;
 * - cmt_request_analysis_cancel(seq) vrátil true (zásah data nikdy nepřečte).
 * Jindy (cmt_request_analysis_cancel() vrátil false: zásah data právě čte
 * nebo už doběhl) je třeba počkat na výsledek. Zásah, jehož výsledek se
 * ztratil (device_request_clear(), přetečení fronty výsledků), se uvolní
 * zrušením: cmt_request_analysis_cancel() pak vrátí true, pokud zásah
 * neproběhl, a false, pokud proběhl (data už nečte).
 *
 * Registr má CMT_REQUEST_EDIT_RESULTS_MAX míst; zařazení, které by obsadilo
 * místo dosud neprovedeného zásahu, ten zásah zneplatní (při provedení skončí
 * chybou "The analysis request was cancelled" bez sáhnutí na data).
 *
 * Kontrola pásky: čítač výměn pásky kopie (p->tape_serial) se porovná
 * s cmttmz_get_tape_serial() (neshoda = CMT_REQUEST_RC_TAPE_CHANGED);
 * cmttmz_analysis_apply() pak ověří i blok (číslo, frekvence, počet pulzů,
 * délka a otisk dat) a příslušnost analýzy ke kopii. Podmínky (STOP,
 * nahrávání, záznam vstupů) ověří obal cmt_tape_edit_allowed().
 *
 * @param p Kopie pulzů (cmt_request_take_pulses()); nesmí být NULL.
 * @param a Analýza z cmttmz_analyze_pulses() nad @p p; nesmí být NULL.
 * @param use Příznaky řádků (cmttmz_analysis_row_count() položek, 1 =
 *        přidat); NULL jen u analýzy bez řádků.
 * @param replace 0 = přidat soubory za surový blok (Add), 1 = nahradit
 *        surový blok (Replace); jiná hodnota = nezařadí se.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (NULL @p p nebo @p a,
 *         neplatné @p replace).
 * @note Návrat zásahu: 0 = uplatněno (st_CMT_REQUEST_EDIT_RESULT: count =
 *       počet přidaných bloků souborů, select_block = první z nich, -1 bez
 *       výběru, message = varování o taktu nových bloků CMTTMZ_CLOCK_WARNING
 *       nebo ""), -1 = chyba (páska beze změny, text v message, např. "Block 1
 *       changed since the analysis; analyze the block again", u zrušeného
 *       zásahu "The analysis request was cancelled"),
 *       CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @note Libovolné vlákno (typicky UI); nečeká na provedení.
 */
uint32_t cmt_request_analysis_apply(const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYSIS *a, const uint8_t *use,
                                    int replace);

/**
 * @brief Zruší zásah uplatnění analýzy, který emu vlákno ještě nezačalo provádět.
 *
 * Po návratu true emu vlákno data zásahu (p, a, use z cmt_request_analysis_apply())
 * nikdy nepřečte a volající je smí hned uvolnit; zásah ve frontě zůstane
 * a při provedení skončí chybou "The analysis request was cancelled"
 * (výsledek s rc -1, páska beze změny). False = zásah data právě čte nebo
 * už doběhl (na data se pak čeká výsledkem), nebo @p seq není zásah
 * uplatnění (0, neznámé číslo, místo registru už obsadil novější zásah,
 * registr vymazal cmt_request_exit()).
 *
 * Zásah, který fronta odmítla při záznamu nebo přehrávání vstupů
 * (DEVICE_REQ_REJECTED_REPLAY), funkci zásahu nezavolá a jeho místo
 * v registru zůstane: cancel(seq) pak vrátí true, i když výsledek už
 * přišel (data jsou volná v obou případech).
 *
 * Při ukončení aplikace je cancel no-op: cmt_exit() volá cmt_request_exit()
 * (registr vymaže) na emu vlákně dřív, než main.c emu vlákno připojí
 * a zavře okna (cmt_analyze_dialog_shutdown()); dialog pak data uvolní
 * bez cancel.
 *
 * @param seq Pořadové číslo zásahu (návrat cmt_request_analysis_apply()).
 * @return true = zrušeno, data jsou volná; false = viz výše.
 * @note Libovolné vlákno (typicky UI); vlastní zámek modulu, na emu vlákno nečeká.
 */
bool cmt_request_analysis_cancel(uint32_t seq);

/**
 * @brief Uvolní data zásahů kazety při ukončení emulátoru (volá cmt_exit()).
 *
 * Nevyzvednutou kopii pulzů (cmt_request_take_pulses()), čekající kopie
 * formulářů informací o pásce a registr zásahů uplatnění (data volajících
 * nevlastní, jen zapomene ukazatele - zásahy, které by ještě proběhly, je
 * nenajdou).
 *
 * @pre Emu vlákno už neprovádí zásahy fronty zařízení.
 */
void cmt_request_exit(void);

/**
 * @brief Zařadí zápis informací o pásce, kterou uživatel viděl (cmt_tape_info_set()).
 *
 * Formulář se při zařazení zkopíruje (volající ho smí hned uvolnit nebo
 * změnit); jádro drží nejvýš CMT_REQUEST_TAPE_INFO_SLOTS čekajících kopií.
 * Emu vlákno porovná @p tape_serial s cmttmz_get_tape_serial() (neshoda =
 * CMT_REQUEST_RC_TAPE_CHANGED) a zapíše formulář; tape_serial formuláře
 * se nahradí @p tape_serial (jádro ho ověří znovu). Formulář beze změny
 * pásku nezmění (count 0).
 *
 * @param tape_serial Čítač výměn pásky v době čtení formuláře (st_CMTTMZ_TAPE_INFO::tape_serial).
 * @param in Formulář (texty UTF-8, machine); nesmí být NULL.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (NULL, nedostatek paměti).
 * @note Návrat zásahu: 0 = zapsáno nebo beze změny (count 1 / 0; message =
 *       varování cmttmz_get_edit_warning(), např. "Characters outside
 *       ISO 8859-1 were replaced with '?' in: title"), -1 = chyba (text
 *       v message; kopii uvolnil novější zápis = "The tape information
 *       request expired"), CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @note Libovolné vlákno (typicky UI); nečeká na provedení.
 */
uint32_t cmt_request_tape_info_set(uint32_t tape_serial, const st_CMTTMZ_TAPE_INFO *in);

/**
 * @brief Zařadí převod bloku 0x46 pásky, kterou uživatel viděl, na profil zapisovače (cmt_tape_convert_block()).
 *
 * Kontrola pásky jako u ostatních zásahů *_on_tape(); podmínky (STOP,
 * nahrávání, záznam vstupů) ověří obal. Převod na tentýž profil bez
 * rychlosti pásku nezmění (count 0).
 *
 * @param tape_serial Čítač výměn pásky v době volby bloku.
 * @param block_id Číslo bloku (záporné = nezařadí se).
 * @param profile_id ID profilu (cmttmz_profile_get(); nesmí být NULL ani
 *        prázdné, nejvýš DEVICE_REQUEST_STR_MAX - 1 bajtů).
 * @param speed Rychlost (CMTSPEED_NONE = výchozí profilu; hodnota mimo
 *        CMTSPEED_NONE .. CMTSPEED_CUSTOM = nezařadí se; CMTSPEED_CUSTOM
 *        odmítne jádro "Invalid speed").
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 * @note Návrat zásahu: 0 = převedeno nebo beze změny (count 1 / 0,
 *       select_block = @p block_id, message = varování převodu, např.
 *       "Settings of profile TC-1.21 not carried over: preloader setting"),
 *       -1 = chyba (páska beze změny, text v message), CMT_REQUEST_RC_TAPE_CHANGED
 *       = jiná páska.
 * @note Libovolné vlákno (typicky UI); nečeká na provedení.
 */
uint32_t cmt_request_convert_block_on_tape(uint32_t tape_serial, int block_id, const char *profile_id,
                                           en_CMTSPEED speed);

#ifdef __cplusplus
}
#endif

#endif /* CMT_REQUEST_H */
