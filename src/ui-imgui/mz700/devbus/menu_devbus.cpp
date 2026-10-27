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
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

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
 * @brief BeginMenu() s šedou poznámkou ve sloupci zkratek menu.
 *
 * ImGui::BeginMenu() nemá parametr zkratky jako MenuItem(). Poznámka
 * vykreslená za položkou přes SameLine() do šířky menu nevstupuje: položka
 * se roztáhne na celou šířku okna a šipka podmenu skončí pod koncem
 * poznámky. Funkce proto před BeginMenu() deklaruje šířku poznámky ve
 * sloupci zkratek (ImGuiMenuColumns, stejně jako MenuItem() se zkratkou),
 * takže se menu rozšíří a šipka posune za sloupec, a poznámku pak vykreslí
 * barvou ImGuiCol_TextDisabled na místo, kam MenuItem() kreslí zkratku.
 *
 * @param label Popisek položky (může obsahovat "###id", jako u BeginMenu()).
 * @param hint  Text poznámky (už přeložený), nesmí být NULL.
 * @return Návratová hodnota ImGui::BeginMenu(): true = podmenu je otevřené
 *         a volající musí zavolat ImGui::EndMenu().
 *
 * @pre Volat jen uvnitř svislého menu (BeginMenu() / BeginPopup()), ne
 *      v liště menu; v liště by poznámka ležela mimo položku.
 * @note Šířka sloupce se projeví až od dalšího snímku (zpětná vazba
 *       ImGuiMenuColumns), stejně jako u nativní zkratky MenuItem().
 * @note Používá interní API ImGui (imgui_internal.h): GetCurrentWindow(),
 *       ImGuiWindow::DC.MenuColumns.
 */
