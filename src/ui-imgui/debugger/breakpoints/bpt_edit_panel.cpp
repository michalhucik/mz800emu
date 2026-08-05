/*
 * bpt_edit_panel.cpp - Nemodální dockable panel pro editaci BP/Group
 *
 * Layout:
 *  - Event: 2 sloupce (Properties left, Code Preview right)
 *  - Group: jednoduchý vertikální layout (Properties only, žádný Code Preview)
 *
 * Single-instance model + dirty tracking + confirm dialogs:
 *  - Otevření jiného BP s dirty = true → "Discard changes?"
 *  - Zavření panelu s dirty = true → "Discard changes?"
 *  - Apply → BPT_ITEM_EVENT: 1 sync dbgapi CMD_BP_UPDATE / _CREATE_WITH_INIT
 *           BPT_ITEM_GROUP: 1 sync dbgapi CMD_BPGRP_UPDATE / _ADD + UPDATE
 *  - Cancel → zahodí working copy, panel zůstává
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include "emulator/mzarch/mzhal.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "bpt_edit_panel.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "ui-imgui/auto_layout.h"
/* dbg_disassembled.h byl pro Code Preview disasm preview - po odstranění
 * Code Preview (2026-05-06) už není potřeba. */

#include <glib.h>

extern "C" {
#include "emulator/debugger/breakpoints.h"
#include "emulator/debugger/bp_event.h"
#include "emulator/debugger/bp_zone.h"
#include "emulator/debugger/bp_expr.h"
#include "emulator/debugger/bp_action.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"  /* V1.7+ BP CRUD via dbgapi */
}

/* External: globální stav UI okna breakpointů (definovaný v bpt_window.cpp) */
extern BptUIState g_bpt_ui;


/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/**
 * Parsuje hex string (max 4 znaky, case-insensitive) na uint16_t.
 * Vrací false pokud string není validní hex.
 */
static bool parse_hex16(const char *s, uint16_t *out)
{
    if (!s || !*s) return false;
    uint32_t val = 0;
    int digits = 0;
    for (const char *p = s; *p; p++)
    {
        char c = *p;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return false;
        val = (val << 4) | d;
        digits++;
        if (digits > 4) return false;
    }
    *out = (uint16_t)val;
    return true;
}


/**
 * Test zda jméno není prázdné nebo jen mezery.
 */
static bool is_name_empty(const char *s)
{
    if (!s) return true;
    for (const char *p = s; *p; p++)
        if (*p != ' ' && *p != '\t') return false;
    return true;
}


/**
 * Konverze RGB float[3] (0..1) na uint32_t 0xRRGGBB.
 */
static uint32_t rgb_to_u32(const float c[3])
{
    return ((uint32_t)(c[0] * 255.0f) << 16) |
           ((uint32_t)(c[1] * 255.0f) << 8) |
           (uint32_t)(c[2] * 255.0f);
}


/**
 * Konverze uint32_t 0xRRGGBB na RGB float[3].
 */
static void u32_to_rgb(uint32_t v, float c[3])
{
    c[0] = ((v >> 16) & 0xFF) / 255.0f;
    c[1] = ((v >> 8) & 0xFF) / 255.0f;
    c[2] = (v & 0xFF) / 255.0f;
}


/* -------------------------------------------------------------------------
 * V1.5 UX helpery: tooltip marker, action mode detect/generate
 * ------------------------------------------------------------------------- */

/**
 * Vykreslí inline "(?)" marker s tooltip textem.
 * Volá se SameLine - sám zařadí ImGui::SameLine() před textem.
 *
 * @param desc Popis pro tooltip (víceřádkový OK; wrapping na 35 fontů).
 */
static void help_marker(const char *desc)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}


/**
 * Detekuje BptActionMode z action stringu.
 *
 * STOP      = NULL nebo prázdný řetězec
 * LOG_ONLY  = single-line `log "..."; continue` pattern (white-space tolerant)
 * CUSTOM    = vše ostatní (multiline skript, jiné akce, ...)
 */
static BptActionMode detect_action_mode(const char *action)
{
    if (!action) return BPT_ACTION_MODE_STOP;
    const char *p = action;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p) return BPT_ACTION_MODE_STOP;

    /* Pattern: log "<msg>"; continue [trailing ws] */
    if (strncmp(p, "log", 3) != 0) return BPT_ACTION_MODE_CUSTOM;
    p += 3;
    if (*p != ' ' && *p != '\t') return BPT_ACTION_MODE_CUSTOM;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return BPT_ACTION_MODE_CUSTOM;
    p++;
    /* Skip do uzavřené uvozovky (s podporou \" escape) */
    while (*p && *p != '"')
    {
        if (*p == '\\' && p[1]) p += 2;
        else p++;
    }
    if (*p != '"') return BPT_ACTION_MODE_CUSTOM;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ';') return BPT_ACTION_MODE_CUSTOM;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "continue", 8) != 0) return BPT_ACTION_MODE_CUSTOM;
    p += 8;
    /* Trailing white-space + optional ';' */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';') p++;
    if (*p) return BPT_ACTION_MODE_CUSTOM;

    return BPT_ACTION_MODE_LOG_ONLY;
}


/**
 * Vygeneruje auto-log action string pro BPT_ACTION_MODE_LOG_ONLY.
 *
 * Tvar: `log "BP <name>"; continue`. Pokud je name prázdné, použije "?".
 */
static void generate_log_action(char *buf, size_t buflen, const char *bp_name)
{
    snprintf(buf, buflen, "log \"BP %s\"; continue",
             (bp_name && *bp_name) ? bp_name : "?");
}


/**
 * Vrátí krátký anglický popis BP typu (pro Type tooltip).
 * Texty v _() jsou anglické zdroje pro pozdější lokalizaci přes gettext.
 */
static const char *get_type_help(en_BPT_TYPE t)
{
    switch (t)
    {
    case BPT_TYPE_PC_EXEC:
        return _("PC execution - trigger when Z80 PC reaches the address.");
    case BPT_TYPE_MEM_R:
        return _("Memory read - trigger on read from address or address range.");
    case BPT_TYPE_MEM_W:
        return _("Memory write - trigger on write to address or address range.");
    case BPT_TYPE_IORQ_R:
        return _("I/O read - trigger on IN instruction targeting the port.");
    case BPT_TYPE_IORQ_W:
        return _("I/O write - trigger on OUT instruction targeting the port.");
    case BPT_TYPE_IRQ:
        return _("IRQ - trigger on CPU acknowledged INT (post-dispatch). "
                 "Filter by IM mode (0/1/2), IM 0 RST opcode, IM 2 vector / ISR.");
    case BPT_TYPE_IRQ_SIG:
        return _("IRQ signal - trigger on peripheral INT line raise (pre-dispatch). "
                 "Filter by source: PIOZ80 PORT_A/B, CTC2, FDC, Other.");
    case BPT_TYPE_HW_EVENT:
        return _("HW event - trigger on named hardware event (vsync, raster, ctc:zc0, ...).");
    case BPT_TYPE_SP_THRESHOLD:
        return _("SP threshold - trigger when stack pointer crosses the threshold.");
    case BPT_TYPE_GLOBAL:
        return _("Global - no address; fires when expression condition is true (per-instruction probe).");
    default:
        return _("(unknown type)");
    }
}


/**
 * Vrátí krátký anglický popis HW eventu (pro Event tooltip).
 */
/* Buffer pro tooltip s naformatovanym dynamickym range pro raster event.
 * Lifetime statickeho bufferu je cely program (zapise se max 1x). */
static char s_raster_event_help[ 128 ] = {0};

static const char *get_event_help(en_BP_EVENT e)
{
    switch (e)
    {
    case BP_EVENT_NONE:
        return _("(no event selected)");
    case BP_EVENT_GDG_VSYNC:
        return _("GDG vertical sync - start of new frame (raster row 0).");
    case BP_EVENT_GDG_RASTER:
        /* Range zalezi na platforme - 0..311 pro PAL, 0..261 pro NTSC.
         * Format jednorazove pri prvnim volani (static buffer pro vraceny ptr). */
        if ( s_raster_event_help[0] == '\0' ) {
            snprintf ( s_raster_event_help, sizeof ( s_raster_event_help ),
                       _("GDG raster line - parametrized N (0..%u); trigger at start of given scanline."),
                       (unsigned)( g_mzhal.video_screen_height - 1 ) );
        }
        return s_raster_event_help;
    default:
        return bp_event_to_string(e);
    }
}


/**
 * Vrátí true pokud daný HW event přijímá numerický parametr (raster:N).
 * Ostatní eventy (vsync, ctc:zc0, ...) mají Param disabled.
 */
static bool event_takes_param(en_BP_EVENT e)
{
    return bp_event_get_kind(e) == BP_EVT_KIND_POINT_PARAM;
}


/**
 * Vrátí true pokud daný event je SIGNAL kind (= má trigger condition).
 * Pro CHANGE/POINT eventy je trigger dropdown skrytý.
 */
static bool event_has_trigger(en_BP_EVENT e)
{
    return bp_event_get_kind(e) == BP_EVT_KIND_SIGNAL;
}


/**
 * Lokalizovaný anglický label pro trigger condition (UI dropdown).
 */
static const char *trigger_label(en_BP_EVENT_TRIGGER t)
{
    switch (t)
    {
    case BP_EVT_TRIG_LOW:     return _("Low (0)");
    case BP_EVT_TRIG_HIGH:    return _("High (1)");
    case BP_EVT_TRIG_RISING:  return _("Rising");
    case BP_EVT_TRIG_FALLING: return _("Falling");
    case BP_EVT_TRIG_CHANGED: return _("Changed");
    default:                  return _("(invalid)");
    }
}


/**
 * Custom render šipky / textu pro trigger dropdown item.
 *
 * Volat v BeginCombo iteraci PŘED Selectable. Vykreslí 16-pixel widget
 * vlevo (trojúhelník nahoru/dolů/dvojtrojúhelník) podle trigger hodnoty;
 * pro LOW/HIGH vykreslí "0"/"1" text. Po vyrenderování posune cursor
 * o 16px doprava (= následuje Selectable label).
 *
 * Layout: hybrid Selectable + custom drawlist marker. Marker = vizuální
 * "icon", label za markerem = text z trigger_label().
 */
static void draw_trigger_arrow(en_BP_EVENT_TRIGGER t)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    float cx = p.x + 8.0f;
    float cy = p.y + h * 0.5f;
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    float r = h * 0.35f;
    float r2 = r * 0.6f;

    switch (t)
    {
    case BP_EVT_TRIG_RISING:
        dl->AddTriangleFilled(
            ImVec2(cx, cy - r),
            ImVec2(cx - r, cy + r),
            ImVec2(cx + r, cy + r),
            col);
        break;
    case BP_EVT_TRIG_FALLING:
        dl->AddTriangleFilled(
            ImVec2(cx, cy + r),
            ImVec2(cx - r, cy - r),
            ImVec2(cx + r, cy - r),
            col);
        break;
    case BP_EVT_TRIG_CHANGED:
        /* Dvojtrojúhelník: nahoru malý + dolů malý (= "↕"). */
        dl->AddTriangleFilled(
            ImVec2(cx, cy - r),
            ImVec2(cx - r2, cy),
            ImVec2(cx + r2, cy),
            col);
        dl->AddTriangleFilled(
            ImVec2(cx, cy + r),
            ImVec2(cx - r2, cy),
            ImVec2(cx + r2, cy),
            col);
        break;
    case BP_EVT_TRIG_LOW:
    {
        /* "0" text vlevo, centrovaný v 16px slotu. */
        ImVec2 sz = ImGui::CalcTextSize("0");
        dl->AddText(ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), col, "0");
        break;
    }
    case BP_EVT_TRIG_HIGH:
    {
        ImVec2 sz = ImGui::CalcTextSize("1");
        dl->AddText(ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), col, "1");
        break;
    }
    default:
        break;
    }
    /* Posun cursor o 16px (následuje text label po SameLine). */
    ImGui::Dummy(ImVec2(16.0f, h));
    ImGui::SameLine();
}


/**
 * Lokalizovaný anglický nadpis pro Event kind sekci v dropdown.
 */
static const char *event_kind_section_label(en_BP_EVENT_KIND k)
{
    switch (k)
    {
    case BP_EVT_KIND_SIGNAL:        return _("Signal (with trigger)");
    case BP_EVT_KIND_CHANGE:        return _("Change");
    case BP_EVT_KIND_POINT_PARAM:   return _("Point (parametrized)");
    case BP_EVT_KIND_POINT_NOPARAM: return _("CPU / Point");
    default:                        return _("Other");
    }
}


/* -------------------------------------------------------------------------
 * Working copy lifecycle
 * ------------------------------------------------------------------------- */

/**
 * Inicializuje working copy z g_breakpoints podle type + edit_id.
 * Pokud edit_id == -1, naplní defaultem "new" položky.
 */
