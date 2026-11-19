/**
 * @file   overlay_insert_sheet.cpp
 * @brief  Overlay menu: výsuvný list výměny média a volba mechaniky pro nedávné médium.
 *
 * List se otevírá tlačítkem "Change" / "Insert disk" / "Change tape" tabu
 * Media (overlay_sheet_open()) a nabízí nedávná média téhož druhu, souborový
 * dialog, vysunutí a ochranu proti zápisu. Kreslí se jako podřízené okno
 * overlaye nad ztmavenou plochou obsahu tabu, ukotvené k tlačítku, které ho
 * otevřelo; není to popup (popup by zdržel modální dialogy emulátoru,
 * imgui_modal_can_open()).
 *
 * Klávesy: fokus má po otevření seznam, ne pole filtru. Číslice 1 až 6
 * vkládají položku, jen dokud je filtr prázdný; tisknutelné znaky jdou do
 * filtru (štítky číslic pak zmizí), Backspace maže, Esc zavře jen list.
 * Číslice se poznává podle klávesy (ImGuiKey_1 až ImGuiKey_6 a numerická
 * klávesnice), ne podle znaku: na rozložení, kde horní řada bez Shiftu píše
 * jiné znaky (české, AZERTY), vkládá stejná klávesa a znak, který stisk
 * vyrobil, se při prázdném filtru zahodí (sheet_digit_key_active()).
 * Pole filtru není widget ImGui::InputText(): znaky se čtou z fronty vstupu
 * ImGui a o textový vstup platformy si list říká sám, aby fokus navigace
 * mohl zůstat na seznamu.
 *
 * Vkládá se funkcemi, které volá souborový dialog klasického menu po výběru
 * souboru (overlay_sheet_request_insert()); dotazy na nahrávání a neuložené
 * změny pásky se tedy nepřeskakují. Ostatní zásahy (dialog souboru,
 * vysunutí, ochrana proti zápisu) jdou přes tabulku akcí.
 *
 * @par Vlákna
 * Jen UI vlákno.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

#include "i18n.h"
#include "mzarch/mzarch_config.h"
#include "emulator/recent_media/recent_media.h"

#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/fdc/fdc_request.h"
#endif
#if CFG_HWEXT_HAVE_QDISK
#include "hw-generic/qdisk/qdisk_request.h"
#endif

extern "C"
{
#include "baseui/baseui.h"
}

#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_style.h"
#include "ui-imgui/overlay/overlay_tabs.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"
#include "ui-imgui/overlay/res/IconsLucide.h"

#ifdef MZTEST_HEADLESS
/* testy prodlužují přeložené texty o 40 % (overlay_test_set_stretch()) */
#undef _
#define _(STRING) overlay_test_tr(STRING)
#undef C_
#define C_(CTX, STR) overlay_test_trc(CTX "\004" STR, STR)
#endif

/* ================================================================
 * Rozměry v návrhových bodech (overlay_px())
 * ================================================================ */

/** @brief Podíl šířky plochy overlaye, který list zabírá v širokém okně. */
#define SHEET_W_FRACTION 0.46f
/** @brief Nejmenší šířka listu v širokém okně. */
#define SHEET_MIN_W 420.0f
/** @brief Šířka volby mechaniky "Insert into". */
#define SHEET_PICK_W 340.0f
/** @brief Vnitřní okraj listu. */
#define SHEET_PAD 16.0f
/** @brief Odstup listu od okrajů plochy obsahu. */
#define SHEET_MARGIN 8.0f
/** @brief Výška řádku s titulkem. */
#define SHEET_TITLE_H 28.0f
/** @brief Výška pole filtru. */
#define SHEET_FILTER_H 40.0f
/** @brief Výška řádku seznamu. */
#define SHEET_ROW_H 34.0f
/** @brief Mezera mezi prvky řádku. */
#define SHEET_GAP 8.0f
/** @brief Svislá mezera mezi částmi listu. */
#define SHEET_SECTION_GAP 10.0f
/** @brief Výška (a polovina základny) ukazatele listu ke kotvě. */
#define SHEET_POINTER 8.0f
/** @brief Strana ikony v titulku listu. */
#define SHEET_TITLE_ICON 24.0f
/** @brief Krytí ztmavení plochy obsahu pod listem. */
#define SHEET_SCRIM_OPACITY 0.5f
/** @brief Největší počet řádků seznamu (číslice 1 až 6). */
#define SHEET_ROWS_MAX 6
/** @brief Největší délka filtru v bajtech včetně ukončovací nuly. */
#define SHEET_FILTER_SIZE 64
/** @brief Počet kreslení po otevření nebo změně filtru, ve kterých dostane fokus první položka listu. */
#define SHEET_FOCUS_FRAMES 2

/* ================================================================
 * Stav
 * ================================================================ */

/**
 * @brief Stav výsuvného listu.
 *
 * Invarianty: target == OVL_SHEET_NONE <=> list je zavřený (ostatní členy
 * se pak nepoužívají, filter je prázdný); drive je 0 až 3 jen při
 * OVL_SHEET_FDD, jinak -1; filter je ukončený nulou a platné UTF-8; pick
 * platí jen při OVL_SHEET_PICK_DRIVE.
 */
static struct {
    en_OVL_SHEET_TARGET target;      /**< co list nabízí; OVL_SHEET_NONE = zavřený */
    int drive;                       /**< mechanika listu OVL_SHEET_FDD */
    char filter[SHEET_FILTER_SIZE];  /**< text filtru */
    int focus_frames;                /**< zbývající kreslení, ve kterých dostane fokus první položka */
    bool take_focus;                 /**< příští kreslení má oknu listu vynutit fokus */
    st_OVERLAY_VIEW_RECENT pick;     /**< médium, pro které se volí mechanika */
    int pick_chip;                   /**< pořadí položky pruhu, ke které se volba ukotvuje */
    ImGuiID return_id;               /**< prvek, který měl fokus navigace při otevření (0 = žádný) */
    ImGuiID return_window;           /**< okno toho prvku */
    bool return_cursor;              /**< byl při otevření vidět kurzor navigace? */
    int shown_rows;                  /**< počet položek seznamu v posledním kreslení (nápověda patičky) */
    bool digits;                     /**< v posledním kreslení měly položky štítky číslic */
} s_sheet;

/** @brief Po zavření listu se má vrátit fokus prvku, který ho otevřel (provede overlay_sheet_run_pending()). */
static bool s_restore_focus = false;
/** @brief V tomto kreslení list obsluhuje klávesy (okno overlaye má fokus); nastavuje overlay_sheet_draw(). */
static bool s_keys_active = false;

/**
 * @brief Poloha kotvy z posledního kreslení tabu Media.
 *
 * Invariant: frame je číslo snímku ImGui, ve kterém byla kotva zaznamenaná; -1 = nikdy.
 */
typedef struct st_SHEET_ANCHOR {
    ImVec2 min; /**< levý horní roh */
    ImVec2 max; /**< pravý dolní roh */
    int frame;  /**< snímek záznamu */
} st_SHEET_ANCHOR;

/** @brief Kotvy tlačítek mechanik. */
static st_SHEET_ANCHOR s_anchor_fdd[OVERLAY_VIEW_FDD_COUNT] = {{ImVec2(), ImVec2(), -1}, {ImVec2(), ImVec2(), -1}, {ImVec2(), ImVec2(), -1}, {ImVec2(), ImVec2(), -1}};
/** @brief Kotva tlačítka "Change tape". */
static st_SHEET_ANCHOR s_anchor_cmt = {ImVec2(), ImVec2(), -1};
/** @brief Kotva tlačítka "Insert disk" Quick Disku. */
static st_SHEET_ANCHOR s_anchor_qd = {ImVec2(), ImVec2(), -1};
/** @brief Kotvy položek pruhu nedávných médií; položka platí jen při s_anchor_chip_valid[i]. */
static st_SHEET_ANCHOR s_anchor_chip[OVERLAY_VIEW_RECENT_MAX];
/** @brief s_anchor_chip[i] bylo někdy zaznamenáno. */
static bool s_anchor_chip_valid[OVERLAY_VIEW_RECENT_MAX];

/**
 * @brief Vyžádané vložení média (overlay_sheet_request_insert()).
 *
 * Invariant: !pending => ostatní členy se nepoužívají.
 */
static struct {
    bool pending;                /**< požadavek čeká */
    st_OVERLAY_VIEW_RECENT item; /**< médium */
    int drive;                   /**< mechanika pro obraz diskety */
} s_op;

/** @brief Počet zpráv "soubor nenalezen" pro testy (overlay_sheet_test_take_missing_reports()). */
static int s_test_missing_reports = 0;

