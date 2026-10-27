/**
 * @file   menu_devbus.cpp
 * @brief  Generické menu volitelných zařízení devbus (kontrakt v menu_devbus.h).
 *
 * Názvy zařízení, popisky voleb a popisky hodnot KEYWORD (keyword_labels)
 * jsou anglické msgid z deskriptorů; do .pot je dostane N_() v UI souboru
 * zařízení, tady se jen překládají. Volba KEYWORD bez keyword_labels
 * ukazuje klíčová slova tak, jak jsou v INI.
 *
 * @par Licence: GPLv3
 */

#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "menu_devbus.h"
#include "device_request.h"
#include "hw-generic/devbus/devbus.h"
#include "hw-generic/devbus/devbus_request.h"


/**
 * @brief Specifické UI jednoho zařízení.
 * @invariant id != NULL pro položky 0 .. s_extra_count - 1.
 */
typedef struct st_DEVBUS_UI_EXTRA {
    const char *id;                  /**< Id zařízení (statický řetězec). */
    devbus_ui_menu_fn menu_fn;       /**< Položky v podmenu, nebo NULL. */
    devbus_ui_windows_fn windows_fn; /**< Okna, nebo NULL. */
} st_DEVBUS_UI_EXTRA;

/** @brief Registrovaná specifická UI (jen UI vlákno). */
static st_DEVBUS_UI_EXTRA s_extra[DEVBUS_MAX_DEVICES];
/** @brief Počet položek v s_extra. */
static unsigned s_extra_count = 0;


int devbus_ui_register_extra(const char *id, devbus_ui_menu_fn menu_fn, devbus_ui_windows_fn windows_fn)
{
    if (!id || s_extra_count >= DEVBUS_MAX_DEVICES) return -1;
    s_extra[s_extra_count].id = id;
    s_extra[s_extra_count].menu_fn = menu_fn;
    s_extra[s_extra_count].windows_fn = windows_fn;
    s_extra_count++;
    return 0;
}


/** @brief Specifické UI zařízení @p id, nebo NULL. */
static const st_DEVBUS_UI_EXTRA *devbus_ui_extra_find(const char *id)
{
    for (unsigned i = 0; i < s_extra_count; i++) {
        if (strcmp(s_extra[i].id, id) == 0) return &s_extra[i];
    }
    return NULL;
}


/**
 * @brief Položka jedné volby zařízení.
 * @param d    Deskriptor.
 * @param slot Slot zařízení.
 * @param opt  Index volby.
 */
static void imgui_menu_devbus_option(const st_DEVBUS_DEVICE_DESC *d, int slot, unsigned opt)
{
    const st_DEVBUS_OPTION *o = &d->options[opt];
    unsigned value = devbus_option_get(slot, opt);
    ImGui::PushID((int) opt);

    switch (o->type) {
    case DEVBUS_OPT_BOOL:
        if (ImGui::MenuItem(_L(o->label), NULL, value != 0)) {
            devbus_request_option(d->id, opt, value ? 0u : 1u);
        }
        break;

    case DEVBUS_OPT_KEYWORD:
        if (ImGui::BeginMenu(_L(o->label))) {
            for (unsigned k = 0; o->keywords[k]; k++) {
                ImGui::PushID((int) k);
                /* Popisek hodnoty (msgid zařízení), jinak klíčové slovo z INI. */
                const char *item = o->keyword_labels ? _L(o->keyword_labels[k]) : o->keywords[k];
                if (ImGui::MenuItem(item, NULL, value == k)) {
                    if (value != k) devbus_request_option(d->id, opt, k);
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        break;

    case DEVBUS_OPT_UNSIGNED: {
        /* Číselné pole přímo v menu; změna se zařadí po Enter. */
        static unsigned s_edit[DEVBUS_MAX_DEVICES][DEVBUS_MAX_OPTIONS];
        static bool s_edit_active[DEVBUS_MAX_DEVICES][DEVBUS_MAX_OPTIONS];
        if (!s_edit_active[slot][opt]) s_edit[slot][opt] = value;
        ImGui::TextUnformatted(_(o->label));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        bool enter = ImGui::InputScalar("###devbus_unsigned", ImGuiDataType_U32, &s_edit[slot][opt],
                                        NULL, NULL, "%u", ImGuiInputTextFlags_EnterReturnsTrue);
        s_edit_active[slot][opt] = ImGui::IsItemActive();
        if (enter) {
            unsigned v = s_edit[slot][opt];
            if (v < o->min) v = o->min;
            if (v > o->max) v = o->max;
            if (v != value) devbus_request_option(d->id, opt, v);
            s_edit_active[slot][opt] = false;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(_("Range %u - %u, press Enter to apply"), o->min, o->max);
        }
        break;
    }
    }

    if (o->apply == DEVBUS_APPLY_CONNECT) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", _("(applies after reconnect)"));
    }
    ImGui::PopID();
}


void imgui_menu_devbus(void)
{
    if (!devbus_is_initialized() || devbus_count() == 0) return;

    ImGui::Separator();
    for (unsigned slot = 0; slot < devbus_count(); slot++) {
        const st_DEVBUS_DEVICE_DESC *d = devbus_desc((int) slot);
        if (!d) continue;
        ImGui::PushID(d->id);
        if (ImGui::BeginMenu(_L(d->name))) {
            bool connected = devbus_is_connected((int) slot);
            if (ImGui::MenuItem(_L("Connected"), NULL, connected)) {
                devbus_request_connect(d->id, !connected);
            }
            if (d->option_count) ImGui::Separator();
            for (unsigned opt = 0; opt < d->option_count; opt++) {
                imgui_menu_devbus_option(d, (int) slot, opt);
            }
            const st_DEVBUS_UI_EXTRA *x = devbus_ui_extra_find(d->id);
            if (x && x->menu_fn) {
                ImGui::Separator();
                x->menu_fn();
            }
            ImGui::EndMenu();
        }
        ImGui::PopID();
    }
}


void imgui_devbus_windows(void)
{
    if (!s_extra_count) return;
    /* Okna zařízení čtou jeho stav (buffery, registry); zásah ho mezitím
     * nezmění. */
    device_state_lock();
    for (unsigned i = 0; i < s_extra_count; i++) {
        if (s_extra[i].windows_fn) s_extra[i].windows_fn();
    }
    device_state_unlock();
}