static void working_copy_init(BptItemType type, int edit_id)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;

    if (type == BPT_ITEM_GROUP)
    {
        if (edit_id >= 0)
        {
            st_BPTGROUP *grp = breakpoints_group_find_by_id(edit_id);
            if (grp)
            {
                ep->wc_enabled = grp->enabled;
                snprintf(ep->wc_name, sizeof(ep->wc_name), "%s", grp->name ? grp->name : "");
                u32_to_rgb(grp->fg_rgb, ep->wc_fg_color);
                u32_to_rgb(grp->bg_rgb, ep->wc_bg_color);
            }
        }
        else
        {
            ep->wc_enabled = true;
            ep->wc_name[0] = '\0';
            ep->wc_fg_color[0] = ep->wc_fg_color[1] = ep->wc_fg_color[2] = 1.0f;
            ep->wc_bg_color[0] = ep->wc_bg_color[1] = ep->wc_bg_color[2] = 0.0f;
        }
        ep->wc_addr[0] = '\0';
        ep->wc_auto_name = true;
        ep->wc_parent_group_id = -1;
    }
    else /* BPT_ITEM_EVENT */
    {
        if (edit_id >= 0)
        {
            st_BPT *bpt = breakpoints_find_by_id(edit_id);
            if (bpt)
            {
                ep->wc_enabled = bpt->enabled;
                ep->wc_auto_name = bpt->auto_name;
                snprintf(ep->wc_name, sizeof(ep->wc_name), "%s", bpt->name ? bpt->name : "");
                snprintf(ep->wc_addr, sizeof(ep->wc_addr), "%04X", bpt->addr);
                ep->wc_parent_group_id = bpt->parent;
                u32_to_rgb(bpt->fg_rgb, ep->wc_fg_color);
                u32_to_rgb(bpt->bg_rgb, ep->wc_bg_color);

                /* Smart BP V1 fields (D.6) */
                ep->wc_type = bpt->type;
                snprintf(ep->wc_addr_end, sizeof(ep->wc_addr_end),
                         "%04X", bpt->addr_end);
                ep->wc_zone = bpt->zone;
                snprintf(ep->wc_bank_id, sizeof(ep->wc_bank_id),
                         "%02X", bpt->bank_id);
                snprintf(ep->wc_port, sizeof(ep->wc_port),
                         "%04X", bpt->port);
                ep->wc_event = bpt->parsed_event;
                snprintf(ep->wc_event_param, sizeof(ep->wc_event_param),
                         "%d", (int)bpt->event_param);
                /* V1.5 HWE: trigger condition pro signal eventy. */
                ep->wc_event_trigger = bpt->event_trigger;
                snprintf(ep->wc_sp_threshold, sizeof(ep->wc_sp_threshold),
                         "%04X", bpt->sp_threshold);
                snprintf(ep->wc_expr, sizeof(ep->wc_expr), "%s",
                         bpt->expr ? bpt->expr : "");
                snprintf(ep->wc_action, sizeof(ep->wc_action), "%s",
                         bpt->action ? bpt->action : "");
                ep->wc_hit_count = bpt->hit_count;
                ep->wc_skip_count = bpt->skip_count;
                ep->wc_edge_triggered = bpt->edge_triggered;

                /* V1.5: detekce action mode podle obsahu action stringu */
                ep->wc_action_mode = detect_action_mode(bpt->action);

                /* V1.5.E: match modes */
                ep->wc_addr_match_mode = bpt->addr_match_mode;
                snprintf(ep->wc_addr_mask, sizeof(ep->wc_addr_mask),
                         "%04X", bpt->addr_mask);
                ep->wc_port_match_mode = bpt->port_match_mode;
                snprintf(ep->wc_port_end, sizeof(ep->wc_port_end),
                         "%04X", bpt->port_end);
                snprintf(ep->wc_port_mask, sizeof(ep->wc_port_mask),
                         "%04X", bpt->port_mask);
                /* V1.5.A7 - port mode (8BIT / 16BIT) */
                ep->wc_port_mode = bpt->port_mode;
                ep->wc_bank_match_mode = bpt->bank_match_mode;
                snprintf(ep->wc_bank_id_end, sizeof(ep->wc_bank_id_end),
                         "%02X", bpt->bank_id_end);
                snprintf(ep->wc_bank_id_mask, sizeof(ep->wc_bank_id_mask),
                         "%02X", bpt->bank_id_mask);
                ep->wc_bp_addr_space = bpt->bp_addr_space;
                ep->wc_sp_mode = bpt->sp_mode;
                snprintf(ep->wc_sp_upper, sizeof(ep->wc_sp_upper),
                         "%04X", bpt->sp_upper);

                /* V1.5.A8 - IRQ vector + ISR filter (BPT_TYPE_IRQ).
                 * Hex string vždy plníme (i pro disabled), aby toggle
                 * checkboxu off/on neztratil dříve zadanou hodnotu. */
                ep->wc_im2_vector_enabled = bpt->im2_vector_enabled;
                snprintf(ep->wc_im2_vector_addr,
                         sizeof(ep->wc_im2_vector_addr),
                         "%04X", bpt->im2_vector_addr);
                ep->wc_im2_isr_enabled = bpt->im2_isr_enabled;
                snprintf(ep->wc_im2_isr_addr,
                         sizeof(ep->wc_im2_isr_addr),
                         "%04X", bpt->im2_isr_addr);

                /* V1.6+ 4.4: IM2 vector/ISR match modes. */
                ep->wc_im2_vector_match_mode = bpt->im2_vector_match_mode;
                snprintf(ep->wc_im2_vector_addr_end,
                         sizeof(ep->wc_im2_vector_addr_end),
                         "%04X", bpt->im2_vector_addr_end);
                snprintf(ep->wc_im2_vector_mask,
                         sizeof(ep->wc_im2_vector_mask),
                         "%04X", bpt->im2_vector_mask);
                ep->wc_im2_isr_match_mode = bpt->im2_isr_match_mode;
                snprintf(ep->wc_im2_isr_addr_end,
                         sizeof(ep->wc_im2_isr_addr_end),
                         "%04X", bpt->im2_isr_addr_end);
                snprintf(ep->wc_im2_isr_mask,
                         sizeof(ep->wc_im2_isr_mask),
                         "%04X", bpt->im2_isr_mask);

                /* V1.5.A8.5 - IRQ IM mode discriminator + RST mask. */
                ep->wc_im0_enabled = bpt->im0_enabled;
                ep->wc_im1_enabled = bpt->im1_enabled;
                ep->wc_im2_enabled = bpt->im2_enabled;
                ep->wc_im0_rst_mask = bpt->im0_rst_mask;

                /* V1.5.A8.5 - IRQ_SIG source mask. */
                ep->wc_irq_sig_source_mask = bpt->irq_sig_source_mask;
            }
        }
        else
        {
            ep->wc_enabled = true;
            ep->wc_auto_name = true;
            ep->wc_name[0] = '\0';
            ep->wc_addr[0] = '\0';
            ep->wc_parent_group_id = -1;
            ep->wc_fg_color[0] = ep->wc_fg_color[1] = ep->wc_fg_color[2] = 1.0f;
            ep->wc_bg_color[0] = ep->wc_bg_color[1] = ep->wc_bg_color[2] = 0.0f;

            /* Smart BP V1 defaulty pro nový BP */
            ep->wc_type = BPT_TYPE_PC_EXEC;
            ep->wc_addr_end[0] = '\0';
            ep->wc_zone = BP_ZONE_CPU_VIEW;
            ep->wc_bank_id[0] = '\0';
            ep->wc_port[0] = '\0';
            ep->wc_event = BP_EVENT_NONE;
            ep->wc_event_param[0] = '\0';
            /* V1.5 HWE: default trigger RISING (= legacy fire-on-event). */
            ep->wc_event_trigger = BP_EVT_TRIG_RISING;
            ep->wc_sp_threshold[0] = '\0';
            ep->wc_expr[0] = '\0';
            ep->wc_action[0] = '\0';
            ep->wc_hit_count = 0;
            ep->wc_skip_count = 0;
            ep->wc_edge_triggered = false;

            /* V1.5: default action mode = Stop */
            ep->wc_action_mode = BPT_ACTION_MODE_STOP;

            /* V1.5.E: match mode defaults (= legacy SINGLE behavior) */
            ep->wc_addr_match_mode = BP_MATCH_SINGLE;
            snprintf(ep->wc_addr_mask, sizeof(ep->wc_addr_mask), "FFFF");
            ep->wc_port_match_mode = BP_MATCH_SINGLE;
            ep->wc_port_end[0] = '\0';
            snprintf(ep->wc_port_mask, sizeof(ep->wc_port_mask), "FFFF");
            /* V1.5.A7 - default 8BIT (= V1 chování) */
            ep->wc_port_mode = BP_PORT_8BIT;
            ep->wc_bank_match_mode = BP_MATCH_SINGLE;
            ep->wc_bank_id_end[0] = '\0';
            snprintf(ep->wc_bank_id_mask, sizeof(ep->wc_bank_id_mask), "FF");
            ep->wc_bp_addr_space = BP_ADDR_SPACE_CPU_VIEW;
            ep->wc_sp_mode = BP_SP_SINGLE;
            ep->wc_sp_upper[0] = '\0';

            /* V1.5.A8 - IRQ filter defaults: oba off, hex bufferů "0000"
             * (nikoliv prázdné) aby user okamžitě viděl validní hex po
             * zaškrtnutí - nemusel řešit empty input. */
            ep->wc_im2_vector_enabled = false;
            snprintf(ep->wc_im2_vector_addr,
                     sizeof(ep->wc_im2_vector_addr), "0000");
            ep->wc_im2_isr_enabled = false;
            snprintf(ep->wc_im2_isr_addr,
                     sizeof(ep->wc_im2_isr_addr), "0000");

            /* V1.6+ 4.4: IM2 match mode defaulty - SINGLE/0000/FFFF zachová
             * V1.5 SINGLE-only chování, mask FFFF = identical to single. */
            ep->wc_im2_vector_match_mode = BP_MATCH_SINGLE;
            snprintf(ep->wc_im2_vector_addr_end,
                     sizeof(ep->wc_im2_vector_addr_end), "0000");
            snprintf(ep->wc_im2_vector_mask,
                     sizeof(ep->wc_im2_vector_mask), "FFFF");
            ep->wc_im2_isr_match_mode = BP_MATCH_SINGLE;
            snprintf(ep->wc_im2_isr_addr_end,
                     sizeof(ep->wc_im2_isr_addr_end), "0000");
            snprintf(ep->wc_im2_isr_mask,
                     sizeof(ep->wc_im2_isr_mask), "FFFF");

            /* V1.5.A8.5 - IRQ IM mode discriminator: all-true = legacy
             * fire-on-every-IRQ (= zachová A8 chování). RST mask 0
             * = match-all RSTs. */
            ep->wc_im0_enabled = true;
            ep->wc_im1_enabled = true;
            ep->wc_im2_enabled = true;
            ep->wc_im0_rst_mask = 0;

            /* V1.5.A8.5 - IRQ_SIG default mask 0 (= invalid; user musí
             * vybrat aspoň 1 source před Apply). */
            ep->wc_irq_sig_source_mask = 0;
        }
        /* Společné resetované pole pro oba větve */
        ep->wc_expr_err[0] = '\0';
        ep->wc_action_err[0] = '\0';
        ep->wc_expr_validated = false;
        ep->wc_action_validated = false;
        ep->wc_test_expr[0] = '\0';
        ep->wc_test_result[0] = '\0';
    }

    ep->dirty = false;
    ep->initialized_for_id = true;
    ep->initialized_for_id_value = edit_id;
}


/**
 * Apply working copy do g_breakpoints. Pro nové (edit_id == -1) vytvoří
 * nový záznam.
 */