/* ================================================================
 * Otevření a zavření
 * ================================================================ */

/**
 * @brief Zapamatuje prvek s fokusem navigace, aby mu ho zavření listu vrátilo.
 */
static void sheet_remember_focus(void)
{
    ImGuiContext *g = ImGui::GetCurrentContext();
    s_sheet.return_id = 0;
    s_sheet.return_window = 0;
    s_sheet.return_cursor = false;
    if (!g || !g->NavWindow) return;
    s_sheet.return_id = g->NavId;
    s_sheet.return_window = g->NavWindow->ID;
    s_sheet.return_cursor = g->NavCursorVisible;
}

void overlay_sheet_open(en_OVL_SHEET_TARGET target, int drive)
{
    if (target != OVL_SHEET_FDD && target != OVL_SHEET_CMT && target != OVL_SHEET_QD) return;
    if (target == OVL_SHEET_FDD && (drive < 0 || drive >= OVERLAY_VIEW_FDD_COUNT)) return;
    /* v tomto snímku už zabrala zkratka akce (Alt+Ctrl+Enter zvolí i prvek s fokusem): list se neotevírá */
    if (overlay_action_is_pending()) return;
    if (s_sheet.target == OVL_SHEET_NONE) sheet_remember_focus();
    s_sheet.target = target;
    s_sheet.drive = (target == OVL_SHEET_FDD) ? drive : -1;
    s_sheet.filter[0] = '\0';
    s_sheet.focus_frames = SHEET_FOCUS_FRAMES;
    s_sheet.take_focus = true;
    s_sheet.shown_rows = 0;
    s_sheet.digits = false;
    s_restore_focus = false;
}

void overlay_sheet_open_pick(const st_OVERLAY_VIEW_RECENT *item, int chip)
{
    if (!item || item->kind != OVERLAY_MEDIA_DSK) return;
    if (overlay_action_is_pending()) return;
    if (s_sheet.target == OVL_SHEET_NONE) sheet_remember_focus();
    s_sheet.target = OVL_SHEET_PICK_DRIVE;
    s_sheet.drive = -1;
    s_sheet.filter[0] = '\0';
    s_sheet.pick = *item;
    s_sheet.pick_chip = chip;
    s_sheet.focus_frames = SHEET_FOCUS_FRAMES;
    s_sheet.take_focus = true;
    s_sheet.shown_rows = 0;
    s_sheet.digits = false;
    s_restore_focus = false;
}

void overlay_sheet_close(void)
{
    if (s_sheet.target == OVL_SHEET_NONE) return;
    s_sheet.target = OVL_SHEET_NONE;
    s_sheet.drive = -1;
    s_sheet.filter[0] = '\0';
    s_sheet.focus_frames = 0;
    s_sheet.take_focus = false;
    s_restore_focus = true;
}

void overlay_sheet_reset(void)
{
    overlay_sheet_close();
    s_restore_focus = false;
    s_op.pending = false;
}

bool overlay_sheet_is_open(void)
{
    return s_sheet.target != OVL_SHEET_NONE;
}

en_OVL_SHEET_TARGET overlay_sheet_target(int *drive)
{
    if (drive) *drive = (s_sheet.target == OVL_SHEET_FDD) ? s_sheet.drive : -1;
    return s_sheet.target;
}

const char *overlay_sheet_test_filter(void)
{
    return s_sheet.filter;
}

int overlay_sheet_test_take_missing_reports(void)
{
    const int n = s_test_missing_reports;
    s_test_missing_reports = 0;
    return n;
}

void overlay_sheet_note_anchor(en_OVL_SHEET_ANCHOR kind, int index, ImVec2 min, ImVec2 max)
{
    st_SHEET_ANCHOR *a = NULL;
    switch (kind) {
        case OVL_SHEET_ANCHOR_FDD:
            if (index >= 0 && index < OVERLAY_VIEW_FDD_COUNT) a = &s_anchor_fdd[index];
            break;
        case OVL_SHEET_ANCHOR_CMT:
            a = &s_anchor_cmt;
            break;
        case OVL_SHEET_ANCHOR_QD:
            a = &s_anchor_qd;
            break;
        case OVL_SHEET_ANCHOR_CHIP:
            if (index >= 0 && index < OVERLAY_VIEW_RECENT_MAX) {
                a = &s_anchor_chip[index];
                s_anchor_chip_valid[index] = true;
            }
            break;
    }
    if (!a) return;
    a->min = min;
    a->max = max;
    a->frame = ImGui::GetFrameCount();
}

/**
 * @brief Kotva otevřeného listu.
 * @return Kotva; NULL = pro cíl listu nebyla nikdy zaznamenaná.
 */
static const st_SHEET_ANCHOR *sheet_anchor(void)
{
    const st_SHEET_ANCHOR *a = NULL;
    switch (s_sheet.target) {
        case OVL_SHEET_FDD:
            a = &s_anchor_fdd[s_sheet.drive];
            break;
        case OVL_SHEET_CMT:
            a = &s_anchor_cmt;
            break;
        case OVL_SHEET_QD:
            a = &s_anchor_qd;
            break;
        case OVL_SHEET_PICK_DRIVE:
            if (s_sheet.pick_chip >= 0 && s_sheet.pick_chip < OVERLAY_VIEW_RECENT_MAX && s_anchor_chip_valid[s_sheet.pick_chip]) return &s_anchor_chip[s_sheet.pick_chip];
            return NULL;
        default:
            break;
    }
    return (a && a->frame >= 0) ? a : NULL;
}

/* ================================================================
 * Dostupnost a vložení
 * ================================================================ */

/**
 * @brief Akce tabulky akcí, jejíž dostupnost rozhoduje o vložení média daného druhu.
 * @param kind  Druh média.
 * @param drive Mechanika pro obraz diskety.
 * @return Akce "vložit dialogem" téhož cíle.
 */
static en_OVL_ACTION sheet_insert_action(en_OVERLAY_MEDIA_KIND kind, int drive)
{
    if (kind == OVERLAY_MEDIA_CMT) return OVL_ACTION_CMT_CHANGE;
    if (kind == OVERLAY_MEDIA_QD) return OVL_ACTION_QD_INSERT;
    return (en_OVL_ACTION)(OVL_ACTION_FDD_INSERT_0 + ((drive >= 0 && drive < OVERLAY_VIEW_FDD_COUNT) ? drive : 0));
}

const char *overlay_sheet_insert_reason(const st_OVERLAY_VIEW *v, const st_OVERLAY_VIEW_RECENT *item)
{
    if (!v || !item) return "";
    if (item->kind == OVERLAY_MEDIA_DSK) {
        /* stačí jedna dostupná mechanika; důvod je společný (replay, odpojený řadič) */
        const char *reason = "";
        for (int d = 0; d < OVERLAY_VIEW_FDD_COUNT; d++) {
            reason = overlay_action_reason(sheet_insert_action(OVERLAY_MEDIA_DSK, d), v);
            if (!reason) return NULL;
        }
        return reason;
    }
    const char *reason = overlay_action_reason(sheet_insert_action(item->kind, 0), v);
    if (reason) return reason;
    /* druh cesty (soubor nebo adresář) platí až po prvním zjištění; do té doby se položka nezakazuje (review O6 Z3) */
    if (item->kind == OVERLAY_MEDIA_QD && item->checked && !item->missing && item->is_dir != v->qd.directory)
        return v->qd.directory ? _("The Quick Disk is set to a mapped directory.") : _("The Quick Disk is set to a disk image.");
    return NULL;
}

void overlay_sheet_request_insert(const st_OVERLAY_VIEW_RECENT *item, int drive)
{
    if (!item || s_op.pending || overlay_action_is_pending()) return;
    s_op.pending = true;
    s_op.item = *item;
    s_op.drive = drive;
}

/**
 * @brief Ohlásí zprávou text s jedním jménem souboru.
 * @param is_error true = chyba.
 * @param format   Přeložený formát s jedním `%s`.
 * @param path     Cesta dosazená za `%s`.
 */
static void sheet_message(bool is_error, const char *format, const char *path) __attribute__((format(printf, 2, 0)));
static void sheet_message(bool is_error, const char *format, const char *path)
{
    gchar *text = g_strdup_printf(format, path);
    baseui_show_message(is_error, (char *)"%s", text);
    g_free(text);
}

/**
 * @brief Vrátí fokus navigace prvku, který list otevřel.
 *
 * Kurzor navigace je potom vidět, pokud byl vidět při otevření listu nebo
 * při jeho zavření (list ovládaný klávesnicí vrací viditelný fokus, list
 * zavřený myší ne).
 *
 * @pre Uvnitř snímku ImGui, mimo Begin() / End().
 */
