/**
 * @file cmt_tape_actions.cpp
 * @brief Akce s páskou okna Virtual CMT: uložení, export bloku, nová páska, dotaz na neuložené změny.
 *
 * Akce, které pásku zahodí (vysunutí, otevření jiné včetně "Fast Open"
 * z menu Devices, nová páska, ukončení aplikace zavřením hlavního okna,
 * načtení snapshotu, Quick Load a start přehrání záznamu vstupů z UI),
 * se nejdřív zeptají modálním dialogem (jeden stavový automat, s_pending):
 * - při nahrávání (RECORD): Stop recording / Discard recording / Cancel;
 *   Stop recording provede běžný STOP (replay_host_cmt(), záznam se
 *   dekóduje a připojí k pásce) a po zastavení pokračuje dotazem na
 *   neuložené změny;
 * - u pásky se změnami neuloženými do souboru (Modified,
 *   cmttmz_is_modified()): Save tape / Discard changes / Cancel. Save tape
 *   u pásky bez souboru, se souborem, do jehož formátu se neukládá (např.
 *   .wav), nebo s formátem, který pásku neunese (cmttmz_save_fits(), např.
 *   .mzf se dvěma bloky), otevře Save As (výchozí TMZ) a čekající akci
 *   provede až po úspěšném uložení (výsledek zásahu cmt_request_save_on_tape();
 *   uložení se váže na pásku, kterou uživatel viděl - po výměně pásky se nic
 *   neuloží a čekající akce se zruší).
 *
 * Uložení a export jdou jako zásahy emu vlákna (cmt_request.h, při
 * záznamu a přehrání vstupů se odmítají); výsledky ohlásí
 * imgui_cmt_device_request_result(). Nová páska jde jako vysunutí přes
 * replay_host_cmt() (záznam vstupů ji zapíše a přehrání zopakuje).
 * Výsledky zásahů kazety (uložení, export, otevření, režim načítání,
 * odmítnutá rychlost) jdou do řádku zpráv okna Virtual CMT
 * (cmt_view_post_message()), výsledek nahrávání tam čte kopie stavu sama;
 * okno zpráv (message_window.cpp) je dál hlásí jen chyby otevření pásky
 * (baseui_error()) a jiné zprávy aplikace. Každá událost se hlásí na jednom
 * místě. Okno zpráv během dotazu automatu zprávy drží
 * (imgui_cmt_tape_flow_active()).
 * Modály se otevírají jen při volnu (imgui_modal.h): jinak by OpenPopup()
 * jiného modálu na téže úrovni dialog zavřel a čekající akce by visela.
 *
 * Záznam vstupů: při jeho PŘEHRÁVÁNÍ páska patří přehrávání (po něm se
 * vrátí páska uživatele), automat se proto na pásku přehrávání nikdy
 * neptá a akce proběhne, jak dovolí pravidla replaye. Výjimkou je
 * ukončení aplikace: páska uživatele je po dobu přehrávání uschovaná
 * (replay_media.c) a konec procesu by ji zahodil bez dotazu, proto ukončení
 * nejdřív přehrávání zastaví běžnou cestou UI (imgui_replay_stop_play(),
 * páska uživatele se vrátí) a pak proběhne běžný dotaz. Při NAHRÁVÁNÍ
 * záznamu vstupů se automat ptá, ale Save tape nenabízí (uložení se tehdy
 * odmítá) - jen Discard changes / Cancel a vysvětlení.
 *
 * Úpravy pásky z okna: smazání, přesun a import bloků jdou jako
 * zásahy cmt_request_*_on_tape() s čítačem výměn pásky; jejich výsledky
 * (zpráva, blok k výběru) zpracuje imgui_cmt_device_request_result() z dat,
 * která imgui_cmt_device_request_capture() přečetla ve stejném držení
 * zámku jako vyzvednutí výsledku z fronty. Import více souborů běží
 * postupně: každý další soubor se zařadí až po výsledku předchozího
 * s čítačem výměn pásky z výsledku. Export celé pásky do WAV jde přes
 * dialog souboru a zásah cmt_request_export_tape_on_tape() s čítačem
 * výměn pásky z doby volby.
 *
 * Odložené provedení: akce automatu (a návrat zrušeného načtení
 * snapshotu) se nikdy neprovede pod device_state_lock(). Některé čekají
 * synchronně na emu vlákno (snapshot_ui_submit(), načtení snapshotu
 * a Quick Load přes dbg_ui_pause(), zrušené načtení přes dbg_ui_run()),
 * které si týž zámek bere pro zásahy MCP; pod zámkem by UI vlákno čekalo
 * na vlákno, které čeká na UI. Akce zvolená v dialogu (nebo zahájená z okna
 * či menu kresleného pod zámkem) se proto zapamatuje a provede
 * imgui_cmt_tape_run_deferred() v témže snímku hned po uvolnění zámku.
 *
 * Veškerý stav tohoto modulu patří UI vláknu.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <glib.h>
#include <string>
#include <vector>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "main.h" /* g_sdlapp, sdlapp_quit() */
#include "baseui/baseui_filechooser.h"
#include "device_request.h"
#include "emulator/replay.h"
#include "emulator/hw-generic/cmt/cmt.h"
#include "emulator/hw-generic/cmt/cmt_request.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_block.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_tmz_record.h"
#include "ui-imgui/message/message_window.h"
#include "imgui_cmt.h"
#include "cmt_view.h"
#include "cmt_analyze_dialog.h"
#include "cmt_tapeinfo_dialog.h"
#include "cmt_convert_dialog.h"
#include "ui-imgui/devices/device_request_ui.h"
#include "ui-imgui/imgui_modal.h"
#include "ui-imgui/snapshot/snapshot_request_ui.h"

extern "C"
{
    /* menu_snapshot.cpp, snapshot_load_dialog.cpp, menu_replay.cpp: provedení
     * akcí po dotazu (bez dalšího dotazu) */
    void snapshot_ui_request_load_now(void);
    void snapshot_ui_request_quickload_now(void);
    void snapshot_ui_load_cancelled(void);
    void imgui_replay_open_play_dialog_now(void);
    void imgui_replay_start_play(const char *path);
    void imgui_replay_stop_play(void);
}


/**
 * @brief Pravidlo replaye pro dotazy: true = přehrává se záznam vstupů (páska patří přehrávání, dotazy se nekladou).
 * @return imgui_cmt_replay_playing().
 */
static bool cmt_ui_replay_playing(void)
{
    return imgui_cmt_replay_playing();
}




/** @brief Dialog automatu dotazu (imgui_cmt_tape_action()). */
typedef enum en_CMT_UI_DIALOG
{
    CMT_UI_DIALOG_NONE = 0,  /**< žádný dialog */
    CMT_UI_DIALOG_RECORDING, /**< probíhající nahrávání: Stop recording / Discard recording / Cancel */
    CMT_UI_DIALOG_UNSAVED,   /**< neuložené změny: Save tape / Discard changes / Cancel */
} en_CMT_UI_DIALOG;

/**
 * @brief Akce čekající na dotaz, na STOP, na dialog Save As nebo na výsledek uložení (NONE = automat stojí).
 *
 * Invariant: s_pending == NONE => s_dialog == NONE, s_save_seq == 0,
 * s_record_stopping == false, s_pending_path == NULL.
 */
static en_IMGUI_CMT_TAPE_ACTION s_pending = IMGUI_CMT_TAPE_ACTION_NONE;
/** @brief Cesta akce "_FILE" (g_free), jinak NULL. */
static gchar *s_pending_path = NULL;
/** @brief Dialog, který automat právě chce ukázat. */
static en_CMT_UI_DIALOG s_dialog = CMT_UI_DIALOG_NONE;
/** @brief true = popup dialogu s_dialog je otevřený (OpenPopup už proběhlo a nikdo ho nezavřel). */
static bool s_dialog_open = false;
/** @brief Uživatel odsouhlasil ztrátu probíhajícího nahrávání (Discard recording); platí jen pro nahrávání s čítačem s_ok_rec_serial. */
static bool s_ok_recording = false;
/**
 * @brief Čítač výsledků nahrávání (cmttmz_get_record_result()) v okamžiku souhlasu s_ok_recording.
 *
 * Nahrávání, se kterým uživatel souhlasil, skončí zvýšením čítače; nahrávání
 * spuštěné potom (např. přes MCP během dalšího dialogu nebo výběru souboru)
 * má jiný čítač a souhlas na něj neplatí - automat se zeptá znovu.
 */
static unsigned s_ok_rec_serial = 0;
/** @brief Uživatel odsouhlasil ztrátu neuložených změn (Discard changes, nebo páska nebyla Modified). */
static bool s_ok_modified = false;
/** @brief true = Stop recording zadán, automat čeká, až transport opustí RECORD. */
static bool s_record_stopping = false;
/** @brief Pořadové číslo zásahu uložení, na jehož výsledek čeká s_pending (0 = nečeká se). */
static uint32_t s_save_seq = 0;
/** @brief Odsouhlasený stav pro návrat souborového dialogu (imgui_cmt_tape_file_action()). */
static bool s_file_ok_recording = false;
/** @brief Čítač výsledků nahrávání pro s_file_ok_recording (souhlas platí jen pro totéž nahrávání). */
static unsigned s_file_ok_rec_serial = 0;
/** @brief Odsouhlasený stav Modified pro návrat souborového dialogu. */
static bool s_file_ok_modified = false;
/**
 * @brief Akce automatu odložená do uvolnění zámku stavu zařízení (NONE = žádná).
 *
 * Nastaví ji cmt_ui_run_or_defer(), provede imgui_cmt_tape_run_deferred().
 * Invariant: s_deferred != NONE => s_pending == NONE (automat akci předal).
 */
static en_IMGUI_CMT_TAPE_ACTION s_deferred = IMGUI_CMT_TAPE_ACTION_NONE;
/** @brief Cesta odložené akce "_FILE" (g_free), jinak NULL. */
static gchar *s_deferred_path = NULL;
/** @brief Souhlas s nahráváním platný při odložení (s_ok_recording) a jeho čítač; ověří se znovu před provedením. */
static bool s_deferred_ok_recording = false;
/** @brief Čítač výsledků nahrávání k s_deferred_ok_recording. */
static unsigned s_deferred_ok_rec_serial = 0;
/** @brief Souhlas se ztrátou neuložených změn platný při odložení (s_ok_modified). */
static bool s_deferred_ok_modified = false;
/** @brief true = odložený návrat zrušeného načtení snapshotu (snapshot_ui_load_cancelled()). */
static bool s_deferred_load_cancelled = false;
/**
 * @brief true = ukončení (s_pending QUIT) čeká, až skončí přehrávání záznamu vstupů.
 *
 * Nastaví cmt_ui_flow_step(), když ukončení přijde během přehrávání (zastaví
 * ho imgui_replay_stop_play()); imgui_cmt_tape_dialogs() po konci přehrávání
 * (páska uživatele je zpět) pokračuje běžným krokem automatu.
 */
static bool s_await_replay_off = false;

#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: volba, kterou "stiskne" příští vykreslený dialog. */
static en_IMGUI_CMT_TEST_CHOICE s_test_choice = IMGUI_CMT_TEST_CHOICE_NONE;
/** @brief Jen pro testy: dialog vykreslený v posledním volání imgui_cmt_tape_dialogs(). */
static int s_test_dialog_shown = 0;
/** @brief Jen pro testy: poslední provedená akce. */
static en_IMGUI_CMT_TAPE_ACTION s_test_last_run = IMGUI_CMT_TAPE_ACTION_NONE;
/** @brief Jen pro testy: blok posledního imgui_cmt_tape_export_block() (-1 = žádný). */
static int s_test_export_block = -1;
/** @brief Jen pro testy: poslední export byl WAV (jinak MZF). */
static bool s_test_export_wav = false;
/** @brief Jen pro testy: čítač výměn pásky posledního imgui_cmt_tape_export_block(). */
static unsigned s_test_export_serial = 0;

/** @brief Jen pro testy: true = volba @p c je na řadě (a spotřebuje se). @param c Volba. */
static bool cmt_ui_test_pick(en_IMGUI_CMT_TEST_CHOICE c)
{
    if (s_test_choice != c)
        return false;
    s_test_choice = IMGUI_CMT_TEST_CHOICE_NONE;
    return true;
}
#else
/** @brief Mimo testy žádná vnucená volba. */
#define cmt_ui_test_pick(c) false
#endif

/** @brief ImGui ID dialogu neuložených změn (titulek se překládá, ID ne). */
#define CMT_UNSAVED_POPUP_ID "###cmt_unsaved_changes"
/** @brief ImGui ID dialogu probíhajícího nahrávání (titulek se překládá, ID ne). */
#define CMT_RECORDING_POPUP_ID "###cmt_recording_in_progress"


