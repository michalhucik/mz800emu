/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file kp80_window.cpp
 * @brief Okno "K&P 80 Column Monitor" (kontrakt v kp80_window.h).
 *
 * Textura OpenGL 640 x 250 RGBA vzniká při prvním vykreslení okna a mění se
 * jen, když výstup karty předá nový snímek (devbus_vout_fetch()). Indexy
 * palety výstupu se převádějí na RGBA podle neměnné palety z popisu výstupu.
 */

#include <stdint.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "kp80_window.h"
#include "ui-imgui/mz700/devbus/menu_devbus.h"
#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

extern "C" {
#include "hw-generic/devbus/devbus.h"
#include "hw-generic/devbus/devbus_request.h"
#include "mzarch/mz700/devices/kp80/kp80_devbus.h"
#include "mzarch/mz700/devices/kp80/kp80_core.h"
}


/**
 * @brief Název karty, popisky voleb a popisky hodnot (keyword_labels)
 *        deskriptoru pro katalog překladů.
 *
 * Deskriptor v jádru emulátoru (kp80_devbus.c) nesmí používat lokalizační
 * makra; generické menu devbus řetězce překládá přes _L(o->label). Tady je
 * označí N_(), aby je xgettext zařadil do .pot. Musí se shodovat s
 * deskriptorem slovo od slova.
 */
[[maybe_unused]] static const char *const s_kp80_i18n_marks[] = {
    N_("K&P 80 Column Card"),
    N_("Read from 70h increments the address"),
    N_("Write to 71h also writes the selected CRTC register"),
    N_("Update address (R18/R19) takes effect on R31 access"),
    N_("Update cycle length (character clocks)"),
    N_("WAIT on ports 71h - 73h (T-states)"),
    N_("Monitor phosphor color"),
    N_("White"),
    N_("Green"),
    N_("Amber"),
};


/** @brief Okno je otevřené (zdroj pravdy; INI ho načítá a ukládá). */
static bool s_show = false;
/** @brief Modul `[KP80_UI]` je zaregistrovaný (kp80_ui_init()). */
static bool s_cfg_registered = false;
/** @brief Položka menu a okno jsou registrované v devbus (kp80_ui_register()). */
static bool s_registered = false;
/** @brief Stav připojení karty z minulého snímku UI (hrana = automatické otevření). */
static bool s_prev_connected = false;
/** @brief s_prev_connected už nese platný stav (první snímek hranu nedělá). */
static bool s_prev_valid = false;

/** @brief Textura obrazu (0 = zatím nevznikla). */
static GLuint s_texture = 0;
/** @brief Pořadí naposledy nahraného snímku (devbus_vout_fetch()). */
static uint32_t s_seen = 0;
/** @brief Indexy palety posledního snímku. */
static uint8_t s_pix[KP80_FB_SIZE];
/** @brief Obraz RGBA pro texturu. */
static uint8_t s_rgba[KP80_FB_SIZE * 4];


/**
 * @brief INI -> viditelnost okna.
 * @param e    Prvek `window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void kp80_ui_cfg_propagate(void *e, void *data)
{
    (void) data;
    s_show = cfgelement_get_bool_value((st_CFGELEMENT *) e) ? true : false;
}


/**
 * @brief Viditelnost okna -> INI.
 * @param e    Prvek `window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void kp80_ui_cfg_save(void *e, void *data)
{
    (void) data;
    cfgelement_set_bool_value((st_CFGELEMENT *) e, s_show ? 1 : 0);
}


/**
 * @brief Položka v podmenu karty (HW & Devices): zobrazit / skrýt okno monitoru.
 * @pre UI vlákno uvnitř podmenu zařízení (zámek stavu zařízení drží volající).
 */
static void kp80_ui_menu(void)
{
    if (ImGui::MenuItem(_L("Show 80 Column Monitor"), NULL, s_show)) {
        s_show = !s_show;
    }
}


/**
 * @brief Nahraje nový snímek výstupu do textury, je-li k dispozici.
 * @param o Výstup karty.
 * @pre UI vlákno s aktivním kontextem OpenGL.
 */