static void sheet_restore_focus(void)
{
    ImGuiWindow *window = s_sheet.return_window ? ImGui::FindWindowByID(s_sheet.return_window) : NULL;
    if (!window || !window->WasActive) return;
    const bool cursor = s_sheet.return_cursor || GImGui->NavCursorVisible;
    ImGui::FocusWindow(window);
    if (s_sheet.return_id) {
        ImGui::SetFocusID(s_sheet.return_id, window);
        ImGui::SetNavCursorVisible(cursor);
    }
}

void overlay_sheet_run_pending(const st_OVERLAY_VIEW *v)
{
    if (s_op.pending && v) {
        s_op.pending = false;
        const st_OVERLAY_VIEW_RECENT *item = &s_op.item;
        const en_RECENT_MEDIA_KIND kind = (item->kind == OVERLAY_MEDIA_CMT) ? RECENT_MEDIA_CMT : ((item->kind == OVERLAY_MEDIA_QD) ? RECENT_MEDIA_QD : RECENT_MEDIA_DSK);
        /* existence podle posledního známého stavu z pracovního vlákna: UI vlákno se disku neptá */
        const bool is_dir = item->is_dir;
        if (overlay_action_reason(sheet_insert_action(item->kind, s_op.drive), v)) {
            /* cíl mezitím přestal být dostupný: nic */
        } else if (!item->checked || (item->missing && g_get_monotonic_time() - (gint64)item->checked_us > OVERLAY_VIEW_RECENT_FRESH_US)) {
            /* stav souboru ještě není známý (disk neodpovídá): nevkládat naslepo. Stejně tak zastaralé "chybí"
             * (disk mohl být znovu připojen a kolo zjišťování visí jinde): nemazat, vyžádat nové zjištění (review O6 Z2) */
            overlay_view_recent_recheck();
            sheet_message(false, _("Still checking whether the file exists. Try again in a moment:\n%s"), item->path);
        } else if (item->missing) {
            /* soubor už neexistuje: z historie pryč, list zůstává otevřený */
            recent_media_remove_path(kind, item->path);
            overlay_view_recent_recheck();
            s_test_missing_reports++;
            sheet_message(false, _("The file no longer exists and was removed from the recent media:\n%s"), item->path);
        } else {
            switch (item->kind) {
                case OVERLAY_MEDIA_DSK:
#if CFG_HWEXT_HAVE_FDC
                    /* jako návrat dialogu fdc_ui_mount(): cesta delší než jméno obrazu mechaniky se nevkládá */
                    if (s_op.drive < 0 || s_op.drive >= (int)FDC_NUM_DRIVES) break;
                    if (strlen(item->path) >= sizeof(g_fdc[FDC0].drive[s_op.drive].filename)) sheet_message(false, _("The file path is too long:\n%s"), item->path);
                    else fdc_request_mount(FDC0, (unsigned)s_op.drive, item->path);
#endif
                    break;
                case OVERLAY_MEDIA_CMT:
                    /* jako návrat dialogu Open... okna Virtual CMT: dotazy na nahrávání a neuložené změny dělá automat */
                    if (!imgui_cmt_tape_open_path(item->path)) sheet_message(false, _("Another tape dialog is still open. The tape was not changed:\n%s"), item->path);
                    break;
                case OVERLAY_MEDIA_QD:
#if CFG_HWEXT_HAVE_QDISK
                    qdisk_request_mount(item->path, is_dir ? 1 : 0);
#endif
                    break;
                default:
                    break;
            }
            overlay_sheet_close();
        }
    }
    s_op.pending = false;
    if (s_restore_focus) {
        s_restore_focus = false;
        if (s_sheet.target == OVL_SHEET_NONE && ImGui::GetCurrentContext()) sheet_restore_focus();
    }
}

/* ================================================================
 * Pomocné kreslení
 * ================================================================ */

/**
 * @brief Text na jednom řádku na dané pozici, při přetečení s výpustkou a tooltipem s plným zněním.
 * @param x         Levý okraj.
 * @param cy        Svislý střed řádku textu.
 * @param font      Písmo.
 * @param design    Velikost písma v návrhových bodech.
 * @param col       Barva.
 * @param text      Text.
 * @param max_w     Největší šířka v pixelech (nejméně 1).
 * @param test_name Jméno obdélníku pro testy; NULL = nezaznamenávat.
 * @return Šířka vykresleného textu v pixelech.
 */
static float sheet_text(float x, float cy, ImFont *font, float design, ImU32 col, const char *text, float max_w, const char *test_name)
{
    const float fs = overlay_font_size(font, design);
    if (max_w < 1.0f) max_w = 1.0f;
    ImGui::SetCursorScreenPos(ImVec2(floorf(x), floorf(cy - fs * 0.5f)));
    ImGui::PushFont(font, fs);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ovl_text_ellipsis(text, max_w);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (test_name) overlay_test_note_item(test_name);
    return ImGui::GetItemRectSize().x;
}

/**
 * @brief Šířka textu daným písmem a návrhovou velikostí.
 * @param font   Písmo.
 * @param design Velikost v návrhových bodech.
 * @param text   Text.
 * @return Šířka v pixelech.
 */
static float sheet_text_w(ImFont *font, float design, const char *text)
{
    ImGui::PushFont(font, overlay_font_size(font, design));
    const float w = ImGui::CalcTextSize(text, NULL, false).x;
    ImGui::PopFont();
    return w;
}

/**
 * @brief Stáří položky slovy ("2 min ago", "yesterday", "last week").
 * @param now  Současný čas v sekundách.
 * @param time Čas vložení v sekundách; 0 = neznámý.
 * @param buf  Výstup (prázdný při neznámém čase).
 * @param size Velikost výstupu.
 */
static void sheet_age_text(long long now, long long time, char *buf, size_t size)
{
    buf[0] = '\0';
    if (time <= 0) return;
    long long age = now - time;
    if (age < 0) age = 0;
    const long long minute = 60, hour = 3600, day = 86400;
    if (age < minute) snprintf(buf, size, "%s", _("just now"));
    else if (age < hour) snprintf(buf, size, _("%d min ago"), (int)(age / minute));
    else if (age < day) snprintf(buf, size, _("%d h ago"), (int)(age / hour));
    else if (age < 2 * day) snprintf(buf, size, "%s", _("yesterday"));
    else if (age < 7 * day) snprintf(buf, size, _("%d days ago"), (int)(age / day));
    else if (age < 14 * day) snprintf(buf, size, "%s", _("last week"));
    else if (age < 60 * day) snprintf(buf, size, _("%d weeks ago"), (int)(age / (7 * day)));
    else if (age < 365 * day) snprintf(buf, size, _("%d months ago"), (int)(age / (30 * day)));
    else snprintf(buf, size, "%s", _("over a year ago"));
}

/**
 * @brief Vyhovuje jméno položky filtru (podřetězec bez ohledu na velikost písmen)?
 * @param name   Jméno souboru.
 * @param folded Filtr po g_utf8_casefold(); prázdný vyhovuje všemu.
 */
static bool sheet_filter_match(const char *name, const char *folded)
{
    if (!folded[0]) return true;
    gchar *n = g_utf8_casefold(name, -1);
    const bool match = strstr(n, folded) != NULL;
    g_free(n);
    return match;
}

/**
 * @brief Ikona média položky.
 * @param item Položka.
 */
static const char *sheet_media_icon(const st_OVERLAY_VIEW_RECENT *item)
{
    if (item->kind == OVERLAY_MEDIA_CMT) return ICON_LC_CASSETTE_TAPE;
    return item->is_dir ? ICON_LC_FOLDER : ICON_LC_SAVE;
}

/**
 * @brief Řádek seznamu: štítek klávesy, ikona, hlavní text a vedlejší text vpravo.
 *
 * Řádek je položka navigace; vybraný (fokus navigace nebo ukazatel myši) má
 * plochu OVL_COL_SELECTED a fokus navíc akcentní linku vlevo.
 *
 * @param index     Pořadí řádku (ID a jméno obdélníku pro testy "<prefix><index>").
 * @param prefix    Předpona jména obdélníku pro testy.
 * @param pos       Levý horní roh.
 * @param size      Velikost.
 * @param keycap    Text štítku klávesy; NULL = bez štítku (sloupec zůstává).
 * @param icon      Ikona nebo NULL.
 * @param text      Hlavní text.
 * @param mono      true = hlavní text neproporcionálním písmem (jméno souboru).
 * @param meta      Vedlejší text vpravo; NULL nebo prázdný = žádný.
 * @param grey      true = hlavní text barvou zakázaného prvku (chybějící soubor).
 * @param reason    Přeložený důvod, proč řádek nejde zvolit; NULL = jde.
 * @param tip       Tooltip řádku (celá cesta); NULL = žádný.
 * @param focus     true = dát řádku fokus navigace.
 * @return true = řádek byl zvolen (klik, Enter).
 */
