#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "emulator/hw-generic/printer/printer.h"
#include "emulator/hw-generic/mz1p16/mz1p16_emu.h"
#include "emulator/hw-generic/pioz80/pioz80.h"
#include "emulator/hw-generic/printer/printer_request.h"

extern "C"
{
    void imgui_menu_printer(void);
}

/**
 * Položka menu pro virtuální tiskárnu (capture + plotter MZ-1P16).
 *
 * Tiskárna je ONLINE, pokud je zapnutý capture NEBO je otevřené okno plotteru
 * MZ-1P16. Oba subsystémy jsou nezávislé (lze zároveň zachytávat i tisknout,
 * nebo jen jedno z toho).
 *
 * Obsahuje:
 *  - stavový řádek ONLINE/OFFLINE,
 *  - radio volbu standardu tiskárny (Centronics/MZ, duplicitně k Rear DIP Switch),
 *  - přepínač "Capture Enabled" (zapne/vypne handshake + capture),
 *  - stavový řádek s názvem aktuálního capture souboru a počtem bajtů,
 *  - akci "Close Capture File" (uzavře soubor; další bajt založí nový),
 *  - připojení/okno plotteru MZ-1P16.
 */
void imgui_menu_printer(void)
{
    if (ImGui::BeginMenu(_L("Printer")))
    {
        // Tiskárna je ONLINE, pokud běží capture nebo je otevřené okno plotteru.
        bool capture_active = printer_get_active();
        bool plotter_on = g_gui->showPlotterWindow;
        bool online = capture_active || plotter_on;
        if (online)
        {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", _("Status: ONLINE"));
        }
        else
        {
            ImGui::TextDisabled("%s", _("Status: OFFLINE"));
        };

        ImGui::Separator();

        /* Standard tiskárny (polarita řídicích signálů, zadní DIP SW2/SW3).
         * Duplicitně k HW & Devices -> Rear DIP Switch, zde jako radio.
         * Mění týž stav v pioz80 (pioz80_set_printer_std, provede emu vlákno
         * přes printer_request_set_std). Dokud je plotter
         * připojený (okno otevřené), je standard zamčený na MZ (auto-přepnutí) -
         * změna by rozhodila handshake, proto disabled. */
        int std_val = (pioz80_get_printer_std() == PIOZ80_PRINTER_STD_MZ) ? 1 : 0;
        ImGui::TextDisabled("%s", _("Printer Standard:"));
        ImGui::BeginDisabled(plotter_on);
        if (ImGui::RadioButton(_L("Centronics"), &std_val, 0))
        {
            printer_request_set_std(PIOZ80_PRINTER_STD_CENTRONICS);
        };
        if (ImGui::RadioButton(_L("MZ"), &std_val, 1))
        {
            printer_request_set_std(PIOZ80_PRINTER_STD_MZ);
        };
        ImGui::EndDisabled();
        if (plotter_on)
        {
            ImGui::TextDisabled("%s", _("(locked: plotter connected)"));
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Capture Enabled"), NULL, capture_active))
        {
            printer_request_set_active(!capture_active);
        };

        ImGui::Separator();

        // Stav capture souboru
        if (g_printer.fp != NULL)
        {
            ImGui::Text(_("Capture file: %s"), g_printer.filename);
            ImGui::Text(_("Bytes captured: %u"), g_printer.byte_count);
        }
        else
        {
            ImGui::TextDisabled("%s", _("No capture file open"));
        };

        if (g_printer.total_byte_count != g_printer.byte_count)
        {
            ImGui::Text(_("Session total: %u bytes"), g_printer.total_byte_count);
        };

        ImGui::Separator();

        // Uzavření aktuálního souboru (další bajt založí nový jedinečný soubor)
        if (ImGui::MenuItem(_L("Close Capture File"), NULL, false, g_printer.fp != NULL))
        {
            printer_request_close_file();
        };

        ImGui::Separator();

        /* --- Plotter MZ-1P16 (mz1p16-plotter) ---
         *
         * Vztah connected <-> okno: plotter je "připojený" a aktivní právě
         * tehdy, když je jeho okno otevřené (zavřené okno = plotter odpojený,
         * MCU se nekrokuje). Proto "connected" toggle i
         * "Open plotter window" sdílí jeden stav = g_gui->showPlotterWindow.
         * Aktivaci/deaktivaci jádra řeší imgui_plotter_window podle hrany
         * stavu okna (v main_window.cpp se volá bezpodmínečně každý frame).
         * plotter_on je deklarováno výše (pro ONLINE status). */
        if (ImGui::MenuItem(_L("Plotter MZ-1P16 connected"), NULL, plotter_on))
        {
            g_gui->showPlotterWindow = !plotter_on;
        };

        if (ImGui::MenuItem(_L("Open plotter window"), NULL, false, !plotter_on))
        {
            g_gui->showPlotterWindow = true;
        };

        ImGui::EndMenu();
    };
}