/**
 * @brief Vezme zámek stavu zařízení, nedrží-li ho volající vlákno už.
 * @return true = zámek vzat tady, volající ho musí uvolnit device_state_unlock().
 */
static bool cmt_ui_lock_if_needed(void)
{
    if (device_state_lock_held())
        return false;
    device_state_lock();
    return true;
}


/**
 * @brief Přečte, zda běží nahrávání, zda je páska Modified a čítač výsledků nahrávání (pod zámkem stavu zařízení).
 * @param[out] recording RECORD. @param[out] modified Páska Modified.
 * @param[out] rec_serial Čítač výsledků nahrávání (cmttmz_get_record_result()), smí být NULL.
 */
static void cmt_ui_tape_state(bool *recording, bool *modified, unsigned *rec_serial)
{
    bool taken = cmt_ui_lock_if_needed();
    *recording = CMT_TEST_FILLED && CMT_TEST_RECORD;
    *modified = CMT_TEST_FILLED && cmttmz_is_modified();
    if (rec_serial)
        cmttmz_get_record_result(NULL, NULL, rec_serial);
    if (taken)
        device_state_unlock();
}


/**
 * @brief Přípona souboru včetně tečky (část jména za poslední tečkou za posledním oddělovačem).
 * @param path Cesta nebo jméno souboru (smí být NULL).
 * @return Ukazatel do @p path na tečku přípony; NULL bez přípony nebo pro NULL.
 */
static const char *cmt_ui_file_ext(const char *path)
{
    if (!path)
        return NULL;
    const char *dot = strrchr(path, '.');
    if (!dot)
        return NULL;
    if (strchr(dot, '/') || strchr(dot, '\\'))
        return NULL;
    return dot;
}


/**
 * @brief Má přípona souboru formát, do kterého cmttmz_save() ukládá?
 * @param path Cesta nebo jméno souboru (smí být NULL).
 * @return true pro .tmz, .tzx, .mzt, .mzf, .m12 (bez ohledu na velikost písmen).
 */
static bool cmt_ui_savable_ext(const char *path)
{
    const char *ext = cmt_ui_file_ext(path);
    if (!ext)
        return false;
    static const char *const exts[] = {".tmz", ".tzx", ".mzt", ".mzf", ".m12"};
    for (const char *e : exts)
    {
        if (0 == strcasecmp(ext, e))
            return true;
    };
    return false;
}


static void cmt_ui_open_chooser(bool play);

#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: 0 = dialog Save As neotevřený, 1 = čeká (bez čekající akce), 2 = čeká s čekající akcí. */
static int s_test_save_as = 0;
#endif


/**
 * @brief Provede akci bez dotazu.
 *
 * Akce se souborovým dialogem si zapamatují stav pásky, se kterým uživatel
 * souhlasil (s_file_ok_*), pro kontrolu v imgui_cmt_tape_file_action().
 *
 * @param action Akce (NONE = nic). @param path Cesta akce "_FILE", jinak NULL.
 * @pre UI vlákno, NEdrží device_state_lock(): některé akce synchronně čekají
 *      na emu vlákno (dbg_ui_pause() v snapshot_ui_submit() a při načtení
 *      snapshotu nebo Quick Load); pod zámkem jde akce přes cmt_ui_run_or_defer().
 */
static void cmt_ui_run_action(en_IMGUI_CMT_TAPE_ACTION action, const char *path)
{
#ifdef MZTEST_HEADLESS
    s_test_last_run = action;
#endif
    switch (action)
    {
        case IMGUI_CMT_TAPE_ACTION_OPEN:
        case IMGUI_CMT_TAPE_ACTION_OPEN_PLAY:
        case IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD:
        case IMGUI_CMT_TAPE_ACTION_REPLAY_PLAY:
        {
            /* dialog souboru: co platí teď, uživatel přijal (dotazem, nebo nebylo
             * co ztratit); souhlas s nahráváním jen pro totéž nahrávání */
            bool recording, modified;
            unsigned serial = 0;
            cmt_ui_tape_state(&recording, &modified, &serial);
            s_file_ok_recording = recording && s_ok_recording && (serial == s_ok_rec_serial);
            s_file_ok_rec_serial = serial;
            s_file_ok_modified = modified || s_ok_modified;
            break;
        }
        default:
            break;
    };
    switch (action)
    {
        case IMGUI_CMT_TAPE_ACTION_EJECT:
            replay_host_cmt(REPLAY_CMT_EJECT, 0, NULL);
            break;
        case IMGUI_CMT_TAPE_ACTION_OPEN:
            cmt_ui_open_chooser(false);
            break;
        case IMGUI_CMT_TAPE_ACTION_OPEN_PLAY:
            cmt_ui_open_chooser(true);
            break;
        case IMGUI_CMT_TAPE_ACTION_OPEN_FILE:
        case IMGUI_CMT_TAPE_ACTION_OPEN_FILE_PLAY:
            if (path && (0 == cmt_request_open(path, action == IMGUI_CMT_TAPE_ACTION_OPEN_FILE_PLAY)))
                imgui_message_create(MESSAGE_TYPE_ERROR, _("The tape could not be opened: the file path is too long."));
            break;
        case IMGUI_CMT_TAPE_ACTION_NEW_TAPE:
            /* jako vysunutí: záznam vstupů operaci zapíše (událost cmt "new_tape") */
            replay_host_cmt(REPLAY_CMT_NEW_TAPE, 0, NULL);
            break;
        case IMGUI_CMT_TAPE_ACTION_QUIT:
            /* jako SDL_EVENT_QUIT: konec hlavní smyčky, úklid v main();
             * nahrávání a neuložené změny zahodí cmt_exit() */
            sdlapp_quit(g_sdlapp);
            break;
        case IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD:
            snapshot_ui_request_load_now();
            break;
        case IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE:
            if (path)
                snapshot_ui_submit(SNAPSHOT_REQ_LOAD, path, NULL);
            break;
        case IMGUI_CMT_TAPE_ACTION_QUICK_LOAD:
            snapshot_ui_request_quickload_now();
            break;
        case IMGUI_CMT_TAPE_ACTION_REPLAY_PLAY:
            imgui_replay_open_play_dialog_now();
            break;
        case IMGUI_CMT_TAPE_ACTION_REPLAY_PLAY_FILE:
            if (path)
                imgui_replay_start_play(path);
            break;
        default:
            break;
    };
}


/**
 * @brief Zrušená akce: úklid automatu a, kde je potřeba, návrat stavu před akcí.
 *
 * SNAPSHOT_LOAD_FILE: emulace se pozastavila už s dialogem snapshotu,
 * zrušení ji vrátí jako zrušený dialog (snapshot_ui_load_cancelled()).
 *
 * @pre UI vlákno.
 * @post Automat stojí (invariant s_pending == NONE).
 */
static void cmt_ui_flow_abort(void)
{
    en_IMGUI_CMT_TAPE_ACTION action = s_pending;
    s_pending = IMGUI_CMT_TAPE_ACTION_NONE;
    s_dialog = CMT_UI_DIALOG_NONE;
    s_save_seq = 0;
    s_record_stopping = false;
    s_await_replay_off = false;
    g_free(s_pending_path);
    s_pending_path = NULL;
    if (action == IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE)
    {
        /* snapshot_ui_load_cancelled() čeká na emu vlákno (dbg_ui_run()):
         * pod zámkem stavu zařízení až po jeho uvolnění */
        if (device_state_lock_held())
            s_deferred_load_cancelled = true;
        else
            snapshot_ui_load_cancelled();
    };
}


/**
 * @brief Provede akci automatu hned, nebo (pod zámkem stavu zařízení) ji odloží.
 *
 * Pod device_state_lock() (dialogy a okno Virtual CMT v main_window.cpp,
 * menu Devices) se akce zapamatuje i se souhlasem, se kterým automat
 * rozhodl (s_ok_*), a provede ji imgui_cmt_tape_run_deferred() po
 * uvolnění zámku ve stejném snímku - stav pásky tam ověří znovu.
 *
 * @param action Akce. @param path Cesta akce "_FILE" (zkopíruje se), jinak NULL.
 * @pre UI vlákno, s_pending == NONE (automat akci už předal).
 */
static void cmt_ui_run_or_defer(en_IMGUI_CMT_TAPE_ACTION action, const char *path)
{
    if (!device_state_lock_held())
    {
        cmt_ui_run_action(action, path);
        return;
    };
    g_free(s_deferred_path);
    s_deferred = action;
    s_deferred_path = path ? g_strdup(path) : NULL;
    s_deferred_ok_recording = s_ok_recording;
    s_deferred_ok_rec_serial = s_ok_rec_serial;
    s_deferred_ok_modified = s_ok_modified;
}


/**
 * @brief Další krok automatu: dotaz na nahrávání, na Modified, nebo provedení akce.
 *
 * Dotazuje se jen na to, co uživatel ještě neodsouhlasil (s_ok_*); stav
 * se čte znovu při každém kroku, takže zhoršení mezi kroky (automatické
 * ukončení nahrávání, MCP) vyvolá další dotaz. Souhlas s nahráváním platí
 * jen pro totéž nahrávání (s_ok_rec_serial): nahrávání spuštěné po něm se
 * zeptá znovu. Při přehrávání záznamu vstupů se neptá vůbec (páska patří
 * přehrávání). Akci provede cmt_ui_run_or_defer() (pod zámkem odloženě).
 *
 * @pre UI vlákno, s_pending != NONE.
 */
static void cmt_ui_flow_step(void)
{
    bool recording, modified;
    unsigned serial = 0;
    cmt_ui_tape_state(&recording, &modified, &serial);
    s_dialog_open = false;
    const bool playing = cmt_ui_replay_playing();
    if (playing && (s_pending == IMGUI_CMT_TAPE_ACTION_QUIT))
    {
        /* páska uživatele je uschovaná přehráváním: nejdřív ho zastavit
         * (vrátí ji), pak se zeptat jako vždy (imgui_cmt_tape_dialogs()) */
        if (!s_await_replay_off)
            imgui_replay_stop_play();
        s_await_replay_off = true;
        s_dialog = CMT_UI_DIALOG_NONE;
        return;
    };
    if (recording && !playing && !(s_ok_recording && (serial == s_ok_rec_serial)))
    {
        s_ok_recording = false;
        s_dialog = CMT_UI_DIALOG_RECORDING;
        s_record_stopping = false;
        return;
    };
    if (modified && !playing && !s_ok_modified)
    {
        s_dialog = CMT_UI_DIALOG_UNSAVED;
        return;
    };
    en_IMGUI_CMT_TAPE_ACTION action = s_pending;
    gchar *path = s_pending_path;
    s_pending = IMGUI_CMT_TAPE_ACTION_NONE;
    s_pending_path = NULL;
    s_dialog = CMT_UI_DIALOG_NONE;
    s_save_seq = 0;
    s_record_stopping = false;
    cmt_ui_run_or_defer(action, path);
    g_free(path);
}


/**
 * @brief Spustí automat pro akci (nic, když už běží jiný dotaz).
 * @param action Akce. @param path Cesta akce "_FILE" (zkopíruje se) nebo NULL.
 * @param ok_recording Nahrávání už odsouhlasené (jen nahrávání s čítačem @p ok_rec_serial).
 * @param ok_rec_serial Čítač výsledků nahrávání, ke kterému souhlas patří.
 * @param ok_modified Modified už odsouhlasené.
 * @return true = automat spuštěn; false = běží jiný dotaz nebo čeká odložená
 *         akce, požadavek se ignoruje.
 */
static bool cmt_ui_flow_start(en_IMGUI_CMT_TAPE_ACTION action, const char *path, bool ok_recording, unsigned ok_rec_serial,
                              bool ok_modified)
{
    if (action == IMGUI_CMT_TAPE_ACTION_NONE)
        return false;
    /* druhý požadavek (např. opakované zavření okna) rozpracovaný dotaz nemění */
    if ((s_pending != IMGUI_CMT_TAPE_ACTION_NONE) || (s_deferred != IMGUI_CMT_TAPE_ACTION_NONE))
        return false;
    s_pending = action;
    s_pending_path = path ? g_strdup(path) : NULL;
    s_ok_recording = ok_recording;
    s_ok_rec_serial = ok_rec_serial;
    s_ok_modified = ok_modified;
    s_save_seq = 0;
    s_record_stopping = false;
    cmt_ui_flow_step();
    return true;
}


