#ifndef IMGUI_TOPMENU_H
#define IMGUI_TOPMENU_H

#include "main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void imgui_topmenu_handler(void);
    void imgui_topmenu_body(void);

    void imgui_menu_devices(void);
    void imgui_menu_fdcontroller(void);
    void imgui_fdc_storage_switch_popup(bool *p_open);
    void imgui_fdc_state_window(bool *p_open);
    void imgui_menu_cmt(void);
    /**
     * @brief Modál Custom value... volby Device for Direct MZF Start (každý snímek, mimo nabídky).
     * @pre UI vlákno uvnitř ImGui snímku (main_window.cpp); viz menu_cmt.cpp.
     */
    void imgui_menu_cmt_bootdev_popup(void);
    void imgui_menu_ramdisk(void);
    void imgui_ramdisk_state_window(bool *p_open);
    void imgui_menu_qdisk(void);
    void imgui_qdisk_storage_switch_popup(bool *p_open);
    void imgui_qdisk_state_window(bool *p_open);
    void imgui_menu_unicard(void);
    void imgui_unicard_runtime_mismatch_dialog(void);
    void imgui_unicard_reinit_choice_dialog(void);
    void imgui_menu_ide8(void);
    void imgui_ide8_state_window(bool *p_open);
    void imgui_menu_memext(void);
    void imgui_menu_rom(void);
    void imgui_menu_printer(void);

    void imgui_menu_interface(void);
    void imgui_menu_display(void);
    void imgui_menu_keyjoy(void);

    void imgui_menu_speed(void);
    void imgui_menu_emulator(void);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /**
     * Identifikuje volajícího imgui_menu_debugger() pro accommodation
     * drobných rozdílů v zobrazení:
     *  - TOPMENU: volání z hlavního menu emulátoru (label "Debugger";
     *    první položka je "MZ-800 Debugger" toggle se separátorem).
     *  - DEBUGGER_WINDOW: volání z menubaru hlavního okna debuggeru
     *    (label "DbgTool"; první položka + separator se vynechá,
     *    protože okno už je zobrazené).
     */
    typedef enum {
        DBG_MENU_CALLER_TOPMENU = 0,
        DBG_MENU_CALLER_DEBUGGER_WINDOW = 1
    } en_DBG_MENU_CALLER;

    void imgui_menu_debugger(en_DBG_MENU_CALLER caller);
#endif

void imgui_menu_mz800_dip_switch(void);

#if MZARCH == 800
    void imgui_menu_mz800_hw_compat(void);
    void imgui_menu_mz800_exvram(void);
#endif

    void imgui_customspeed_setup(void);
    void imgui_menu_snapshot(void);
    void imgui_menu_tools(void);

    /** @brief Nabídka Replay: záznam a přehrání vstupů (menu_replay.cpp). */
    void imgui_menu_replay(void);

    /**
     * @brief Každý snímek UI: souborové dialogy nabídky Replay a oznámení
     *        výsledku záznamu / přehrávání (chyba startu, uloženo, konec, rozjetí).
     */
    void imgui_replay_dialogs(void);

    /**
     * @brief Indikátor REC / PLAY v levém dolním rohu obrazu (jen UI).
     * @pre Volá se hned po vykreslení obrazu emulátoru (poslední položka = obraz).
     */
    void imgui_replay_overlay(void);

    /** @brief Popisek zkratky okna Replay Control (zobrazí ho menu i okno). */
#define IMGUI_REPLAY_SHORTCUT_WINDOW "Alt+J"
    /** @brief Popisek zkratky Rewind (o jeden checkpoint zpět). */
#define IMGUI_REPLAY_SHORTCUT_REWIND "Alt+Shift+J"

    /** @brief Otevře souborový dialog startu záznamu (zobrazí se v imgui_replay_dialogs()). */
    void imgui_replay_open_record_dialog(void);

    /** @brief Otevře souborový dialog přehrání (imgui_replay_dialogs()) po dotazu Virtual CMT na nahrávání a neuložené změny pásky. */
    void imgui_replay_open_play_dialog(void);

    /**
     * @brief Zapamatuje si číslo požadavku záznamu, jehož chybu ohlásí oznámení.
     * @param seq Číslo z replay_request_*().
     */
    void imgui_replay_request(uint32_t seq);

    /** @brief Rewind o jeden checkpoint (nic, když nic neběží nebo běží seek). */
    void imgui_replay_rewind(void);

    /**
     * @brief Globální zkratky záznamu vstupů: Alt+J okno Replay Control,
     *        Alt+Shift+J Rewind. Volá global_shortcuts.cpp.
     */
    void imgui_replay_shortcuts(void);

    /**
     * @brief Plovoucí okno Replay Control (`###ReplayRemote`): stav, ovládání,
     *        rewind a seek se zkratkami (replay_window.cpp).
     */
    void imgui_replay_window(void);

    /**
     * @brief Zaregistruje sekci INI `[REPLAY_UI]` (viditelnost okna); jen jednou,
     *        i v headless režimu, aby uložení INI sekci nepřepsalo.
     */
    void imgui_replay_window_cfg_init(void);

    /** @brief Aplikuje uloženou viditelnost okna do `g_gui` (po jeho vzniku). */
    void imgui_replay_window_apply_persisted(void);

    void imgui_global_shortcuts(void);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_TOPMENU_H