static void working_copy_apply(void)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;
    uint32_t fg = rgb_to_u32(ep->wc_fg_color);
    uint32_t bg = rgb_to_u32(ep->wc_bg_color);

    if (ep->type == BPT_ITEM_GROUP)
    {
        /* V1.7+ migrace na dbgapi BPGRP CRUD: 1 sync dbgapi CMD_BPGRP_UPDATE
         * (existing) nebo CMD_BPGRP_ADD + UPDATE (new). UPDATE_PARAM nese
         * enabled/name/colors/parent přes update_mask. */
        if (ep->edit_id >= 0)
        {
            st_DBGAPI_BPGRP_UPDATE_PARAM p;
            memset(&p, 0, sizeof(p));
            p.id = ep->edit_id;
            p.update_mask = DBGAPI_BPGRP_UM_ENABLED
                          | DBGAPI_BPGRP_UM_NAME
                          | DBGAPI_BPGRP_UM_COLORS;
            p.enabled = ep->wc_enabled;
            p.name = ep->wc_name;
            p.bg_rgb = bg;
            p.fg_rgb = fg;
            (void)dbg_ui_bpgrp_update(&p);
        }
        else
        {
            int new_id = -1;
            if (dbg_ui_bpgrp_add(ep->wc_name, -1, &new_id) && new_id >= 0)
            {
                /* Initial add nastavil name + parent; doplníme enabled + colors
                 * (= add_group nepřebírá tyto fieldy přes parametry). */
                st_DBGAPI_BPGRP_UPDATE_PARAM p;
                memset(&p, 0, sizeof(p));
                p.id = new_id;
                p.update_mask = DBGAPI_BPGRP_UM_ENABLED
                              | DBGAPI_BPGRP_UM_COLORS;
                p.enabled = ep->wc_enabled;
                p.bg_rgb = bg;
                p.fg_rgb = fg;
                (void)dbg_ui_bpgrp_update(&p);
                ep->edit_id = new_id;   /* další apply edituje již existující */
            }
        }
    }
    else /* BPT_ITEM_EVENT - V1.7+ migrace na dbgapi BP CRUD ===================
         *
         * Working copy zkopírujeme do plochého st_DBGAPI_BP_UPDATE_PARAM
         * snapshotu a odešleme jako 1 sync dbgapi call (UPDATE pro existující,
         * CREATE_WITH_INIT pro nový BP). EMU thread iteruje update_mask a
         * volá existující breakpoints_set_*() settery (= single source of
         * truth pro mutaci g_breakpoints).
         *
         * Per-type relevance je zachována přes podmíněné OR-ování bitů do
         * update_mask (= identický pattern co předchozí přímá volání s
         * podmínkami "if type == ...").
         */
    {
        /* Pro PC_EXEC / MEM_R / MEM_W typy je adresa povinná (= primární
         * identifikátor). GLOBAL / IORQ / HW_EVENT / SP_THRESHOLD / IRQ
         * používají primární adresu jen jako placeholder pro legacy bptmap
         * dispatch (= 0, enforcement bezí přes per-typ list / vlastní fieldy). */
        uint16_t addr = 0;
        bool need_addr = (ep->wc_type == BPT_TYPE_PC_EXEC ||
                           ep->wc_type == BPT_TYPE_MEM_R ||
                           ep->wc_type == BPT_TYPE_MEM_W);
        if (need_addr)
        {
            if (!parse_hex16(ep->wc_addr, &addr)) return;
        }
        else
        {
            /* Best-effort parse, ale nevyžadujeme */
            (void)parse_hex16(ep->wc_addr, &addr);
        }

        /* Build flat snapshot - vždy plníme všechna pole, update_mask
         * řídí které z nich se opravdu aplikují. */
        st_DBGAPI_BP_UPDATE_PARAM p;
        memset(&p, 0, sizeof(p));
        uint64_t mask = 0;

        /* === Identifikace === */
        p.enabled = ep->wc_enabled;
        mask |= DBGAPI_BP_UM_ENABLED;
        p.auto_name = ep->wc_auto_name;
        mask |= DBGAPI_BP_UM_AUTO_NAME;
        p.name = ep->wc_name;
        if (!ep->wc_auto_name)
            mask |= DBGAPI_BP_UM_NAME;  /* auto_name=true -> set_auto_name regeneruje, nepřepisujeme */
        p.bg_rgb = bg;
        p.fg_rgb = fg;
        mask |= DBGAPI_BP_UM_COLORS;
        p.parent = ep->wc_parent_group_id;
        mask |= DBGAPI_BP_UM_PARENT;

        /* === Smart core: type + addr === */
        p.type = (uint8_t)ep->wc_type;
        mask |= DBGAPI_BP_UM_TYPE;
        p.addr = addr;
        mask |= DBGAPI_BP_UM_ADDR;

        /* Range konec - jen pro MEM_R / MEM_W; ostatní = addr (= single point) */
        uint16_t end_addr = addr;
        if ((ep->wc_type == BPT_TYPE_MEM_R || ep->wc_type == BPT_TYPE_MEM_W) &&
            ep->wc_addr_end[0] != '\0')
        {
            uint16_t parsed_end;
            if (parse_hex16(ep->wc_addr_end, &parsed_end))
                end_addr = parsed_end;
        }
        p.addr_end = end_addr;
        mask |= DBGAPI_BP_UM_ADDR_END;

        /* Zone + bank - jen pro PC_EXEC / MEM_R / MEM_W */
        if (ep->wc_type == BPT_TYPE_PC_EXEC ||
            ep->wc_type == BPT_TYPE_MEM_R ||
            ep->wc_type == BPT_TYPE_MEM_W)
        {
            p.zone = (uint8_t)ep->wc_zone;
            mask |= DBGAPI_BP_UM_ZONE;
            if (ep->wc_zone == BP_ZONE_MMEXT_BANK && ep->wc_bank_id[0] != '\0')
            {
                uint16_t bank16;
                if (parse_hex16(ep->wc_bank_id, &bank16))
                {
                    p.bank_id = (uint8_t)(bank16 & 0xFF);
                    mask |= DBGAPI_BP_UM_BANK_ID;
                }
            }
        }

        /* Port pro IORQ */
        if (ep->wc_type == BPT_TYPE_IORQ_R || ep->wc_type == BPT_TYPE_IORQ_W)
        {
            uint16_t port;
            if (parse_hex16(ep->wc_port, &port))
            {
                p.port = port;
                mask |= DBGAPI_BP_UM_PORT;
            }
        }

        /* HW event - jen pro HW_EVENT s vybraným eventem */
        char composed[BPT_EDIT_EVENT_NAME_LEN + 16];
        if (ep->wc_type == BPT_TYPE_HW_EVENT && ep->wc_event != BP_EVENT_NONE)
        {
            const char *ename = bp_event_to_string(ep->wc_event);
            if (ep->wc_event == BP_EVENT_GDG_RASTER && ep->wc_event_param[0])
            {
                snprintf(composed, sizeof(composed), "%s:%s",
                         ename, ep->wc_event_param);
                p.event_name = composed;
            }
            else
            {
                p.event_name = ename;
            }
            mask |= DBGAPI_BP_UM_EVENT_NAME;
            /* V1.5 HWE: trigger condition (relevant jen pro signal eventy,
             * pro change/point je hodnota uložena ale enforce ji ignoruje). */
            p.event_trigger = (uint8_t)ep->wc_event_trigger;
            mask |= DBGAPI_BP_UM_EVENT_TRIGGER;
        }

        /* SP threshold */
        if (ep->wc_type == BPT_TYPE_SP_THRESHOLD)
        {
            uint16_t sp;
            if (parse_hex16(ep->wc_sp_threshold, &sp))
            {
                p.sp_threshold = sp;
                mask |= DBGAPI_BP_UM_SP_THRESHOLD;
            }
        }

        /* Condition expression - parser cache se obnoví uvnitř setteru.
         * Chyba parse byla již detekována UI (wc_expr_err) - setter ji
         * uloží jako string, ale parsed_expr cache zůstane NULL = BP
         * conservatively fires jako "true". */
        p.expr = ep->wc_expr[0] ? ep->wc_expr : NULL;
        mask |= DBGAPI_BP_UM_EXPR;

        /* Action - V1.5 přeloží UI mode na concrete string:
         *   STOP      -> NULL
         *   LOG_ONLY  -> auto-generovaný `log "BP <name>"; continue`
         *   CUSTOM    -> textbox content (= wc_action)
         *
         * Pro LOG_ONLY se action string regeneruje vždy s aktuálním jménem,
         * aby renaming BP zůstal konzistentní s log textem. */
        char auto_action[BPT_EDIT_ACTION_BUFLEN];
        if (ep->wc_action_mode == BPT_ACTION_MODE_STOP)
        {
            p.action = NULL;
        }
        else if (ep->wc_action_mode == BPT_ACTION_MODE_LOG_ONLY)
        {
            generate_log_action(auto_action, sizeof(auto_action), ep->wc_name);
            p.action = auto_action;
            /* Synchronizuj wc_action ať UI vidí finální string po Apply */
            snprintf(ep->wc_action, sizeof(ep->wc_action), "%s", auto_action);
        }
        else /* CUSTOM */
        {
            p.action = ep->wc_action[0] ? ep->wc_action : NULL;
        }
        mask |= DBGAPI_BP_UM_ACTION;

        /* Hit/skip/edge */
        p.hit_count = ep->wc_hit_count;
        mask |= DBGAPI_BP_UM_HIT_COUNT;
        p.skip_count = ep->wc_skip_count;
        mask |= DBGAPI_BP_UM_SKIP_COUNT;
        p.edge_triggered = ep->wc_edge_triggered;
        mask |= DBGAPI_BP_UM_EDGE_TRIGGERED;

        /* === V1.5.E match modes ============================================
         *
         * Aplikujeme VSECHNA match mode pole bez ohledu na aktualni typ -
         * to umoznuje zachovat hodnoty pri prepnuti typu (= user nezmaze
         * mask 0xF000 prepnutim na IORQ a zpet na PC_EXEC). Per-typ
         * relevance je rizena UI viditelnosti (= dropdown se zobrazi jen
         * pro relevantni typ).
         */
        p.addr_match_mode = (uint8_t)ep->wc_addr_match_mode;
        mask |= DBGAPI_BP_UM_ADDR_MATCH_MODE;
        {
            uint16_t addr_mask = 0xFFFF;
            if (ep->wc_addr_mask[0] != '\0')
                (void)parse_hex16(ep->wc_addr_mask, &addr_mask);
            p.addr_mask = addr_mask;
            mask |= DBGAPI_BP_UM_ADDR_MASK;
        }

        p.port_match_mode = (uint8_t)ep->wc_port_match_mode;
        mask |= DBGAPI_BP_UM_PORT_MATCH_MODE;
        {
            uint16_t port_end = 0;
            if (ep->wc_port_end[0] != '\0')
                (void)parse_hex16(ep->wc_port_end, &port_end);
            p.port_end = port_end;
            mask |= DBGAPI_BP_UM_PORT_END;
        }
        {
            uint16_t port_mask = 0xFFFF;
            if (ep->wc_port_mask[0] != '\0')
                (void)parse_hex16(ep->wc_port_mask, &port_mask);
            p.port_mask = port_mask;
            mask |= DBGAPI_BP_UM_PORT_MASK;
        }
        /* V1.5.A7 - port mode (8BIT / 16BIT). Persist plnou hodnotu i v
         * 8BIT módu (= žádné cropování při Apply); enforcement runtime
         * crop low byte. */
        p.port_mode = (uint8_t)ep->wc_port_mode;
        mask |= DBGAPI_BP_UM_PORT_MODE;

        p.bank_match_mode = (uint8_t)ep->wc_bank_match_mode;
        mask |= DBGAPI_BP_UM_BANK_MATCH_MODE;
        {
            uint16_t bend = 0;
            if (ep->wc_bank_id_end[0] != '\0')
                (void)parse_hex16(ep->wc_bank_id_end, &bend);
            p.bank_id_end = (uint8_t)(bend & 0xFF);
            mask |= DBGAPI_BP_UM_BANK_ID_END;
        }
        {
            uint16_t bmask = 0xFF;
            if (ep->wc_bank_id_mask[0] != '\0')
                (void)parse_hex16(ep->wc_bank_id_mask, &bmask);
            p.bank_id_mask = (uint8_t)(bmask & 0xFF);
            mask |= DBGAPI_BP_UM_BANK_ID_MASK;
        }
        /* Feature D: interpretace addr pole pro MMEXT_BANK. */
        p.bp_addr_space = (uint8_t)ep->wc_bp_addr_space;
        mask |= DBGAPI_BP_UM_ADDR_SPACE;

        p.sp_mode = (uint8_t)ep->wc_sp_mode;
        mask |= DBGAPI_BP_UM_SP_MODE;
        {
            uint16_t sp_up = 0;
            if (ep->wc_sp_upper[0] != '\0')
                (void)parse_hex16(ep->wc_sp_upper, &sp_up);
            p.sp_upper = sp_up;
            mask |= DBGAPI_BP_UM_SP_UPPER;
        }

        /* === V1.5.A8 - IRQ vector + ISR filter ============================
         *
         * Persist obě fieldy bez ohledu na aktualní typ - drzime user
         * hodnoty pri prepnuti typu BP (= nezmaze IM2 vector hodnotu
         * prepnutim typu na PC_EXEC a zpet). Per-typ relevance je rizena
         * UI viditelnosti (= filtery se renderuji jen v IRQ vetvi).
         *
         * Parse fail -> address 0 (= bezpecna fallback hodnota; user uz
         * dostal validation feedback v OK button gate). */
        {
            uint16_t v_addr = 0;
            if (ep->wc_im2_vector_addr[0] != '\0')
                (void)parse_hex16(ep->wc_im2_vector_addr, &v_addr);
            p.im2_vector_enabled = ep->wc_im2_vector_enabled;
            p.im2_vector_addr = v_addr;
            mask |= DBGAPI_BP_UM_IM2_VECTOR_FILTER;
            /* V1.6+ 4.4: match mode + RANGE end + MASK bitmask. */
            p.im2_vector_match_mode = (uint8_t)ep->wc_im2_vector_match_mode;
            mask |= DBGAPI_BP_UM_IM2_VECTOR_MATCH_MODE;
            uint16_t v_end = 0;
            if (ep->wc_im2_vector_addr_end[0] != '\0')
                (void)parse_hex16(ep->wc_im2_vector_addr_end, &v_end);
            p.im2_vector_addr_end = v_end;
            mask |= DBGAPI_BP_UM_IM2_VECTOR_ADDR_END;
            uint16_t v_mask = 0xFFFF;
            if (ep->wc_im2_vector_mask[0] != '\0')
                (void)parse_hex16(ep->wc_im2_vector_mask, &v_mask);
            p.im2_vector_mask = v_mask;
            mask |= DBGAPI_BP_UM_IM2_VECTOR_MASK;
        }
        {
            uint16_t i_addr = 0;
            if (ep->wc_im2_isr_addr[0] != '\0')
                (void)parse_hex16(ep->wc_im2_isr_addr, &i_addr);
            p.im2_isr_enabled = ep->wc_im2_isr_enabled;
            p.im2_isr_addr = i_addr;
            mask |= DBGAPI_BP_UM_IM2_ISR_FILTER;
            /* V1.6+ 4.4: match mode + RANGE end + MASK bitmask. */
            p.im2_isr_match_mode = (uint8_t)ep->wc_im2_isr_match_mode;
            mask |= DBGAPI_BP_UM_IM2_ISR_MATCH_MODE;
            uint16_t i_end = 0;
            if (ep->wc_im2_isr_addr_end[0] != '\0')
                (void)parse_hex16(ep->wc_im2_isr_addr_end, &i_end);
            p.im2_isr_addr_end = i_end;
            mask |= DBGAPI_BP_UM_IM2_ISR_ADDR_END;
            uint16_t i_mask = 0xFFFF;
            if (ep->wc_im2_isr_mask[0] != '\0')
                (void)parse_hex16(ep->wc_im2_isr_mask, &i_mask);
            p.im2_isr_mask = i_mask;
            mask |= DBGAPI_BP_UM_IM2_ISR_MASK;
        }

        /* === V1.5.A8.5 - IRQ IM mode discriminator + RST mask + IRQ_SIG === */
        p.im0_enabled = ep->wc_im0_enabled;
        mask |= DBGAPI_BP_UM_IM0_ENABLED;
        p.im1_enabled = ep->wc_im1_enabled;
        mask |= DBGAPI_BP_UM_IM1_ENABLED;
        p.im2_enabled = ep->wc_im2_enabled;
        mask |= DBGAPI_BP_UM_IM2_ENABLED;
        p.im0_rst_mask = ep->wc_im0_rst_mask;
        mask |= DBGAPI_BP_UM_IM0_RST_MASK;
        p.irq_sig_source_mask = ep->wc_irq_sig_source_mask;
        mask |= DBGAPI_BP_UM_IRQ_SIG_SOURCE_MASK;

        /* === Commit přes dbgapi - UPDATE pro existující, CREATE pro nový. */
        p.update_mask = mask;
        if (ep->edit_id < 0)
        {
            /* Nový BP - atomický create + init. dbg_ui_bp_create_with_init()
             * sám nastaví p.id = -1 + naplní po úspěchu. */
            int new_id = -1;
            if (!dbg_ui_bp_create_with_init(&p, &new_id)) return;
            ep->edit_id = new_id;
        }
        else
        {
            p.id = ep->edit_id;
            if (!dbg_ui_bp_update(&p)) return;
        }
    }

    ep->dirty = false;
}


/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void bpt_edit_panel_open(BptItemType type, int id)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;

    /* Pokud je panel už otevřený s dirty změnami pro JINÝ záznam, ptá se */
    if (ep->visible && ep->dirty &&
        (ep->type != type || ep->edit_id != id))
    {
        ep->show_discard_confirm = true;
        ep->pending_type = type;
        ep->pending_edit_id = id;
        return;
    }

    ep->visible = true;
    ep->type = type;
    ep->edit_id = id;
    ep->initialized_for_id = false;   /* re-init working copy v render */
}


void bpt_edit_panel_close(void)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;
    if (ep->dirty)
    {
        /* Skryt parent okno aby popup byl jasně viditelný top-level
         * (jinak ImGui kreslí parent + popup souběžně a popup se chová
         * jako dítě právě zavíraného okna -> nevykreslí se). Re-show
         * přes "Cancel" v popup. */
        ep->show_close_confirm = true;
        ep->visible = false;
        return;
    }
    ep->visible = false;
}


/**
 * Otevre Edit panel pro novy BP s pre-fill na IORQ R/W + port.
 *
 * Implementace:
 *   1) Zavola bpt_edit_panel_open(EVENT, -1) - to oznami "novy BP"
 *      a vyhodi confirm pri dirty existujicim editu.
 *   2) Pokud panel jiz neni dirty (= open prosel), spusti rucne
 *      working_copy_init pro -1 (= naplni defaulty) a pak prepise
 *      wc_type a wc_port.
 *   3) Nastavi initialized_for_id = true, aby render-time init
 *      neprepsal nase pre-fill defaulty.
 *
 * Pri dirty pendingu: helper proste zachova chovani open() (= request
 * "discard?"), pre-fill se neaplikuje. User musi volbu opakovat po
 * resolve dialogu - akceptovany trade-off pro V1.
 */
void bpt_edit_panel_open_new_iorq(uint16_t port_addr, bool is_write)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;

    /* Nejprve standardni open. Pokud existujici edit je dirty, vrati
     * pending discard a vse co dal udelame by se stejne pri potvrzeni
     * pretiselo. V tom pripade nevykreslujeme pre-fill. */
    bool was_dirty_before = (ep->visible && ep->dirty);
    bpt_edit_panel_open(BPT_ITEM_EVENT, -1);
    if (was_dirty_before && ep->show_discard_confirm)
    {
        return;
    }

    /* Inicializuj working copy "z toho mista" - jinak by se to udelalo
     * az pri prvnim render frame (lazy init). */
    working_copy_init(BPT_ITEM_EVENT, -1);

    /* Pre-fill IORQ specifikum */
    ep->wc_type = is_write ? BPT_TYPE_IORQ_W : BPT_TYPE_IORQ_R;
    snprintf(ep->wc_port, sizeof(ep->wc_port), "%04X",
             (unsigned)port_addr);
    /* Adresa neni pro IORQ povinna - nech prazdnou */
    ep->wc_addr[0] = '\0';
}


/**
 * @see bpt_edit_panel_open_new_mem (header).
 *
 * Symetricky helper pro MEM_R/MEM_W. Stejny pattern jako _new_iorq:
 * nejdriv standardni open, pak working_copy_init pro -1 (= naplni
 * defaulty), pak prepiseme wc_type + wc_addr + wc_addr_end. Adresa
 * konce default = SINGLE match (= stejna jako start), user pak v
 * panelu muze prepnout na RANGE.
 */
