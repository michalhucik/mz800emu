#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/mywidgets/mywidgets.h"
#include "emulator/display.h"
#include "iface/iface_video.h"
#include "emulator/hw-generic/gdg/video.h"

extern "C"
{
    void imgui_menu_display(void);
}

void imgui_menu_display(void)
{
    if (ImGui::BeginMenu(_L("Display")))
    {

        if (ImGui::BeginMenu(_L("Color Schema")))
        {
            if (MenuRadioItem(_("Normal"), DISPLAY_TEST_COLOR_NORMAL))
            {
                display_set_colors(DISPLAY_NORMAL);
            };
            if (MenuRadioItem(_("Grayscale"), DISPLAY_TEST_COLOR_GRAYSCALE))
            {
                display_set_colors(DISPLAY_GRAYSCALE);
            };
            if (MenuRadioItem(_("Green"), DISPLAY_TEST_COLOR_GREEN))
            {
                display_set_colors(DISPLAY_GREEN);
            };

            ImGui::Separator();


            ImGui::BeginDisabled();
            if (MenuRadioItem(_("Custom Colormap..."), false))
            {
            };
            ImGui::EndDisabled();

            ImGui::EndMenu();
        };

        if (ImGui::BeginMenu(_L("Window Size on Startup")))
        {
            if (MenuRadioItem(_("Half"), DISPLAY_TEST_STARTUP_SIZE_HALF))
            {
                display_set_window_startup_scale(DISPLAY_STARTUP_SIZE_HALF);
            };
            if (MenuRadioItem(_("Native"), DISPLAY_TEST_STARTUP_SIZE_NATIVE))
            {
                display_set_window_startup_scale(DISPLAY_STARTUP_SIZE_NATIVE);
            };
            if (MenuRadioItem(_("Normal"), DISPLAY_TEST_STARTUP_SIZE_NORMAL))
            {
                display_set_window_startup_scale(DISPLAY_STARTUP_SIZE_NORMAL);
            };
            if (MenuRadioItem(_("Bigger"), DISPLAY_TEST_STARTUP_SIZE_BIGGER))
            {
                display_set_window_startup_scale(DISPLAY_STARTUP_SIZE_BIGGER);
            };
            if (MenuRadioItem(_("Big"), DISPLAY_TEST_STARTUP_SIZE_BIG))
            {
                display_set_window_startup_scale(DISPLAY_STARTUP_SIZE_BIG);
            };
            if (MenuRadioItem(_("Fullscreen"), DISPLAY_TEST_STARTUP_SIZE_FULLSCREEN))
            {
                display_set_window_startup_scale(DISPLAY_STARTUP_SIZE_FULLSCREEN);
            };
            ImGui::EndMenu();
        };

#ifdef WINDOWS
        ImGui::Separator();

        if (ImGui::BeginMenu(_L("Windows HDR")))
        {
            if (ImGui::MenuItem(_L("Forced Auto Reset HDR After Fullscreen"), NULL, DISPLAY_TEST_FORCED_RESET_HDR))
            {
                ImGui::SetItemTooltip(_("Windows sometimes switches to HDR mode when application is in fullscreen and does not reset after leaving FS mode.\nThis option will force Windows to reset HDR mode after leaving fullscreen mode."));
                DISPLAY_SET_FORCED_RESET_HDR(!DISPLAY_TEST_FORCED_RESET_HDR);
            };

            if (ImGui::MenuItem(_L("Reset HDR Mode Now..."), NULL))
            {
                ImGui::SetItemTooltip(_("Try reset HDR mode now."));
                if (g_iface_video_callbacks->reset_hdr)
                {
                    g_iface_video_callbacks->reset_hdr();
                }
                else
                {
                    WARN("Reset HDR - callback not found!\n");
                };
            };
            ImGui::EndMenu();
        };
#endif

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Fix Window Aspect Ratio by Width"), "Alt+W"))
        {
            if (g_iface_video_callbacks->fix_window_aspect_ratio)
            {
                g_iface_video_callbacks->fix_window_aspect_ratio('W');
            }
            else
            {
                WARN("Fix window aspect ratio - callback not found!\n");
            };
        };

        if (ImGui::MenuItem(_L("Fix Window Aspect Ratio by Height"), "Alt+H"))
        {
            if (g_iface_video_callbacks->fix_window_aspect_ratio)
            {
                g_iface_video_callbacks->fix_window_aspect_ratio('H');
            }
            else
            {
                WARN("Fix window aspect ratio - callback not found!\n");
            };
        };

        if (ImGui::MenuItem(_L("Lock Window Aspect Ratio"), NULL, DISPLAY_TEST_LOCKED_ASPECT_RATIO))
        {
            display_set_locked_aspect_ratio(!DISPLAY_TEST_LOCKED_ASPECT_RATIO);
        };

        ImGui::Separator();

        /* Scanlines CRT - jen vrstva zobrazení (scanlines_overlay.h). */
        if (ImGui::BeginMenu(_L("CRT Scanlines")))
        {
            if (ImGui::MenuItem(_L("Enable Scanlines"), NULL, DISPLAY_TEST_SCANLINES))
            {
                display_set_scanlines(!DISPLAY_TEST_SCANLINES);
            };
            ImGui::SetItemTooltip(_("Darkens the gaps between raster lines like a CRT screen.\nDisplay only - screenshots and video recordings are not affected."));

            bool is_disabled = (!DISPLAY_TEST_SCANLINES);
            if (is_disabled)
                ImGui::BeginDisabled();

            int intensity = g_display.scanlines_intensity;
            if (ImGui::SliderInt(_L("Intensity"), &intensity, DISPLAY_SCANLINES_INTENSITY_MIN, DISPLAY_SCANLINES_INTENSITY_MAX, "%d %%", ImGuiSliderFlags_AlwaysClamp))
            {
                display_set_scanlines_intensity(intensity);
            };
            ImGui::SetItemTooltip(_("How much the gap between raster lines is darkened (100 %% = black)."));

            if (is_disabled)
                ImGui::EndDisabled();

            ImGui::EndMenu();
        };

        ImGui::Separator();

        if (ImGui::BeginMenu(_L("Frame Rate Control")))
        {
            char slave_label[64];
            snprintf(slave_label, sizeof(slave_label), _("Slave (%dHz)"), VIDEO_SCREENS_PER_SEC);
            if (MenuRadioItem(slave_label, DISPLAY_TEST_FRAMERATE_MODE_SLAVE))
            {
                display_set_framerate_mode(DISPLAY_FRAMERATE_MODE_SLAVE);
            };
            ImGui::SetItemTooltip(_("The interface will attempt to refresh the image at the frequency set by the emulator."));

            if (MenuRadioItem(_("Swap With V-Sync"), DISPLAY_TEST_FRAMERATE_MODE_VSYNC))
            {
                display_set_framerate_mode(DISPLAY_FRAMERATE_MODE_VSYNC);
            };
            ImGui::SetItemTooltip(_("Synchronized swap interval with the vertical retrace."));

            if (MenuRadioItem(_("Swap With V-Sync (Adaptive)"), DISPLAY_TEST_FRAMERATE_MODE_ADAPTIVE))
            {
                display_set_framerate_mode(DISPLAY_FRAMERATE_MODE_ADAPTIVE);
            };
            ImGui::SetItemTooltip(_("Adaptive vsync works the same as Vsync, but if is already missed the vertical retrace for a given frame,\nit swaps buffers immediately, which might be less jarring for the user during occasional framerate drops.\nThis function must be supported by your system."));

            if (MenuRadioItem(_("Immediate Swap"), DISPLAY_TEST_FRAMERATE_MODE_IMMEDIATE))
            {
                display_set_framerate_mode(DISPLAY_FRAMERATE_MODE_IMMEDIATE);
            };
            ImGui::SetItemTooltip(_("Immediate swap interval using a maximum available refresh rate."));

            if (MenuRadioItem(_("Custom FPS"), DISPLAY_TEST_FRAMERATE_MODE_CUSTOM))
            {
                display_set_framerate_mode(DISPLAY_FRAMERATE_MODE_CUSTOM);
            };
            ImGui::SetItemTooltip(_("The interface will attempt to refresh the image at the frequency set by the user."));

            ImGui::Separator();

            bool is_dissabled = (!DISPLAY_TEST_FRAMERATE_MODE_CUSTOM);

            if (is_dissabled)
                ImGui::BeginDisabled();

            if (ImGui::SliderInt(" Hz###custom_fps_slider", &g_display.custom_fps, DISPLAY_CUSTOM_FPS_MIN, DISPLAY_CUSTOM_FPS_MAX))
            {
                display_set_framerate_mode(DISPLAY_FRAMERATE_MODE_CUSTOM);
            };

            if (ImGui::InputInt(" Hz###custom_fps_input", &g_display.custom_fps, DISPLAY_CUSTOM_FPS_MIN, DISPLAY_CUSTOM_FPS_MAX))
            {
                display_set_framerate_mode(DISPLAY_FRAMERATE_MODE_CUSTOM);
            };

            if (is_dissabled)
                ImGui::EndDisabled();

            ImGui::EndMenu();
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Forced Redraw Every Screen"), NULL, DISPLAY_TEST_FORCED_FULL_SCREEN_REDRAWING))
        {
            display_set_forced_redrawing(!DISPLAY_TEST_FORCED_FULL_SCREEN_REDRAWING);
        };
        ImGui::SetItemTooltip(_("A frame redraw will be forced regardless of whether the framebuffer has changed.\nThis mode will put a little more strain on your GPU and will be reduce of interface FPS."));

        ImGui::Separator();

        if (ImGui::MenuItem(_L("FPS Measurement..."), NULL))
        {
            // TODO: pokud uz je okno otevreno, tak ho vyzadat do popredi;
            g_gui->showVideoIfaceMeasurementWindow = true;
        };

        ImGui::EndMenu();
    };
}
