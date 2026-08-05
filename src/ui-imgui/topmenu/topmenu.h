#ifndef IMGUI_TOPMENU_H
#define IMGUI_TOPMENU_H

#include "main.h"

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

/* Implementace jen v mz800 per-arch stromu (ui-imgui/mz800/);
 * volajici musi volani gatovat per-EXE nebo mit per-arch stub. */
void imgui_menu_mz800_hw_compat(void);

    void imgui_customspeed_setup(void);
    void imgui_menu_snapshot(void);
    void imgui_menu_tools(void);

    void imgui_global_shortcuts(void);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_TOPMENU_H