void bpt_edit_panel_open_new_mem(uint16_t mem_addr, bool is_write)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;

    bool was_dirty_before = (ep->visible && ep->dirty);
    bpt_edit_panel_open(BPT_ITEM_EVENT, -1);
    if (was_dirty_before && ep->show_discard_confirm)
    {
        return;
    }

    working_copy_init(BPT_ITEM_EVENT, -1);

    /* Pre-fill MEM_R/W specifikum */
    ep->wc_type = is_write ? BPT_TYPE_MEM_W : BPT_TYPE_MEM_R;
    snprintf(ep->wc_addr, sizeof(ep->wc_addr), "%04X",
             (unsigned)mem_addr);
    /* SINGLE match default (start == end). User muze rozsirit na RANGE. */
    snprintf(ep->wc_addr_end, sizeof(ep->wc_addr_end), "%04X",
             (unsigned)mem_addr);
    /* Port neni pro MEM_R/W relevantni */
    ep->wc_port[0] = '\0';
}


/* -------------------------------------------------------------------------
 * Render: Properties section (společná část)
 * ------------------------------------------------------------------------- */

/**
 * Vykreslí color picker pole (ColorButton + popup s ColorPicker3).
 * Vrátí true pokud uživatel změnil hodnotu.
 */
static bool render_color_field(const char *label, float color[3])
{
    bool changed = false;
    ImGui::Text("%s", label);
    ImGui::SameLine();
    ImVec4 col4(color[0], color[1], color[2], 1.0f);
    char popup_id[32];
    snprintf(popup_id, sizeof(popup_id), "##picker_%s", label);
    if (ImGui::ColorButton(popup_id, col4, ImGuiColorEditFlags_NoTooltip,
                           ImVec2(20, 20)))
    {
        ImGui::OpenPopup(popup_id);
    }
    if (ImGui::BeginPopup(popup_id))
    {
        if (ImGui::ColorPicker3("##picker", color,
                                ImGuiColorEditFlags_NoSidePreview))
            changed = true;
        ImGui::EndPopup();
    }
    return changed;
}


/**
 * Vykreslí Group dropdown (parent picker pro BPT events).
 * Vrátí true pokud uživatel změnil výběr.
 */