/**
 * @brief Ohlásí výsledek uložení: chyba uložení z dialogu neuložených změn při ukončení aplikace modálně, jinak řádek zpráv.
 *
 * Jediné místo pravidla "při ukončení aplikace je chyba uložení vždy
 * modální" (okno Virtual CMT se zavírá, řádek zpráv by nikdo neviděl).
 * Ostatní výsledky jdou přes cmt_view_post_message() (podle
 * viditelnosti okna řádek zpráv, nebo modálně / zahodit).
 *
 * @param text Zpráva (přeložená). @param is_error true = chyba.
 * @param quit_save true = uložení z dialogu neuložených změn při ukončení
 *        aplikace (s_pending == IMGUI_CMT_TAPE_ACTION_QUIT v okamžiku zařazení
 *        nebo výsledku, dřív než ho cmt_ui_flow_abort() vynuluje).
 * @pre UI vlákno.
 */
static void cmt_ui_report_save(const char *text, bool is_error, bool quit_save)
{
    if (is_error && quit_save)
        imgui_message_create(MESSAGE_TYPE_ERROR, text);
    else
        cmt_view_post_message(text, is_error);
}


/**
 * @brief Text jádra pro řádek zpráv s čísly bloků od 1; cesta souboru se nikdy nezmění.
 *
 * Volající zná strukturu zprávy: zásahy, jejichž texty cestu nenesou
 * (mazání, přesun), předají NULL; zásahy s cestou (uložení, export, import)
 * předají cestu, kterou jádru daly - ta zůstane beze změny
 * (cmt_view_core_text_path()). Neznámá cesta ("") = text se nepřevádí
 * vůbec (čísla bloků zůstanou od 0, cesta určitě beze změny).
 *
 * @param core Text jádra (NULL = ""). @param path NULL = text cestu nenese; "" = cesta neznámá; jinak cesta ze zásahu.
 * @return Text pro zobrazení.
 */
static std::string cmt_ui_core_text(const char *core, const char *path)
{
    if (!path)
        return cmt_view_core_text(core);
    if (!path[0])
        return std::string(core ? core : "");
    return cmt_view_core_text_path(core, path);
}


/**
 * @brief Cesta souboru zásahu uložení nebo exportu (pro cmt_ui_core_text() ve výsledku).
 *
 * @invariant Nejvýš CMT_UI_PATHS_MAX položek (starší se zapomenou: výsledek se ztratil, device_request_clear()).
 */
typedef struct st_CMT_UI_PATH {
    uint32_t seq;     /**< pořadové číslo zásahu */
    std::string path; /**< cesta, kterou zásah předal jádru */
} st_CMT_UI_PATH;

/** @brief Nejvýš tolik cest zásahů čeká na výsledek. */
#define CMT_UI_PATHS_MAX 32u

/** @brief Cesty čekajících zásahů uložení a exportu (UI vlákno). */
static std::vector<st_CMT_UI_PATH> s_paths;

/**
 * @brief Zapamatuje cestu zásahu (uložení, export bloku, export pásky).
 * @param seq Pořadové číslo zásahu (0 = nic). @param path Cesta (NULL = nic).
 */
static void cmt_ui_remember_path(uint32_t seq, const char *path)
{
    if (!seq || !path)
        return;
    if (s_paths.size() >= CMT_UI_PATHS_MAX)
        s_paths.erase(s_paths.begin());
    st_CMT_UI_PATH p;
    p.seq = seq;
    p.path = path;
    s_paths.push_back(p);
}

/**
 * @brief Vyzvedne cestu zásahu (a zapomene ji).
 * @param seq Pořadové číslo zásahu. @return Cesta; "" = neznámá (zásah z jiné cesty, zapomenutý).
 */
static std::string cmt_ui_take_path(uint32_t seq)
{
    for (size_t i = 0; i < s_paths.size(); i++)
    {
        if (s_paths[i].seq != seq)
            continue;
        std::string p = s_paths[i].path;
        s_paths.erase(s_paths.begin() + (long)i);
        return p;
    };
    return std::string();
}


/**
 * @brief Zařadí uložení pásky; s @p continue_pending po úspěchu pokračuje automat dotazu (s_pending).
 *
 * Zásah nese čítač výměn pásky (cmt_request_save_on_tape()): emu vlákno
 * jinou pásku neuloží a výsledek CMT_REQUEST_RC_TAPE_CHANGED čekající akci
 * zruší (imgui_cmt_device_request_result()). Cesta delší než mez fronty
 * zásahů: chyba (při ukončení aplikace modálně, cmt_ui_report_save())
 * a zrušení čekající akce.
 *
 * @param path Cíl, NULL = dosavadní soubor pásky.
 * @param continue_pending true = uložení z dialogu neuložených změn.
 * @param tape_serial Čítač výměn pásky (cmttmz_get_tape_serial()) pásky, kterou uživatel ukládá.
 */
static void cmt_ui_submit_save(const char *path, bool continue_pending, unsigned tape_serial)
{
    uint32_t seq = cmt_request_save_on_tape(tape_serial, path);
    if (seq == 0)
    {
        cmt_ui_report_save(_("The tape could not be saved: the file path is too long."), true,
                           continue_pending && (s_pending == IMGUI_CMT_TAPE_ACTION_QUIT));
        if (continue_pending)
            cmt_ui_flow_abort();
        return;
    };
    /* cesta ve zprávě výsledku se nepřevádí (čísla bloků ano): bez cesty ukládá jádro do souboru pásky */
    if (path)
        cmt_ui_remember_path(seq, path);
    else
    {
        bool taken = cmt_ui_lock_if_needed();
        st_CMTEXT_CONTAINER *c = CMT_TEST_FILLED ? cmtext_get_container(g_cmt.ext) : NULL;
        const char *fp = c ? cmtext_container_get_filepath(c) : NULL;
        std::string own = fp ? fp : "";
        if (taken)
            device_state_unlock();
        cmt_ui_remember_path(seq, own.c_str());
    };
    if (continue_pending)
        s_save_seq = seq;
}


/**
 * @brief Páska čekající na dialog Save tape as: pokračování automatu a páska, ke které dialog patří.
 *
 * Dialog souboru je asynchronní; mezitím může MCP, záznam vstupů nebo návrat
 * na checkpoint pásku vyměnit a uložila by se jiná páska do souboru
 * zvoleného pro tu, kterou uživatel viděl.
 * Vlastnictví: alokuje cmt_ui_save_as() (g_new), uvolní callback dialogu.
 */
typedef struct st_CMT_UI_SAVE_AS {
    bool continue_pending; /**< true = po úspěšném uložení pokračovat automatem dotazu (s_pending) */
    unsigned tape_serial;  /**< čítač výměn pásky (cmttmz_get_tape_serial()) při otevření dialogu */
} st_CMT_UI_SAVE_AS;

#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: čítač výměn pásky "otevřeného" dialogu Save As (s_test_save_as). */
static unsigned s_test_save_as_serial = 0;
#endif


/**
 * @brief Konec dialogu Save tape as: vybraná cesta, nebo zrušení.
 *
 * Bez přípony doplní ".tmz" a zařadí uložení; zrušený dialog (nebo prázdná
 * cesta) zruší čekající akci dialogu neuložených změn. Je-li vložená jiná
 * páska než při otevření dialogu (čítač výměn pásky), nic neuloží, ohlásí
 * "The tape changed; the action was cancelled." (při ukončení aplikace
 * modálně, cmt_ui_report_save()) a čekající akci zruší - neprovede ji jako
 * po úspěšném uložení. Kontrolu opakuje i emu vlákno (cmt_request_save_on_tape()).
 *
 * @param path Vybraná cesta (převezme ji, g_free), NULL = zrušeno.
 * @param continue_pending true = po úspěšném uložení pokračovat automatem dotazu (s_pending).
 * @param tape_serial Čítač výměn pásky z otevření dialogu.
 * @pre UI vlákno; zámek stavu zařízení si vezme, nedrží-li ho.
 */
static void cmt_ui_save_as_done(char *path, bool continue_pending, unsigned tape_serial)
{
    if (!path || !path[0])
    {
        g_free(path);
        if (continue_pending)
            cmt_ui_flow_abort();
        return;
    };
    bool taken = cmt_ui_lock_if_needed();
    bool same = (cmttmz_get_tape_serial() == tape_serial);
    if (taken)
        device_state_unlock();
    if (!same)
    {
        g_free(path);
        cmt_ui_report_save(_("The tape changed; the action was cancelled."), true,
                           continue_pending && (s_pending == IMGUI_CMT_TAPE_ACTION_QUIT));
        if (continue_pending)
            cmt_ui_flow_abort();
        return;
    };
    if (!cmt_ui_file_ext(path))
    {
        char *with_ext = g_strconcat(path, ".tmz", NULL);
        g_free(path);
        path = with_ext;
    };
    cmt_ui_submit_save(path, continue_pending, tape_serial);
    g_free(path);
}


/**
 * @brief Callback dialogu Save tape as (UI vlákno).
 *
 * Převezme vybranou cestu (fch->selected_filePathName se odpojí, destroy ji
 * neuvolní) a dokončí uložení (cmt_ui_save_as_done()).
 *
 * @param fch Dialog; callback ho uvolní i s user_data (st_CMT_UI_SAVE_AS, g_new).
 */
static void imgui_cmt_tape_save_as_cb(baseui_fchooser_t *fch)
{
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        cmt_ui_flow_abort();
        return;
    };
    st_CMT_UI_SAVE_AS *sa = (st_CMT_UI_SAVE_AS *)fch->user_data;
    char *path = NULL;
    if (fch->state == BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        path = fch->selected_filePathName;
        fch->selected_filePathName = NULL;
    };
    baseui_filechooser_destroy(fch);
    bool continue_pending = sa ? sa->continue_pending : false;
    unsigned tape_serial = sa ? sa->tape_serial : 0;
    g_free(sa);
    cmt_ui_save_as_done(path, continue_pending, tape_serial);
}


/**
 * @brief Otevře dialog Save tape as.
 *
 * Filtry TMZ (výchozí), TZX, MZT, MZF; navržené jméno = jméno pásky
 * s příponou .tmz, u nové pásky "newtape.tmz"; adresář = adresář pásky.
 * Dialog si pamatuje čítač výměn pásky (st_CMT_UI_SAVE_AS): uloží se jen
 * tatáž páska.
 *
 * @param continue_pending true = po úspěšném uložení pokračovat automatem dotazu (s_pending).
 * @pre UI vlákno, drží zámek stavu zařízení (čte jméno pásky a čítač výměn pásky).
 */
static void cmt_ui_save_as(bool continue_pending)
{
    gchar *filters = g_strdup_printf("%s (*.tmz){.tmz},%s (*.tzx){.tzx},%s (*.mzt){.mzt},%s (*.mzf *.m12){.mzf,.m12}",
                                     _("TMZ tape"), _("TZX tape"), _("MZT tape"), _("MZF file"));
    const char *base = cmt_get_ui_base_filename();
    gchar *name;
    if (base[0])
    {
        const char *ext = baseui_filechooser_get_extension_ptr(base);
        gchar *stem = ext ? g_strndup(base, (gsize)(ext - base)) : g_strdup(base);
        name = g_strconcat(stem, ".tmz", NULL);
        g_free(stem);
    }
    else
    {
        name = g_strdup("newtape.tmz");
    };
    gchar *dir = (g_cmt.last_filename && g_cmt.last_filename[0]) ? g_path_get_dirname(g_cmt.last_filename) : NULL;
    const unsigned tape_serial = cmttmz_get_tape_serial();

#ifdef MZTEST_HEADLESS
    /* test dialog nekreslí: "otevřený" Save As dokončí imgui_cmt_tape_test_save_as_finish() */
    s_test_save_as = continue_pending ? 2 : 1;
    s_test_save_as_serial = tape_serial;
    g_free(dir);
    g_free(name);
    g_free(filters);
    return;
#endif
    st_CMT_UI_SAVE_AS *sa = g_new(st_CMT_UI_SAVE_AS, 1);
    sa->continue_pending = continue_pending;
    sa->tape_serial = tape_serial;
    baseui_fchooser_t *fch = baseui_filechooser_save_file(BASEUI_FCHOOSER_CAT_CMT, _("Save tape as"), filters, dir, name, NULL,
                                                          imgui_cmt_tape_save_as_cb, sa);
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        g_free(sa);
        if (continue_pending)
            cmt_ui_flow_abort();
    };
    g_free(dir);
    g_free(name);
    g_free(filters);
}


