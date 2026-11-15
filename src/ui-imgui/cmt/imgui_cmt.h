/**
 * @file   imgui_cmt.h
 * @brief  Akce Virtual CMT z UI: automat dotazů nad páskou, uložení, export, úpravy pásky, testovací háky.
 *
 * Definice jsou v cmt_tape_actions.cpp (automat, dialogy souborů, zásahy
 * úprav pásky a jejich výsledky), cmt_window.cpp (stav záznamu vstupů)
 * a cmt_fix_mzfsize.cpp (popup kontroly velikosti MZF). Všechny funkce
 * patří UI vláknu; jádro mění jen frontami zásahů.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#ifndef IMGUI_CMT_H
#define IMGUI_CMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void imgui_cmt_fix_mzfsize_popup(bool *p_open);
    void imgui_cmt_fix_mzfsize_popup_activate(const char *filename, int valid_size, int file_size);

    /**
     * @brief true = běží záznam vstupů (ne přehrávání).
     *
     * Transport kazety (play, stop, pauza, výběr bloku, nahrávání, vysunutí)
     * jde přes replay_host_cmt() a záznam ho zapíše; vložení jiné pásky,
     * rychlost, vlastní pulzy, polarita a režim načítání se při záznamu
     * zamykají. Definice v cmt_window.cpp (okno Virtual CMT).
     */
    bool imgui_cmt_replay_recording(void);

    /**
     * @brief true = přehrává se záznam vstupů (ne nahrává).
     *
     * Páska pak patří přehrávání (po něm se vrátí páska uživatele): dotazy
     * Virtual CMT (imgui_cmt_tape_action(), imgui_cmt_quit_requested()) se
     * nekladou a akce proběhne, jak dovolí pravidla replaye.
     */
    bool imgui_cmt_replay_playing(void);

    /**
     * @brief Nastaví příštímu modálu polohu uprostřed okna Virtual CMT a jeho viewport.
     *
     * Volá se těsně před BeginPopupModal() každého modálu okna kazety
     * (potvrzení mazání, dotazy na nahrávání a neuložené změny, editor
     * vlastních pulzů, oprava velikosti MZF): modál se při každém otevření
     * (ImGuiCond_Appearing, pivot 0,5) objeví uprostřed okna kazety, i když
     * okno leží u kraje obrazovky nebo ve vlastním okně OS (multi-viewport).
     * Nekreslilo-li se okno v tomto ani minulém snímku (zavřené), nic
     * nenastaví: modál dostane výchozí polohu ImGui (střed hlavního viewportu).
     * Definice v cmt_window.cpp.
     *
     * @pre UI vlákno, uvnitř snímku ImGui; další volání Begin*() musí být modál.
     * @post Při otevřeném okně kazety: SetNextWindowPos() nastavené (viewport se nepřipíná, modál jde přetáhnout mimo okno).
     */
    void imgui_cmt_window_modal_next_pos(void);

    /**
     * @brief Akce, které vloženou pásku zahodí (dotaz na nahrávání a neuložené změny).
     *
     * Akce s "_FILE" nesou cestu (imgui_cmt_tape_file_action()): jsou to
     * návraty souborových dialogů akcí bez "_FILE", u kterých se stav pásky
     * kontroluje znovu (mezi dotazem a výběrem souboru se mohl zhoršit).
     */
    typedef enum en_IMGUI_CMT_TAPE_ACTION
    {
        IMGUI_CMT_TAPE_ACTION_NONE = 0,           /**< Žádná akce. */
        IMGUI_CMT_TAPE_ACTION_EJECT,              /**< Vysunutí pásky (replay_host_cmt(REPLAY_CMT_EJECT)). */
        IMGUI_CMT_TAPE_ACTION_OPEN,               /**< Dialog otevření pásky; vybraný soubor = OPEN_FILE. */
        IMGUI_CMT_TAPE_ACTION_OPEN_PLAY,          /**< Dialog otevření pásky a přehrání; soubor = OPEN_FILE_PLAY. */
        IMGUI_CMT_TAPE_ACTION_NEW_TAPE,           /**< Nová prázdná páska (replay_host_cmt(REPLAY_CMT_NEW_TAPE)). */
        IMGUI_CMT_TAPE_ACTION_QUIT,               /**< Ukončení aplikace (sdlapp_quit(), zavření hlavního okna). */
        IMGUI_CMT_TAPE_ACTION_OPEN_FILE,          /**< Otevření vybraného souboru pásky (cmt_request_open()). */
        IMGUI_CMT_TAPE_ACTION_OPEN_FILE_PLAY,     /**< Otevření vybraného souboru pásky a přehrání. */
        IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD,      /**< Dialog načtení snapshotu; soubor = SNAPSHOT_LOAD_FILE. */
        IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, /**< Načtení vybraného snapshotu (snapshot_ui_submit(SNAPSHOT_REQ_LOAD)). */
        IMGUI_CMT_TAPE_ACTION_QUICK_LOAD,         /**< Quick Load (snapshot_ui_request_quickload_now()). */
        IMGUI_CMT_TAPE_ACTION_REPLAY_PLAY,        /**< Dialog přehrání záznamu vstupů; soubor = REPLAY_PLAY_FILE. */
        IMGUI_CMT_TAPE_ACTION_REPLAY_PLAY_FILE,   /**< Start přehrání vybraného záznamu vstupů (imgui_replay_start_play()). */
    } en_IMGUI_CMT_TAPE_ACTION;

    /**
     * @brief Provede akci s páskou; při nahrávání nebo u pásky Modified se nejdřív zeptá.
     *
     * Bez nahrávání a bez neuložených změn akci provede hned. Jinak ji odloží
     * a otevře modální dialog (imgui_cmt_tape_dialogs()):
     * - běží nahrávání: "A recording is in progress. Stop and keep it,
     *   discard it, or cancel?" - Stop recording (běžný STOP: dekódování
     *   a připojení k pásce; po zastavení pokračuje kontrolou Modified),
     *   Discard recording (nahrávání zahodí až sama akce; pokračuje kontrolou
     *   Modified), Cancel (nic);
     * - páska Modified: Save tape (uložit, u pásky bez souboru nebo
     *   s formátem, který pásku neunese, přes Save As; akce až po úspěšném
     *   uložení), Discard changes (provést akci), Cancel (nic). Během
     *   nahrávání záznamu vstupů se Save tape nenabízí (uložení se odmítá):
     *   Discard changes / Cancel a řádek "The tape cannot be saved while
     *   input recording is active.".
     *
     * Během přehrávání záznamu vstupů se automat na pásku přehrávání neptá:
     * akce proběhne, jak dovolí pravidla replaye. Ukončení (QUIT) ale
     * nejdřív přehrávání zastaví (imgui_replay_stop_play(), páska uživatele
     * uschovaná přehráváním se vrátí) a pak se zeptá jako vždy - páska
     * uživatele se při ukončení nikdy neztratí bez dotazu.
     *
     * Akce se nikdy neprovede pod device_state_lock() (některé synchronně
     * čekají na emu vlákno): volaná pod zámkem (okno Virtual CMT, menu
     * Devices, dialogy) se odloží a provede ji imgui_cmt_tape_run_deferred()
     * po uvolnění zámku v témže snímku.
     *
     * Dotaz běží jako jeden stavový automat: druhý požadavek během
     * rozpracovaného dotazu (dialog, čekání na STOP, na uložení nebo na
     * dialog Save As) se ignoruje. Dialog, který zavřel někdo jiný (jiný
     * modál), se znovu otevře, jakmile je volno (imgui_modal_can_open()).
     * Akce s dialogem souboru (OPEN, SNAPSHOT_LOAD, REPLAY_PLAY) si
     * zapamatují, co uživatel odsouhlasil; vybraný soubor jde přes
     * imgui_cmt_tape_file_action() a při horším stavu se dotaz zopakuje.
     * Souhlas s nahráváním (Discard recording) platí jen pro nahrávání,
     * které tehdy běželo; nahrávání spuštěné později (MCP) se zeptá znovu.
     *
     * @param action Akce (NONE = nic).
     * @pre UI vlákno; zámek stavu zařízení si vezme, nedrží-li ho.
     */
    void imgui_cmt_tape_action(en_IMGUI_CMT_TAPE_ACTION action);

    /**
     * @brief Návrat souborového dialogu akce: provést akci "_FILE" s cestou, je-li stav pásky stejný.
     *
     * Porovná nahrávání a Modified s tím, co uživatel odsouhlasil (nebo
     * co platilo) při spuštění akce bez "_FILE". Zhoršil-li se stav (začalo
     * nahrávání, páska je teď Modified - automatické ukončení záznamu, MCP),
     * zeptá se znovu jako imgui_cmt_tape_action(); jinak akci provede hned.
     * Cancel v dialogu u SNAPSHOT_LOAD_FILE obnoví běh emulace jako zrušený
     * dialog snapshotu (snapshot_ui_load_cancelled()).
     *
     * @param action OPEN_FILE, OPEN_FILE_PLAY, SNAPSHOT_LOAD_FILE nebo REPLAY_PLAY_FILE.
     * @param path Vybraný soubor (zkopíruje se).
     * @pre UI vlákno.
     */
    void imgui_cmt_tape_file_action(en_IMGUI_CMT_TAPE_ACTION action, const char *path);

    /**
     * @brief Běží dotaz automatu imgui_cmt_tape_action() (dialog nebo čekání na jeho krok)?
     *
     * Okno zpráv (message_window.cpp) během něj zprávy drží ve frontě:
     * dotazy Virtual CMT mají přednost, zprávy se ukážou po dokončení.
     *
     * @return true = čeká akce (s_pending), nebo akce odložená do uvolnění zámku.
     * @pre UI vlákno.
     */
    bool imgui_cmt_tape_flow_active(void);

    /**
     * @brief Provede akci automatu odloženou pod zámkem stavu zařízení.
     *
     * Akce zvolená v dialogu automatu nebo zahájená z okna či menu
     * kresleného pod device_state_lock() se zapamatuje (cmt_ui_run_or_defer());
     * tahle funkce ji provede, včetně odloženého návratu zrušeného načtení
     * snapshotu (snapshot_ui_load_cancelled()). Volá ji main_window.cpp hned
     * po device_state_unlock() úseku s oknem Virtual CMT a dialogy a menu
     * (topmenu.cpp) po úseku menu Devices, takže akce proběhne ve stejném
     * snímku. Stav pásky (nahrávání, Modified, čítač nahrávání) se před
     * provedením přečte znovu se souhlasem z doby rozhodnutí: zhoršil-li
     * se mezitím, automat se zeptá znovu. Bez odložené akce nic.
     *
     * @pre UI vlákno, NEdrží device_state_lock() (akce může synchronně čekat
     *      na emu vlákno: dbg_ui_pause(), dbg_ui_run()).
     */
    void imgui_cmt_tape_run_deferred(void);

