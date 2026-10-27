/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file kp80_window.h
 * @brief Okno "K&P 80 Column Monitor" - druhý monitor 80sloupcové karty K&P (MZ-700).
 *
 * Samostatné zavíratelné okno ImGui (ID `###KP80Monitor`) s obrazem
 * sekundárního výstupu karty (devbus_vout "kp80", 640 x 250 bodů). Obraz se
 * škáluje na velikost okna se zachováním poměru stran 4:3. Obraz MZ-700 se
 * nemění.
 *
 * Otevření: položka "Show 80 Column Monitor" v podmenu karty v HW & Devices.
 * Okno se samo otevře, když se karta připojí za běhu GUI (z odpojeného do
 * připojeného stavu, např. menu nebo MCP); stav při startu programu určuje
 * INI. Viditelnost okna se ukládá do INI (sekce `[KP80_UI]`, klíč `window`).
 *
 * Inicializace má dva kroky: kp80_ui_init() (persistence okna v INI, volá
 * video rozhraní i v headless režimu) a kp80_ui_register() (položka menu
 * a okno v devbus, volá mz700_devices_ui_register() při inicializaci GUI).
 *
 * Vlákna: vše v UI vlákně. Obraz se čte přes devbus_vout_fetch() (jediný
 * čtenář kanálu fetch), stav připojení přes devbus_is_connected().
 */

#ifndef KP80_WINDOW_H
#define KP80_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Persistence okna karty: sekce INI `[KP80_UI]` (viditelnost okna).
 *
 * Volá se i v headless režimu, aby uložení INI sekci nepřepsalo.
 *
 * @pre UI vlákno při inicializaci video rozhraní, po cfgmain (INI načtené),
 *      před prvním snímkem UI. Volat jednou (další volání nic nedělá).
 * @post Viditelnost okna odpovídá INI.
 */
void kp80_ui_init(void);

/**
 * @brief Zaregistruje specifické UI karty v generickém menu devbus.
 *
 * Přidá do podmenu karty položku "Show 80 Column Monitor" a okno monitoru
 * (devbus_ui_register_extra()).
 *
 * @pre UI vlákno při inicializaci GUI (mz700_devices_ui_register() z
 *      myimgui_init_cb), jen jádro MZ-700. Volat jednou (další volání nic
 *      nedělá).
 * @post Položka menu a okno jsou registrované do konce procesu.
 */
void kp80_ui_register(void);

#ifdef __cplusplus
}
#endif

#endif /* KP80_WINDOW_H */