static bool render_group_dropdown(int *selected_parent_id)
{
    bool changed = false;
    /* Najdi name aktuální grupy pro preview */
    const char *current_name = _("(root)");
    if (*selected_parent_id >= 0)
    {
        st_BPTGROUP *grp = breakpoints_group_find_by_id(*selected_parent_id);
        if (grp && grp->name) current_name = grp->name;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##bpt_grp_dd", current_name))
    {
        bool is_root_selected = (*selected_parent_id < 0);
        if (ImGui::Selectable(_("(root)"), is_root_selected))
        {
            if (!is_root_selected)
            {
                *selected_parent_id = -1;
                changed = true;
            }
        }
        /* Iteruj všechny grupy přes GArray */
        for (unsigned i = 0; i < g_breakpoints.groups->len; i++)
        {
            st_BPTGROUP *grp = &g_array_index(g_breakpoints.groups, st_BPTGROUP, i);
            bool is_selected = (*selected_parent_id == grp->id);
            const char *name = grp->name ? grp->name : "(unnamed)";
            char label[160];
            snprintf(label, sizeof(label), "%s##grp_%d", name, grp->id);
            if (ImGui::Selectable(label, is_selected))
            {
                if (!is_selected)
                {
                    *selected_parent_id = grp->id;
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}


/**
 * V1.5.E - vykreslí Match Mode dropdown jako jeden rádek 3-col tabulky.
 *
 * @param label   Anglicky text labelu (musi byt v _() na callsite).
 * @param id      ImGui ID stub (napr. "##bpt_addr_mm") - pripoji se k combo.
 * @param mode    In/out match mode.
 * @param tooltip Help text pro (?) marker.
 * @return true pokud user zmenil hodnotu.
 */
static bool render_match_mode_combo(const char *label, const char *id,
                                     en_BP_MATCH_MODE *mode,
                                     const char *tooltip)
{
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::Text("%s", label);
    ImGui::TableNextColumn(); /* no 0x prefix */
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(120);
    const char *preview = bp_match_mode_to_string(*mode);
    if (ImGui::BeginCombo(id, preview))
    {
        for (int m = 0; m < (int)BP_MATCH_COUNT; m++)
        {
            bool is_sel = (*mode == (en_BP_MATCH_MODE)m);
            const char *mn = bp_match_mode_to_string((en_BP_MATCH_MODE)m);
            if (ImGui::Selectable(mn, is_sel))
            {
                if (!is_sel)
                {
                    *mode = (en_BP_MATCH_MODE)m;
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }
    help_marker(tooltip);
    return changed;
}


/**
 * V1.5.E - vykreslí SP Mode dropdown (SINGLE / WINDOW).
 */
static bool render_sp_mode_combo(const char *label, const char *id,
                                  en_BP_SP_MODE *mode,
                                  const char *tooltip)
{
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::Text("%s", label);
    ImGui::TableNextColumn(); /* no 0x prefix */
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(120);
    const char *preview = bp_sp_mode_to_string(*mode);
    if (ImGui::BeginCombo(id, preview))
    {
        for (int m = 0; m < (int)BP_SP_MODE_COUNT; m++)
        {
            bool is_sel = (*mode == (en_BP_SP_MODE)m);
            const char *mn = bp_sp_mode_to_string((en_BP_SP_MODE)m);
            if (ImGui::Selectable(mn, is_sel))
            {
                if (!is_sel)
                {
                    *mode = (en_BP_SP_MODE)m;
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }
    help_marker(tooltip);
    return changed;
}


/**
 * V1.5.A7 - vykreslí IORQ Port Mode dropdown (8BIT / 16BIT) jako řádek
 * 3-col tabulky. Hodnoty enum mapuje na lidské názvy (8-bit / 16-bit) pro
 * čitelnost; serializace do .bpt používá bp_port_mode_to_string a zůstává
 * "8BIT" / "16BIT".
 *
 * @param label   Anglicky text labelu (musí být v _() na callsite).
 * @param id      ImGui ID stub (např. "##bpt_port_pm").
 * @param mode    In/out port mode.
 * @param tooltip Help text pro (?) marker.
 * @return true pokud user změnil hodnotu.
 */
static bool render_port_mode_combo(const char *label, const char *id,
                                    en_BP_PORT_MODE *mode,
                                    const char *tooltip)
{
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::Text("%s", label);
    ImGui::TableNextColumn(); /* no 0x prefix */
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(120);
    /* Lidsky čitelné labely (preview + Selectable). Persist string je
     * "8BIT"/"16BIT" - používá ho serializace, ne UI.
     * Pořadí MUSÍ odpovídat en_BP_PORT_MODE - C99 array designators
     * ("[INDEX] = value") nejsou v C++ portable, používáme sequential. */
    static const char *names[ BP_PORT_MODE_COUNT ] = {
        "8-bit",        /* BP_PORT_8BIT */
        "16-bit",       /* BP_PORT_16BIT */
    };
    const char *preview = (*mode >= 0 && *mode < BP_PORT_MODE_COUNT)
                              ? names[ *mode ] : names[ BP_PORT_8BIT ];
    if (ImGui::BeginCombo(id, preview))
    {
        for (int m = 0; m < (int)BP_PORT_MODE_COUNT; m++)
        {
            bool is_sel = (*mode == (en_BP_PORT_MODE)m);
            if (ImGui::Selectable(names[ m ], is_sel))
            {
                if (!is_sel)
                {
                    *mode = (en_BP_PORT_MODE)m;
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }
    help_marker(tooltip);
    return changed;
}


/* -------------------------------------------------------------------------
 * Render: Event (BP) panel - V1.5 layout (per-section helpery)
 *
 * Layout pořadí (per ux-edit-bp.md):
 *   1. Type dropdown        (vždy nahoře, určuje obsah dalších sekcí)
 *   2. Adresové pole        (per-type adaptive: Address/Port/Event/SP)
 *   3. Zone + Bank          (jen mem typy: PC_EXEC, MEM_R, MEM_W)
 *   4. General properties   (Enabled, Auto Name, Name, Group, Colors, Hits)
 *   5. Trigger logic        (Condition + Edge + Hit/Skip)
 *   6. Action on trigger    (Mode radio + textbox)
 *   7. Live Eval Test       (volitelné)
 *   Pravý sloupec: Code Preview adaptivní per typ.
 * ------------------------------------------------------------------------- */


/**
 * BP Options - 3-sloupcová tabulka aby se srovnaly labely + 0x prefixy +
 * inputy:
 *   sloupec 1: label ("Type:", "Zone:", "Address:", ...)
 *   sloupec 2: hex prefix "0x" nebo prázdný (per-řádek conditionally)
 *   sloupec 3: input/combo + tooltip + případný suffix text
 *
 * Pořadí řádků (per ux-edit-bp.md + Michal feedback):
 *   1. Type (vždy)
 *   2. Zone + Bank ID (jen mem typy: PC_EXEC, MEM_R, MEM_W)
 *   3. Address + End Addr (PC_EXEC, MEM_R, MEM_W) /
 *      Port (IORQ_R, IORQ_W) /
 *      Event + Param (HW_EVENT) /
 *      SP Threshold (SP_THRESHOLD) /
 *      placeholder text (IRQ, GLOBAL)
 */
static void render_section_bp_options(BptEditPanelState *ep)
{
    if (!ImGui::BeginTable("##bp_opts_table", 3,
                            ImGuiTableFlags_SizingFixedFit))
        return;

    /* === Type row === */
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::Text("%s", _("Type:"));
    ImGui::TableNextColumn(); /* no 0x prefix */
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(180);
    const char *type_preview = bpt_type_to_string(ep->wc_type);
    if (ImGui::BeginCombo("##bpt_smart_type", type_preview))
    {
        for (int t = 0; t < (int)BPT_TYPE_COUNT; t++)
        {
            bool is_selected = (ep->wc_type == (en_BPT_TYPE)t);
            const char *name = bpt_type_to_string((en_BPT_TYPE)t);
            if (ImGui::Selectable(name, is_selected))
            {
                if (!is_selected)
                {
                    ep->wc_type = (en_BPT_TYPE)t;
                    ep->dirty = true;
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted(get_type_help((en_BPT_TYPE)t));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        ImGui::EndCombo();
    }
    help_marker(get_type_help(ep->wc_type));

    /* === Zone row (jen mem typy) === */
    bool is_mem_type = (ep->wc_type == BPT_TYPE_PC_EXEC ||
                        ep->wc_type == BPT_TYPE_MEM_R ||
                        ep->wc_type == BPT_TYPE_MEM_W);
    if (is_mem_type)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", _("Zone:"));
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(180);
        const char *zpv = bp_zone_to_string(ep->wc_zone);
        if (ImGui::BeginCombo("##bpt_zone", zpv))
        {
            for (int z = 0; z < (int)BP_ZONE_COUNT; z++)
            {
                /* PCG zóna existuje jen na MZ-1500 (= programmable
                 * character generator). MZ-700 a MZ-800 ji nemají. */
                /* PCG zona existuje jen na MZ-1500 (runtime, mzhal 11i). */
                if (g_mzhal.arch != 1500 && (en_BP_ZONE)z == BP_ZONE_PCG) continue;
                bool is_sel = (ep->wc_zone == (en_BP_ZONE)z);
                const char *zn = bp_zone_to_string((en_BP_ZONE)z);
                if (ImGui::Selectable(zn, is_sel))
                {
                    if (!is_sel)
                    {
                        ep->wc_zone = (en_BP_ZONE)z;
                        ep->dirty = true;
                    }
                }
            }
            ImGui::EndCombo();
        }
        help_marker(_("Banking-aware filter. CPU view = whatever PC sees now. "
                      "Specific zone = only when the bank is paged in."));

        if (ep->wc_zone == BP_ZONE_MMEXT_BANK)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("Bank ID (hex):"));
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(80);
            if (ImGui::InputText("##bpt_bank_id", ep->wc_bank_id,
                                  sizeof(ep->wc_bank_id),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase))
                ep->dirty = true;

            /* V1.5.E - bank Match Mode */
            if (render_match_mode_combo(_("Bank Match Mode:"), "##bpt_bank_mm",
                                         &ep->wc_bank_match_mode,
                                         _("Single = trigger on exact bank ID (default).\n"
                                           "Range  = trigger when active bank within [Bank ID..End Bank].\n"
                                           "Mask   = trigger when (bank & Mask) == (Bank ID & Mask).\n"
                                           "         Useful for matching multiple overlay banks at once.\n"
                                           "Reference: docs/cz/debugger/breakpoints/match-modes.md")))
                ep->dirty = true;

            if (ep->wc_bank_match_mode == BP_MATCH_RANGE)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", _("End Bank:"));
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputText("##bpt_bank_id_end", ep->wc_bank_id_end,
                                      sizeof(ep->wc_bank_id_end),
                                      ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_CharsUppercase))
                    ep->dirty = true;
                help_marker(_("Upper bound for RANGE match (inclusive). "
                              "Active only when Match Mode = Range."));
            }
            else if (ep->wc_bank_match_mode == BP_MATCH_MASK)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", _("Mask:"));
                ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputText("##bpt_bank_id_mask", ep->wc_bank_id_mask,
                                      sizeof(ep->wc_bank_id_mask),
                                      ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_CharsUppercase))
                    ep->dirty = true;
                help_marker(_("AND mask for MASK match. Example: Bank=0x08, "
                              "Mask=0x18 triggers on banks 8, 9, 10, 11 (= 0x08..0x0B "
                              "with high bits cleared).\n"
                              "Reference: docs/cz/debugger/breakpoints/match-modes.md"));
            }

            /* Feature D: interpretace Address pole - CPU view vs offset v bance. */
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("Address as:"));
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(120);
            const char *as_preview =
                (ep->wc_bp_addr_space == BP_ADDR_SPACE_BANK_OFFSET)
                    ? _("Bank offset") : _("CPU view");
            if (ImGui::BeginCombo("##bpt_addr_space", as_preview))
            {
                if (ImGui::Selectable(_("CPU view"),
                                      ep->wc_bp_addr_space == BP_ADDR_SPACE_CPU_VIEW)
                    && ep->wc_bp_addr_space != BP_ADDR_SPACE_CPU_VIEW)
                {
                    ep->wc_bp_addr_space = BP_ADDR_SPACE_CPU_VIEW;
                    ep->dirty = true;
                }
                if (ImGui::Selectable(_("Bank offset"),
                                      ep->wc_bp_addr_space == BP_ADDR_SPACE_BANK_OFFSET)
                    && ep->wc_bp_addr_space != BP_ADDR_SPACE_BANK_OFFSET)
                {
                    ep->wc_bp_addr_space = BP_ADDR_SPACE_BANK_OFFSET;
                    ep->dirty = true;
                }
                ImGui::EndCombo();
            }
            help_marker(_("CPU view = Address is a Z80 address (0000-FFFF).\n"
                          "Bank offset = Address is an offset within the PEHU bank "
                          "(0000-1FFF), matched wherever the bank is mapped.\n"
                          "PEHU memext only."));
        }
    }

    /* === Per-type identifier row(s) === */
    if (is_mem_type)
    {
        /* Address (feature D: v offset módu se jmenuje "Offset:"). */
        bool bank_offset_mode = (ep->wc_zone == BP_ZONE_MMEXT_BANK &&
                                 ep->wc_bp_addr_space == BP_ADDR_SPACE_BANK_OFFSET);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%s", bank_offset_mode ? _("Offset:") : _("Address:"));
        ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputText("##bpt_addr", ep->wc_addr, sizeof(ep->wc_addr),
                              ImGuiInputTextFlags_CharsHexadecimal |
                                  ImGuiInputTextFlags_CharsUppercase))
            ep->dirty = true;
        help_marker(bank_offset_mode
                    ? _("Offset within the PEHU bank (0000-1FFF).")
                    : _("CPU address for PC_EXEC, memory address for MEM_R/W. "
                        "For MEM range use End Addr below as well."));

        /* V1.5.E - addr Match Mode dropdown + adaptive End/Mask radky.
         * Plati pro PC_EXEC i MEM_R/W. End Addr (pro MEM) zustava
         * legacy textovy field, ale rendrujeme ho jen kdyz mode=RANGE
         * (= konzistence s ostatnimi typy v V1.5.E). Pro PC_EXEC End
         * Addr byl predtim skryt -> ted se zobrazi pri RANGE mode.
         */
        if (render_match_mode_combo(_("Match Mode:"), "##bpt_addr_mm",
                                     &ep->wc_addr_match_mode,
                                     _("Single = trigger only on exact value (default V1 behavior).\n"
                                       "Range  = trigger when value is within [Address..End Addr] inclusive.\n"
                                       "Mask   = trigger when (value & Mask) == (Address & Mask).\n"
                                       "         Useful for groups of addresses sharing a bit pattern.\n"
                                       "Reference: docs/cz/debugger/breakpoints/match-modes.md")))
            ep->dirty = true;

        if (ep->wc_addr_match_mode == BP_MATCH_RANGE)
        {
            /* End Addr */
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("End Addr:"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(80);
            if (ImGui::InputText("##bpt_addr_end", ep->wc_addr_end,
                                  sizeof(ep->wc_addr_end),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase))
                ep->dirty = true;
            help_marker(_("Upper bound for RANGE match (inclusive). "
                          "Active only when Match Mode = Range."));
        }
        else if (ep->wc_addr_match_mode == BP_MATCH_MASK)
        {
            /* Mask */
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("Mask:"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(80);
            if (ImGui::InputText("##bpt_addr_mask", ep->wc_addr_mask,
                                  sizeof(ep->wc_addr_mask),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase))
                ep->dirty = true;
            help_marker(_("AND mask for MASK match. Trigger when "
                          "(value & Mask) == (Address & Mask). "
                          "Example: Address=0x0042, Mask=0x00FF triggers on any "
                          "address with low byte 0x42 (0x0042, 0x0142, ...). "
                          "Default 0xFFFF = identical to Single mode. "
                          "Note: PC_EXEC MASK is V1.5.E-limited (fallback to "
                          "ref addr only) - use MEM_R/W for full mask support.\n"
                          "Reference: docs/cz/debugger/breakpoints/match-modes.md"));
        }
    }
    else if (ep->wc_type == BPT_TYPE_IORQ_R || ep->wc_type == BPT_TYPE_IORQ_W)
    {
        /* V1.5.A7 - Port Mode (8-bit / 16-bit) PŘED Port řádkem.
         * Mód řídí adaptive šířku všech port input boxů níž (Port / End
         * Port / Mask) - 60px stačí pro 8-bit (max "FF"), 90px pro 16-bit
         * (max "FFFF"). */
        if (render_port_mode_combo(_("Port Mode:"), "##bpt_port_pm",
                                    &ep->wc_port_mode,
                                    _("Port Mode:\n"
                                      "  8-bit  = IN A,(n) / OUT (n),A pattern "
                                      "(port literal 0..0xFF, B register on bus is don't care).\n"
                                      "  16-bit = IN r,(C) / OUT (C),r pattern "
                                      "(port = full BC register 0..0xFFFF, B = high, C = low).\n"
                                      "Default 8-bit covers most Z80 software. 16-bit needed for "
                                      "hardware that decodes B as part of port address.\n"
                                      "Reference: docs/cz/debugger/breakpoints/match-modes.md")))
            ep->dirty = true;

        /* Adaptive width pro všechny port hex inputy v této IORQ větvi. */
        const float port_input_w = (ep->wc_port_mode == BP_PORT_8BIT) ? 60.0f : 90.0f;

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", _("Port:"));
        ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(port_input_w);
        if (ImGui::InputText("##bpt_port", ep->wc_port, sizeof(ep->wc_port),
                              ImGuiInputTextFlags_CharsHexadecimal |
                                  ImGuiInputTextFlags_CharsUppercase))
            ep->dirty = true;
        help_marker(_("Z80 I/O port value. Width follows Port Mode "
                      "(8-bit = 0..0xFF, 16-bit = 0..0xFFFF where B = high, "
                      "C = low byte). In 8-bit mode upper byte of input is "
                      "ignored at runtime."));

        /* V1.5.E - port Match Mode */
        if (render_match_mode_combo(_("Match Mode:"), "##bpt_port_mm",
                                     &ep->wc_port_match_mode,
                                     _("Single = trigger on exact port (default).\n"
                                       "Range  = trigger when port within [Port..End Port].\n"
                                       "Mask   = trigger when (port & Mask) == (Port & Mask).\n"
                                       "         Useful for I/O port group matching (e.g. 0xCC..0xCF).\n"
                                       "Reference: docs/cz/debugger/breakpoints/match-modes.md")))
            ep->dirty = true;

        if (ep->wc_port_match_mode == BP_MATCH_RANGE)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("End Port:"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(port_input_w);
            if (ImGui::InputText("##bpt_port_end", ep->wc_port_end,
                                  sizeof(ep->wc_port_end),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase))
                ep->dirty = true;
            help_marker(_("Upper bound for RANGE match (inclusive). "
                          "Active only when Match Mode = Range."));
        }
        else if (ep->wc_port_match_mode == BP_MATCH_MASK)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("Mask:"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(port_input_w);
            if (ImGui::InputText("##bpt_port_mask", ep->wc_port_mask,
                                  sizeof(ep->wc_port_mask),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase))
                ep->dirty = true;
            help_marker(_("AND mask for MASK match. Example: Port=0xCC, "
                          "Mask=0xFC triggers on 0xCC, 0xCD, 0xCE, 0xCF.\n"
                          "Reference: docs/cz/debugger/breakpoints/match-modes.md"));
        }
    }
    else if (ep->wc_type == BPT_TYPE_HW_EVENT)
    {
        /* Event - dropdown s 28 hodnotami v 4 sekcích (Signal/Change/Point/CPU). */
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", _("Event:"));
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(220);
        const char *evt_preview =
            (ep->wc_event == BP_EVENT_NONE) ?
                _("(select event)") : bp_event_to_string(ep->wc_event);
        if (ImGui::BeginCombo("##bpt_event", evt_preview,
                               ImGuiComboFlags_HeightLargest))
        {
            bool none_selected = (ep->wc_event == BP_EVENT_NONE);
            if (ImGui::Selectable(_("(none)"), none_selected))
            {
                if (!none_selected)
                {
                    ep->wc_event = BP_EVENT_NONE;
                    ep->dirty = true;
                }
            }
            /* Grouping per kind: vykreslime postupne SIGNAL, CHANGE,
             * POINT_PARAM, POINT_NOPARAM s SeparatorText nadpisem. */
            const en_BP_EVENT_KIND order[] = {
                BP_EVT_KIND_SIGNAL,
                BP_EVT_KIND_CHANGE,
                BP_EVT_KIND_POINT_PARAM,
                BP_EVT_KIND_POINT_NOPARAM
            };
            for (size_t k = 0; k < sizeof(order)/sizeof(order[0]); k++)
            {
                en_BP_EVENT_KIND target_kind = order[k];
                bool section_emitted = false;
                for (int e = 1; e < (int)BP_EVENT_COUNT; e++)
                {
                    en_BP_EVENT ev = (en_BP_EVENT)e;
                    if (bp_event_get_kind(ev) != target_kind) continue;
                    /* Per-arch filter: skry eventy ktere na aktualni MZARCH
                     * platforme nemaji smysl (MZ-700 nema PIOZ80, MZ-700 a
                     * MZ-1500 nemaji full GDG change eventy). */
                    if (!bp_event_is_supported_on_current_arch(ev)) continue;
                    if (!section_emitted)
                    {
                        ImGui::SeparatorText(event_kind_section_label(target_kind));
                        section_emitted = true;
                    }
                    bool is_sel = (ep->wc_event == ev);
                    const char *en = bp_event_to_string(ev);
                    if (ImGui::Selectable(en, is_sel))
                    {
                        if (!is_sel)
                        {
                            ep->wc_event = ev;
                            ep->dirty = true;
                        }
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(get_event_help(ev));
                        ImGui::EndTooltip();
                    }
                }
            }
            ImGui::EndCombo();
        }
        help_marker(_("Hardware event to monitor. Categorized:\n"
                      "  Signal - 0/1 line with trigger condition (Low/High/Rising/Falling/Changed)\n"
                      "  Change - fires on every change (mode, palette, palgrp, border)\n"
                      "  Point  - parametrized (raster:N) or one-shot (CPU events)\n"
                      "Reference: docs/cz/debugger/breakpoints/hw-events.md"));

        /* Trigger condition - viditelný jen pro SIGNAL kind. */
        if (event_has_trigger(ep->wc_event))
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("Trigger:"));
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(220);
            /* Preview = sipka + label aktualni hodnoty. ImGui combo preview je
             * jen string - nakreslime sipku az v BeginCombo items. */
            const char *trig_preview = trigger_label(ep->wc_event_trigger);
            if (ImGui::BeginCombo("##bpt_event_trigger", trig_preview,
                                   ImGuiComboFlags_HeightLargest))
            {
                static const en_BP_EVENT_TRIGGER order_trig[] = {
                    BP_EVT_TRIG_LOW,
                    BP_EVT_TRIG_HIGH,
                    BP_EVT_TRIG_RISING,
                    BP_EVT_TRIG_FALLING,
                    BP_EVT_TRIG_CHANGED
                };
                for (size_t i = 0; i < sizeof(order_trig)/sizeof(order_trig[0]); i++)
                {
                    en_BP_EVENT_TRIGGER t = order_trig[i];
                    /* Hybrid item: drawlist marker (sipka/text) + Selectable
                     * label vedle. ImGui ID musi byt stabilni - pouzijeme
                     * "###trig_<i>" suffix. */
                    draw_trigger_arrow(t);
                    bool is_sel = (ep->wc_event_trigger == t);
                    char id_buf[32];
                    snprintf(id_buf, sizeof(id_buf), "%s###bpt_trig_%d",
                             trigger_label(t), (int)t);
                    if (ImGui::Selectable(id_buf, is_sel))
                    {
                        if (!is_sel)
                        {
                            ep->wc_event_trigger = t;
                            ep->dirty = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            help_marker(_("Trigger condition (only for Signal events):\n"
                          "  0 (Low)   - fires while signal level is low\n"
                          "  1 (High)  - fires while signal level is high\n"
                          "  Rising    - fires on 0->1 transition\n"
                          "  Falling   - fires on 1->0 transition\n"
                          "  Changed   - fires on any transition\n"
                          "Default Rising preserves legacy 'fire-on-event' behavior.\n"
                          "Reference: docs/cz/debugger/breakpoints/hw-events.md"));
        }

        /* Param - jen pro POINT_PARAM (raster). */
        bool param_active = event_takes_param(ep->wc_event);
        if (param_active)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("Param:"));
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(80);
            if (ImGui::InputText("##bpt_event_param", ep->wc_event_param,
                                  sizeof(ep->wc_event_param),
                                  ImGuiInputTextFlags_CharsDecimal))
                ep->dirty = true;
            if (ep->wc_event == BP_EVENT_GDG_RASTER)
            {
                ImGui::SameLine();
                /* Range zalezi na platforme: PAL (MZ-800/MZ-700-PAL) = 0..311,
                 * NTSC (MZ-1500/MZ-700-NTSC) = 0..261. Pouzivame compile-time
                 * g_mzhal.video_screen_height - 1 jako horni mez. */
                char hint[64];
                snprintf(hint, sizeof(hint), _("(scanline 0..%u)"),
                         (unsigned)(g_mzhal.video_screen_height - 1));
                ImGui::TextDisabled("%s", hint);
            }
        }
    }
    else if (ep->wc_type == BPT_TYPE_SP_THRESHOLD)
    {
        ImGui::TableNextRow();
        /* V1.5.E - label se mení podle modu: SINGLE = "SP Threshold",
         * WINDOW = "Lower bound" (= dolni mez okna). */
        const char *sp_label = (ep->wc_sp_mode == BP_SP_WINDOW) ?
                                 _("Lower bound:") : _("SP Threshold:");
        ImGui::TableNextColumn(); ImGui::Text("%s", sp_label);
        ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputText("##bpt_sp_threshold", ep->wc_sp_threshold,
                              sizeof(ep->wc_sp_threshold),
                              ImGuiInputTextFlags_CharsHexadecimal |
                                  ImGuiInputTextFlags_CharsUppercase))
            ep->dirty = true;
        help_marker(_("SINGLE: SP value to cross down (stack overflow detect).\n"
                      "WINDOW: Lower bound of allowed SP range."));

        /* V1.5.E - SP Mode dropdown */
        if (render_sp_mode_combo(_("Mode:"), "##bpt_sp_mode",
                                  &ep->wc_sp_mode,
                                  _("Single = trigger when SP descends through SP Threshold "
                                    "(stack overflow detect, default V1).\n"
                                    "Window = trigger when SP leaves [Lower..Upper] range "
                                    "(stack corruption / cross-task switch detect).\n"
                                    "Reference: docs/cz/debugger/breakpoints/match-modes.md")))
            ep->dirty = true;

        if (ep->wc_sp_mode == BP_SP_WINDOW)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("Upper bound:"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(80);
            if (ImGui::InputText("##bpt_sp_upper", ep->wc_sp_upper,
                                  sizeof(ep->wc_sp_upper),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase))
                ep->dirty = true;
            help_marker(_("Upper SP value of the allowed window. Lower = SP Threshold. "
                          "Trigger fires on transition from inside [Lower..Upper] to outside. "
                          "Edge-triggered: pre-existing outside SP at startup will not "
                          "spam - waits for transition.\n"
                          "Reference: docs/cz/debugger/breakpoints/match-modes.md"));
        }
    }
    else if (ep->wc_type == BPT_TYPE_IRQ)
    {
        /* V1.5.A8.5 - IRQ post-dispatch BP:
         *   1. Info řádek (= co BP dělá)
         *   2. IM mode discriminator (3 checkboxes inline)
         *   3. Sub-section IM 0 (RST opcode mask) - jen pokud im0_enabled
         *   4. Sub-section IM 1 (jen tooltip) - jen pokud im1_enabled
         *   5. Sub-section IM 2 (vector + ISR filter, A8 legacy) - jen
         *      pokud im2_enabled
         *
         * Hex/checkbox hodnoty drží UI i v disabled stavu (= toggle
         * nezahodí dříve zadané hodnoty). */

        /* Info row. */
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s",
            _("IRQ - CPU acknowledged INT (post-dispatch)"));

        /* === IM mode discriminator (3 checkboxes inline) === */
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", _("IM modes:"));
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        if (ImGui::Checkbox(_L("IM 0##bpt_irq_im0_en"),
                             &ep->wc_im0_enabled))
            ep->dirty = true;
        ImGui::SameLine();
        if (ImGui::Checkbox(_L("IM 1##bpt_irq_im1_en"),
                             &ep->wc_im1_enabled))
            ep->dirty = true;
        ImGui::SameLine();
        if (ImGui::Checkbox(_L("IM 2##bpt_irq_im2_en"),
                             &ep->wc_im2_enabled))
            ep->dirty = true;
        ImGui::SameLine();
        help_marker(_("Choose Z80 interrupt modes to monitor. At least one "
                      "must be selected. BP fires only when CPU dispatches in "
                      "an enabled mode. Each enabled IM mode reveals its own "
                      "sub-filter section below.\n"
                      "Reference: docs/cz/debugger/breakpoints/irq-filter.md"));

        /* === IM 0 sub-section: RST opcode mask (8 checkboxů 4x2) === */
        if (ep->wc_im0_enabled)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s",
                _("IM 0 RST filter (none = match all):"));
            ImGui::SameLine();
            help_marker(_("Filter triggers by RST opcode pushed on the bus "
                          "during IM 0 dispatch. Empty (no checkbox set) = "
                          "match all RSTs (legacy V1 behavior). Mask bit i "
                          "matches RST opcode (0xC7 + i*8).\n"
                          "Reference: docs/cz/debugger/breakpoints/irq-filter.md"));

            /* 8 RST checkboxů ve dvou řádcích po 4. ### (3 hashe) =
             * stable ID nezávislý na visible labelu. */
            static const struct {
                uint8_t bit;
                const char *label;
            } rst_items[8] = {
                { 0, "RST #00###bpt_irq_rst0" },
                { 1, "RST #08###bpt_irq_rst1" },
                { 2, "RST #10###bpt_irq_rst2" },
                { 3, "RST #18###bpt_irq_rst3" },
                { 4, "RST #20###bpt_irq_rst4" },
                { 5, "RST #28###bpt_irq_rst5" },
                { 6, "RST #30###bpt_irq_rst6" },
                { 7, "RST #38###bpt_irq_rst7" },
            };
            /* Layout 3+3+2: row 0 idx 0..2, row 1 idx 3..5, row 2 idx 6..7 */
            for (int idx = 0; idx < 8; idx++)
            {
                int col_in_row = idx % 3;
                if (col_in_row == 0)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                }
                else
                {
                    ImGui::SameLine();
                }
                bool checked = (ep->wc_im0_rst_mask &
                                 (uint8_t)(1u << rst_items[idx].bit)) != 0;
                if (ImGui::Checkbox(_L(rst_items[idx].label), &checked))
                {
                    if (checked)
                        ep->wc_im0_rst_mask |=
                            (uint8_t)(1u << rst_items[idx].bit);
                    else
                        ep->wc_im0_rst_mask &=
                            (uint8_t) ~(1u << rst_items[idx].bit);
                    ep->dirty = true;
                }
            }
        }

        /* === IM 1 sub-section: jen tooltip-info, žádný filter === */
        if (ep->wc_im1_enabled)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s",
                _("IM 1 always dispatches RST 38h (no extra filter)"));
            ImGui::SameLine();
            help_marker(_("In IM 1 the Z80 always dispatches via RST 38h. "
                          "There are no per-vector or per-source sub-filters "
                          "available - if IM 1 is enabled, every IM 1 "
                          "dispatch will fire the BP (subject to other "
                          "BP rules - condition, hits, skip).\n"
                          "Reference: docs/cz/debugger/breakpoints/irq-filter.md"));
        }

        /* === IM 2 sub-section: vector + ISR filter (A8 legacy UI) === */
        if (ep->wc_im2_enabled)
        {
            /* Sub-header. */
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", _("IM 2 sub-filters:"));

            /* Vector filter checkbox + addr. */
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox(_L("Filter by IM2 vector address##bpt_irq_vec_en"),
                                 &ep->wc_im2_vector_enabled))
                ep->dirty = true;
            help_marker(_("Filter triggers only when CPU dispatches IM2 INT "
                          "through this exact vector slot. Vector address is "
                          "computed as (I register << 8) | (low byte from "
                          "peripheral & 0xFE). Comparison applies AND with "
                          "0xFE on bit 0 (= HW vector page boundary), so "
                          "user value bit 0 is ignored at runtime.\n"
                          "Reference: docs/cz/debugger/breakpoints/irq-filter.md"));

            /* IM2 Vector adresa - SINGLE label + reused address input. Pri
             * RANGE/MASK je to "Address" v common pattern (low bound /
             * reference); separatni End/Mask radky se renderuji nize. */
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("IM2 Vector:"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(90);
            if (!ep->wc_im2_vector_enabled) ImGui::BeginDisabled();
            if (ImGui::InputText("##bpt_irq_vec_addr", ep->wc_im2_vector_addr,
                                  sizeof(ep->wc_im2_vector_addr),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase))
                ep->dirty = true;
            if (!ep->wc_im2_vector_enabled) ImGui::EndDisabled();

            /* V1.6+ 4.4: IM2 vector match mode dropdown + adaptive radky.
             * Disable cely radek pri vector_enabled = false (= zachova
             * konzistenci se zbytkem vector inputu). */
            if (!ep->wc_im2_vector_enabled) ImGui::BeginDisabled();
            if (render_match_mode_combo(_("Vector Match Mode:"),
                                         "##bpt_irq_vec_mm",
                                         &ep->wc_im2_vector_match_mode,
                                         _("Single = trigger on exact vector address (default V1.5).\n"
                                           "Range  = trigger when vector within [Vector..End] inclusive.\n"
                                           "Mask   = trigger when (vector & Mask) == (Vector & Mask).\n"
                                           "         Useful for matching vector page slots.\n"
                                           "Note: HW page boundary mask 0xFFFE is applied to both "
                                           "user value and runtime vector before comparison.\n"
                                           "Reference: docs/cz/debugger/breakpoints/irq-filter.md")))
                ep->dirty = true;

            if (ep->wc_im2_vector_match_mode == BP_MATCH_RANGE)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", _("Vector End:"));
                ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(90);
                if (ImGui::InputText("##bpt_irq_vec_end",
                                      ep->wc_im2_vector_addr_end,
                                      sizeof(ep->wc_im2_vector_addr_end),
                                      ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_CharsUppercase))
                    ep->dirty = true;
                help_marker(_("Upper bound for RANGE match (inclusive). "
                              "Active only when Vector Match Mode = Range."));
            }
            else if (ep->wc_im2_vector_match_mode == BP_MATCH_MASK)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", _("Vector Mask:"));
                ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(90);
                if (ImGui::InputText("##bpt_irq_vec_mask",
                                      ep->wc_im2_vector_mask,
                                      sizeof(ep->wc_im2_vector_mask),
                                      ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_CharsUppercase))
                    ep->dirty = true;
                help_marker(_("AND mask for MASK match. Trigger when "
                              "(vector & Mask) == (Vector & Mask). "
                              "Default 0xFFFF = identical to Single mode. "
                              "Example: Vector=0xFE10, Mask=0xFFE0 triggers on "
                              "any vector in 0xFE00..0xFE1F page slot range."));
            }
            if (!ep->wc_im2_vector_enabled) ImGui::EndDisabled();

            /* ISR filter checkbox + addr. */
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox(_L("Filter by IM2 ISR address##bpt_irq_isr_en"),
                                 &ep->wc_im2_isr_enabled))
                ep->dirty = true;
            help_marker(_("Filter triggers only when CPU jumps to this ISR "
                          "after IM2 dispatch. ISR address is read by CPU "
                          "from the IM2 vector table during dispatch.\n"
                          "Reference: docs/cz/debugger/breakpoints/irq-filter.md"));

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", _("IM2 ISR:"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(90);
            if (!ep->wc_im2_isr_enabled) ImGui::BeginDisabled();
            if (ImGui::InputText("##bpt_irq_isr_addr", ep->wc_im2_isr_addr,
                                  sizeof(ep->wc_im2_isr_addr),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase))
                ep->dirty = true;
            if (!ep->wc_im2_isr_enabled) ImGui::EndDisabled();

            /* V1.6+ 4.4: IM2 ISR match mode dropdown + adaptive radky. */
            if (!ep->wc_im2_isr_enabled) ImGui::BeginDisabled();
            if (render_match_mode_combo(_("ISR Match Mode:"),
                                         "##bpt_irq_isr_mm",
                                         &ep->wc_im2_isr_match_mode,
                                         _("Single = trigger on exact ISR address (default V1.5).\n"
                                           "Range  = trigger when ISR within [ISR..End] inclusive.\n"
                                           "Mask   = trigger when (isr & Mask) == (ISR & Mask).\n"
                                           "         Useful for matching ISR memory regions.\n"
                                           "Reference: docs/cz/debugger/breakpoints/irq-filter.md")))
                ep->dirty = true;

            if (ep->wc_im2_isr_match_mode == BP_MATCH_RANGE)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", _("ISR End:"));
                ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(90);
                if (ImGui::InputText("##bpt_irq_isr_end",
                                      ep->wc_im2_isr_addr_end,
                                      sizeof(ep->wc_im2_isr_addr_end),
                                      ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_CharsUppercase))
                    ep->dirty = true;
                help_marker(_("Upper bound for RANGE match (inclusive). "
                              "Active only when ISR Match Mode = Range."));
            }
            else if (ep->wc_im2_isr_match_mode == BP_MATCH_MASK)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", _("ISR Mask:"));
                ImGui::TableNextColumn(); ImGui::TextDisabled("0x");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(90);
                if (ImGui::InputText("##bpt_irq_isr_mask",
                                      ep->wc_im2_isr_mask,
                                      sizeof(ep->wc_im2_isr_mask),
                                      ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_CharsUppercase))
                    ep->dirty = true;
                help_marker(_("AND mask for MASK match. Trigger when "
                              "(isr & Mask) == (ISR & Mask). "
                              "Default 0xFFFF = identical to Single mode."));
            }
            if (!ep->wc_im2_isr_enabled) ImGui::EndDisabled();
        }
    }
    else if (ep->wc_type == BPT_TYPE_IRQ_SIG)
    {
        /* V1.5.A8.5 - IRQ_SIG pre-dispatch BP:
         *   Source mask checkboxes (5 sources) + multi-source OR semantics.
         *
         * Per-source bity drží UI v wc_irq_sig_source_mask. Mask 0 =
         * invalid (UI validation blokuje OK). */

        /* Info row. */
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s",
            _("IRQ_SIG - peripheral INT line raise (pre-dispatch)"));

        /* Sources header. */
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", _("Sources:"));
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s",
            _("(at least one required, OR semantics)"));
        ImGui::SameLine();
        help_marker(_("Choose peripheral INT line sources to monitor "
                      "(pre-dispatch). PIOZ80 PORT_A/B = Z80 PIO chip "
                      "interrupts. CTC2 = CTC channel 2 (timer/counter). "
                      "FDC = floppy disk controller (WD279x). Other = "
                      "other unrecognized INT sources (bus latch). "
                      "Multi-source BP fires when ANY selected source "
                      "raises (OR semantics).\n"
                      "Reference: docs/cz/debugger/breakpoints/irq-sig.md"));

        /* Source checkboxes - 3+2 layout (PIOZ80_A, PIOZ80_B, CTC2 / FDC, OTHER). */
        static const struct {
            uint8_t bit;
            const char *label;
        } sig_items[5] = {
            { (uint8_t) BP_IRQ_SIG_PIOZ80_PORT_A, "PIOZ80 PORT_A##bpt_sig_pa" },
            { (uint8_t) BP_IRQ_SIG_PIOZ80_PORT_B, "PIOZ80 PORT_B##bpt_sig_pb" },
            { (uint8_t) BP_IRQ_SIG_CTC2,          "CTC2##bpt_sig_ctc2" },
            { (uint8_t) BP_IRQ_SIG_FDC,           "FDC##bpt_sig_fdc" },
            { (uint8_t) BP_IRQ_SIG_OTHER,         "Other (bus latch)##bpt_sig_other" },
        };
        /* Řada 1: PIOZ80_A, PIOZ80_B, CTC2. */
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        for (int i = 0; i < 3; i++)
        {
            bool checked = (ep->wc_irq_sig_source_mask &
                             sig_items[i].bit) != 0;
            if (i > 0) ImGui::SameLine();
            if (ImGui::Checkbox(_L(sig_items[i].label), &checked))
            {
                if (checked)
                    ep->wc_irq_sig_source_mask |= sig_items[i].bit;
                else
                    ep->wc_irq_sig_source_mask &=
                        (uint8_t) ~sig_items[i].bit;
                ep->dirty = true;
            }
        }
        /* Řada 2: FDC, Other. */
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        for (int i = 3; i < 5; i++)
        {
            bool checked = (ep->wc_irq_sig_source_mask &
                             sig_items[i].bit) != 0;
            if (i > 3) ImGui::SameLine();
            if (ImGui::Checkbox(_L(sig_items[i].label), &checked))
            {
                if (checked)
                    ep->wc_irq_sig_source_mask |= sig_items[i].bit;
                else
                    ep->wc_irq_sig_source_mask &=
                        (uint8_t) ~sig_items[i].bit;
                ep->dirty = true;
            }
        }
    }
    else if (ep->wc_type == BPT_TYPE_GLOBAL)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s",
            _("Global BP - fires when condition is true (per-instruction probe)"));
    }

    ImGui::EndTable();
}