static void kp80_ui_update_texture(st_DEVBUS_VOUT *o)
{
    const st_DEVBUS_VOUT_DESC *d = devbus_vout_desc(o);
    if (!d || d->width != KP80_FB_WIDTH || d->height != KP80_FB_HEIGHT) return;

    if (!s_texture) {
        glGenTextures(1, &s_texture);
        glBindTexture(GL_TEXTURE_2D, s_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        memset(s_rgba, 0, sizeof(s_rgba));
        for (size_t i = 3; i < sizeof(s_rgba); i += 4) s_rgba[i] = 0xFF;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, KP80_FB_WIDTH, KP80_FB_HEIGHT, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_rgba);
        glBindTexture(GL_TEXTURE_2D, 0);
        s_seen = 0; /* nová textura = nahrát i stávající snímek */
    }

    if (!devbus_vout_fetch(o, s_pix, sizeof(s_pix), &s_seen)) return;

    for (size_t i = 0; i < KP80_FB_SIZE; i++) {
        uint8_t ix = s_pix[i];
        uint32_t c = (ix < d->palette_size) ? d->palette[ix] : 0x000000FFu;
        s_rgba[i * 4 + 0] = (uint8_t) (c >> 24);
        s_rgba[i * 4 + 1] = (uint8_t) (c >> 16);
        s_rgba[i * 4 + 2] = (uint8_t) (c >> 8);
        s_rgba[i * 4 + 3] = (uint8_t) c;
    }
    glBindTexture(GL_TEXTURE_2D, s_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, KP80_FB_WIDTH, KP80_FB_HEIGHT,
                    GL_RGBA, GL_UNSIGNED_BYTE, s_rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
}


/**
 * @brief Okno monitoru karty (windows_fn devbus, volá se každý snímek UI).
 *
 * Při připojení karty za běhu (hrana odpojeno -> připojeno) okno otevře.
 * Obraz se škáluje na dostupnou plochu okna s poměrem stran podle
 * pixel_aspect výstupu (4:3) a centruje se.
 *
 * @pre UI vlákno; zámek stavu zařízení drží volající (imgui_devbus_windows()).
 */
static void kp80_ui_windows(void)
{
    if (!devbus_is_initialized()) return;
    st_DEVBUS_VOUT *o = kp80_devbus_vout();
    if (!o) return;
    int slot = devbus_vout_slot(o);
    bool connected = devbus_is_connected(slot);
    if (s_prev_valid && connected && !s_prev_connected) s_show = true;
    s_prev_connected = connected;
    s_prev_valid = true;

    if (!s_show) return;

    const st_DEVBUS_VOUT_DESC *d = devbus_vout_desc(o);
    float aspect = ((float) d->width * d->pixel_aspect) / (float) d->height;

    ImGui::SetNextWindowSize(ImVec2(680.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_L("K&P 80 Column Monitor###KP80Monitor"), &s_show,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    if (!connected) {
        ImGui::TextDisabled("%s", _("The K&P 80 column card is not connected."));
        if (ImGui::Button(_L("Connect"))) {
            const st_DEVBUS_DEVICE_DESC *dd = devbus_desc(slot);
            if (dd) devbus_request_connect(dd->id, true);
        }
        ImGui::End();
        return;
    }

    kp80_ui_update_texture(o);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x >= 8.0f && avail.y >= 8.0f && s_texture) {
        float w = avail.x;
        float h = w / aspect;
        if (h > avail.y) {
            h = avail.y;
            w = h * aspect;
        }
        ImVec2 cur = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(cur.x + (avail.x - w) * 0.5f, cur.y + (avail.y - h) * 0.5f));
        ImGui::Image((ImTextureID) (intptr_t) s_texture, ImVec2(w, h));
    }
    ImGui::End();
}


void kp80_ui_init(void)
{
    if (s_cfg_registered) return;
    s_cfg_registered = true;

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, (char *) "KP80_UI");
    if (cmod) {
        CFGELM *elm = cfgmodule_register_new_element(cmod, (char *) "window", CFGENTYPE_BOOL, 0);
        cfgelement_set_propagate_cb(elm, kp80_ui_cfg_propagate, NULL);
        cfgelement_set_save_cb(elm, kp80_ui_cfg_save, NULL);
        cfgmodule_parse(cmod);
        cfgmodule_propagate(cmod);
    }
}


void kp80_ui_register(void)
{
    if (s_registered) return;
    s_registered = true;
    devbus_ui_register_extra(KP80_DEVBUS_ID, kp80_ui_menu, kp80_ui_windows);
}