#ifdef MZTEST_HEADLESS
    /** @brief Jen pro testy: volba, kterou dialog automatu "stiskne" v příštím snímku. */
    typedef enum en_IMGUI_CMT_TEST_CHOICE
    {
        IMGUI_CMT_TEST_CHOICE_NONE = 0,  /**< nic, čeká se na uživatele */
        IMGUI_CMT_TEST_CHOICE_STOP_REC,  /**< Stop recording */
        IMGUI_CMT_TEST_CHOICE_DISCARD,   /**< Discard recording / Discard changes */
        IMGUI_CMT_TEST_CHOICE_CANCEL,    /**< Cancel */
        IMGUI_CMT_TEST_CHOICE_SAVE,      /**< Save tape (jen když ho dialog nabízí) */
    } en_IMGUI_CMT_TEST_CHOICE;

    /** @brief Jen pro testy: nastaví volbu (spotřebuje ji první vykreslený dialog). @param c Volba. */
    void imgui_cmt_tape_test_choose(en_IMGUI_CMT_TEST_CHOICE c);

    /**
     * @brief Jen pro testy: dialog automatu vykreslený v posledním snímku.
     * @return 0 žádný, 1 probíhající nahrávání, 2 neuložené změny.
     */
    int imgui_cmt_tape_test_dialog_shown(void);

    /**
     * @brief Jen pro testy: čeká dialog Save tape as?
     *
     * V testovacím sestavení se dialog Save As neotevírá (test ho nekreslí):
     * automat jen zaznamená, že by se otevřel, a test ho dokončí
     * imgui_cmt_tape_test_save_as_finish().
     *
     * @return 0 = ne, 1 = čeká (Save tape as z menu), 2 = čeká s čekající akcí dotazu.
     */
    int imgui_cmt_tape_test_save_as_open(void);

    /**
     * @brief Jen pro testy: dokončí čekající dialog Save tape as.
     *
     * Jako callback skutečného dialogu: uloží jen pásku se stejným čítačem
     * výměn pásky jako při otevření dialogu, jinak ohlásí "The tape changed;
     * the action was cancelled." a čekající akci zruší.
     *
     * @param path Vybraná cesta (zkopíruje se; bez přípony doplní .tmz), NULL = zrušeno.
     */
    void imgui_cmt_tape_test_save_as_finish(const char *path);

    /**
     * @brief Jen pro testy: poslední imgui_cmt_tape_export_block() (a vynulování).
     * @param[out] block_id Blok (NULL = nezajímá). @param[out] wav Export do WAV (NULL = nezajímá).
     * @return true = export byl vyžádán od posledního volání.
     */
    bool imgui_cmt_tape_test_take_last_export(int *block_id, bool *wav);

    /** @brief Jen pro testy: čítač výměn pásky, se kterým byl vyžádán poslední export. @return Čítač. */
    unsigned imgui_cmt_tape_test_last_export_serial(void);

    /**
     * @brief Jen pro testy: dokončí export, jako by dialog souboru vrátil cestu (test dialog nekreslí).
     * @param block_id Blok. @param tape_serial Čítač výměn pásky z doby volby bloku. @param path Cíl.
     */
    void imgui_cmt_tape_test_export_finish(int block_id, unsigned tape_serial, const char *path);

    /** @brief Jen pro testy: poslední provedená akce (a vynulování). @return Akce, NONE = žádná. */
    en_IMGUI_CMT_TAPE_ACTION imgui_cmt_tape_test_take_last_run(void);

    /** @brief Jen pro testy: vynuluje stav automatu (mezi testy). */
    void imgui_cmt_tape_test_reset(void);

    /**
     * @brief Jen pro testy: poslední imgui_cmt_tape_import() (dialog Import file) a vynulování.
     *
     * V testovacím sestavení se dialog souboru neotevírá; test ho dokončí
     * imgui_cmt_tape_test_import_finish().
     *
     * @param[out] at Pozice vložení (NULL = nezajímá). @param[out] tape_serial Čítač (NULL = nezajímá).
     * @return true = dialog byl vyžádán od posledního volání.
     */
    bool imgui_cmt_tape_test_take_last_import(int *at, unsigned *tape_serial);

    /**
     * @brief Jen pro testy: dokončí dialog Import file, jako by vrátil cestu.
     * @param path Vybraný soubor, NULL = zrušeno.
     */
    void imgui_cmt_tape_test_import_finish(const char *path);

    /**
     * @brief Jen pro testy: poslední imgui_cmt_tape_export_tape() (dialog Export tape as WAV) a vynulování.
     * @param[out] tape_serial Čítač (NULL = nezajímá).
     * @return true = dialog byl vyžádán od posledního volání.
     */
    bool imgui_cmt_tape_test_take_last_export_tape(unsigned *tape_serial);

    /**
     * @brief Jen pro testy: dokončí dialog Export tape as WAV, jako by vrátil cestu.
     * @param path Cíl, NULL = zrušeno.
     */
    void imgui_cmt_tape_test_export_tape_finish(const char *path);

    /** @brief Jen pro testy: čeká postupný import na výsledek zásahu? @return true = ano. */
    bool imgui_cmt_tape_test_import_active(void);
