/**
 * @file   screenshot_ui.h
 * @brief  Screenshot z GUI: menu Tools -> Screenshot, zkratka Alt+F, dialog nastavení.
 *
 * Ukládá PNG obrazu, který okno emulátoru právě zobrazuje
 * (iface_sdl3_video_get_screen_surface()), s oblastí a měřítkem podle
 * g_screenshot_settings (INI `[SCREENSHOT]`, viz screenshot_config.h).
 *
 * @par Vlákna
 * Všechny funkce jen z UI vlákna.
 *
 * @par Licence: GPLv3
 */

#ifndef SCREENSHOT_UI_H
#define SCREENSHOT_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Uloží screenshot do výstupního adresáře pod jménem podle času.
 *
 * Výsledek ohlásí notifikací (cesta a rozměr, nebo chyba).
 */
void screenshot_ui_save_quick(void);

/** @brief Otevře dialog pro uložení screenshotu do zvoleného souboru. */
void screenshot_ui_save_as(void);

/**
 * @brief Spustí nebo ukončí sérii snímků (každý změněný emulovaný snímek jako PNG).
 *
 * Start bere oblast, měřítko a "jen změněné" z g_screenshot_settings, adresář
 * série vytvoří ve výstupní složce. Chybu startu ohlásí notifikací.
 */
void screenshot_ui_toggle_series(void);

/**
 * @brief Otevře výstupní adresář screenshotů ve správci souborů (položka "Open Output Folder").
 *
 * Neúspěch ohlásí notifikací.
 *
 * @pre UI vlákno.
 */
void screenshot_ui_open_output_folder(void);

/** @brief Podmenu "Screenshot" v menu Tools (uložit, uložit jako, série, nastavení, složka). */
void imgui_screenshot_menu(void);

/**
 * @brief Zkratky screenshotu; volá se v Alt větvi globálních zkratek.
 *
 * Alt+F = uložit screenshot, Alt+Shift+F = spustit / ukončit sérii snímků,
 * Alt+G = zobrazit / skrýt okno Screenshot Control.
 */
void imgui_screenshot_shortcuts(void);

/**
 * @brief Indikátor běžící série v levém horním rohu obrazu (jen UI).
 *
 * Volá se hned po vykreslení obrazu emulátoru (ImGui::Image), obdélník
 * obrazu bere z GetItemRect*.
 */
void imgui_screenshot_series_overlay(void);

/**
 * @brief Po dokončení zápisu série zobrazí notifikaci (počet souborů, adresář, chyba).
 *
 * Volá se jednou za snímek UI.
 */
void imgui_screenshot_series_poll(void);

/**
 * @brief Dialog nastavení screenshotu (zobrazí se při g_gui->showScreenshotSetupWindow).
 *
 * Edituje kopii nastavení, OK ji zapíše do g_screenshot_settings (uloží se
 * do INI při ukončení emulátoru), Cancel ji zahodí.
 */
void imgui_screenshot_setup_dialog(void);

/**
 * @brief Plovoucí okno Screenshot Control (`###ScreenshotRemote`), zobrazí se
 *        při g_gui->showScreenshotWindow.
 *
 * Ukazuje nastavení (oblast, měřítko, výstupní složka), poslední screenshot
 * z GUI (soubor a rozměr, nebo chyba) a stav série snímků; tlačítka Save
 * Screenshot, Save Screenshot As..., Start / Stop Frame Series s viditelnými
 * zkratkami, Open Output Folder a Settings... Volá UI vlákno jednou za snímek.
 */
void imgui_screenshot_window(void);

/**
 * @brief Zaregistruje sekci INI `[SCREENSHOT_UI]` (viditelnost okna), jen jednou.
 *
 * Volá se i v headless režimu, aby uložení INI sekci nepřepsalo.
 * @pre g_cfgmain existuje.
 */
void imgui_screenshot_window_cfg_init(void);

/** @brief Aplikuje uloženou viditelnost okna do `g_gui` (po jeho vzniku). */
void imgui_screenshot_window_apply_persisted(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSHOT_UI_H */