static bool sheet_row(int index, const char *prefix, ImVec2 pos, ImVec2 size, const char *keycap, const char *icon, const char *text, bool mono, const char *meta,
                      bool grey, const char *reason, const char *tip, bool focus)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    ImDrawList *dl = window->DrawList;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    const float rounding = overlay_px(OVL_RADIUS_CONTROL);
    const float gap = floorf(overlay_px(SHEET_GAP));
    const float cy = floorf(bb.Min.y + size.y * 0.5f);
    char name[40];

    ImGui::PushID(index);
    const ImGuiID id = window->GetID("###ovl_sheet_row");
    ImGui::PopID();
    ImGui::SetCursorScreenPos(bb.Min);
    ImGui::ItemSize(bb.GetSize());
    if (reason) ImGui::BeginDisabled();
    const bool added = ImGui::ItemAdd(bb, id);
    bool hovered = false, held = false, pressed = false, focused = false;
    if (added) {
        if (focus && !reason) overlay_focus_last_item();
        pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        focused = ovl_last_item_focused() && !reason;
    }
    if (reason) ImGui::EndDisabled();
    snprintf(name, sizeof(name), "%s%d", prefix, index);
    overlay_test_note_rect(name, bb.Min, bb.Max);
    if (!added) return false;
    if (reason) {
        hovered = held = pressed = false;
        ovl_tooltip_disabled(reason);
    } else if (tip) {
        ovl_tooltip(tip);
    }

    if (focused || hovered || held) dl->AddRectFilled(bb.Min, bb.Max, held ? OVL_COL_PRESSED : OVL_COL_SELECTED, rounding);
    if (focused) dl->AddRectFilled(bb.Min, ImVec2(bb.Min.x + floorf(overlay_px(OVL_FOCUS_W)), bb.Max.y), OVL_COL_ACCENT);

    const bool dim = reason != NULL;
    float x = bb.Min.x + gap;
    /* sloupec štítku má pevnou šířku, aby texty začínaly pod sebou i bez štítků */
    const ImVec2 kc_col = ovl_keycap_size("8");
    if (keycap) {
        const ImVec2 kc = ovl_keycap_size(keycap);
        ovl_draw_keycap(dl, ImVec2(floorf(x + (kc_col.x - kc.x) * 0.5f), floorf(cy - kc.y * 0.5f)), keycap, dim);
    }
    x += kc_col.x + gap;
    const float icon_sz = floorf(overlay_px(OVL_ICON));
    if (ovl_draw_icon(dl, ImVec2(x, floorf(cy - icon_sz * 0.5f)), icon_sz, icon, (dim || grey) ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT_SECONDARY)) x += icon_sz + gap;

    float rx = bb.Max.x - gap;
    if (meta && *meta) {
        float mw = ceilf(sheet_text_w(overlay_font_ui(), OVL_FONT_HINT, meta));
        const float limit = floorf((bb.Max.x - x) * 0.5f);
        if (mw > limit) mw = limit;
        sheet_text(rx - mw, cy, overlay_font_ui(), OVL_FONT_HINT, dim ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT_SECONDARY, meta, mw, NULL);
        rx -= mw + gap;
    }
    sheet_text(x, cy, mono ? overlay_font_mono() : overlay_font_ui(), OVL_FONT_BODY, (dim || grey) ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT, text, rx - x, NULL);
    return pressed;
}

/**
 * @brief Tlačítko "Esc" v titulku: zavře list (jen pro myš, zastávkou navigace není).
 * @param right Pravý okraj tlačítka.
 * @param cy    Svislý střed.
 * @return true = tlačítko bylo zvoleno.
 */
static bool sheet_close_button(float right, float cy)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    const ImVec2 kc = ovl_keycap_size("Esc");
    const ImRect bb(ImVec2(floorf(right - kc.x), floorf(cy - kc.y * 0.5f)), ImVec2(floorf(right - kc.x) + kc.x, floorf(cy - kc.y * 0.5f) + kc.y));
    const ImGuiID id = window->GetID("###ovl_sheet_close");
    ImGui::SetCursorScreenPos(bb.Min);
    ImGui::ItemSize(bb.GetSize());
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const bool added = ImGui::ItemAdd(bb, id);
    bool hovered = false, held = false, pressed = false;
    if (added) pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    ImGui::PopItemFlag();
    overlay_test_note_rect("sheet_close", bb.Min, bb.Max);
    if (!added) return false;
    if (hovered || held) window->DrawList->AddRectFilled(bb.Min, bb.Max, held ? OVL_COL_PRESSED : OVL_COL_SELECTED, overlay_px(OVL_RADIUS_CHIP));
    ovl_draw_keycap(window->DrawList, bb.Min, "Esc", false);
    ovl_tooltip(_("Cancel"));
    return pressed;
}

/**
 * @brief Titulek listu: ikona, tučný text a tlačítko "Esc" vpravo.
 * @param icon  Ikona nebo NULL.
 * @param title Přeložený titulek.
 * @param p     Levý horní roh řádku titulku.
 * @param w     Šířka řádku.
 * @return true = tlačítko "Esc" bylo zvoleno.
 */
static bool sheet_title(const char *icon, const char *title, ImVec2 p, float w)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float h = floorf(overlay_px(SHEET_TITLE_H));
    const float cy = floorf(p.y + h * 0.5f);
    const float icon_sz = floorf(overlay_px(SHEET_TITLE_ICON));
    const float gap = floorf(overlay_px(SHEET_GAP));
    float x = p.x;
    if (ovl_draw_icon(dl, ImVec2(x, floorf(cy - icon_sz * 0.5f)), icon_sz, icon, OVL_COL_TEXT)) x += icon_sz + gap + gap * 0.5f;
    const float kc_w = ovl_keycap_size("Esc").x;
    sheet_text(x, cy, overlay_font_ui_bold(), OVL_FONT_VALUE, OVL_COL_TEXT, title, p.x + w - kc_w - gap - x, "sheet_title");
    return sheet_close_button(p.x + w, cy);
}

/* ================================================================
 * List výměny média
 * ================================================================ */

/**
 * @brief Rozměry listu v pixelech pro jedno kreslení.
 *
 * Invariant: všechny hodnoty jsou celé pixely odvozené z jednotky overlaye.
 */
typedef struct st_SHEET_METRICS {
    float pad;       /**< vnitřní okraj listu */
    float gap;       /**< svislá mezera mezi částmi */
    float title_h;   /**< výška titulku */
    float line_h;    /**< výška řádku běžného textu */
    float filter_h;  /**< výška pole filtru */
    float label_h;   /**< výška nadpisu seznamu */
    float row_h;     /**< výška řádku seznamu */
    float control_h; /**< výška tlačítek */
} st_SHEET_METRICS;

/**
 * @brief Spočítá rozměry listu.
 * @param[out] m Rozměry.
 */
static void sheet_metrics(st_SHEET_METRICS *m)
{
    m->pad = floorf(overlay_px(SHEET_PAD));
    m->gap = floorf(overlay_px(SHEET_SECTION_GAP));
    m->title_h = floorf(overlay_px(SHEET_TITLE_H));
    m->line_h = overlay_font_px(OVL_FONT_BODY);
    m->filter_h = floorf(overlay_px(SHEET_FILTER_H));
    m->label_h = overlay_font_px(OVL_FONT_CARD_TITLE);
    m->row_h = floorf(overlay_px(SHEET_ROW_H));
    m->control_h = floorf(overlay_px(OVL_CONTROL_H));
}

/**
 * @brief Výška listu výměny média bez řádků seznamu.
 * @param m Rozměry.
 * @return Výška v pixelech.
 */
static float sheet_list_fixed_h(const st_SHEET_METRICS *m)
{
    /* okraj, titulek, "Now:", filtr, nadpis seznamu, [řádky], oddělovač, tlačítka, okraj */
    return 2.0f * m->pad + m->title_h + floorf(m->gap * 0.4f) + m->line_h + m->gap + m->filter_h + m->gap + m->label_h + floorf(m->gap * 0.6f) + m->gap
           + overlay_border_px() + m->gap + m->control_h;
}

/**
 * @brief Druh média, který list výměny nabízí.
 */