#endif

    /**
     * @brief Požadavek na zavření hlavního okna (ukončení aplikace) z GUI.
     *
     * Bez nahrávání a bez neuložených změn pásky vrátí true (zavřít hned).
     * Během přehrávání záznamu vstupů vrátí false: přehrávání se nejdřív
     * zastaví (vrátí se uschovaná páska uživatele) a pak automat proběhne
     * běžně (zeptá se, je-li ta páska Modified, jinak aplikaci ukončí).
     * Jinak zahájí imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_QUIT) a vrátí
     * false: aplikace skončí až po Save / Discard (nebo Stop recording
     * a dotazu na Modified) přes sdlapp_quit(); Cancel ukončení zruší.
     * Headless běh a automatizace (--exit-*, MCP shutdown) tudy nejdou
     * a nikdy nečekají na dialog (cmt_exit() jen zapíše varování do logu).
     *
     * @return true = okno zavřít, false = ukončení odloženo (dialog).
     * @pre UI vlákno (callback close_requested sdlapp); zámek stavu zařízení
     *      si vezme, nedrží-li ho.
     */
    bool imgui_cmt_quit_requested(void);

    /**
     * @brief Jméno vložené pásky pro UI: jméno souboru, u nové pásky "(new tape)",
     *        s "*" na konci při neuložených změnách.
     * @param buf Výstup (vždy ukončený nulou). @param size Velikost @p buf.
     * @pre UI vlákno, drží zámek stavu zařízení, páska vložená.
     */
    void imgui_cmt_tape_name(char *buf, size_t size);

    /**
     * @brief Uloží vloženou pásku (Save), nebo otevře dialog Save tape as (Save as...).
     *
     * Save: páska se souborem formátu, do kterého se ukládá a který pásku
     * unese (cmttmz_save_fits()), se uloží zásahem cmt_request_save_on_tape();
     * jinak (bez souboru, .wav, .tap, formát pásku neunese) se otevře Save
     * tape as (výchozí TMZ). Uloží se jen páska, kterou uživatel viděl
     * (čítač výměn pásky při volbě, resp. při otevření dialogu); jinou
     * pásku emu vlákno ani UI neuloží a ohlásí "The tape changed; the
     * action was cancelled.". Výsledek uložení jde do řádku zpráv okna
     * Virtual CMT, při skrytém okně chyba modálně (imgui_cmt_device_request_result(),
     * cmt_view_post()). Volá menu Tape okna
     * a zkratka Ctrl+S; zákaz (STOP, mimo záznam vstupů) hlídá volající.
     *
     * @param save_as true = vždy dialog Save tape as.
     * @pre UI vlákno; zámek stavu zařízení si vezme, nedrží-li ho.
     */
    void imgui_cmt_tape_save(bool save_as);

    /**
     * @brief Položka hlavního menu CMT Options > Enable CPU Boost: zařadí zásah cmt_request_set_loading_boost().
     *
     * Zapamatuje si pořadové číslo zásahu, aby jeho výsledek šel ohlásit
     * podle toho, co se stalo: se zapnutým CMT hackem zůstává režim Direct
     * a mění se jen volba CPU Boost - řádek zpráv pak nic nehlásí (ne
     * "Loading mode: Direct"); jinak výsledný režim jako u přepínače Loading.
     *
     * @param enabled true = CPU Boost zapnout.
     * @pre UI vlákno.
     */
    void imgui_cmt_menu_set_cpu_boost(bool enabled);

    /**
     * @brief Otevře dialog exportu bloku pásky do MZF nebo WAV.
     *
     * Po výběru souboru zařadí cmt_request_export_block_on_tape() (export
     * provede emu vlákno, výsledek jde do řádku zpráv okna Virtual CMT).
     * Dialog je asynchronní: je-li po výběru souboru vložená jiná páska
     * (čítač výměn pásky se liší od @p tape_serial), export se zahodí se
     * zprávou "The tape changed; the action was cancelled."; totéž ověří
     * emu vlákno při provedení. Volá ho menu Tape (otevřený blok)
     * i kontextové menu tabulky bloků (libovolný blok).
     *
     * @param block_id Index bloku v souboru pásky (záporný = nic).
     * @param wav true = WAV, false = MZF (jen blok, který MZF nese; ověří
     *        ho emu vlákno při exportu).
     * @param tape_serial Čítač výměn pásky (st_CMT_VIEW::tape_serial) pásky, ze které blok pochází.
     * @pre UI vlákno, drží zámek stavu zařízení (čte poslední adresář pásky).
     */
    void imgui_cmt_tape_export_block(int block_id, bool wav, unsigned tape_serial);

    /**
     * @brief Smaže bloky pásky, kterou okno ukazuje (zásah cmt_request_remove_blocks_on_tape()).
     *
     * Bez dotazu: potvrzení u neuložených bloků dělá okno před voláním.
     * Výsledek ohlásí imgui_cmt_device_request_result(): "Deleted block %d."
     * (jeden blok, číslo od 1) nebo "Deleted %d blocks.", chyba textem jádra;
     * blok na místě prvního smazaného se vybere (cmt_view_select_after_edit()).
     * Když se seznam rozsahů nevejde do argumentu zásahu, ohlásí chybu
     * "Too many separate blocks selected." a nic nezařadí.
     *
     * @param tape_serial Čítač výměn pásky kopie stavu (st_CMT_VIEW::tape_serial).
     * @param block_ids Čísla bloků (libovolné pořadí, bez opakování); NULL s @p count 0 = nic.
     * @param count Počet bloků.
     * @return Pořadové číslo zásahu, 0 = nezařazeno (chyba ohlášená, nebo prázdný seznam).
     * @pre UI vlákno.
     */
    uint32_t imgui_cmt_tape_remove_blocks(unsigned tape_serial, const int *block_ids, int count);

    /**
     * @brief Přesune blok pásky, kterou okno ukazuje (zásah cmt_request_move_block_on_tape()).
     *
     * Výsledek ohlásí imgui_cmt_device_request_result() jen při chybě;
     * po úspěchu se vybere přesunutý blok na pozici @p to.
     *
     * @param tape_serial Čítač výměn pásky kopie stavu. @param block_id Blok.
     * @param to Pozice po přesunu (0 .. počet bloků - 1).
     * @return Pořadové číslo zásahu, 0 = nezařazeno (neplatné argumenty).
     * @pre UI vlákno.
     */
    uint32_t imgui_cmt_tape_move_block(unsigned tape_serial, int block_id, int to);

    /**
     * @brief Je výsledek zásahu přesunu @p seq už vyřízený (úspěch, chyba, jiná páska i odmítnutí)?
     *
     * Okno podle něj pozná, kdy může použít přesun čekající na předchozí
     * (rychlé Alt+Up / Alt+Down). Pořadová čísla zásahů rostou
     * a přesuny se vyřizují v pořadí zařazení.
     *
     * @param seq Pořadové číslo z imgui_cmt_tape_move_block() (0 = vždy true).
     * @return true = výsledek přesunu s tímto nebo vyšším číslem už prošel imgui_cmt_device_request_result().
     * @pre UI vlákno.
     */
    bool imgui_cmt_tape_move_done(uint32_t seq);

    /**
     * @brief Otevře dialog Import file (filtry jako Open...) a vybraný soubor importuje před blok @p at.
     *
     * Po výběru souboru zavolá imgui_cmt_tape_import_paths() s jedním
     * souborem. Dialog je asynchronní; čítač výměn pásky z doby volby ověří
     * emu vlákno (jiná páska = "The tape changed; the action was cancelled.").
     *
     * @param at Pozice vložení (0 .. počet bloků), záporná = na konec.
     * @param tape_serial Čítač výměn pásky kopie stavu; bez pásky 0 (import vloží novou pásku).
     * @pre UI vlákno.
     */
    void imgui_cmt_tape_import(int at, unsigned tape_serial);

    /**
     * @brief Importuje soubory postupně, každý až po výsledku předchozího.
     *
     * Zařadí cmt_request_import_on_tape() pro první soubor; další zařadí
     * výsledek zásahu (imgui_cmt_device_request_result()) s čítačem výměn
     * pásky z výsledku a pozicí za bloky předchozího souboru (na konec
     * zůstane na konec), takže soubory leží v pořadí, v jakém je dostal.
     * U prvního neúspěšného souboru (nečitelný soubor, dlouhá cesta, jiná
     * páska, odmítnutí při záznamu vstupů) se import zastaví: zpráva toho
     * souboru zůstane v řádku zpráv (při skrytém okně modálně) s větou "The
     * remaining files were not imported." a zbylé soubory se neimportují
     * Výsledek, který nepřijde (vyprázdněná nebo přeplněná
     * fronta výsledků), import ukončí varováním "The import could not be
     * completed." (hlídač v imgui_cmt_tape_dialogs()); import tak nikdy
     * nečeká navždy. Po importu se vybere první importovaný blok prvního
     * souboru. Běží-li jiný import, nový se nezařadí ("Another import is
     * still in progress.").
     *
     * @param paths Cesty (UTF-8, zkopírují se). @param count Počet cest (0 = nic).
     * @param at Pozice vložení prvního souboru, záporná = na konec.
     * @param tape_serial Čítač výměn pásky kopie stavu; bez pásky 0.
     * @pre UI vlákno.
     */
    void imgui_cmt_tape_import_paths(const char *const *paths, int count, int at, unsigned tape_serial);

    /**
     * @brief Otevře pásku ze souboru obvyklým postupem (dotazy na nahrávání a neuložené změny).
     *
     * Jako návrat dialogu Open... bez dřívějšího souhlasu
     * (IMGUI_CMT_TAPE_ACTION_OPEN_FILE); používá ho soubor přetažený z OS nad
     * okno Virtual CMT mimo tabulku bloků.
     *
     * @param path Soubor (UTF-8, zkopíruje se; NULL nebo prázdný = nic).
     * @return true = automat spuštěný (nebo nic k otevření); false = běží jiný
     *         dotaz automatu nebo čeká odložená akce, požadavek se nepoužil
     *         (volající ohlásí varování).
     * @pre UI vlákno; pod zámkem stavu zařízení se akce odloží (imgui_cmt_tape_run_deferred()).
     */
    bool imgui_cmt_tape_open_path(const char *path);

    /**
     * @brief Otevře dialog Export tape as WAV a vybraný soubor předá zásahu cmt_request_export_tape_on_tape().
     *
     * Dialog je asynchronní: je-li po výběru souboru vložená jiná páska nebo
     * se páska mezitím upravila (čítač výměn pásky jiný než @p tape_serial),
     * export se nezařadí a okno ukáže "The tape changed; the action was
     * cancelled.". Nestojí-li deck ve STOP nebo běží-li záznam či přehrávání
     * vstupů, export se nezařadí s varováním "The tape state changed; the
     * action was cancelled.". Zásah nese čítač dál: změní-li páska mezi
     * kontrolou a provedením (MCP), emu vlákno nic nezapíše a okno ukáže
     * totéž varování. Výsledek (text
     * cmttmz_get_export_message(), varování cmttmz_get_export_warning())
     * ohlásí imgui_cmt_device_request_result().
     *
     * @param tape_serial Čítač výměn pásky kopie stavu.
     * @pre UI vlákno, drží zámek stavu zařízení (čte jméno pásky).
     */
    void imgui_cmt_tape_export_tape(unsigned tape_serial);

    /**
     * @brief Dialogy automatu imgui_cmt_tape_action() a oznámení výsledku nahrávání.
     *
     * Každý snímek bez ohledu na otevřené okno Virtual CMT (akci lze zahájit
     * i z hlavního menu). Modál se otevře jen při volnu (imgui_modal_can_open()),
     * zavřený někým jiným se otevře znovu. Výsledek nahrávání (i záznamu
     * zahozeného vysunutím) ohlásí jednou přes cmt_view_poll_record(): při
     * viditelném okně Virtual CMT do řádku zpráv, jinak zahozený záznam
     * modálně (varování) a dokončený vůbec. Operace okna zahozené emu
     * vláknem kvůli změně stavu decku ohlásí jedním varováním
     * cmt_view_poll_dropped(). Dialog otevřený ve
     * chvíli, kdy začalo přehrávání záznamu vstupů, se zavře a akce
     * pokračuje bez dotazu. Akci zvolenou v dialogu jen odloží, provede ji
     * imgui_cmt_tape_run_deferred() po uvolnění zámku. Údržba dialogu
     * analýzy záznamu (cmt_analyze_dialog_frame()): dialog, který
     * okno v tomto snímku nenakreslilo (zavřené okno), se opustí; data
     * ztraceného zásahu uplatnění analýzy se uvolní. Totéž opuštění pro
     * modály informací o pásce a převodu profilu (cmt_tapeinfo_dialog_frame(),
     * cmt_convert_dialog_frame()).
     *
     * @pre UI vlákno, drží zámek stavu zařízení (main_window); volá se po
     *      kreslení okna Virtual CMT téhož snímku.
     */
    void imgui_cmt_tape_dialogs(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CMT_H */