/**
 * Sekce Name / Parent: Group dropdown (1.), Auto Name + Name (2.), Colors (3.).
 *
 * Name input je vždy zobrazený - když Auto Name je on, input je read-only
 * a obsahuje auto-generovaný text. Když off, input je editovatelný a bere
 * wc_name (start je auto-text, user může přepsat).
 *
 * Hits + Reset jsou vykreslené samostatně v top row render_event_panel.
 */
static void render_section_name_parent(BptEditPanelState *ep)
{
    /* 1. Group */
    ImGui::Text("%s", _("Group:"));
    ImGui::SameLine();
    if (render_group_dropdown(&ep->wc_parent_group_id))
        ep->dirty = true;
    help_marker(_("Tree organization. Drag-drop in BP list = move between groups."));

    /* 2. Auto-generated name + Name (always shown, readonly when auto) */
    if (ImGui::Checkbox(_L("Auto-generated name##bpt_panel"), &ep->wc_auto_name))
        ep->dirty = true;

    ImGui::Text("%s", _("Name:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ep->wc_auto_name)
    {
        /* Sync wc_name kontinuálně s auto-generated textem - když user
         * přepne off, má v textboxu auto-text jako start a může editovat.
         * Push disabled text color aby readonly pole vizuálně zašednlo
         * (= odlišení od editovatelného). */
        uint16_t a;
        if (parse_hex16(ep->wc_addr, &a))
            snprintf(ep->wc_name, sizeof(ep->wc_name), "(auto: %04X)", a);
        else
            snprintf(ep->wc_name, sizeof(ep->wc_name), "(auto: ----)");
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::InputText("##bpt_name", ep->wc_name, sizeof(ep->wc_name),
                          ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
    }
    else
    {
        if (ImGui::InputText("##bpt_name", ep->wc_name, sizeof(ep->wc_name)))
            ep->dirty = true;
    }

    /* 3. Colors - zarovnané vpravo (FG + BG bloky).
     * SetCursorPosX přímo na (right_edge - total_w) je spolehlivější. */
    bool fg_changed = false, bg_changed = false;
    float fg_label_w = ImGui::CalcTextSize("FG").x;
    float bg_label_w = ImGui::CalcTextSize("BG").x;
    float color_w = 20.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float total_w = fg_label_w + spacing + color_w + 20.0f
                    + bg_label_w + spacing + color_w;
    float right_x = ImGui::GetWindowContentRegionMax().x;
    if (right_x - total_w > 0)
        ImGui::SetCursorPosX(right_x - total_w - 2.0f);
    fg_changed = render_color_field("FG", ep->wc_fg_color);
    ImGui::SameLine(0, 20);
    bg_changed = render_color_field("BG", ep->wc_bg_color);
    if (fg_changed || bg_changed) ep->dirty = true;
}


/**
 * Sekce 5: Trigger logic - Condition expression + Edge / Hit / Skip.
 */
static void render_section_trigger_logic(BptEditPanelState *ep)
{
    ImGui::Text("%s", _("Condition (expression):"));
    help_marker(_("Expression evaluated BEFORE the trigger. Returns zero = "
                  "BP does not fire. Reference: docs/cz/debugger/breakpoints/expression-syntax.md"));
    float multi_h = ImGui::GetTextLineHeight() * 3.0f +
                     ImGui::GetStyle().FramePadding.y * 2.0f;
    if (ImGui::InputTextMultiline("##bpt_expr", ep->wc_expr,
                                   sizeof(ep->wc_expr),
                                   ImVec2(-FLT_MIN, multi_h)))
    {
        ep->dirty = true;
        ep->wc_expr_validated = false;
        ep->wc_expr_err[0] = '\0';
    }
    if (ep->wc_expr_validated && ep->wc_expr_err[0])
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("! %s", ep->wc_expr_err);
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    if (ImGui::Checkbox(_L("Edge-triggered##bpt_edge"),
                         &ep->wc_edge_triggered))
        ep->dirty = true;
    help_marker(_("Trigger only on false->true transition (= for state machines "
                  "and global conditions where level-trigger would spam)."));

    /* Hit / Skip count v 2-sloupcové tabulce: label | input + (?), aby se
     * pozice text-entry v obou řádcích zarovnaly. */
    if (ImGui::BeginTable("##bpt_hitskip", 2, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", _("Hit count:"));
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(160);
        int hc = (int)ep->wc_hit_count;
        if (ImGui::InputInt("##bpt_hitc", &hc, 1, 10,
                             ImGuiInputTextFlags_CharsDecimal))
        {
            if (hc < 0) hc = 0;
            ep->wc_hit_count = (uint32_t)hc;
            ep->dirty = true;
        }
        help_marker(_("Fire on Nth pass (0 = every hit)."));

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", _("Skip count:"));
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(160);
        int sc = (int)ep->wc_skip_count;
        if (ImGui::InputInt("##bpt_skipc", &sc, 1, 10,
                             ImGuiInputTextFlags_CharsDecimal))
        {
            if (sc < 0) sc = 0;
            ep->wc_skip_count = (uint32_t)sc;
            ep->dirty = true;
        }
        help_marker(_("Ignore first N passes, then start counting hit count."));

        ImGui::EndTable();
    }
}


/**
 * Sekce 6: Action mode radio + adaptive textbox.
 *
 * Mode = STOP -> textbox skryt, action = NULL.
 * Mode = LOG_ONLY -> auto-generovaný `log "BP <name>"; continue`.
 * Mode = CUSTOM -> editovatelný textbox s mini-DSL.
 */
static void render_section_action(BptEditPanelState *ep)
{
    /* Mode jako combobox - kompaktnější než 3 RadioButtons + jednotná
     * UX s ostatními dropdowny v dialogu (Type, Zone, Group, ...). */
    static const char *mode_labels[] = {
        "Stop",       /* BPT_ACTION_MODE_STOP */
        "Log only",   /* BPT_ACTION_MODE_LOG_ONLY */
        "Custom",     /* BPT_ACTION_MODE_CUSTOM */
    };
    ImGui::Text("%s", _("Mode:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    BptActionMode prev_mode = ep->wc_action_mode;
    const char *mode_preview = mode_labels[(int)ep->wc_action_mode];
    if (ImGui::BeginCombo("##bpt_act_mode", _(mode_preview)))
    {
        for (int m = 0; m < 3; m++)
        {
            bool is_sel = ((int)ep->wc_action_mode == m);
            if (ImGui::Selectable(_(mode_labels[m]), is_sel))
            {
                if (!is_sel)
                    ep->wc_action_mode = (BptActionMode)m;
            }
        }
        ImGui::EndCombo();
    }
    help_marker(_("Reaction AFTER the trigger:\n"
                  "  Stop = classic BP, halts the emulator\n"
                  "  Log only = TTY output + continue (log \"BP X\"; continue)\n"
                  "  Custom = full mini-DSL script\n"
                  "Reference: docs/cz/debugger/breakpoints/action-dsl.md"));

    if (prev_mode != ep->wc_action_mode)
    {
        switch (ep->wc_action_mode)
        {
        case BPT_ACTION_MODE_STOP:
            ep->wc_action[0] = '\0';
            break;
        case BPT_ACTION_MODE_LOG_ONLY:
            generate_log_action(ep->wc_action,
                                 sizeof(ep->wc_action),
                                 ep->wc_name);
            break;
        case BPT_ACTION_MODE_CUSTOM:
            /* Ponechá současný textbox content tak, jak je. */
            break;
        }
        ep->wc_action_validated = false;
        ep->wc_action_err[0] = '\0';
        ep->dirty = true;
    }

    /* Textbox - zobrazený jen pro LOG_ONLY (read-only preview) a CUSTOM */
    if (ep->wc_action_mode == BPT_ACTION_MODE_STOP)
    {
        ImGui::TextDisabled("%s",
            _("(action empty - BP halts the emulator)"));
        return;
    }

    bool readonly = (ep->wc_action_mode == BPT_ACTION_MODE_LOG_ONLY);
    if (readonly) ImGui::BeginDisabled();
    float multi_h = ImGui::GetTextLineHeight() * 3.0f +
                     ImGui::GetStyle().FramePadding.y * 2.0f;
    if (ImGui::InputTextMultiline("##bpt_action", ep->wc_action,
                                   sizeof(ep->wc_action),
                                   ImVec2(-FLT_MIN, multi_h)))
    {
        ep->dirty = true;
        ep->wc_action_validated = false;
        ep->wc_action_err[0] = '\0';
    }
    if (readonly) ImGui::EndDisabled();
    if (ep->wc_action_validated && ep->wc_action_err[0])
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("! %s", ep->wc_action_err);
        ImGui::PopStyleColor();
    }
}


/**
 * Sekce Live Eval Test - vstupní textbox (full width) + tlačítko Test Eval
 * (samostatný řádek vpravo) + formátovaný výsledek.
 *
 * Label "Live Eval Test (optional)" je v parent SeparatorText.
 *
 * V1: ctx je naplněna nulami (= stateless test). V1.5.A10 doplní real ctx
 * z g_mzarch_main.
 */
static void render_section_test_eval(BptEditPanelState *ep)
{
    /* Textbox přes celou šířku */
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##bpt_test_expr", ep->wc_test_expr,
                      sizeof(ep->wc_test_expr));

    /* Test Eval na samostatný řádek, zarovnaný vpravo */
    const char *btn_label = _L("Test Eval##bpt_panel");
    float btn_w = 100.0f;
    float row_avail = ImGui::GetContentRegionAvail().x;
    if (row_avail > btn_w)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + row_avail - btn_w);
    if (ImGui::Button(btn_label, ImVec2(btn_w, 0)))
    {
        char errbuf[BPT_EDIT_ERR_BUFLEN];
        errbuf[0] = '\0';
        bp_expr_ctx_t ctx;
        /* V1.5.A10: ctx_zero pak naplň real emu state z g_mzarch_main
         * + g_gdg (= cpu registers, BankPC). Test Eval tak může pracovat
         * s skutečnými hodnotami CPU registrů a memory probe. self_id=-1
         * = test eval out-of-band (= disable_self loguje warning). */
        bp_expr_ctx_zero(&ctx);
        breakpoints_fill_global_ctx(&ctx);
        ctx.self_id = -1;
        int32_t result = 0;
        if (bp_expr_parse_eval(ep->wc_test_expr, &ctx, &result,
                                 errbuf, sizeof(errbuf)))
        {
            snprintf(ep->wc_test_result, sizeof(ep->wc_test_result),
                      "= %d  (0x%X)", (int)result, (unsigned)result);
        }
        else
        {
            snprintf(ep->wc_test_result, sizeof(ep->wc_test_result),
                      "ERR: %s", errbuf);
        }
    }
    if (ep->wc_test_result[0])
    {
        ImGui::TextWrapped("%s", ep->wc_test_result);
    }
}


/**
 * Pravý sloupec: Code Preview adaptivní per-typ. Pro PC_EXEC / MEM_R / MEM_W
 * disasm 12 řádků centrovaný na BP. Pro ostatní typy V1 placeholder
 * (V1.5.A9 doplní bit decode / event timing / SP region / IRQ vector).
 */
/**
 * Hlavní render Event panelu - jednosloupcový layout (jen Properties).
 *
 * Code Preview blok byl odstraněn (rozhodnutí Michala 2026-05-06):
 * - Preview pro většinu BP typů nelze smysluplně vizualizovat
 * - SP_THRESHOLD vizualizace bude v budoucnu samostatné okno
 * - PC/MEM disasm preview duplikoval Disassembler panel
 *
 * Outer BeginChild ("##bpt_edit_content" v bpt_edit_panel_render) řeší
 * scrolling pokud content přesáhne výšku okna.
 */
static void render_event_panel(void)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;

    /* 0a. Top row: Enabled vlevo, Hits text vpravo na stejném řádku
     *     (jen pro existující BP). Reset button na další řádek vpravo. */
    if (ImGui::Checkbox(_L("Enabled##bpt_panel_top"), &ep->wc_enabled))
        ep->dirty = true;
    if (ep->edit_id >= 0)
    {
        st_BPT *bpt = breakpoints_find_by_id(ep->edit_id);
        if (bpt)
        {
            /* Hits na stejném řádku jako Enabled, vpravo.
             * SetCursorPosX přímo na (right_edge - text_w) - spolehlivější
             * než Dummy+SameLine které trpí glyph antialiasing overflow. */
            char hits_buf[64];
            snprintf(hits_buf, sizeof(hits_buf), _("Hits: %llu events"),
                      (unsigned long long)bpt->hits);
            const float right_safety = 2.0f;
            float right_x = ImGui::GetWindowContentRegionMax().x;
            float hits_w = ImGui::CalcTextSize(hits_buf, NULL, true).x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_x - hits_w - right_safety);
            ImGui::Text("%s", hits_buf);

            /* Reset na samostatný řádek vpravo */
            const char *reset_label = _L("Reset###bpt_hits");
            float reset_w = ImGui::CalcTextSize(reset_label, NULL, true).x
                              + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorPosX(right_x - reset_w - right_safety);
            if (ImGui::Button(reset_label))
                breakpoints_reset_hits(ep->edit_id);
        }
    }

    /* 1. BP Options sekce: Type + Zone + Bank + per-type Address/Port/Event/SP */
    ImGui::Spacing();
    ImGui::SeparatorText(_("BP Options"));
    render_section_bp_options(ep);

    /* 2. Name / Parent */
    ImGui::Spacing();
    ImGui::SeparatorText(_("Name / Parent"));
    render_section_name_parent(ep);

    /* 3. Trigger logic */
    ImGui::Spacing();
    ImGui::SeparatorText(_("Trigger logic"));
    render_section_trigger_logic(ep);

    /* 4. Action on trigger */
    ImGui::Spacing();
    ImGui::SeparatorText(_("Action on trigger"));
    render_section_action(ep);

    /* 5. Live Eval Test - label v separátoru */
    ImGui::Spacing();
    ImGui::SeparatorText(_("Live Eval Test (optional)"));
    render_section_test_eval(ep);
}


/* -------------------------------------------------------------------------
 * Render: Group panel (jednoduchý)
 * ------------------------------------------------------------------------- */

static void render_group_panel(void)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;

    if (ImGui::Checkbox(_L("Enabled (cascade)##bpt_grp_panel"), &ep->wc_enabled))
        ep->dirty = true;

    ImGui::Spacing();
    ImGui::Text("%s", _("Name:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##bpt_grp_name", ep->wc_name, sizeof(ep->wc_name)))
        ep->dirty = true;

    ImGui::Spacing();
    if (render_color_field("FG", ep->wc_fg_color)) ep->dirty = true;
    ImGui::SameLine(0, 20);
    if (render_color_field("BG", ep->wc_bg_color)) ep->dirty = true;

    /* Children counter (jen pro existující grupu) */
    if (ep->edit_id >= 0)
    {
        int total = 0, enabled = 0;
        for (unsigned i = 0; i < g_breakpoints.breakpoints->len; i++)
        {
            st_BPT *bpt = &g_array_index(g_breakpoints.breakpoints, st_BPT, i);
            if (bpt->parent == ep->edit_id)
            {
                total++;
                if (bpt->enabled) enabled++;
            }
        }
        ImGui::Spacing();
        ImGui::Text(_("Children: %d BP (%d enabled)"), total, enabled);
    }
}