static en_OVERLAY_MEDIA_KIND sheet_kind(void)
{
    if (s_sheet.target == OVL_SHEET_CMT) return OVERLAY_MEDIA_CMT;
    if (s_sheet.target == OVL_SHEET_QD) return OVERLAY_MEDIA_QD;
    return OVERLAY_MEDIA_DSK;
}

/**
 * @brief Pole filtru: lupa a text filtru, nebo vedlejší barvou nápověda.
 *
 * Není to vstupní widget (znaky čte sheet_handle_text()); pole jen ukazuje
 * stav. Neprázdný filtr má akcentní okraj a značku konce textu.
 *
 * @param p           Levý horní roh.
 * @param w           Šířka.
 * @param h           Výška.
 * @param placeholder Přeložená nápověda prázdného pole.
 */
static void sheet_filter_field(ImVec2 p, float w, float h, const char *placeholder)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImRect bb(p, ImVec2(p.x + w, p.y + h));
    const float rounding = overlay_px(OVL_RADIUS_CONTROL);
    const float gap = floorf(overlay_px(SHEET_GAP));
    const bool has_text = s_sheet.filter[0] != '\0';
    ImGui::SetCursorScreenPos(bb.Min);
    ImGui::Dummy(bb.GetSize());
    overlay_test_note_rect("sheet_filter", bb.Min, bb.Max);
    dl->AddRectFilled(bb.Min, bb.Max, OVL_COL_RAISED, rounding);
    if (has_text) dl->AddRect(bb.Min, bb.Max, OVL_COL_ACCENT, rounding, 0, overlay_px(OVL_FOCUS_W));
    else dl->AddRect(bb.Min, bb.Max, OVL_COL_BORDER_CONTROL, rounding, 0, overlay_border_px());

    const float cy = floorf(bb.Min.y + h * 0.5f);
    const float icon_sz = floorf(overlay_px(OVL_ICON));
    float x = bb.Min.x + floorf(overlay_px(12.0f));
    if (ovl_draw_icon(dl, ImVec2(x, floorf(cy - icon_sz * 0.5f)), icon_sz, ICON_LC_SEARCH, OVL_COL_TEXT_SECONDARY)) x += icon_sz + gap;
    const float max_w = bb.Max.x - floorf(overlay_px(12.0f)) - x;
    if (has_text) {
        const float tw = sheet_text(x, cy, overlay_font_ui(), OVL_FONT_BODY, OVL_COL_TEXT, s_sheet.filter, max_w, "sheet_filter_text");
        /* značka konce textu */
        const float fs = overlay_font_px(OVL_FONT_BODY);
        const float cx = floorf(x + tw + overlay_px(2.0f));
        if (cx < bb.Max.x - gap) dl->AddLine(ImVec2(cx, cy - fs * 0.6f), ImVec2(cx, cy + fs * 0.6f), OVL_COL_ACCENT, overlay_border_px());
    } else {
        sheet_text(x, cy, overlay_font_ui(), OVL_FONT_BODY, OVL_COL_TEXT_SECONDARY, placeholder, max_w, NULL);
    }
}

/**
 * @brief Obsah listu výměny média.
 * @param v     Kopie stavu.
 * @param m     Rozměry.
 * @param rows  Počet řádků seznamu, které se do listu vejdou (1 až SHEET_ROWS_MAX).
 * @param[out] close Nastaví na true, když má list zavřít (jinak nemění).
 * @pre Uvnitř okna listu.
 */