/**
 * @brief Uloží pásku do jejího souboru, jinak otevře Save As (výchozí TMZ).
 *
 * Save As místo uložení: páska bez souboru, soubor formátu, do kterého se
 * neukládá (.wav, .tap), nebo formátu, který pásku neunese
 * (cmttmz_save_fits(): např. otevřený game.mzf s nahraným druhým blokem,
 * .mzt se surovým blokem). S @p continue_pending čekající akce Save As
 * přežije a provede se po úspěšném uložení. Zásah nese čítač výměn pásky
 * z tohoto okamžiku (cmt_ui_submit_save()).
 *
 * @param continue_pending true = po úspěšném uložení pokračovat automatem dotazu (s_pending).
 * @pre UI vlákno, drží zámek stavu zařízení.
 */
static void cmt_ui_save(bool continue_pending)
{
    if (cmt_ui_savable_ext(cmt_get_ui_base_filename()) && g_cmt.last_filename && cmttmz_save_fits(g_cmt.last_filename))
        cmt_ui_submit_save(NULL, continue_pending, cmttmz_get_tape_serial());
    else
        cmt_ui_save_as(continue_pending);
}


/**
 * @brief Blok čekající na dialog exportu: číslo bloku a páska, ke které patří.
 *
 * Dialog souboru je asynchronní; mezitím může MCP, záznam vstupů nebo návrat
 * na checkpoint pásku vyměnit a číslo bloku by patřilo jiné pásce.
 * Vlastnictví: alokuje imgui_cmt_tape_export_block(), uvolní callback dialogu.
 */
typedef struct st_CMT_UI_EXPORT {
    int block_id;         /**< index bloku v souboru pásky */
    unsigned tape_serial; /**< čítač výměn pásky (cmttmz_get_tape_serial()) v době volby bloku */
} st_CMT_UI_EXPORT;


/**
 * @brief Dokončí export po výběru souboru: zařadí zásah, jen když je vložená tatáž páska.
 *
 * Jinak export zahodí se zprávou "The tape changed; the action was
 * cancelled." v řádku zpráv okna Virtual CMT. Kontrolu opakuje i emu vlákno
 * (cmt_request_export_block_on_tape()).
 *
 * @param block_id Blok. @param tape_serial Čítač výměn pásky z doby volby bloku.
 * @param path Cíl (UTF-8, neprázdný).
 * @pre UI vlákno; zámek stavu zařízení si vezme, nedrží-li ho.
 */
static void cmt_ui_export_chosen(int block_id, unsigned tape_serial, const char *path)
{
    bool taken = cmt_ui_lock_if_needed();
    bool same = (cmttmz_get_tape_serial() == tape_serial);
    if (taken)
        device_state_unlock();
    if (!same)
    {
        cmt_view_post_message(_("The tape changed; the action was cancelled."), true);
        return;
    };
    uint32_t seq = cmt_request_export_block_on_tape(tape_serial, block_id, path);
    if (0 == seq)
        cmt_view_post_message(_("The block could not be exported: the file path is too long."), true);
    cmt_ui_remember_path(seq, path);
}


/**
 * @brief Callback dialogu exportu bloku (UI vlákno): převezme cestu a dokončí export.
 * @param fch Dialog; callback ho uvolní i s user_data (st_CMT_UI_EXPORT, g_new).
 */
static void imgui_cmt_tape_export_cb(baseui_fchooser_t *fch)
{
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        return;
    };
    st_CMT_UI_EXPORT *ex = (st_CMT_UI_EXPORT *)fch->user_data;
    char *path = NULL;
    if (fch->state == BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        path = fch->selected_filePathName;
        fch->selected_filePathName = NULL;
    };
    baseui_filechooser_destroy(fch);
    if (ex && path && path[0])
        cmt_ui_export_chosen(ex->block_id, ex->tape_serial, path);
    g_free(ex);
    g_free(path);
}


void imgui_cmt_tape_export_block(int block_id, bool wav, unsigned tape_serial)
{
    if (block_id < 0)
        return;
#ifdef MZTEST_HEADLESS
    s_test_export_block = block_id;
    s_test_export_wav = wav;
    s_test_export_serial = tape_serial;
#endif
    st_CMT_UI_EXPORT *ex = g_new(st_CMT_UI_EXPORT, 1);
    ex->block_id = block_id;
    ex->tape_serial = tape_serial;
    gchar *name = g_strdup_printf("block%02d%s", block_id, wav ? ".wav" : ".mzf");
    gchar *dir = (g_cmt.last_filename && g_cmt.last_filename[0]) ? g_path_get_dirname(g_cmt.last_filename) : NULL;
    baseui_fchooser_t *fch = baseui_filechooser_save_file(BASEUI_FCHOOSER_CAT_CMT,
                                                          wav ? _("Export block as WAV") : _("Export block as MZF"),
                                                          wav ? ".wav" : ".mzf,.m12", dir, name, NULL,
                                                          imgui_cmt_tape_export_cb, ex);
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        g_free(ex);
    };
    g_free(dir);
    g_free(name);
}



/**
 * @brief Callback dialogu otevření pásky (UI vlákno): vybraný soubor jde zpět do automatu.
 *
 * Mezi dotazem a výběrem souboru se stav pásky mohl zhoršit (automatické
 * ukončení nahrávání, MCP): imgui_cmt_tape_file_action() ho zkontroluje znovu.
 *
 * @param fch Dialog; callback ho uvolní. user_data != NULL = po otevření přehrát.
 */
static void imgui_cmt_tape_open_cb(baseui_fchooser_t *fch)
{
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        return;
    };
    bool play = (fch->user_data != NULL);
    if (fch->state == BASEUI_FCHOOSER_STATE_CLOSED_OK && fch->selected_filePathName && fch->selected_filePathName[0])
    {
        imgui_cmt_tape_file_action(play ? IMGUI_CMT_TAPE_ACTION_OPEN_FILE_PLAY : IMGUI_CMT_TAPE_ACTION_OPEN_FILE,
                                   fch->selected_filePathName);
    };
    baseui_filechooser_destroy(fch);
}


/**
 * @brief Otevře dialog výběru souboru pásky (filtry cmt_ui_open_filters(), výchozí cesta = vložená páska).
 * @param play true = po otevření přehrát.
 * @pre UI vlákno.
 */
static void cmt_ui_open_chooser(bool play)
{
    bool taken = cmt_ui_lock_if_needed();
    gchar *last = g_strdup(g_cmt.last_filename ? g_cmt.last_filename : "");
    if (taken)
        device_state_unlock();
    baseui_fchooser_t *fch = baseui_filechooser_open_file(BASEUI_FCHOOSER_CAT_CMT, _("Select CMT file to open"),
                                                          cmt_ui_open_filters(), NULL, NULL, last,
                                                          imgui_cmt_tape_open_cb, play ? GINT_TO_POINTER(1) : NULL);
    if (!fch)
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    g_free(last);
}


void imgui_cmt_tape_action(en_IMGUI_CMT_TAPE_ACTION action)
{
    (void)cmt_ui_flow_start(action, NULL, false, 0, false);
}


void imgui_cmt_tape_file_action(en_IMGUI_CMT_TAPE_ACTION action, const char *path)
{
    /* odsouhlasené při spuštění dialogu souboru: znovu se ptát jen na zhoršení */
    if (!cmt_ui_flow_start(action, path, s_file_ok_recording, s_file_ok_rec_serial, s_file_ok_modified)
        && (action == IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE))
    {
        /* jiný dotaz běží: jako zrušený dialog (čeká na emu vlákno, ne pod zámkem) */
        if (device_state_lock_held())
            s_deferred_load_cancelled = true;
        else
            snapshot_ui_load_cancelled();
    };
}


bool imgui_cmt_tape_flow_active(void)
{
    return (s_pending != IMGUI_CMT_TAPE_ACTION_NONE) || (s_deferred != IMGUI_CMT_TAPE_ACTION_NONE);
}


void imgui_cmt_tape_run_deferred(void)
{
    if (s_deferred_load_cancelled)
    {
        s_deferred_load_cancelled = false;
        snapshot_ui_load_cancelled();
    };
    if (s_deferred == IMGUI_CMT_TAPE_ACTION_NONE)
        return;
    en_IMGUI_CMT_TAPE_ACTION action = s_deferred;
    gchar *path = s_deferred_path;
    s_deferred = IMGUI_CMT_TAPE_ACTION_NONE;
    s_deferred_path = NULL;
    /* mezi rozhodnutím (pod zámkem) a teď mohlo emu vlákno stav zhoršit
     * (MCP spustilo nahrávání, páska dostala změny): krok automatu se
     * stejným souhlasem stav přečte znovu - beze změny akci hned provede
     * (zámek už nedrží), při zhoršení se zeptá */
    (void)cmt_ui_flow_start(action, path, s_deferred_ok_recording, s_deferred_ok_rec_serial, s_deferred_ok_modified);
    g_free(path);
}


bool imgui_cmt_quit_requested(void)
{
    /* přehrávání záznamu vstupů: páska uživatele je uschovaná a konec procesu
     * by ji zahodil - automat přehrávání zastaví a pak se zeptá (veto) */
    if (cmt_ui_replay_playing())
    {
        imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_QUIT);
        return false;
    };
    bool taken = cmt_ui_lock_if_needed();
    bool unsaved = cmt_has_unsaved_changes();
    if (taken)
        device_state_unlock();
    if (!unsaved)
        return true;
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_QUIT);
    return false;
}


void imgui_cmt_tape_name(char *buf, size_t size)
{
    const char *base = cmt_get_ui_base_filename();
    snprintf(buf, size, "%s%s", base[0] ? base : _("(new tape)"), cmttmz_is_modified() ? "*" : "");
}


void imgui_cmt_tape_save(bool save_as)
{
    bool taken = cmt_ui_lock_if_needed();
    if (save_as)
        cmt_ui_save_as(false);
    else
        cmt_ui_save(false);
    if (taken)
        device_state_unlock();
}


/**
 * @brief Obsah dialogu probíhajícího nahrávání (uvnitř BeginPopupModal()).
 *
 * Stop recording zařadí běžný STOP (replay_host_cmt(), záznam vstupů ho
 * zapíše) a čeká, až transport opustí RECORD (kontrola v
 * imgui_cmt_tape_dialogs() i bez otevřeného popupu); pak automat pokračuje
 * dotazem na Modified. Discard recording odsouhlasí ztrátu nahrávání
 * (zahodí ho až akce: vysunutí, otevření, nová páska, konec) a pokračuje.
 * Cancel akci zruší (i během čekání na STOP).
 *
 * @return true = dialog skončil (popup zavřený), automat udělal další krok.
 * @pre UI vlákno, drží zámek stavu zařízení, uvnitř popupu.
 */