/* -------------------------------------------------------------------------
 * Render: Confirm dialogs
 * ------------------------------------------------------------------------- */

static void render_confirm_dialogs(void)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;

    /* Discard confirm při přepnutí na jiný BP */
    if (ep->show_discard_confirm)
    {
        ImGui::OpenPopup(_L("Discard changes?##bpt_discard"));
        ep->show_discard_confirm = false;
    }
    bool open = true;
    if (ImGui::BeginPopupModal(_L("Discard changes?##bpt_discard"), &open,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s", _("Editor has unsaved changes. Discard them?"));
        ImGui::Spacing();
        if (ImGui::Button(_L("Discard##bpt_disc"), ImVec2(100, 0)))
        {
            ep->visible = true;
            ep->type = ep->pending_type;
            ep->edit_id = ep->pending_edit_id;
            ep->dirty = false;
            ep->initialized_for_id = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_L("Cancel##bpt_disc"), ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    /* Close confirm při zavírání panelu */
    if (ep->show_close_confirm)
    {
        ImGui::OpenPopup(_L("Discard and close?##bpt_close"));
        ep->show_close_confirm = false;
    }
    bool open2 = true;
    if (ImGui::BeginPopupModal(_L("Discard and close?##bpt_close"), &open2,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s", _("Editor has unsaved changes. Close anyway?"));
        ImGui::Spacing();
        if (ImGui::Button(_L("Close##bpt_clo"), ImVec2(100, 0)))
        {
            ep->visible = false;
            ep->dirty = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_L("Cancel##bpt_clo"), ImVec2(100, 0)))
        {
            /* Re-show parent okna - close() ho schoval pro popup. */
            ep->visible = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}


/* -------------------------------------------------------------------------
 * Public render entry
 * ------------------------------------------------------------------------- */

void bpt_edit_panel_render(void)
{
    BptEditPanelState *ep = &g_bpt_ui.edit_panel;

    /* Render confirm dialogs i když panel není visible (= mohou být queued
     * z openu jiné položky). */
    render_confirm_dialogs();

    if (!ep->visible) return;

    /* Re-init working copy pokud změna edit_id (= prevence overwrite
     * uživatelského inputu při každém frame). */
    if (!ep->initialized_for_id || ep->initialized_for_id_value != ep->edit_id)
        working_copy_init(ep->type, ep->edit_id);

    /* Title obsahuje identifikaci editovaného záznamu (adresa pro BP, jméno
     * pro Group) + hvězdičku pro dirty stav. POZOR: musí být TŘI '#' v
     * '###bpt_edit_panel', ne dvě:
     *   '##suffix'  -> ID hash je celý label, změna title = nové okno (jump)
     *   '###suffix' -> ID hash je jen "suffix", title je volný a ID stable
     * Stable ID = dock layout zachován, focus state nepřeskočí, žádný
     * appearing-frame visual skok při změně dirty marku. */
    char title[160];
    const char *dirty_mark = ep->dirty ? " *" : "";
    if (ep->type == BPT_ITEM_GROUP)
    {
        if (ep->edit_id >= 0)
        {
            const char *gname = (ep->wc_name[0]) ? ep->wc_name : "?";
            snprintf(title, sizeof(title), "%s \"%s\"%s###bpt_edit_panel",
                     _("Edit Group"), gname, dirty_mark);
        }
        else
        {
            snprintf(title, sizeof(title), "%s%s###bpt_edit_panel",
                     _("New Group"), dirty_mark);
        }
    }
    else /* BPT_ITEM_EVENT */
    {
        if (ep->edit_id >= 0)
        {
            /* V1.5: titulek per-typ identifier - addr pro PC/MEM, port pro
             * IORQ, event name pro HW_EVENT, sp threshold pro SP_THRESHOLD,
             * jen typ-název pro IRQ / GLOBAL. */
            char id_str[32];
            uint16_t v;
            switch (ep->wc_type)
            {
            case BPT_TYPE_PC_EXEC:
            case BPT_TYPE_MEM_R:
            case BPT_TYPE_MEM_W:
                if (parse_hex16(ep->wc_addr, &v))
                    snprintf(id_str, sizeof(id_str), "%04X", v);
                else
                    snprintf(id_str, sizeof(id_str), "????");
                break;
            case BPT_TYPE_IORQ_R:
            case BPT_TYPE_IORQ_W:
                if (parse_hex16(ep->wc_port, &v))
                    snprintf(id_str, sizeof(id_str), "port %04X", v);
                else
                    snprintf(id_str, sizeof(id_str), "port ??");
                break;
            case BPT_TYPE_HW_EVENT:
                snprintf(id_str, sizeof(id_str), "%s",
                          ep->wc_event == BP_EVENT_NONE ? "(no event)"
                          : bp_event_to_string(ep->wc_event));
                break;
            case BPT_TYPE_SP_THRESHOLD:
                if (parse_hex16(ep->wc_sp_threshold, &v))
                    snprintf(id_str, sizeof(id_str), "SP %04X", v);
                else
                    snprintf(id_str, sizeof(id_str), "SP ????");
                break;
            case BPT_TYPE_IRQ:
                snprintf(id_str, sizeof(id_str), "IRQ");
                break;
            case BPT_TYPE_IRQ_SIG:
                snprintf(id_str, sizeof(id_str), "IRQ_SIG");
                break;
            case BPT_TYPE_GLOBAL:
                snprintf(id_str, sizeof(id_str), "GLOBAL");
                break;
            default:
                snprintf(id_str, sizeof(id_str), "?");
                break;
            }
            snprintf(title, sizeof(title), "%s %s%s###bpt_edit_panel",
                     _("Edit BP"), id_str, dirty_mark);
        }
        else
        {
            snprintf(title, sizeof(title), "%s%s###bpt_edit_panel",
                     _("New BP"), dirty_mark);
        }
    }

    bool window_open = true;
    /* SizeConstraints místo SetNextWindowSize - vynutí MIN velikost
     * vždy (zabraňuje překlopení do scrollbar mode pokud user dříve
     * zmenšil okno nebo .ini má uloženou malou velikost z předchozích
     * sessions). User si může zvětšit dle libosti. */
    /* 1-sloupcový layout (Code Preview odstraněna 2026-05-06) - menší
     * minimální šířka, výška zachována (= dlouhý content sekcí). */
    float min_w = (ep->type == BPT_ITEM_EVENT) ? 480.0f : 420.0f;
    float min_h = (ep->type == BPT_ITEM_EVENT) ? 720.0f : 360.0f;
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(min_w, min_h),
        ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(
        ImVec2(min_w, min_h),
        ImGuiCond_FirstUseEver);
    /* Auto-layout poblíž kurzoru pro modální editor breakpointů. */
    auto_layout_first_use_near_mouse(title, 500.0f, 500.0f);
    if (ImGui::Begin(title, &window_open, ImGuiWindowFlags_NoCollapse))
    {
        /* Vyhrazený prostor na spodu pro Cancel/OK buttons (= mimo
         * scrollable content area). Inner BeginChild s height
         * (-button_row_h) zaručí že content nepřetéká přes button row.
         * Buttons se renderují PO EndChild = vždy viditelné. */
        float btn_row_h = ImGui::GetFrameHeight()
                            + ImGui::GetStyle().ItemSpacing.y * 2.0f
                            + 4.0f; /* + Separator height */

        /* Auto vertikální scrollbar - pokud user okno hodně zmenší
         * vertikálně, content přesahuje a vertical scroll se objeví. */
        ImGui::BeginChild("##bpt_edit_content",
                           ImVec2(0, -btn_row_h),
                           ImGuiChildFlags_None,
                           ImGuiWindowFlags_None);
        if (ep->type == BPT_ITEM_GROUP)
            render_group_panel();
        else
            render_event_panel();
        ImGui::EndChild();

        /* Apply / Cancel buttons - zarovnáno doprava, mimo scroll area */
        ImGui::Separator();
        float btn_w = 100.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float total = btn_w * 2 + spacing;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > total)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - total);

        if (ImGui::Button(_L("Cancel##bpt_panel_btn"), ImVec2(btn_w, 0)))
        {
            /* Cancel = zahodit working copy + zavřít panel (explicitní akce
             * uživatele, žádný confirm - X v title baru má confirm pro dirty). */
            ep->dirty = false;
            ep->visible = false;
        }
        ImGui::SameLine();

        /* Validace - pro Group jméno; pro Event jméno (pokud ne auto) +
         * per-type adresové pole. V V1.5 layoutu už Address není povinný
         * pro IORQ_R/W, HW_EVENT, SP_THRESHOLD, IRQ a GLOBAL - validace
         * musí kontrolovat jen typu odpovídající identifier.
         */
        bool valid = !is_name_empty(ep->wc_name) || ep->wc_auto_name;
        if (ep->type == BPT_ITEM_EVENT)
        {
            uint16_t tmp;
            switch (ep->wc_type)
            {
            case BPT_TYPE_PC_EXEC:
            case BPT_TYPE_MEM_R:
            case BPT_TYPE_MEM_W:
                if (!parse_hex16(ep->wc_addr, &tmp)) valid = false;
                break;
            case BPT_TYPE_IORQ_R:
            case BPT_TYPE_IORQ_W:
                if (!parse_hex16(ep->wc_port, &tmp)) valid = false;
                break;
            case BPT_TYPE_HW_EVENT:
                if (ep->wc_event == BP_EVENT_NONE) valid = false;
                /* Pro raster vyžaduj i numerický param */
                if (event_takes_param(ep->wc_event) &&
                    ep->wc_event_param[0] == '\0')
                    valid = false;
                break;
            case BPT_TYPE_SP_THRESHOLD:
                if (!parse_hex16(ep->wc_sp_threshold, &tmp)) valid = false;
                /* V1.5.E - WINDOW mode: vyzaduj parse upper a end >= start. */
                if (ep->wc_sp_mode == BP_SP_WINDOW)
                {
                    uint16_t up;
                    if (!parse_hex16(ep->wc_sp_upper, &up)) valid = false;
                    else
                    {
                        uint16_t lo;
                        if (parse_hex16(ep->wc_sp_threshold, &lo) && up < lo)
                            valid = false;
                    }
                }
                break;
            case BPT_TYPE_IRQ:
                /* V1.5.A8.5: vyžaduj aspoň 1 IM mode enabled (= jinak BP
                 * nikdy nefire). IM 2 sub-filter validation jen pokud
                 * IM 2 enabled. */
                if (!ep->wc_im0_enabled && !ep->wc_im1_enabled
                    && !ep->wc_im2_enabled)
                {
                    valid = false;
                }
                if (ep->wc_im2_enabled && ep->wc_im2_vector_enabled)
                {
                    uint16_t vtmp;
                    if (!parse_hex16(ep->wc_im2_vector_addr, &vtmp))
                        valid = false;
                    /* V1.6+ 4.4: RANGE/MASK validation - End/Mask musi
                     * byt parsovatelne. RANGE end >= start na HW page
                     * mask 0xFFFE (= bit 0 ignored). */
                    if (ep->wc_im2_vector_match_mode == BP_MATCH_RANGE)
                    {
                        uint16_t vend;
                        if (!parse_hex16(ep->wc_im2_vector_addr_end, &vend))
                            valid = false;
                        else
                        {
                            uint16_t vstart;
                            if (parse_hex16(ep->wc_im2_vector_addr, &vstart)
                                && (vend & 0xFFFE) < (vstart & 0xFFFE))
                                valid = false;
                        }
                    }
                    else if (ep->wc_im2_vector_match_mode == BP_MATCH_MASK)
                    {
                        uint16_t vmask;
                        if (!parse_hex16(ep->wc_im2_vector_mask, &vmask))
                            valid = false;
                    }
                }
                if (ep->wc_im2_enabled && ep->wc_im2_isr_enabled)
                {
                    uint16_t itmp;
                    if (!parse_hex16(ep->wc_im2_isr_addr, &itmp))
                        valid = false;
                    if (ep->wc_im2_isr_match_mode == BP_MATCH_RANGE)
                    {
                        uint16_t iend;
                        if (!parse_hex16(ep->wc_im2_isr_addr_end, &iend))
                            valid = false;
                        else
                        {
                            uint16_t istart;
                            if (parse_hex16(ep->wc_im2_isr_addr, &istart)
                                && iend < istart)
                                valid = false;
                        }
                    }
                    else if (ep->wc_im2_isr_match_mode == BP_MATCH_MASK)
                    {
                        uint16_t imask;
                        if (!parse_hex16(ep->wc_im2_isr_mask, &imask))
                            valid = false;
                    }
                }
                break;
            case BPT_TYPE_IRQ_SIG:
                /* V1.5.A8.5: vyžaduj aspoň 1 source bit (= mask != 0). */
                if (ep->wc_irq_sig_source_mask == 0) valid = false;
                break;
            case BPT_TYPE_GLOBAL:
                /* Bez povinného identifikátoru. */
                break;
            default:
                break;
            }

            /* V1.5.E - kross-typ validace match mode RANGE: end >= start. */
            if (ep->wc_addr_match_mode == BP_MATCH_RANGE &&
                (ep->wc_type == BPT_TYPE_PC_EXEC ||
                 ep->wc_type == BPT_TYPE_MEM_R ||
                 ep->wc_type == BPT_TYPE_MEM_W))
            {
                uint16_t lo, hi;
                if (parse_hex16(ep->wc_addr, &lo) &&
                    parse_hex16(ep->wc_addr_end, &hi) && hi < lo)
                    valid = false;
            }
            if (ep->wc_port_match_mode == BP_MATCH_RANGE &&
                (ep->wc_type == BPT_TYPE_IORQ_R || ep->wc_type == BPT_TYPE_IORQ_W))
            {
                uint16_t lo, hi;
                if (parse_hex16(ep->wc_port, &lo) &&
                    parse_hex16(ep->wc_port_end, &hi) && hi < lo)
                    valid = false;
            }
            if (ep->wc_bank_match_mode == BP_MATCH_RANGE &&
                ep->wc_zone == BP_ZONE_MMEXT_BANK &&
                (ep->wc_type == BPT_TYPE_PC_EXEC ||
                 ep->wc_type == BPT_TYPE_MEM_R ||
                 ep->wc_type == BPT_TYPE_MEM_W))
            {
                uint16_t lo, hi;
                if (parse_hex16(ep->wc_bank_id, &lo) &&
                    parse_hex16(ep->wc_bank_id_end, &hi) && hi < lo)
                    valid = false;
            }
        }
        ImGui::BeginDisabled(!valid);
        bool ok_clicked = ImGui::Button(_L("OK##bpt_panel_btn"), ImVec2(btn_w, 0));
        /* V1.5: dříve `if (ep->dirty) SetItemDefaultFocus()` - to ale způsobilo
         * "poskočení" okna při prvním kliknutí (false->true transition na
         * dirty), protože focus shift po render content trigguje viewport
         * scroll/relayout. Odstraněno - default focus okna není kritický. */
        ImGui::EndDisabled();

        /* V1.5.E - inline warning u OK button kdyz Range end < start nebo
         * Window upper < lower. valid je v takovem pripade false a OK
         * disabled - warning vysvetluje proc. */
        if (!valid && ep->type == BPT_ITEM_EVENT)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.4f, 1.0f));
            ImGui::TextUnformatted(_("(check ranges: end >= start)"));
            ImGui::PopStyleColor();
        }
        if (ok_clicked && valid)
        {
            /* Pre-apply parser validace condition / action - chyby
             * neblokují Apply (= setter uloží řetězec i když parsed_*
             * cache zůstane NULL), ale UI zobrazí inline errbuf. */
            if (ep->type == BPT_ITEM_EVENT)
            {
                ep->wc_expr_validated = true;
                ep->wc_action_validated = true;
                ep->wc_expr_err[0] = '\0';
                ep->wc_action_err[0] = '\0';
                if (ep->wc_expr[0])
                {
                    bp_expr_t *e = bp_expr_parse(ep->wc_expr,
                                                  ep->wc_expr_err,
                                                  sizeof(ep->wc_expr_err));
                    if (e) bp_expr_free(e);
                }
                if (ep->wc_action[0])
                {
                    bp_action_t *a = bp_action_parse(ep->wc_action,
                                                      ep->wc_action_err,
                                                      sizeof(ep->wc_action_err));
                    if (a) bp_action_free(a);
                }
            }
            working_copy_apply();
            /* Pokud je obě parse OK, zavřeme. Při chybě necháme panel
             * otevřený, aby uživatel viděl errbuf pod textareou. */
            if (!ep->wc_expr_err[0] && !ep->wc_action_err[0])
                ep->visible = false;
        }
    }
    ImGui::End();

    /* X v title baru zavřel okno */
    if (!window_open)
        bpt_edit_panel_close();
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