static void sheet_draw_list(const st_OVERLAY_VIEW *v, const st_SHEET_METRICS *m, int rows, bool *close)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const en_OVERLAY_MEDIA_KIND kind = sheet_kind();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = floorf(ImGui::GetContentRegionAvail().x);
    const float gap = floorf(overlay_px(SHEET_GAP));
    float y = p.y;

    /* titulek, současné médium a texty podle cíle */
    char title[160], now[OVERLAY_VIEW_NAME_SIZE + 64];
    const char *icon = ICON_LC_SAVE, *placeholder, *empty_text, *current = NULL;
    /* slovo pro prázdný cíl v řádku "Now:"; mechanika má vlastní kontext kvůli rodu v překladu (posudek N09) */
    const char *now_empty = C_("overlay media", "empty");
    en_OVL_ACTION a_browse, a_eject, a_wp = OVL_ACTION_NONE;
    bool wp_value = false;
    if (s_sheet.target == OVL_SHEET_CMT) {
        icon = ICON_LC_CASSETTE_TAPE;
        snprintf(title, sizeof(title), "%s", _("Change tape"));
        placeholder = _("Type to filter recent tapes");
        empty_text = _("No recent tapes yet. Use Browse files...");
        if (v->cmt.has_tape) current = v->cmt.name[0] ? v->cmt.name : _("New tape");
        a_browse = OVL_ACTION_CMT_CHANGE;
        a_eject = OVL_ACTION_CMT_EJECT;
    } else if (s_sheet.target == OVL_SHEET_QD) {
        if (v->qd.directory) icon = ICON_LC_FOLDER;
        snprintf(title, sizeof(title), "%s", _("Insert into Quick Disk"));
        placeholder = v->qd.directory ? _("Type to filter recent directories") : _("Type to filter recent disks");
        empty_text = v->qd.directory ? _("No recent directories yet. Use Browse files...") : _("No recent disks yet. Use Browse files...");
        if (v->qd.mounted) current = v->qd.name;
        a_browse = OVL_ACTION_QD_INSERT;
        a_eject = OVL_ACTION_QD_EJECT;
        a_wp = OVL_ACTION_QD_WP;
        wp_value = v->qd.write_protected || v->qd.unicard;
    } else {
        const st_OVERLAY_VIEW_FDD *d = &v->fdd[s_sheet.drive];
        snprintf(title, sizeof(title), _("Insert into FDD %d"), s_sheet.drive);
        placeholder = _("Type to filter recent disks");
        empty_text = _("No recent disks yet. Use Browse files...");
        if (d->mounted) current = d->name;
        now_empty = C_("overlay drive", "empty");
        a_browse = (en_OVL_ACTION)(OVL_ACTION_FDD_INSERT_0 + s_sheet.drive);
        a_eject = (en_OVL_ACTION)(OVL_ACTION_FDD_EJECT_0 + s_sheet.drive);
        a_wp = (en_OVL_ACTION)(OVL_ACTION_FDD_WP_0 + s_sheet.drive);
        /* jako klasické menu: soubor chráněný na disku a obraz sdílený s jinou mechanikou ukazují ochranu zapnutou */
        wp_value = overlay_fdd_wp_on(d);
    }

    if (sheet_title(icon, title, ImVec2(p.x, y), w)) *close = true;
    y += m->title_h + floorf(m->gap * 0.4f);
    snprintf(now, sizeof(now), _("Now: %s"), current ? current : now_empty);
    sheet_text(p.x, y + m->line_h * 0.5f, overlay_font_ui(), OVL_FONT_BODY, OVL_COL_TEXT_SECONDARY, now, w, "sheet_now");
    y += m->line_h + m->gap;

    sheet_filter_field(ImVec2(p.x, y), w, m->filter_h, placeholder);
    y += m->filter_h + m->gap;

    /* nadpis seznamu: verzálky, prostrkaně, jako nadpis karty */
    {
        gchar *upper = g_utf8_strup(_("Recent"), -1);
        ImFont *font = overlay_font_ui_bold();
        const float fs = overlay_font_px(OVL_FONT_CARD_TITLE);
        float x = p.x;
        for (const char *s = upper; *s;) {
            unsigned int c = 0;
            const int bytes = ImTextCharFromUtf8(&c, s, NULL);
            if (bytes <= 0) break;
            dl->AddText(font, fs, ImVec2(x, y), OVL_COL_TEXT_SECONDARY, s, s + bytes);
            x += font->CalcTextSizeA(fs, FLT_MAX, 0.0f, s, s + bytes).x + fs * 0.08f;
            s += bytes;
        }
        g_free(upper);
        overlay_test_note_rect("sheet_label", ImVec2(p.x, y), ImVec2(x, y + fs));
    }
    y += m->label_h + floorf(m->gap * 0.6f);

    /* položky: nedávná média téhož druhu, která vyhovují filtru */
    gchar *folded = g_utf8_casefold(s_sheet.filter, -1);
    const bool digits = s_sheet.filter[0] == '\0';
    const bool focus_first = s_sheet.focus_frames > 0;
    /* list je otevřený jen u dostupného cíle (overlay_sheet_draw()); důvod se přesto čte pro každé kreslení */
    const char *row_reason = overlay_action_reason(sheet_insert_action(kind, s_sheet.drive), v);
    int shown = 0, of_kind = 0;
    for (unsigned i = 0; i < v->recent_count && shown < rows; i++) {
        const st_OVERLAY_VIEW_RECENT *r = &v->recent[i];
        if (r->kind != kind) continue;
        /* virtuální Quick Disk bere adresáře, obraz soubory; chybějící položka se ukazuje vždy */
        if (kind == OVERLAY_MEDIA_QD && !r->missing && r->is_dir != v->qd.directory) continue;
        of_kind++;
        if (!sheet_filter_match(r->name, folded)) continue;

        char meta[OVERLAY_VIEW_PATH_SIZE + 64], age[64];
        if (r->missing) {
            snprintf(meta, sizeof(meta), "%s", _("File not found"));
        } else {
            gchar *dir = g_path_get_dirname(r->path);
            sheet_age_text(v->recent_now, r->time, age, sizeof(age));
            if (age[0]) snprintf(meta, sizeof(meta), "%s \xC2\xB7 %s", dir, age);
            else snprintf(meta, sizeof(meta), "%s", dir);
            g_free(dir);
        }
        const char digit[2] = {(char)('1' + shown), '\0'};
        if (sheet_row(shown, "sheet_row", ImVec2(p.x, y + (float)shown * m->row_h), ImVec2(w, m->row_h), digits ? digit : NULL, sheet_media_icon(r), r->name, true, meta,
                      r->missing, row_reason, r->path, focus_first && shown == 0))
            overlay_sheet_request_insert(r, s_sheet.drive);
        /* číslice vkládá jen při prázdném filtru a bez modifikátorů; rozhoduje klávesa, ne znak (rozložení klávesnice) */
        const ImGuiIO &io = ImGui::GetIO();
        if (s_keys_active && digits && !row_reason && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !io.KeyShift
            && (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + shown), false) || ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad1 + shown), false)))
            overlay_sheet_request_insert(r, s_sheet.drive);
        shown++;
    }
    g_free(folded);
    s_sheet.shown_rows = shown;
    s_sheet.digits = digits && shown > 0 && !row_reason;
    if (shown == 0) {
        const char *text = (of_kind == 0) ? empty_text : _("No recent file matches the filter.");
        sheet_text(p.x + gap, y + m->row_h * 0.5f, overlay_font_ui(), OVL_FONT_BODY, OVL_COL_TEXT_SECONDARY, text, w - 2.0f * gap, "sheet_empty");
    }
    y += (float)rows * m->row_h + m->gap;

    /* oddělovač a ovládání: Browse files..., Eject, Write protect */
    dl->AddLine(ImVec2(p.x, floorf(y)), ImVec2(p.x + w, floorf(y)), OVL_COL_BORDER, overlay_border_px());
    y += overlay_border_px() + m->gap;

    const st_OVL_ACTION_DEF *def_browse = overlay_action_def(OVL_ACTION_BROWSE_FILES);
    const st_OVL_ACTION_DEF *def_eject = overlay_action_def(a_eject);
    const char *t_browse = _(def_browse->label), *t_eject = _(def_eject->label), *t_wp = _("Write protect");
    char l_browse[200], l_eject[200];
    snprintf(l_browse, sizeof(l_browse), "%s###ovl_sheet_browse", t_browse);
    snprintf(l_eject, sizeof(l_eject), "%s###ovl_sheet_eject", t_eject);
    float w_browse = ovl_button_natural_width(l_browse, def_browse->icon, NULL);
    float w_eject = ovl_button_natural_width(l_eject, def_eject->icon, NULL);
    /* ochrana proti zápisu jen tam, kde ji nabízí klasické menu (mechanika, Quick Disk) */
    const bool have_wp = a_wp != OVL_ACTION_NONE;
    const ImVec2 tg = ovl_toggle_size();
    float w_wp_label = have_wp ? ceilf(sheet_text_w(overlay_font_ui(), OVL_FONT_BODY, t_wp)) : 0.0f;
    float w_wp = have_wp ? tg.x + gap + w_wp_label : 0.0f;
    float avail = w - (have_wp ? w_wp + 2.0f * gap : 0.0f) - gap;
    if (w_browse + w_eject > avail && have_wp) {
        /* nevejde se: popisek přepínače ustoupí do tooltipu */
        w_wp_label = 0.0f;
        w_wp = tg.x;
        avail = w - w_wp - 3.0f * gap;
    }
    if (w_browse + w_eject > avail) {
        /* pořád se nevejde: tlačítka si místo rozdělí a popisky se zkrátí */
        const float half = floorf(avail * 0.5f);
        if (w_eject <= half) w_browse = avail - w_eject;
        else if (w_browse <= half) w_eject = avail - w_browse;
        else w_browse = w_eject = half;
    }

    const char *r_browse = overlay_action_reason(a_browse, v);
    ImGui::SetCursorScreenPos(ImVec2(p.x, y));
    if (r_browse) ImGui::BeginDisabled();
    const bool browse_pressed = ovl_button_ex(l_browse, def_browse->icon, NULL, OVL_BUTTON_NORMAL, ImVec2(w_browse, m->control_h));
    if (r_browse) ImGui::EndDisabled();
    overlay_test_note_item("sheet_browse");
    if (shown == 0 && focus_first && !r_browse) overlay_focus_last_item();
    if (r_browse) ovl_tooltip_disabled(r_browse);
    else if (browse_pressed) {
        /* souborový dialog se otevře nad overlayem; list už není potřeba */
        overlay_action_request(a_browse);
        *close = true;
    }

    const char *r_eject = overlay_action_reason(a_eject, v);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w_browse + gap, y));
    if (r_eject) ImGui::BeginDisabled();
    const bool eject_pressed = ovl_button_ex(l_eject, def_eject->icon, NULL, OVL_BUTTON_NORMAL, ImVec2(w_eject, m->control_h));
    if (r_eject) ImGui::EndDisabled();
    overlay_test_note_item("sheet_eject");
    if (r_eject) ovl_tooltip_disabled(r_eject);
    else if (eject_pressed) {
        overlay_action_request(a_eject);
        *close = true;
    }

    if (have_wp) {
        const char *r_wp = overlay_action_reason(a_wp, v);
        const float ty = floorf(y + (m->control_h - tg.y) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - w_wp, ty));
        if (r_wp) ImGui::BeginDisabled();
        bool value = wp_value;
        /* stav přepínače přijde z jádra; zvolení akci jen vyžádá */
        const bool toggled = ovl_toggle("###ovl_sheet_wp", &value);
        if (r_wp) ImGui::EndDisabled();
        overlay_test_note_item("sheet_wp");
        if (r_wp) ovl_tooltip_disabled(r_wp);
        else if (w_wp_label <= 0.0f) ovl_tooltip(t_wp);
        if (toggled && !r_wp) overlay_action_request(a_wp);
        if (w_wp_label > 0.0f)
            sheet_text(p.x + w - w_wp_label, y + m->control_h * 0.5f, overlay_font_ui(), OVL_FONT_BODY, r_wp ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT, t_wp, w_wp_label, "sheet_wp_label");
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, y + m->control_h));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
    ImGui::PopStyleVar();
}

/* ================================================================
 * Volba mechaniky "Insert into"
 * ================================================================ */

/**
 * @brief Výška volby mechaniky.
 * @param m Rozměry.
 * @return Výška v pixelech.
 */
static float sheet_pick_h(const st_SHEET_METRICS *m)
{
    return 2.0f * m->pad + m->title_h + floorf(m->gap * 0.4f) + m->line_h + m->gap + (float)OVERLAY_VIEW_FDD_COUNT * m->row_h;
}

/**
 * @brief Obsah volby mechaniky: titulek, jméno souboru a čtyři mechaniky (volné první).
 *
 * Klávesy 1 až 4 volí mechaniky FDD 0 až FDD 3 (stejné přiřazení jako Alt+1
 * až Alt+4), bez ohledu na pořadí řádků. Rozhoduje klávesa, ne znak, takže
 * volba nezávisí na rozložení klávesnice; znaky z fronty vstupu se tu nečtou.
 *
 * @param v     Kopie stavu.
 * @param m     Rozměry.
 * @param[out] close Nastaví na true, když se má volba zavřít (jinak nemění).
 * @pre Uvnitř okna listu.
 */
