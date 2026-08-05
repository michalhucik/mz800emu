#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */
#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "emulator/mzarch/mzarch.h"
#include "emulator/hw-generic/cmt/cmt.h"
#include "emulator/hw-generic/pioz80/pioz80.h"
#include "emulator/hw-generic/mz1p16/mz1p16_emu.h"

extern "C"
{
    void imgui_menu_mz800_dip_switch(void);
}

void imgui_menu_mz800_dip_switch(void)
{
    if (ImGui::BeginMenu(_L("Rear DIP Switch")))
    {
#if MZARCH != 700
        GString *str0 = g_string_new(_("Mode 700 Compat.: "));
        if (MZARCH_TEST_REAR_DIP_SWITCH700)
        {
            g_string_append(str0, _("OFF"));
        }
        else
        {
            g_string_append(str0, _("ON (default)"));
        };

        if (ImGui::MenuItem(str0->str, NULL, !MZARCH_TEST_REAR_DIP_SWITCH700))
        {
            mzarch_rear_dip_switch_mz700_compat(!MZARCH_TEST_REAR_DIP_SWITCH700);
        };
        g_string_free(str0, TRUE);
#endif /* MZARCH != 700 */

        GString *str1 = g_string_new(_("CMT Polarity: "));
        if (CMT_TEST_POLARITY_INVERTED)
        {
            g_string_append(str1, _("Inverted"));
        }
        else
        {
            g_string_append(str1, _("Normal"));
        };

        if (ImGui::MenuItem(str1->str, NULL, CMT_TEST_POLARITY_INVERTED, CMT_TEST_STOP))
        {
            cmt_rear_dip_switch_cmt_inverted_polarity(!CMT_TEST_POLARITY_INVERTED);
        };

#if (MZARCH == 800) || (MZARCH == 1500)
        /* SW2 + SW3: standard rozhraní tiskárny = polarita řídicích signálů
         * (PA6/PA7 přes XOR). Oba spínače musí být ve stejné poloze, takže je
         * modelujeme jako jeden přepínač MZ <-> Centronics. */
        bool printer_mz = (pioz80_get_printer_std() == PIOZ80_PRINTER_STD_MZ);
        /* Dokud je plotter připojený (okno otevřené), je standard zamčený na MZ
         * (auto-přepnutí při otevření) - změna by rozhodila handshake. */
        bool std_locked = mz1p16_emu_get_active();
        GString *str2 = g_string_new(_("Printer Standard: "));
        if (printer_mz)
        {
            g_string_append(str2, _("MZ"));
        }
        else
        {
            g_string_append(str2, _("Centronics (default)"));
        };
        if (std_locked)
        {
            g_string_append(str2, _(" (locked: plotter connected)"));
        };
        g_string_append(str2, "###RearDipPrinterStandard");

        if (ImGui::MenuItem(str2->str, NULL, printer_mz, !std_locked))
        {
            pioz80_set_printer_std(printer_mz ? PIOZ80_PRINTER_STD_CENTRONICS
                                              : PIOZ80_PRINTER_STD_MZ);
        };
        g_string_free(str2, TRUE);
#endif /* (MZARCH == 800) || (MZARCH == 1500) */

        ImGui::EndMenu();
    };
}
