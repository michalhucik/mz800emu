/**
 * @file   scanlines_overlay.h
 * @brief  Emulace scanlines CRT: tmavé mezery mezi rastrovými řádky přes obraz hlavního okna.
 *
 * MZ-800 PAL má 312 celých řádků na snímek (báze mz800-knowledge,
 * hw/08a-video-timing.md), tedy bez půlřádku, který by posunul sudé a liché
 * půlsnímky proti sobě (odvozeno, neměřeno). Každý snímek se tak kreslí na
 * stejné řádky obrazovky a mezi nimi zůstává tmavá mezera. Emulátor obraz
 * jen svisle roztahuje, proto se mezery napodobují vrstvou nad obrazem:
 * svisle opakující se textura 1 x 8 texelů (horní polovina průhledná, dolní
 * černá) roztažená tak, že na každý řádek framebufferu připadá jedna perioda.
 * Lineární filtrování textury vyhladí hrany čar (na 1/8 řádku), takže ani
 * neceločíselné zvětšení nedá nestejně široké ostré čáry. Intenzitu řídí
 * alfa barvy vrcholů.
 *
 * Efekt je jen vrstva zobrazení: framebuffer emulátoru, snímky obrazovky
 * (screenshot_ui) ani nahrávání videa (videorec) se nemění.
 *
 * @par Vlákna
 * Jen UI vlákno s aktivním OpenGL kontextem hlavního okna.
 *
 * @par Licence: GPLv3
 */

#ifndef SCANLINES_OVERLAY_H
#define SCANLINES_OVERLAY_H

/**
 * @brief Nakreslí scanlines přes právě vykreslený obraz emulátoru.
 *
 * Volá se uvnitř okna `##MainEmulatorWindow` hned po `ImGui::Image()` s obrazem
 * emulátoru: obdélník obrazu bere z `ImGui::GetItemRectMin()` a
 * `ImGui::GetItemRectMax()` a kreslí do draw listu tohoto okna (ostatní okna
 * ImGui zůstávají nad efektem). Při vypnutých scanlines (g_display.scanlines
 * == 0) nedělá nic.
 *
 * Při prvním volání se zapnutými scanlines vytvoří OpenGL texturu vzoru
 * (8 texelů); ta žije do konce procesu (uvolní ji zánik GL kontextu).
 * Selže-li vytvoření textury, efekt se nekreslí a pokus se už neopakuje
 * (chyba se vypíše jednou).
 *
 * @pre Poslední položkou ImGui je obraz emulátoru, roztažený přes celý
 *      framebuffer (VIDEO_DISPLAY_WIDTH x VIDEO_DISPLAY_HEIGHT).
 * @pre Aktivní je OpenGL kontext hlavního okna (renderer SDLAPP_RENDERER_OPENGL).
 */
void imgui_scanlines_overlay(void);

#endif /* SCANLINES_OVERLAY_H */