static void sheet_draw_pick(const st_OVERLAY_VIEW *v, const st_SHEET_METRICS *m, bool *close)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = floorf(ImGui::GetContentRegionAvail().x);
    float y = p.y;
    if (sheet_title(ICON_LC_SAVE, _("Insert into"), ImVec2(p.x, y), w)) *close = true;
    y += m->title_h + floorf(m->gap * 0.4f);
    sheet_text(p.x, y + m->line_h * 0.5f, overlay_font_mono(), OVL_FONT_BODY, OVL_COL_TEXT_SECONDARY, s_sheet.pick.name, w, "pick_name");
    y += m->line_h + m->gap;

    /* pořadí: nejdřív volné mechaniky, potom obsazené */
    int order[OVERLAY_VIEW_FDD_COUNT], n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int d = 0; d < OVERLAY_VIEW_FDD_COUNT; d++)
            if (v->fdd[d].present && v->fdd[d].mounted == (pass == 1)) order[n++] = d;

    const ImGuiIO &io = ImGui::GetIO();
    const bool no_mods = s_keys_active && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !io.KeyShift;
    bool focus_given = false;
    for (int i = 0; i < n; i++) {
        const int d = order[i];
        const char *reason = overlay_action_reason(sheet_insert_action(OVERLAY_MEDIA_DSK, d), v);
        char label[32], name[16];
        const char key[2] = {(char)('1' + d), '\0'};
        snprintf(label, sizeof(label), _("FDD %d"), d);
        snprintf(name, sizeof(name), "pick_fdd");
        const bool focus = s_sheet.focus_frames > 0 && !focus_given && !reason;
        if (focus) focus_given = true;
        bool chosen = sheet_row(d, name, ImVec2(p.x, y + (float)i * m->row_h), ImVec2(w, m->row_h), reason ? NULL : key, NULL, label, false,
                                v->fdd[d].mounted ? v->fdd[d].name : C_("overlay drive", "Empty"), false, reason, NULL, focus);
        if (!reason && no_mods && (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + d), false) || ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad1 + d), false))) chosen = true;
        if (chosen) overlay_sheet_request_insert(&s_sheet.pick, d);
    }
    s_sheet.shown_rows = n;
    ImGui::SetCursorScreenPos(ImVec2(p.x, y + (float)OVERLAY_VIEW_FDD_COUNT * m->row_h));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
    ImGui::PopStyleVar();
}

/* ================================================================
 * Klávesy a kreslení
 * ================================================================ */

/**
 * @brief Je v tomto snímku ve hře klávesa číslice listu (1 až SHEET_ROWS_MAX) bez modifikátorů?
 *
 * Klávesou číslice je ImGuiKey_1 a další v horní řadě i ImGuiKey_Keypad1 a
 * další. Počítá se klávesa držená i právě uvolněná: znak téhož stisku může
 * přijít ve snímku stisku, při opakování klávesy i ve snímku uvolnění.
 * Podmínka modifikátorů je stejná jako u vložení položky v sheet_draw_list()
 * (bez Ctrl, Alt, Super a Shift); se Shiftem klávesa nevkládá a její znak
 * se proto nezahazuje.
 *
 * @return true = znaky z fronty vstupu v tomto snímku patří klávese číslice.
 * @pre Uvnitř snímku ImGui.
 */
static bool sheet_digit_key_active(void)
{
    const ImGuiIO &io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyAlt || io.KeySuper || io.KeyShift) return false;
    for (int d = 0; d < SHEET_ROWS_MAX; d++) {
        const ImGuiKey top = (ImGuiKey)(ImGuiKey_1 + d), pad = (ImGuiKey)(ImGuiKey_Keypad1 + d);
        if (ImGui::IsKeyDown(top) || ImGui::IsKeyReleased(top) || ImGui::IsKeyDown(pad) || ImGui::IsKeyReleased(pad)) return true;
    }
    return false;
}

/**
 * @brief Přečte znaky z fronty vstupu ImGui do filtru a obslouží Backspace.
 *
 * Při prázdném filtru se znaky snímku, ve kterém je ve hře klávesa číslice
 * 1 až 6 bez modifikátorů (sheet_digit_key_active()), zahodí všechny: klávesa
 * vkládá položku (sheet_draw_list()) a znak, který vyrobila, je podle
 * rozložení klávesnice "1", "+" nebo "ě". Zahodí se i tehdy, když položka
 * s tím pořadím není. O číslici rozhoduje klávesa, ne znak; znak číslice
 * bez klávesy číslice (Shift na českém rozložení) do prázdného filtru jde.
 * Mezera se do prázdného filtru nebere. Při neprázdném filtru jde do filtru
 * každý znak včetně číslic. Řídicí znaky a znaky při drženém samotném Ctrl
 * nebo samotném Alt (zkratky) se přeskakují; Ctrl+Alt (AltGr) znaky píše.
 * Platformě se ohlásí zájem o textový vstup, aby znaky chodily i bez
 * aktivního vstupního pole (ImGuiPlatformImeData::WantTextInput).
 *
 * @return true = filtr se změnil.
 * @pre Uvnitř snímku ImGui; list výměny média je otevřený a overlay má fokus.
 */
static bool sheet_handle_text(void)
{
    ImGuiContext &g = *GImGui;
    ImGuiIO &io = g.IO;
    g.PlatformImeData.WantTextInput = true;
    g.PlatformImeData.ViewportId = ImGui::GetMainViewport()->ID;
    /* mezerník patří filtru, ne navigaci (ta by jím zvolila položku s fokusem); položku volí Enter */
    const ImGuiID owner = ImGui::GetID("###ovl_sheet_keys");
    ImGui::SetKeyOwner(ImGuiKey_Space, owner);

    bool changed = false;
    size_t len = strlen(s_sheet.filter);
    /* Ctrl nebo Alt samotný je zkratka; Ctrl+Alt je AltGr a znaky píše */
    const bool ignore_chars = io.KeyCtrl != io.KeyAlt;
    /* prázdný filtr a klávesa číslice: stisk vkládá položku, jeho znak (podle rozložení "1", "+", "ě") se zahodí */
    const bool digit_key = len == 0 && sheet_digit_key_active();
    for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
        const unsigned int c = (unsigned int)io.InputQueueCharacters[i];
        if (ignore_chars || digit_key || c < 0x20 || c == 0x7F) continue;
        if (len == 0 && c == ' ') continue;
        char utf8[8];
        const int bytes = ImTextCharToUtf8(utf8, c);
        if (bytes <= 0 || len + (size_t)bytes >= sizeof(s_sheet.filter)) continue;
        memcpy(s_sheet.filter + len, utf8, (size_t)bytes);
        len += (size_t)bytes;
        s_sheet.filter[len] = '\0';
        changed = true;
    }
    if (len > 0 && ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
        /* smazat poslední znak UTF-8 */
        do {
            len--;
        } while (len > 0 && ((unsigned char)s_sheet.filter[len] & 0xC0) == 0x80);
        s_sheet.filter[len] = '\0';
        changed = true;
    }
    return changed;
}

/**
 * @brief Umístí list: vedle kotvy, a když se tam nevejde, posunutý dovnitř plochy obsahu.
 * @param a        Kotva nebo NULL (list se vystředí).
 * @param area_min Levý horní roh plochy obsahu.
 * @param area_max Pravý dolní roh plochy obsahu.
 * @param size     Velikost listu.
 * @param below    true = umisťovat nad nebo pod kotvu (volba mechaniky), false = vpravo nebo vlevo od ní.
 * @param[out] pointer_side 0 = bez ukazatele, 1 = ukazatel na levé hraně listu, 2 = na pravé.
 * @return Levý horní roh listu.
 */
static ImVec2 sheet_place(const st_SHEET_ANCHOR *a, ImVec2 area_min, ImVec2 area_max, ImVec2 size, bool below, int *pointer_side)
{
    const float margin = floorf(overlay_px(SHEET_MARGIN));
    const float x_min = area_min.x + margin, x_max = area_max.x - margin - size.x;
    const float y_min = area_min.y + margin, y_max = area_max.y - margin - size.y;
    *pointer_side = 0;
    ImVec2 pos(floorf((area_min.x + area_max.x - size.x) * 0.5f), floorf((area_min.y + area_max.y - size.y) * 0.5f));
    if (a) {
        const float gap = floorf(overlay_px(SHEET_POINTER + 4.0f));
        const float acy = (a->min.y + a->max.y) * 0.5f;
        if (below) {
            pos.x = a->min.x;
            pos.y = a->min.y - gap - size.y;
            if (pos.y < y_min) pos.y = a->max.y + gap;
        } else {
            pos.y = acy - size.y * 0.22f;
            const float right_x = a->max.x + gap, left_x = a->min.x - gap - size.x;
            if (!overlay_is_narrow() && right_x <= x_max) {
                pos.x = right_x;
                *pointer_side = 1;
            } else if (!overlay_is_narrow() && left_x >= x_min) {
                pos.x = left_x;
                *pointer_side = 2;
            } else {
                /* vedle kotvy místo není: list u okraje, posunutý dovnitř */
                pos.x = x_max;
            }
        }
    }
    if (pos.x > x_max) pos.x = x_max;
    if (pos.x < x_min) pos.x = x_min;
    if (pos.y > y_max) pos.y = y_max;
    if (pos.y < y_min) pos.y = y_min;
    return ImVec2(floorf(pos.x), floorf(pos.y));
}