static bool cmt_ui_recording_dialog_body(void)
{
    if (s_record_stopping)
    {
        ImGui::TextUnformatted(_("Stopping the recording..."));
        ImGui::Separator();
        if (ImGui::Button(_L("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape) || cmt_ui_test_pick(IMGUI_CMT_TEST_CHOICE_CANCEL))
        {
            ImGui::CloseCurrentPopup();
            cmt_ui_flow_abort();
            return true;
        };
        return false;
    };
    ImGui::TextUnformatted(_("A recording is in progress. Stop and keep it, discard it, or cancel?"));
    ImGui::Separator();
    if (ImGui::Button(_L("Stop recording")) || cmt_ui_test_pick(IMGUI_CMT_TEST_CHOICE_STOP_REC))
    {
        replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
        s_record_stopping = true;
        return false;
    };
    ImGui::SameLine();
    if (ImGui::Button(_L("Discard recording")) || cmt_ui_test_pick(IMGUI_CMT_TEST_CHOICE_DISCARD))
    {
        ImGui::CloseCurrentPopup();
        /* souhlas platí jen pro toto nahrávání (čítač výsledků) */
        s_ok_recording = true;
        cmttmz_get_record_result(NULL, NULL, &s_ok_rec_serial);
        cmt_ui_flow_step();
        return true;
    };
    ImGui::SameLine();
    if (ImGui::Button(_L("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape) || cmt_ui_test_pick(IMGUI_CMT_TEST_CHOICE_CANCEL))
    {
        ImGui::CloseCurrentPopup();
        cmt_ui_flow_abort();
        return true;
    };
    return false;
}


/**
 * @brief Obsah dialogu neuložených změn (uvnitř BeginPopupModal()).
 *
 * Save tape uloží (u pásky bez vhodného souboru přes Save As) a akce
 * pokračuje po úspěšném uložení (imgui_cmt_device_request_result());
 * Discard changes odsouhlasí ztrátu a pokračuje; Cancel akci zruší.
 * Během nahrávání záznamu vstupů se uložení odmítá: Save tape se
 * nenabízí a dialog to vysvětlí řádkem "The tape cannot be saved while
 * input recording is active.".
 *
 * @return true = dialog skončil (popup zavřený).
 * @pre UI vlákno, drží zámek stavu zařízení, uvnitř popupu.
 */
static bool cmt_ui_unsaved_dialog_body(void)
{
    const bool can_save = !imgui_cmt_replay_recording();
    char name[256];
    imgui_cmt_tape_name(name, sizeof(name));
    ImGui::Text("%s", name);
    ImGui::TextUnformatted(_("The tape has changes that are not saved to a file."));
    if (can_save)
        ImGui::TextUnformatted(_("Save them before you continue?"));
    else
        ImGui::TextUnformatted(_("The tape cannot be saved while input recording is active."));
    ImGui::Separator();

    if (can_save && (ImGui::Button(_L("Save tape")) || cmt_ui_test_pick(IMGUI_CMT_TEST_CHOICE_SAVE)))
    {
        ImGui::CloseCurrentPopup();
        s_dialog = CMT_UI_DIALOG_NONE;
        s_dialog_open = false;
        cmt_ui_save(true);
        return true;
    };
    if (can_save)
        ImGui::SameLine();
    if (ImGui::Button(_L("Discard changes")) || cmt_ui_test_pick(IMGUI_CMT_TEST_CHOICE_DISCARD))
    {
        ImGui::CloseCurrentPopup();
        s_ok_modified = true;
        cmt_ui_flow_step();
        return true;
    };
    ImGui::SameLine();
    if (ImGui::Button(_L("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape) || cmt_ui_test_pick(IMGUI_CMT_TEST_CHOICE_CANCEL))
    {
        ImGui::CloseCurrentPopup();
        cmt_ui_flow_abort();
        return true;
    };
    return false;
}


static void cmt_ui_import_watch(void);


void imgui_cmt_tape_dialogs(void)
{
#ifdef MZTEST_HEADLESS
    s_test_dialog_shown = 0;
#endif
    /* výsledek nahrávání i se zavřeným oknem (zahozené nahrávání pak modálně) */
    cmt_view_poll_record();
    /* ROM SAVE uložený přímo na pásku: zpráva i se zavřeným oknem (informace se pak nehlásí) */
    cmt_view_poll_direct_save();
    /* operace okna zahozené emu vláknem kvůli změně decku: jedno varování i se zavřeným oknem */
    cmt_view_poll_dropped();
    /* rychlosti bloků nevrácené po přehrání záznamu vstupů: jedno varování i se zavřeným oknem */
    cmt_view_poll_replay_warning();
    /* postupný import: výsledek, který se ztratil, import ukončí */
    cmt_ui_import_watch();
    /* dialog analýzy: opuštění se zavřeným oknem, data ztraceného zásahu uplatnění */
    cmt_analyze_dialog_frame();
    /* modály informací o pásce a převodu profilu: opuštění, když je okno nenakreslilo */
    cmt_tapeinfo_dialog_frame();
    cmt_convert_dialog_frame();

    /* ukončení během přehrávání: po jeho konci (páska uživatele zpět) běžný krok */
    if (s_await_replay_off)
    {
        if (cmt_ui_replay_playing())
            return;
        s_await_replay_off = false;
        if (s_pending != IMGUI_CMT_TAPE_ACTION_NONE)
            cmt_ui_flow_step();
    };
    if (s_dialog == CMT_UI_DIALOG_NONE)
        return;

    /* přehrávání záznamu vstupů začalo během dotazu: páska patří přehrávání,
     * dotaz se neklade (replay_host_cmt() by Stop recording stejně zahodil)
     * a akce pokračuje podle pravidel replaye */
    if (cmt_ui_replay_playing())
    {
        const bool rec_dlg = (s_dialog == CMT_UI_DIALOG_RECORDING);
        const char *pid = rec_dlg ? CMT_RECORDING_POPUP_ID : CMT_UNSAVED_POPUP_ID;
        if (s_dialog_open && ImGui::IsPopupOpen(pid))
        {
            char t[128];
            snprintf(t, sizeof(t), "%s%s", rec_dlg ? _("Recording in progress") : _("Unsaved tape changes"), pid);
            if (ImGui::BeginPopupModal(t, NULL, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            };
        };
        cmt_ui_flow_step();
        return;
    };

    /* čekání na STOP běží i bez otevřeného popupu (zavřel ho jiný modál) */
    if ((s_dialog == CMT_UI_DIALOG_RECORDING) && s_record_stopping && !(CMT_TEST_FILLED && CMT_TEST_RECORD))
    {
        /* STOP proběhl: záznam je na pásce, ta je teď Modified */
        if (s_dialog_open && ImGui::IsPopupOpen(CMT_RECORDING_POPUP_ID))
        {
            char t[128];
            snprintf(t, sizeof(t), "%s%s", _("Recording in progress"), CMT_RECORDING_POPUP_ID);
            if (ImGui::BeginPopupModal(t, NULL, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            };
        };
        /* nahrávání skončilo, není co zahodit; souhlas se nepřenáší na
         * nahrávání, které by začalo později (automat se zeptá znovu) */
        s_ok_recording = false;
        cmt_ui_flow_step();
        if (s_dialog == CMT_UI_DIALOG_NONE)
            return;
    };

    const bool rec = (s_dialog == CMT_UI_DIALOG_RECORDING);
    const char *id = rec ? CMT_RECORDING_POPUP_ID : CMT_UNSAVED_POPUP_ID;
    char title[128];
    snprintf(title, sizeof(title), "%s%s", rec ? _("Recording in progress") : _("Unsaved tape changes"), id);

    if (!s_dialog_open)
    {
        /* jen při volnu: OpenPopup() by jiný otevřený modál zavřel (a ten zase tento) */
        if (!imgui_modal_can_open())
            return;
        ImGui::OpenPopup(id);
        s_dialog_open = true;
    };
    /* uprostřed okna Virtual CMT a v jeho viewportu */
    imgui_cmt_window_modal_next_pos();
    if (!ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        /* popup zavřel někdo jiný: automat čeká dál a otevře ho znovu, až bude volno */
        s_dialog_open = false;
        return;
    };
#ifdef MZTEST_HEADLESS
    s_test_dialog_shown = rec ? 1 : 2;
#endif
    if (rec)
        (void)cmt_ui_recording_dialog_body();
    else
        (void)cmt_ui_unsaved_dialog_body();
    ImGui::EndPopup();
}


/**
 * @brief Pořadové číslo zásahu Enable CPU Boost z hlavního menu (imgui_cmt_menu_set_cpu_boost()), 0 = žádný nečeká.
 *
 * Patří UI vláknu. Slouží jen k rozlišení výsledku: zásah mění jen CPU
 * Boost a se zapnutým CMT hackem zůstává režim Direct.
 */
static uint32_t s_menu_boost_seq = 0;


void imgui_cmt_menu_set_cpu_boost(bool enabled)
{
    s_menu_boost_seq = cmt_request_set_loading_boost(enabled);
}


/**
 * @brief Zpráva řádku zpráv k výsledku zásahu režimu načítání (cmt_request_set_loading_mode()).
 *
 * Výsledek Enable CPU Boost z hlavního menu se zapnutým CMT hackem (režim
 * zůstal Direct, změnila se jen volba CPU Boost, kterou ukazuje zaškrtnutí
 * v menu) nehlásí nic: "Loading mode: Direct" by tvrdilo změnu režimu,
 * která nenastala.
 *
 * @param res Výsledek zásahu.
 */
static void cmt_ui_loading_mode_result(const st_DEVICE_REQ_RESULT *res)
{
    const bool menu_boost = (s_menu_boost_seq != 0) && (res->seq == s_menu_boost_seq);
    if (menu_boost)
        s_menu_boost_seq = 0;
    if (res->status != DEVICE_REQ_DONE)
    {
        cmt_view_post_message(_("The loading mode cannot be changed while an input replay is recorded or played."), true);
        return;
    };
    if (menu_boost && (res->rc == CMT_LOADING_MODE_DIRECT))
        return;
    /* texty režimů na jednom místě (cmt_view.cpp), sdílí je testovací stav Direct */
    const char *text = cmt_view_loading_mode_message(res->rc);
    if (text)
        cmt_view_post_message(text, false);
    else
        cmt_view_post_message(_("Stop the tape to switch to or from Direct mode."), true);
}


/**
 * @brief Zpráva řádku zpráv k výsledku zásahu Direct Save (cmt_request_set_direct_save()).
 *
 * Odmítnutí jako u přepínače CMT hacku: mimo STOP "Stop the tape to switch
 * Direct Save on or off." (chyba), při záznamu a přehrávání vstupů text
 * o záznamu vstupů (chyba). Provedený zásah ohlásí nový stav (informace).
 *
 * @param res Výsledek zásahu.
 */
static void cmt_ui_direct_save_result(const st_DEVICE_REQ_RESULT *res)
{
    if (res->status != DEVICE_REQ_DONE)
    {
        cmt_view_post_message(_("Direct Save cannot be changed while an input replay is recorded or played."), true);
        return;
    };
    if (res->rc < 0)
    {
        cmt_view_post_message(_("Stop the tape to switch Direct Save on or off."), true);
        return;
    };
    cmt_view_post_message(res->rc ? _("Direct Save: on (a ROM SAVE without Record goes straight to the tape)")
                                  : _("Direct Save: off (a ROM SAVE needs Record)"),
                          false);
}


/**
 * @brief Zpráva řádku zpráv k výsledku Load directly (cmt_request_load_directly_on_tape()).
 *
 * Přepnutí do Direct = táž zpráva jako přepínač Loading ("Loading mode:
 * Direct", cmt_view_loading_mode_message()); Direct už zapnutý = nic
 * (vybraný blok ukáže okno). Jiná páska = "The tape changed; the action
 * was cancelled." (varování, jako ostatní zásahy s číslem bloku). Návrat
 * -1 = "The tape state changed; the action was cancelled." (varování):
 * deck mezitím mimo STOP, blok nejde vybrat, nebo se mezi snímkem
 * a provedením vyměnila ROM za ROM bez podpory CMT hacku
 * (CMT_LOAD_DIRECTLY_NO_HACK, cmthack_rom_supported()). Položku menu okno za
 * žádného z těchto stavů nenabízí (s ROM bez podpory je zakázaná s důvodem,
 * st_CMT_VIEW::direct_supported), změnil se tedy stav od kreslení menu;
 * zásah v takovém případě nic nezmění (blok, CPU Boost ani hack) a zůstává
 * jen jako pojistka. Odmítnutí při záznamu a přehrávání vstupů = text
 * režimu načítání (chyba).
 *
 * @param res Výsledek zásahu.
 */
static void cmt_ui_load_directly_result(const st_DEVICE_REQ_RESULT *res)
{
    if (res->status != DEVICE_REQ_DONE)
    {
        cmt_view_post_message(_("The loading mode cannot be changed while an input replay is recorded or played."), true);
        return;
    };
    if (res->rc == CMT_REQUEST_RC_TAPE_CHANGED)
    {
        cmt_view_post(_("The tape changed; the action was cancelled."), CMT_VIEW_MSG_WARNING);
        return;
    };
    if (res->rc < 0)
    {
        cmt_view_post(_("The tape state changed; the action was cancelled."), CMT_VIEW_MSG_WARNING);
        return;
    };
    if (res->rc == 1)
    {
        const char *text = cmt_view_loading_mode_message(CMT_LOADING_MODE_DIRECT);
        if (text)
            cmt_view_post_message(text, false);
    };
}


/**
 * @brief Zpráva řádku zpráv k otevřené pásce (cmt_request_open()): jméno a počet bloků.
 *
 * Chybu otevření hlásí jádro samo (baseui_error(), modální okno zpráv),
 * tady se ohlásí jen úspěch a odmítnutí při záznamu vstupů. Páska s jediným
 * surovým blokem (WAV, záznam) dostane nápovědu k analýze
 * (cmt_view_opened_raw_message()).
 *
 * @param res Výsledek zásahu.
 */
static void cmt_ui_open_result(const st_DEVICE_REQ_RESULT *res)
{
    if (res->status != DEVICE_REQ_DONE)
    {
        cmt_view_post_message(_("Another tape cannot be opened while an input replay is recorded or played."), true);
        return;
    };
    if (res->rc != 0)
        return;
    bool taken = cmt_ui_lock_if_needed();
    st_CMTEXT_CONTAINER *c = CMT_TEST_FILLED ? cmtext_get_container(g_cmt.ext) : NULL;
    const char *base = cmt_get_ui_base_filename();
    /* formát na jednom místě (cmt_view_opened_message()), sdílí ho testovací stavy */
    bool have = (c != NULL);
    /* páska s jediným surovým blokem (WAV, záznam): nápověda k analýze */
    std::string text;
    if (have)
        text = cmt_view_tape_single_raw() ? cmt_view_opened_raw_message(base, cmtext_container_get_count_blocks(c))
                                          : cmt_view_opened_message(base, cmtext_container_get_count_blocks(c));
    if (taken)
        device_state_unlock();
    if (have)
        cmt_view_post_message(text.c_str(), false);
}


/* ------------------------------------------------------------------------- */
/*  Úpravy pásky z okna                                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief Data výsledku úpravy pásky přečtená pod zámkem při vyzvednutí z fronty (imgui_cmt_device_request_capture()).
 *
 * @invariant seq == 0 = nic zachycené; jinak patří výsledku zásahu se stejným
 *            pořadovým číslem. edit platí jen při have_edit, export_* jen u exportu.
 */
typedef struct st_CMT_UI_CAPTURE {
    uint32_t seq;                     /**< pořadové číslo zásahu (0 = nic) */
    bool have_edit;                   /**< edit je platný (cmt_request_get_edit_result() vrátilo true) */
    st_CMT_REQUEST_EDIT_RESULT edit;  /**< výsledek úpravy (smazání, přesun, import) */
    bool is_export;                   /**< zásah exportu celé pásky (export_message, export_warning platí) */
    std::string export_message;       /**< cmttmz_get_export_message() */
    std::string export_warning;       /**< cmttmz_get_export_warning() */
} st_CMT_UI_CAPTURE;

/** @brief Poslední zachycená data výsledku (UI vlákno). */
static st_CMT_UI_CAPTURE s_capture;

/**
 * @brief Postupný import souborů (imgui_cmt_tape_import_paths()).
 *
 * @invariant active => seq je pořadové číslo zásahu importu souboru
 *            paths[next - 1], na jehož výsledek se čeká.
 * @invariant at < 0 = na konec; jinak pozice dalšího souboru.
 * @invariant idle_frame >= 0 jen při active: snímek, ve kterém hlídač
 *            (cmt_ui_import_watch()) viděl frontu zásahů prázdnou.
 */
typedef struct st_CMT_UI_IMPORT {
    bool active;                    /**< čeká se na výsledek importu */
    std::vector<std::string> paths; /**< soubory v pořadí importu */
    size_t next;                    /**< index dalšího souboru k zařazení */
    int at;                         /**< pozice vložení dalšího souboru (záporná = na konec) */
    uint32_t seq;                   /**< pořadové číslo zásahu, na který se čeká */
    int first_select;               /**< první importovaný blok prvního úspěšného souboru (-1 = žádný) */
    int idle_frame;                 /**< ImGui::GetFrameCount(), kdy hlídač viděl prázdnou frontu (-1 = neviděl) */
    bool warned;                    /**< některý soubor řetězu skončil varováním (zpráva varování se informací nepřepíše) */
} st_CMT_UI_IMPORT;

/** @brief Stav postupného importu (UI vlákno). */
static st_CMT_UI_IMPORT s_import = {false, {}, 0, -1, 0, -1, -1, false};

#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: dialog Import file byl vyžádán (imgui_cmt_tape_import()). */
static bool s_test_import_req = false;
/** @brief Jen pro testy: pozice vložení posledního dialogu Import file. */
static int s_test_import_at = -1;
/** @brief Jen pro testy: čítač výměn pásky posledního dialogu Import file. */
static unsigned s_test_import_serial = 0;
/** @brief Jen pro testy: dialog Export tape as WAV byl vyžádán. */
static bool s_test_export_tape_req = false;
/** @brief Jen pro testy: čítač výměn pásky posledního dialogu Export tape as WAV. */
static unsigned s_test_export_tape_serial = 0;
#endif


void imgui_cmt_device_request_capture(const st_DEVICE_REQ_RESULT *res)
{
    const bool edit = (0 == strcmp(res->name, CMT_REQUEST_REMOVE_BLOCKS_NAME)) || (0 == strcmp(res->name, CMT_REQUEST_MOVE_BLOCK_NAME))
        || (0 == strcmp(res->name, CMT_REQUEST_IMPORT_NAME)) || (0 == strcmp(res->name, CMT_REQUEST_ANALYSIS_PULSES_NAME))
        || (0 == strcmp(res->name, CMT_REQUEST_ANALYSIS_APPLY_NAME)) || (0 == strcmp(res->name, CMT_REQUEST_TAPE_INFO_SET_NAME))
        || (0 == strcmp(res->name, CMT_REQUEST_CONVERT_BLOCK_NAME));
    const bool exp = (0 == strcmp(res->name, CMT_REQUEST_EXPORT_TAPE_NAME));
    if (!edit && !exp)
        return;
    s_capture.seq = res->seq;
    s_capture.have_edit = edit && cmt_request_get_edit_result(res->seq, &s_capture.edit);
    s_capture.is_export = exp;
    s_capture.export_message = exp ? cmttmz_get_export_message() : "";
    s_capture.export_warning = exp ? cmttmz_get_export_warning() : "";
}


/**
 * @brief Výsledek úpravy pásky k výsledku zásahu: zachycený při vyzvednutí, jinak přečtený teď pod zámkem.
 * @param res Výsledek zásahu. @param[out] out Výsledek úpravy.
 * @return true = výsledek úpravy je k dispozici; false = zásah odmítnutý frontou nebo výsledek přepsaný.
 * @pre UI vlákno; zámek si vezme, nedrží-li ho.
 */
static bool cmt_ui_edit_result(const st_DEVICE_REQ_RESULT *res, st_CMT_REQUEST_EDIT_RESULT *out)
{
    if (s_capture.seq && (s_capture.seq == res->seq) && !s_capture.is_export)
    {
        if (s_capture.have_edit)
            *out = s_capture.edit;
        return s_capture.have_edit;
    };
    bool taken = cmt_ui_lock_if_needed();
    bool ok = cmt_request_get_edit_result(res->seq, out);
    if (taken)
        device_state_unlock();
    return ok;
}


uint32_t imgui_cmt_tape_remove_blocks(unsigned tape_serial, const int *block_ids, int count)
{
    if (!block_ids || count <= 0)
        return 0;
    uint32_t seq = cmt_request_remove_blocks_on_tape(tape_serial, block_ids, (uint32_t)count);
    if (seq == 0)
    {
        /* okno posílá jen platné a neopakované bloky: nezařadí se seznam rozsahů,
         * který se nevejde do argumentu zásahu */
        cmt_view_post(_("Too many separate blocks selected."), CMT_VIEW_MSG_ERROR);
        return 0;
    };
    return seq;
}


uint32_t imgui_cmt_tape_move_block(unsigned tape_serial, int block_id, int to)
{
    return cmt_request_move_block_on_tape(tape_serial, block_id, to);
}


/** @brief Ukončí postupný import (zbylé soubory se zahodí). */
static void cmt_ui_import_end(void)
{
    s_import.active = false;
    s_import.paths.clear();
    s_import.next = 0;
    s_import.seq = 0;
    s_import.first_select = -1;
    s_import.idle_frame = -1;
    s_import.warned = false;
}


/**
 * @brief Zastaví postupný import u neúspěšného souboru: ohlásí jeho chybu a zbytek neimportuje.
 *
 * Zpráva zůstane v řádku zpráv (při skrytém okně modálně, cmt_view_post()),
 * protože žádný další soubor ji nepřepíše. Zbývají-li neimportované
 * soubory, připojí se věta "The remaining files were not imported.".
 *
 * @param text Text chyby nebo varování (anglicky z jádra nebo přeložený).
 * @param level Úroveň zprávy (CMT_VIEW_MSG_ERROR / CMT_VIEW_MSG_WARNING).
 * @post Postupný import ukončený (cmt_ui_import_end()).
 */
static void cmt_ui_import_stop(const char *text, en_CMT_VIEW_MSG_LEVEL level)
{
    std::string msg = text ? text : "";
    if (s_import.next < s_import.paths.size())
    {
        msg += " ";
        msg += _("The remaining files were not imported.");
    };
    cmt_view_post(msg.c_str(), level);
    cmt_ui_import_end();
}


/**
 * @brief Zařadí import dalšího souboru postupného importu, nebo import ukončí.
 *
 * Soubor, jehož cesta se do argumentu zásahu nevejde (zásah se nezařadí),
 * řetěz zastaví chybou (cmt_ui_import_stop()): import nikdy nečeká na
 * výsledek nezařazeného zásahu.
 *
 * @param tape_serial Čítač výměn pásky (z výsledku předchozího importu nebo z okna).
 * @post s_import.active == true a seq zařazeného zásahu, nebo import ukončený.
 */
static void cmt_ui_import_submit_next(unsigned tape_serial)
{
    if (s_import.next >= s_import.paths.size())
    {
        cmt_ui_import_end();
        return;
    };
    const std::string &path = s_import.paths[s_import.next++];
    uint32_t seq = cmt_request_import_on_tape(tape_serial, path.c_str(), s_import.at);
    if (!seq)
    {
        cmt_ui_import_stop(_("The file could not be imported: the file path is too long."), CMT_VIEW_MSG_ERROR);
        return;
    };
    s_import.active = true;
    s_import.seq = seq;
    s_import.idle_frame = -1;
}


/**
 * @brief Hlídač postupného importu: výsledek, který nepřijde, import ukončí.
 *
 * Fronta zásahů sníží počet čekajících až po zápisu výsledku do fronty
 * výsledků (device_request_poll()). Vidí-li hlídač prázdnou frontu
 * (device_request_pending() == 0), výsledek čekaného zásahu už ve frontě
 * výsledků je, nebo se ztratil (device_request_clear(), přetečení fronty
 * výsledků). Výsledky se vyřizují na konci snímku (imgui_device_request_results()
 * po imgui_cmt_tape_dialogs()); čeká-li import i v dalším snímku na týž
 * zásah, výsledek se ztratil: import skončí varováním "The import could
 * not be completed." (se zbytkem souborů). Hlídač nikdy nečeká na čas.
 *
 * @pre UI vlákno, jednou za snímek (imgui_cmt_tape_dialogs()).
 */
static void cmt_ui_import_watch(void)
{
    if (!s_import.active)
        return;
    const int frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0;
    if (device_request_pending() != 0)
    {
        s_import.idle_frame = -1;
        return;
    };
    if (s_import.idle_frame < 0)
    {
        s_import.idle_frame = frame;
        return;
    };
    if (frame == s_import.idle_frame)
        return;
    cmt_ui_import_stop(_("The import could not be completed."), CMT_VIEW_MSG_WARNING);
}


void imgui_cmt_tape_import_paths(const char *const *paths, int count, int at, unsigned tape_serial)
{
    if (!paths || count <= 0)
        return;
    if (s_import.active)
    {
        cmt_view_post(_("Another import is still in progress."), CMT_VIEW_MSG_WARNING);
        return;
    };
    cmt_ui_import_end();
    for (int i = 0; i < count; i++)
    {
        if (paths[i] && paths[i][0])
            s_import.paths.push_back(paths[i]);
    };
    s_import.at = (at < 0) ? -1 : at;
    cmt_ui_import_submit_next(tape_serial);
}


/**
 * @brief Pozice a čítač dialogu Import file.
 * Vlastnictví: alokuje imgui_cmt_tape_import() (g_new), uvolní callback dialogu.
 */
typedef struct st_CMT_UI_IMPORT_DLG {
    int at;               /**< pozice vložení (záporná = na konec) */
    unsigned tape_serial; /**< čítač výměn pásky v době volby (bez pásky 0) */
} st_CMT_UI_IMPORT_DLG;


/**
 * @brief Callback dialogu Import file (UI vlákno): vybraný soubor importuje.
 * @param fch Dialog; callback ho uvolní i s user_data (st_CMT_UI_IMPORT_DLG).
 */
static void imgui_cmt_tape_import_cb(baseui_fchooser_t *fch)
{
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        return;
    };
    st_CMT_UI_IMPORT_DLG *d = (st_CMT_UI_IMPORT_DLG *)fch->user_data;
    if (d && (fch->state == BASEUI_FCHOOSER_STATE_CLOSED_OK) && fch->selected_filePathName && fch->selected_filePathName[0])
    {
        const char *p[1] = {fch->selected_filePathName};
        imgui_cmt_tape_import_paths(p, 1, d->at, d->tape_serial);
    };
    g_free(d);
    baseui_filechooser_destroy(fch);
}


void imgui_cmt_tape_import(int at, unsigned tape_serial)
{
#ifdef MZTEST_HEADLESS
    /* test dialog nekreslí: dokončí ho imgui_cmt_tape_test_import_finish() */
    s_test_import_req = true;
    s_test_import_at = at;
    s_test_import_serial = tape_serial;
    return;
#endif
    bool taken = cmt_ui_lock_if_needed();
    gchar *last = g_strdup(g_cmt.last_filename ? g_cmt.last_filename : "");
    if (taken)
        device_state_unlock();
    st_CMT_UI_IMPORT_DLG *d = g_new(st_CMT_UI_IMPORT_DLG, 1);
    d->at = at;
    d->tape_serial = tape_serial;
    /* filtry jako Open... (všechny formáty, které import převede) */
    baseui_fchooser_t *fch = baseui_filechooser_open_file(BASEUI_FCHOOSER_CAT_CMT, _("Import file"), cmt_ui_open_filters(), NULL, NULL,
                                                          last, imgui_cmt_tape_import_cb, d);
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        g_free(d);
    };
    g_free(last);
}


bool imgui_cmt_tape_open_path(const char *path)
{
    if (!path || !path[0])
        return true;
    /* jako návrat dialogu Open..., ale bez dřívějšího souhlasu: automat se zeptá */
    return cmt_ui_flow_start(IMGUI_CMT_TAPE_ACTION_OPEN_FILE, path, false, 0, false);
}


/**
 * @brief Dokončí export celé pásky po výběru souboru: zařadí zásah jen nad páskou z doby volby.
 *
 * Jiný čítač výměn pásky (jiná páska nebo mezitím upravená) = nic se
 * nezařadí, chyba "The tape changed; the action was cancelled." (jako export
 * bloku). Deck mezitím mimo STOP, nebo běží záznam či přehrávání vstupů =
 * nic se nezařadí, varování "The tape state changed; the action was
 * cancelled." (export za běhu by na emu vlákně zastavil přehrávání).
 * Zásah nese čítač (cmt_request_export_tape_on_tape()): úpravu mezi
 * touto kontrolou a provedením pozná emu vlákno a nic nezapíše.
 *
 * @param tape_serial Čítač výměn pásky z doby volby. @param path Cíl (UTF-8, neprázdný).
 * @pre UI vlákno; zámek si vezme, nedrží-li ho.
 */
static void cmt_ui_export_tape_chosen(unsigned tape_serial, const char *path)
{
    bool taken = cmt_ui_lock_if_needed();
    bool same = CMT_TEST_FILLED && (cmttmz_get_tape_serial() == tape_serial);
    bool stopped = CMT_TEST_FILLED && CMT_TEST_STOP && !cmttmz_rec_is_active();
    if (taken)
        device_state_unlock();
    if (!same)
    {
        cmt_view_post_message(_("The tape changed; the action was cancelled."), true);
        return;
    };
    if (!stopped || imgui_cmt_replay_recording() || imgui_cmt_replay_playing())
    {
        cmt_view_post(_("The tape state changed; the action was cancelled."), CMT_VIEW_MSG_WARNING);
        return;
    };
    uint32_t seq = cmt_request_export_tape_on_tape(tape_serial, path);
    if (0 == seq)
        cmt_view_post_message(_("The tape could not be exported: the file path is too long."), true);
    cmt_ui_remember_path(seq, path);
}


/**
 * @brief Callback dialogu Export tape as WAV (UI vlákno).
 * @param fch Dialog; callback ho uvolní. user_data = čítač výměn pásky (GUINT_TO_POINTER).
 */
static void imgui_cmt_tape_export_tape_cb(baseui_fchooser_t *fch)
{
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        return;
    };
    unsigned serial = GPOINTER_TO_UINT(fch->user_data);
    if ((fch->state == BASEUI_FCHOOSER_STATE_CLOSED_OK) && fch->selected_filePathName && fch->selected_filePathName[0])
        cmt_ui_export_tape_chosen(serial, fch->selected_filePathName);
    baseui_filechooser_destroy(fch);
}


void imgui_cmt_tape_export_tape(unsigned tape_serial)
{
#ifdef MZTEST_HEADLESS
    /* test dialog nekreslí: dokončí ho imgui_cmt_tape_test_export_tape_finish() */
    s_test_export_tape_req = true;
    s_test_export_tape_serial = tape_serial;
    return;
#endif
    const char *base = cmt_get_ui_base_filename();
    gchar *name;
    if (base[0])
    {
        const char *ext = baseui_filechooser_get_extension_ptr(base);
        gchar *stem = ext ? g_strndup(base, (gsize)(ext - base)) : g_strdup(base);
        name = g_strconcat(stem, ".wav", NULL);
        g_free(stem);
    }
    else
    {
        name = g_strdup("tape.wav");
    };
    gchar *dir = (g_cmt.last_filename && g_cmt.last_filename[0]) ? g_path_get_dirname(g_cmt.last_filename) : NULL;
    baseui_fchooser_t *fch = baseui_filechooser_save_file(BASEUI_FCHOOSER_CAT_CMT, _("Export tape as WAV"), ".wav", dir, name, NULL,
                                                          imgui_cmt_tape_export_tape_cb, GUINT_TO_POINTER(tape_serial));
    if (!fch)
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    g_free(dir);
    g_free(name);
}


/**
 * @brief Text neúspěchu úpravy pásky (společné pro smazání, přesun a import), bez ohlášení.
 *
 * Odmítnutí frontou (záznam nebo přehrávání vstupů) = chyba; jiná páska =
 * varování "The tape changed; the action was cancelled." (uživatel nic
 * nezkazil, jako Load directly); chyba jádra = jeho text s čísly bloků od 1
 * a s cestou souboru beze změny (cmt_ui_core_text()).
 *
 * @param res Výsledek zásahu. @param r Výsledek úpravy, NULL = není k dispozici.
 * @param path Cesta v textu jádra (jako cmt_ui_core_text(): NULL = text cestu nenese - mazání, přesun;
 *        "" = neznámá cesta importu; jinak cesta importovaného souboru).
 * @param[out] text Text zprávy (jen při true). @param[out] level Úroveň zprávy (jen při true).
 * @return true = úprava se neprovedla; false = úprava proběhla.
 */
static bool cmt_ui_edit_failure_text(const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *r, const char *path,
                                     std::string *text, en_CMT_VIEW_MSG_LEVEL *level)
{
    if (res->status != DEVICE_REQ_DONE)
    {
        *text = _("The tape cannot be edited while an input replay is recorded or played.");
        *level = CMT_VIEW_MSG_ERROR;
        return true;
    };
    if (res->rc == CMT_REQUEST_RC_TAPE_CHANGED)
    {
        *text = _("The tape changed; the action was cancelled.");
        *level = CMT_VIEW_MSG_WARNING;
        return true;
    };
    if (res->rc != 0)
    {
        *text = (r && r->message[0]) ? cmt_ui_core_text(r->message, path) : std::string(_("The tape could not be edited."));
        *level = CMT_VIEW_MSG_ERROR;
        return true;
    };
    return false;
}


/**
 * @brief Ohlásí neúspěch úpravy pásky (cmt_ui_edit_failure_text() a cmt_view_post()).
 * @param res Výsledek zásahu. @param r Výsledek úpravy, NULL = není k dispozici.
 * @return true = neúspěch ohlášen (úprava se neprovedla); false = úprava proběhla.
 */
static bool cmt_ui_edit_failure(const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *r)
{
    std::string text;
    en_CMT_VIEW_MSG_LEVEL level = CMT_VIEW_MSG_ERROR;
    /* mazání a přesun: texty jádra cestu nenesou */
    if (!cmt_ui_edit_failure_text(res, r, NULL, &text, &level))
        return false;
    cmt_view_post(text.c_str(), level);
    return true;
}


/**
 * @brief Výsledek smazání bloků: zpráva a výběr bloku na místě prvního smazaného.
 *
 * Jeden smazaný blok = "Deleted block %d." s číslem bloku z výsledku úpravy
 * (removed_block, číslo od 1 jako sloupec #), jinak "Deleted %d blocks.";
 * "Deleted 1 blocks." nevznikne nikdy. Varování jádra
 * (změna taktu CPU bloků 0x46) následuje v závorce s úrovní varování.
 *
 * @param res Výsledek zásahu.
 */
static void cmt_ui_remove_result(const st_DEVICE_REQ_RESULT *res)
{
    st_CMT_REQUEST_EDIT_RESULT r;
    bool have = cmt_ui_edit_result(res, &r);
    if (cmt_ui_edit_failure(res, have ? &r : NULL) || !have)
        return;
    char buf[96];
    if (r.count == 1)
        snprintf(buf, sizeof(buf), _("Deleted block %d."), r.removed_block + 1);
    else
        snprintf(buf, sizeof(buf), _("Deleted %d blocks."), (int)r.count);
    std::string text = buf;
    if (r.message[0])
    {
        /* varování jádra (změna taktu CPU bloků) anglicky v závorce */
        text += " (";
        text += r.message;
        text += ")";
    };
    cmt_view_post(text.c_str(), r.message[0] ? CMT_VIEW_MSG_WARNING : CMT_VIEW_MSG_INFO);
    cmt_view_select_after_edit(r.tape_serial, r.select_block);
}


/** @brief Nejvyšší pořadové číslo vyřízeného výsledku přesunu (imgui_cmt_tape_move_done(); UI vlákno). */
static uint32_t s_move_done_seq = 0;


bool imgui_cmt_tape_move_done(uint32_t seq)
{
    return (seq == 0) || (s_move_done_seq >= seq);
}


/**
 * @brief Výsledek přesunu bloku: chyba nebo varování jádra (změna taktu), po úspěchu výběr přesunutého bloku.
 * @param res Výsledek zásahu.
 * @post imgui_cmt_tape_move_done(res->seq) == true (i při chybě a odmítnutí).
 */
static void cmt_ui_move_result(const st_DEVICE_REQ_RESULT *res)
{
    if (res->seq > s_move_done_seq)
        s_move_done_seq = res->seq;
    st_CMT_REQUEST_EDIT_RESULT r;
    bool have = cmt_ui_edit_result(res, &r);
    if (cmt_ui_edit_failure(res, have ? &r : NULL) || !have)
        return;
    /* přesun sám nic nehlásí; varování jádra (změna taktu CPU bloků) ano */
    if (r.message[0])
        cmt_view_post(r.message, CMT_VIEW_MSG_WARNING);
    cmt_view_select_after_edit(r.tape_serial, r.select_block);
}


/**
 * @brief Výsledek importu souboru: zpráva, výběr prvního importovaného bloku, další soubor postupného importu.
 *
 * Postupný import se u prvního neúspěšného souboru
 * zastaví (chyba jádra, nečitelný soubor, jiná páska, odmítnutí frontou,
 * chybějící výsledek úpravy): zpráva toho souboru zůstane v řádku zpráv
 * (žádný další výsledek ji nepřepíše) s větou "The remaining files were not
 * imported." a zbylé soubory se neimportují. Úroveň zprávy během řetězu
 * neklesne: varování importu dřívějšího souboru (takt CPU, nepřehratelné
 * bloky) nepřepíše informace o dalším souboru.
 *
 * @param res Výsledek zásahu.
 */
static void cmt_ui_import_result(const st_DEVICE_REQ_RESULT *res)
{
    const bool chain = s_import.active && (res->seq == s_import.seq);
    std::string path = (chain && s_import.next > 0) ? s_import.paths[s_import.next - 1] : std::string();
    st_CMT_REQUEST_EDIT_RESULT r;
    bool have = cmt_ui_edit_result(res, &r);
    std::string fail;
    en_CMT_VIEW_MSG_LEVEL level = CMT_VIEW_MSG_ERROR;
    if (cmt_ui_edit_failure_text(res, have ? &r : NULL, path.c_str(), &fail, &level))
    {
        if (chain)
            cmt_ui_import_stop(fail.c_str(), level);
        else
            cmt_view_post(fail.c_str(), level);
        return;
    };
    if (!have)
    {
        /* úspěch bez výsledku úpravy (přepsaný kruh): nevíme, co se vložilo, řetěz nepokračuje */
        if (chain)
            cmt_ui_import_stop(_("The import could not be completed."), CMT_VIEW_MSG_WARNING);
        return;
    };
    gchar *base = g_path_get_basename(path.c_str());
    std::string text = cmt_view_imported_message(path.empty() ? "" : base, (int)r.count);
    g_free(base);
    if (r.message[0])
    {
        /* varování importu (takt CPU, nepřehratelné bloky) anglicky z jádra, čísla bloků od 1 */
        text += " (";
        text += cmt_ui_core_text(r.message, path.c_str());
        text += ")";
    };
    const bool warned = (r.message[0] != '\0');
    /* v řetězu nepřepíše informace varování dřívějšího souboru */
    if (!(chain && s_import.warned && !warned))
        cmt_view_post(text.c_str(), warned ? CMT_VIEW_MSG_WARNING : CMT_VIEW_MSG_INFO);
    if (!chain)
    {
        cmt_view_select_after_edit(r.tape_serial, r.select_block);
        return;
    };
    if (warned)
        s_import.warned = true;
    if (s_import.first_select < 0)
        s_import.first_select = r.select_block;
    cmt_view_select_after_edit(r.tape_serial, s_import.first_select);
    if (s_import.at >= 0)
        s_import.at += (int)r.count;
    cmt_ui_import_submit_next(r.tape_serial);
}


/**
 * @brief Výsledek exportu celé pásky: text jádra (cmttmz_get_export_message()), s varováním úrovní varování.
 * @param res Výsledek zásahu.
 */
static void cmt_ui_export_tape_result(const st_DEVICE_REQ_RESULT *res)
{
    if (res->status != DEVICE_REQ_DONE)
    {
        cmt_view_post(_("The tape cannot be exported while an input replay is recorded or played."), CMT_VIEW_MSG_ERROR);
        return;
    };
    /* páska se změnila mezi volbou souboru a provedením: nic se nezapsalo */
    if (res->rc == CMT_REQUEST_RC_TAPE_CHANGED)
    {
        cmt_view_post(_("The tape changed; the action was cancelled."), CMT_VIEW_MSG_WARNING);
        return;
    };
    std::string detail, warning;
    if (s_capture.seq && (s_capture.seq == res->seq) && s_capture.is_export)
    {
        detail = s_capture.export_message;
        warning = s_capture.export_warning;
    }
    else
    {
        bool taken = cmt_ui_lock_if_needed();
        detail = cmttmz_get_export_message();
        warning = cmttmz_get_export_warning();
        if (taken)
            device_state_unlock();
    };
    const bool ok = (res->rc == 0);
    /* čísla bloků od 1, cesta exportu beze změny */
    const std::string path = cmt_ui_take_path(res->seq);
    detail = cmt_ui_core_text(detail.c_str(), path.c_str());
    std::string text;
    if (ok)
        text = detail.empty() ? std::string(_("The tape was exported.")) : detail;
    else
        text = detail.empty() ? std::string(_("The tape could not be exported.")) : std::string(_("The tape could not be exported.")) + " " + detail;
    cmt_view_post(text.c_str(), !ok ? CMT_VIEW_MSG_ERROR : (warning.empty() ? CMT_VIEW_MSG_INFO : CMT_VIEW_MSG_WARNING));
}


bool imgui_cmt_device_request_result(const st_DEVICE_REQ_RESULT *res)
{
    /* úpravy pásky a export celé pásky */
    if (0 == strcmp(res->name, CMT_REQUEST_REMOVE_BLOCKS_NAME))
    {
        cmt_ui_remove_result(res);
        return true;
    };
    if (0 == strcmp(res->name, CMT_REQUEST_MOVE_BLOCK_NAME))
    {
        cmt_ui_move_result(res);
        return true;
    };
    if (0 == strcmp(res->name, CMT_REQUEST_IMPORT_NAME))
    {
        cmt_ui_import_result(res);
        return true;
    };
    if (0 == strcmp(res->name, CMT_REQUEST_EXPORT_TAPE_NAME))
    {
        cmt_ui_export_tape_result(res);
        return true;
    };
    /* dialog analýzy: kopie pulzů a uplatnění analýzy, výsledek úpravy zachycený při vyzvednutí */
    if ((0 == strcmp(res->name, CMT_REQUEST_ANALYSIS_PULSES_NAME)) || (0 == strcmp(res->name, CMT_REQUEST_ANALYSIS_APPLY_NAME)))
    {
        st_CMT_REQUEST_EDIT_RESULT r;
        bool have = cmt_ui_edit_result(res, &r);
        return cmt_analyze_dialog_request_result(res, have ? &r : NULL);
    };
    /* informace o pásce a převod profilu */
    if (0 == strcmp(res->name, CMT_REQUEST_TAPE_INFO_SET_NAME))
    {
        st_CMT_REQUEST_EDIT_RESULT r;
        bool have = cmt_ui_edit_result(res, &r);
        return cmt_tapeinfo_dialog_request_result(res, have ? &r : NULL);
    };
    if (0 == strcmp(res->name, CMT_REQUEST_CONVERT_BLOCK_NAME))
    {
        st_CMT_REQUEST_EDIT_RESULT r;
        bool have = cmt_ui_edit_result(res, &r);
        return cmt_convert_dialog_request_result(res, have ? &r : NULL);
    };
    /* výpočet délek bloků pro okno (cmt_request_lengths()) nic neohlašuje, ani odmítnutí při replay */
    if (0 == strcmp(res->name, CMT_REQUEST_LENGTHS_NAME))
        return true;
    if (0 == strcmp(res->name, CMT_REQUEST_LOADING_MODE_NAME))
    {
        cmt_ui_loading_mode_result(res);
        return true;
    };
    if (0 == strcmp(res->name, CMT_REQUEST_LOAD_DIRECTLY_NAME))
    {
        cmt_ui_load_directly_result(res);
        return true;
    };
    if (0 == strcmp(res->name, CMT_REQUEST_DIRECT_SAVE_NAME))
    {
        cmt_ui_direct_save_result(res);
        return true;
    };
    if (0 == strcmp(res->name, "cmt_open"))
    {
        cmt_ui_open_result(res);
        return true;
    };
    if (0 == strcmp(res->name, "cmt_set_block_speed"))
    {
        /* rychlost bloku (kontextové menu, editor pulzů): hlásí se jen odmítnutí a jiná páska */
        if (res->status != DEVICE_REQ_DONE)
            cmt_view_post_message(_("The speed of a block cannot be changed while an input replay is recorded or played."), true);
        else if (res->rc == CMT_REQUEST_RC_TAPE_CHANGED)
            cmt_view_post_message(_("The tape changed; the action was cancelled."), true);
        else if (res->rc != 0)
            cmt_view_post_message(_("The speed of the block could not be changed."), true);
        return true;
    };
    if ((0 == strcmp(res->name, "cmt_set_speed")) || (0 == strcmp(res->name, "cmt_set_custom_pulses")))
    {
        /* změna rychlosti: hlásí se jen odmítnutí a chyba (úspěch ukáže výběr rychlosti) */
        if (res->status != DEVICE_REQ_DONE)
            cmt_view_post_message(_("The signal speed cannot be changed while an input replay is recorded or played."), true);
        else if (res->rc != 0)
            cmt_view_post_message(_("The signal speed could not be changed."), true);
        return true;
    };
    const bool is_save = (0 == strcmp(res->name, "cmt_save"));
    const bool is_export = (0 == strcmp(res->name, "cmt_export_block"));
    if (!is_save && !is_export)
        return false;

    const bool ok = (res->status == DEVICE_REQ_DONE) && (res->rc == 0);
    /* uložení z dialogu neuložených změn při ukončení aplikace: chyba vždy modálně
     * (okno se zavírá), i odmítnutí při záznamu vstupů (cmt_ui_report_save()) */
    bool quit_save = false;
    if (is_save && s_save_seq && (res->seq == s_save_seq))
    {
        quit_save = (s_pending == IMGUI_CMT_TAPE_ACTION_QUIT);
        /* uložení z dialogu neuložených změn: akce pokračuje jen po úspěchu
         * (další krok automatu stav pásky přečte znovu); jiná páska
         * (CMT_REQUEST_RC_TAPE_CHANGED) není úspěch, akce se zruší */
        s_save_seq = 0;
        if (ok)
            cmt_ui_flow_step();
        else
            cmt_ui_flow_abort();
    };
    /* uložení nebo export nad jinou páskou, než kterou uživatel viděl: nic se neprovedlo */
    if ((res->status == DEVICE_REQ_DONE) && (res->rc == CMT_REQUEST_RC_TAPE_CHANGED))
    {
        cmt_ui_report_save(_("The tape changed; the action was cancelled."), true, quit_save);
        return true;
    };
    if (res->status != DEVICE_REQ_DONE)
    {
        cmt_ui_report_save(is_save ? _("The tape cannot be saved while an input replay is recorded or played.")
                                   : _("A block cannot be exported while an input replay is recorded or played."),
                           true, quit_save);
        return true;
    };

    bool taken = cmt_ui_lock_if_needed();
    /* čísla bloků od 1, cesta uložení / exportu beze změny */
    const std::string path = cmt_ui_take_path(res->seq);
    gchar *detail = g_strdup(cmt_ui_core_text(cmttmz_get_save_message(), path.c_str()).c_str());
    if (taken)
        device_state_unlock();

    /* jádro dává celou větu ("Tape saved: <cesta> (<varování>)"); bez ní obecný text */
    const char *head;
    if (is_save)
        head = ok ? _("The tape was saved.") : _("The tape could not be saved.");
    else
        head = ok ? _("The block was exported.") : _("The block could not be exported.");
    gchar *text = detail[0] ? (ok ? g_strdup(detail) : g_strdup_printf("%s %s", head, detail)) : g_strdup(head);
    cmt_ui_report_save(text, !ok, quit_save);
    g_free(text);
    g_free(detail);
    return true;
}


#ifdef MZTEST_HEADLESS
void imgui_cmt_tape_test_choose(en_IMGUI_CMT_TEST_CHOICE c)
{
    s_test_choice = c;
}


int imgui_cmt_tape_test_dialog_shown(void)
{
    return s_test_dialog_shown;
}


int imgui_cmt_tape_test_save_as_open(void)
{
    return s_test_save_as;
}


void imgui_cmt_tape_test_save_as_finish(const char *path)
{
    if (!s_test_save_as)
        return;
    bool continue_pending = (s_test_save_as == 2);
    s_test_save_as = 0;
    cmt_ui_save_as_done(path ? g_strdup(path) : NULL, continue_pending, s_test_save_as_serial);
}


void imgui_cmt_tape_test_export_finish(int block_id, unsigned tape_serial, const char *path)
{
    cmt_ui_export_chosen(block_id, tape_serial, path);
}


unsigned imgui_cmt_tape_test_last_export_serial(void)
{
    return s_test_export_serial;
}


bool imgui_cmt_tape_test_take_last_export(int *block_id, bool *wav)
{
    if (s_test_export_block < 0)
        return false;
    if (block_id)
        *block_id = s_test_export_block;
    if (wav)
        *wav = s_test_export_wav;
    s_test_export_block = -1;
    return true;
}


en_IMGUI_CMT_TAPE_ACTION imgui_cmt_tape_test_take_last_run(void)
{
    en_IMGUI_CMT_TAPE_ACTION a = s_test_last_run;
    s_test_last_run = IMGUI_CMT_TAPE_ACTION_NONE;
    return a;
}


void imgui_cmt_tape_test_reset(void)
{
    s_pending = IMGUI_CMT_TAPE_ACTION_NONE;
    g_free(s_pending_path);
    s_pending_path = NULL;
    s_dialog = CMT_UI_DIALOG_NONE;
    s_dialog_open = false;
    s_save_seq = 0;
    s_record_stopping = false;
    s_test_choice = IMGUI_CMT_TEST_CHOICE_NONE;
    s_test_last_run = IMGUI_CMT_TAPE_ACTION_NONE;
    s_test_export_block = -1;
    s_ok_recording = false;
    s_ok_rec_serial = 0;
    s_file_ok_recording = false;
    s_file_ok_rec_serial = 0;
    s_file_ok_modified = false;
    s_deferred = IMGUI_CMT_TAPE_ACTION_NONE;
    g_free(s_deferred_path);
    s_deferred_path = NULL;
    s_deferred_load_cancelled = false;
    s_test_save_as = 0;
    s_test_save_as_serial = 0;
    s_await_replay_off = false;
    cmt_ui_import_end();
    s_capture.seq = 0;
    s_test_import_req = false;
    s_test_export_tape_req = false;
}


bool imgui_cmt_tape_test_take_last_import(int *at, unsigned *tape_serial)
{
    if (!s_test_import_req)
        return false;
    s_test_import_req = false;
    if (at)
        *at = s_test_import_at;
    if (tape_serial)
        *tape_serial = s_test_import_serial;
    return true;
}


void imgui_cmt_tape_test_import_finish(const char *path)
{
    if (!path || !path[0])
        return;
    const char *p[1] = {path};
    imgui_cmt_tape_import_paths(p, 1, s_test_import_at, s_test_import_serial);
}


bool imgui_cmt_tape_test_take_last_export_tape(unsigned *tape_serial)
{
    if (!s_test_export_tape_req)
        return false;
    s_test_export_tape_req = false;
    if (tape_serial)
        *tape_serial = s_test_export_tape_serial;
    return true;
}


void imgui_cmt_tape_test_export_tape_finish(const char *path)
{
    if (!path || !path[0])
        return;
    cmt_ui_export_tape_chosen(s_test_export_tape_serial, path);
}


bool imgui_cmt_tape_test_import_active(void)
{
    return s_import.active;
}
#endif