static bool imgui_begin_menu_hint(const char *label, const char *hint)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return ImGui::BeginMenu(label);

    /* Stejné sloupce jako BeginMenu() (bez ikony, se značkou), navíc zkratka. */
    const float label_w = ImGui::CalcTextSize(label, NULL, true).x;
    const float hint_w = ImGui::CalcTextSize(hint).x;
    const float mark_w = IM_TRUNC(ImGui::GetFontSize() * 1.20f);
    const float min_w = window->DC.MenuColumns.DeclColumns(0.0f, label_w, hint_w, mark_w);
    const float stretch_w = ImMax(0.0f, ImGui::GetContentRegionAvail().x - min_w);
    const ImVec2 hint_pos(window->DC.CursorPos.x + window->DC.MenuColumns.OffsetShortcut + stretch_w,
                          window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    /* Po otevření podmenu je aktuální okno podmenu: kreslit do seznamu tohoto okna. */
    ImDrawList *draw_list = window->DrawList;

    bool open = ImGui::BeginMenu(label);
    draw_list->AddText(hint_pos, ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    return open;
}


/**
 * @brief Stav okna nastavení jednoho zařízení (index = slot zařízení).
 *
 * Okno edituje kopii voleb; zařízení se změní až tlačítkem OK.
 *
 * @invariant Je-li open, orig[] a edit[] mají platné hodnoty pro volby
 *            0 .. option_count - 1 zařízení ve slotu.
 */
typedef struct st_DEVBUS_SETTINGS_UI {
    bool open;                          /**< Okno je otevřené. */
    bool focus;                         /**< Při příštím vykreslení dát oknu fokus. */
    unsigned orig[DEVBUS_MAX_OPTIONS];  /**< Hodnoty voleb při otevření okna. */
    unsigned edit[DEVBUS_MAX_OPTIONS];  /**< Rozepsané hodnoty v okně. */
} st_DEVBUS_SETTINGS_UI;

/** @brief Okna nastavení zařízení (jen UI vlákno). */
static st_DEVBUS_SETTINGS_UI s_settings[DEVBUS_MAX_DEVICES];


/**
 * @brief Otevře (nebo vyzdvihne) okno nastavení zařízení.
 *
 * Převezme aktuální hodnoty všech voleb jako výchozí stav okna; u už
 * otevřeného okna rozepsané hodnoty zahodí a načte je znovu.
 *
 * @param slot Slot zařízení.
 * @pre UI vlákno, drží zámek stavu zařízení (čte devbus_option_get()).
 */
static void devbus_settings_open(int slot)
{
    if (slot < 0 || slot >= DEVBUS_MAX_DEVICES) return;
    const st_DEVBUS_DEVICE_DESC *d = devbus_desc(slot);
    if (!d) return;
    st_DEVBUS_SETTINGS_UI *s = &s_settings[slot];
    for (unsigned opt = 0; opt < d->option_count && opt < DEVBUS_MAX_OPTIONS; opt++) {
        s->orig[opt] = s->edit[opt] = devbus_option_get(slot, opt);
    }
    s->open = true;
    s->focus = true;
}


/**
 * @brief Okno nastavení jednoho zařízení: všechny volby, OK a Cancel.
 *
 * BOOL zaškrtávátko, KEYWORD výběr (popisky keyword_labels, jinak klíčová
 * slova z INI), UNSIGNED číselné pole (rozsah v tooltipu, po ukončení
 * editace se hodnota oseká na min..max). Volba s DEVBUS_APPLY_CONNECT má
 * šedou poznámku "(applies after reconnect)".
 *
 * OK zařadí devbus_request_option() jen u voleb, které se v okně změnily
 * proti stavu při otevření (změna stejné volby jinudy, např. přes MCP,
 * se tak nepřepíše starou hodnotou); Cancel i zavírací křížek okno zavřou
 * bez změny.
 *
 * Okno je samostatné okno ImGui, ne popup hlavního okna: s fokusem v něm
 * hlavní okno fokus nemá (main_window.cpp) a stisky kláves při psaní do
 * pole nedostává emulovaná klávesnice. Číselné pole proto není v menu.
 *
 * @param slot Slot zařízení.
 * @pre UI vlákno, drží zámek stavu zařízení (imgui_devbus_windows()).
 */
static void devbus_settings_window(int slot)
{
    const st_DEVBUS_DEVICE_DESC *d = devbus_desc(slot);
    st_DEVBUS_SETTINGS_UI *s = &s_settings[slot];
    if (!d) {
        s->open = false;
        return;
    }

    char title[160];
    snprintf(title, sizeof(title), "%s - %s###devbus_settings_%s", _(d->name), _("Settings"), d->id);
    if (s->focus) {
        ImGui::SetNextWindowFocus();
        s->focus = false;
    }
    bool keep_open = true;
    if (ImGui::Begin(title, &keep_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        const float field_w = ImGui::GetFontSize() * 12.0f;
        for (unsigned opt = 0; opt < d->option_count && opt < DEVBUS_MAX_OPTIONS; opt++) {
            const st_DEVBUS_OPTION *o = &d->options[opt];
            ImGui::PushID((int) opt);
            switch (o->type) {
            case DEVBUS_OPT_BOOL: {
                bool b = s->edit[opt] != 0;
                if (ImGui::Checkbox(_L(o->label), &b)) s->edit[opt] = b ? 1u : 0u;
                break;
            }

            case DEVBUS_OPT_KEYWORD: {
                unsigned cur = s->edit[opt];
                /* Popisek hodnoty (msgid zařízení), jinak klíčové slovo z INI. */
                const char *preview = o->keyword_labels ? _(o->keyword_labels[cur]) : o->keywords[cur];
                ImGui::SetNextItemWidth(field_w);
                if (ImGui::BeginCombo(_L(o->label), preview)) {
                    for (unsigned k = 0; o->keywords[k]; k++) {
                        ImGui::PushID((int) k);
                        const char *item = o->keyword_labels ? _(o->keyword_labels[k]) : o->keywords[k];
                        if (ImGui::Selectable(item, cur == k)) s->edit[opt] = k;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                break;
            }

            case DEVBUS_OPT_UNSIGNED:
                /* Bez ImGuiInputTextFlags_EnterReturnsTrue: InputScalar() ho
                 * nepodporuje (IM_ASSERT v ImGui 1.92.6). */
                ImGui::SetNextItemWidth(field_w);
                ImGui::InputScalar(_L(o->label), ImGuiDataType_U32, &s->edit[opt], NULL, NULL, "%u");
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    if (s->edit[opt] < o->min) s->edit[opt] = o->min;
                    if (s->edit[opt] > o->max) s->edit[opt] = o->max;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s %u - %u", _("Range:"), o->min, o->max);
                }
                break;
            }
            if (o->apply == DEVBUS_APPLY_CONNECT) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", _("(applies after reconnect)"));
            }
            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Button(_L("OK"), ImVec2(120, 0))) {
            for (unsigned opt = 0; opt < d->option_count && opt < DEVBUS_MAX_OPTIONS; opt++) {
                const st_DEVBUS_OPTION *o = &d->options[opt];
                unsigned v = s->edit[opt];
                if (o->type == DEVBUS_OPT_UNSIGNED) {
                    if (v < o->min) v = o->min;
                    if (v > o->max) v = o->max;
                }
                if (v != s->orig[opt]) devbus_request_option(d->id, opt, v);
            }
            keep_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) keep_open = false;
    }
    ImGui::End();
    if (!keep_open) s->open = false;
}


/**
 * @brief Položka jedné volby zařízení v jeho podmenu.
 *
 * BOOL a KEYWORD se přepínají přímo v menu. UNSIGNED se v menu jen
 * zobrazí (hodnota ve sloupci zkratek) a kliknutí otevře okno nastavení
 * zařízení: textové pole v popupu menu by stisky kláves posílalo i do
 * emulované klávesnice (popup se počítá k hlavnímu oknu).
 *
 * Volba s DEVBUS_APPLY_CONNECT má šedou poznámku "(applies after reconnect)"
 * ve sloupci zkratek menu (MenuItem() se zkratkou, imgui_begin_menu_hint()).
 *
 * @param d    Deskriptor.
 * @param slot Slot zařízení.
 * @param opt  Index volby.
 */
static void imgui_menu_devbus_option(const st_DEVBUS_DEVICE_DESC *d, int slot, unsigned opt)
{
    const st_DEVBUS_OPTION *o = &d->options[opt];
    unsigned value = devbus_option_get(slot, opt);
    const char *hint = (o->apply == DEVBUS_APPLY_CONNECT) ? _("(applies after reconnect)") : NULL;
    ImGui::PushID((int) opt);

    switch (o->type) {
    case DEVBUS_OPT_BOOL:
        if (ImGui::MenuItem(_L(o->label), hint, value != 0)) {
            devbus_request_option(d->id, opt, value ? 0u : 1u);
        }
        break;

    case DEVBUS_OPT_KEYWORD: {
        bool open = hint ? imgui_begin_menu_hint(_L(o->label), hint) : ImGui::BeginMenu(_L(o->label));
        if (open) {
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
    }

    case DEVBUS_OPT_UNSIGNED: {
        /* Hodnota ve sloupci zkratek; úprava jen v okně nastavení. */
        char shortcut[96];
        snprintf(shortcut, sizeof(shortcut), "%u%s%s", value, hint ? "  " : "", hint ? hint : "");
        if (ImGui::MenuItem(_L(o->label), shortcut)) devbus_settings_open(slot);
        break;
    }
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
            if (d->option_count && ImGui::MenuItem(_L("Settings..."))) {
                devbus_settings_open((int) slot);
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
    if (!devbus_is_initialized() || devbus_count() == 0) return;
    /* Okna zařízení čtou jeho stav (buffery, registry, volby); zásah ho
     * mezitím nezmění. */
    device_state_lock();
    for (unsigned slot = 0; slot < devbus_count() && slot < DEVBUS_MAX_DEVICES; slot++) {
        if (s_settings[slot].open) devbus_settings_window((int) slot);
    }
    for (unsigned i = 0; i < s_extra_count; i++) {
        if (s_extra[i].windows_fn) s_extra[i].windows_fn();
    }
    device_state_unlock();
}