void overlay_sheet_draw(const st_OVERLAY_VIEW *v, ImVec2 area_min, ImVec2 area_max, bool overlay_focused, bool media_tab)
{
    if (s_sheet.target == OVL_SHEET_NONE) return;
    if (!v || !media_tab || area_max.x <= area_min.x || area_max.y <= area_min.y) {
        overlay_sheet_close();
        return;
    }
    /* cíl přestal být dostupný (začátek záznamu vstupů, odpojení řadiče): list nemá co nabízet */
    if (s_sheet.target == OVL_SHEET_PICK_DRIVE) {
        if (overlay_sheet_insert_reason(v, &s_sheet.pick)) {
            overlay_sheet_close();
            return;
        }
    } else if (overlay_action_reason(sheet_insert_action(sheet_kind(), s_sheet.drive), v)) {
        overlay_sheet_close();
        return;
    }

    const bool pick = s_sheet.target == OVL_SHEET_PICK_DRIVE;
    bool close = false;
    s_keys_active = overlay_focused;
    if (overlay_focused) {
        const ImGuiIO &io = ImGui::GetIO();
        if (!io.KeyCtrl && !io.KeyAlt && !io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
        if (!pick && sheet_handle_text()) s_sheet.focus_frames = SHEET_FOCUS_FRAMES;
    }

    st_SHEET_METRICS m;
    sheet_metrics(&m);
    const float margin = floorf(overlay_px(SHEET_MARGIN));
    const ImVec2 area(area_max.x - area_min.x, area_max.y - area_min.y);

    /* velikost: šířka podle okna, výška podle počtu řádků, které se vejdou */
    ImVec2 size;
    int rows = SHEET_ROWS_MAX;
    if (pick) {
        size.x = floorf(overlay_px(SHEET_PICK_W));
        size.y = sheet_pick_h(&m);
    } else {
        size.x = overlay_is_narrow() ? area.x - 2.0f * margin : floorf(area.x * SHEET_W_FRACTION);
        if (size.x < floorf(overlay_px(SHEET_MIN_W))) size.x = floorf(overlay_px(SHEET_MIN_W));
        const float fixed = sheet_list_fixed_h(&m);
        const int fit = (int)floorf((area.y - 2.0f * margin - fixed) / m.row_h);
        if (rows > fit) rows = fit;
        if (rows < 1) rows = 1;
        size.y = fixed + (float)rows * m.row_h;
    }
    if (size.x > area.x - 2.0f * margin) size.x = area.x - 2.0f * margin;
    if (size.y > area.y - 2.0f * margin) size.y = area.y - 2.0f * margin;
    size = ImVec2(floorf(size.x), floorf(size.y));

    const st_SHEET_ANCHOR *anchor = sheet_anchor();
    int pointer_side = 0;
    const ImVec2 pos = sheet_place(anchor, area_min, area_max, size, pick, &pointer_side);

    /* ztmavená plocha obsahu; klik mimo list ho zavře */
    ImGui::SetCursorScreenPos(area_min);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, overlay_color_alpha(IM_COL32(0, 0, 0, 255), SHEET_SCRIM_OPACITY));
    const bool scrim = ImGui::BeginChild("###ovl_sheet_scrim", area, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (scrim) {
        ImGuiWindow *scrim_window = ImGui::GetCurrentWindow();
        ImGui::SetCursorScreenPos(area_min);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        if (ImGui::InvisibleButton("###ovl_sheet_outside", area)) close = true;
        ImGui::PopItemFlag();

        /* měkký stín a ukazatel ke kotvě */
        ImDrawList *sdl = scrim_window->DrawList;
        const float rounding = overlay_px(OVL_RADIUS_CARD);
        for (int i = 4; i >= 1; i--) {
            const float d = overlay_px(3.0f) * (float)i;
            sdl->AddRectFilled(ImVec2(pos.x - d, pos.y - d + overlay_px(4.0f)), ImVec2(pos.x + size.x + d, pos.y + size.y + d + overlay_px(4.0f)), IM_COL32(0, 0, 0, 22),
                               rounding + d);
        }
        const float pw = floorf(overlay_px(SHEET_POINTER));
        float pointer_y = 0.0f;
        if (pointer_side != 0 && anchor) {
            pointer_y = floorf((anchor->min.y + anchor->max.y) * 0.5f);
            const float lo = pos.y + rounding + pw, hi = pos.y + size.y - rounding - pw;
            if (pointer_y < lo) pointer_y = lo;
            if (pointer_y > hi) pointer_y = hi;
            const float bx = (pointer_side == 1) ? pos.x : pos.x + size.x - 1.0f;
            const float tx = (pointer_side == 1) ? bx - pw : bx + pw;
            sdl->AddTriangleFilled(ImVec2(bx, pointer_y - pw), ImVec2(tx, pointer_y), ImVec2(bx, pointer_y + pw), OVL_COL_CARD);
            sdl->AddLine(ImVec2(bx, pointer_y - pw), ImVec2(tx, pointer_y), OVL_COL_BORDER_STRONG, overlay_border_px());
            sdl->AddLine(ImVec2(tx, pointer_y), ImVec2(bx, pointer_y + pw), OVL_COL_BORDER_STRONG, overlay_border_px());
        }

        ImGui::SetCursorScreenPos(pos);
        if (s_sheet.take_focus) {
            ImGui::SetNextWindowFocus();
            s_sheet.take_focus = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, overlay_border_px());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m.pad, m.pad));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, OVL_COL_CARD);
        ImGui::PushStyleColor(ImGuiCol_Border, OVL_COL_BORDER_STRONG);
        const bool visible = ImGui::BeginChild("###ovl_sheet", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        ImGuiWindow *sheet_window = ImGui::GetCurrentWindow();
        if (visible) {
            if (pointer_side != 0) {
                /* základna ukazatele splývá s listem: přes okraj listu plocha karty */
                const float bx = (pointer_side == 1) ? pos.x : pos.x + size.x - overlay_border_px();
                sheet_window->DrawList->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
                sheet_window->DrawList->AddRectFilled(ImVec2(bx, pointer_y - pw + 1.0f), ImVec2(bx + overlay_border_px(), pointer_y + pw - 1.0f), OVL_COL_CARD);
                sheet_window->DrawList->PopClipRect();
            }
            if (pick) sheet_draw_pick(v, &m, &close);
            else sheet_draw_list(v, &m, rows, &close);
        }
        ImGui::EndChild();
        overlay_test_note_item(pick ? "pick" : "sheet");
        /* fokus overlaye patří listu: po návratu z dialogu nebo kliknutí jinam si ho list vezme zpět */
        if (overlay_focused && s_sheet.target != OVL_SHEET_NONE) {
            const ImGuiWindow *nav = GImGui->NavWindow;
            if (nav != sheet_window) s_sheet.take_focus = true;
        }
    }
    ImGui::EndChild();
    if (s_sheet.focus_frames > 0) s_sheet.focus_frames--;
    if (close) overlay_sheet_close();
}

int overlay_sheet_footer_hints(st_OVERLAY_HINT *hints)
{
    if (!hints || s_sheet.target == OVL_SHEET_NONE) return 0;
    static char last[2];
    int n = 0;
    if (s_sheet.target == OVL_SHEET_PICK_DRIVE) {
        hints[n++] = {"1", C_("overlay footer", "to"), "4", _("Insert into drive")};
        hints[n++] = {"Enter", NULL, NULL, _("Insert into selected")};
    } else {
        if (s_sheet.digits) {
            last[0] = (char)('0' + s_sheet.shown_rows);
            last[1] = '\0';
            if (s_sheet.shown_rows > 1) hints[n++] = {"1", C_("overlay footer", "to"), last, _("Insert")};
            else hints[n++] = {"1", NULL, NULL, _("Insert")};
        }
        if (s_sheet.shown_rows > 0) hints[n++] = {"Enter", NULL, NULL, _("Insert selected")};
        if (s_sheet.filter[0]) hints[n++] = {"Backspace", NULL, NULL, _("Delete")};
    }
    hints[n++] = {"Esc", NULL, NULL, _("Cancel")};
    return n;
}
