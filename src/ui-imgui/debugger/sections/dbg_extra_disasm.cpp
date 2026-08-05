/*
 * dbg_extra_disasm.cpp - Sekundární Disassembly okna #2 - #5
 *
 * Sekundární okna jsou samostatné top-level ImGui okno, které hosti jednu
 * instanci DisassembledView (vytvořenou s history=false, animation=false).
 * Lazy-create pri prvnim otevření, lazy-destroy při zavření.
 *
 * Uživatelský pohled:
 *   Debugger -> Other disassembly -> Disassembly #N (toggle)
 *
 * Persistence:
 *   - Layout (pozice, velikost): ImGui imgui.ini (automaticky)
 *   - focus_addr: cfgmain modul DEBUGGER, klíč disasm_extra<N>_focus
 *     (UNSIGNED 0..0xFFFF). Stored value se naplní z config v
 *     debugger_init() do s_persisted_focus[]; pri lazy-create instance
 *     se aplikuje přes dbg_disasm_view_set_focus_addr(). Pri každém
 *     framu (= když je instance otevřená) se naopak hodnota čte z
 *     instance zpět do s_persisted_focus[] aby cfgmain při shutdownu
 *     uložil aktuální stav.
 *   - Visibility: NEpersistuje se - default closed, uživatel otevírá z menu.
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <stdint.h>

#include "libs/imgui/imgui.h"
#include "i18n.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/debugger/sections/dbg_extra_disasm.h"
#include "ui-imgui/auto_layout.h"
#include "emulator/emulator.h"


/**
 * @brief Per-instance state extra okna.
 *
 * Drží lazy-vytvořený DisassembledView a stabilní window_id string
 * (pasovaný do dbg_disasm_view_create() jako pointer - musí žít po celou
 * existenci instance, proto static array). Persistovaná focus_addr slouží
 * jako cfgelement handler target - hodnota se aktualizuje z instance při
 * každém framu kdy je instance otevřená.
 */
typedef struct
{
    DisassembledView *view;     /**< Instance, NULL pokud okno zavřeno */
    bool focus_applied;         /**< persisted_focus už byl aplikován do view */
    const char *window_id;      /**< stabilní string "2"/"3"/"4"/"5" */
    const char *imgui_id_full;  /**< stabilní string "###disasm_extra_2" */
    const char *cfg_key;        /**< stabilní string "disasm_extra2_focus" */
} ExtraDisasmSlot;


/* Stabilní statické řetězce - nezmění se po dobu programu, pointer
 * předaný do cfgmodule_register_new_element() / dbg_disasm_view_create()
 * musí být validní celou dobu. */
static const char *const SLOT_WINDOW_ID[DBG_EXTRA_DISASM_COUNT] = {
    "2", "3", "4", "5"
};

static const char *const SLOT_IMGUI_ID_FULL[DBG_EXTRA_DISASM_COUNT] = {
    "###disasm_extra_2",
    "###disasm_extra_3",
    "###disasm_extra_4",
    "###disasm_extra_5",
};

static const char *const SLOT_CFG_KEY[DBG_EXTRA_DISASM_COUNT] = {
    "disasm_extra2_focus",
    "disasm_extra3_focus",
    "disasm_extra4_focus",
    "disasm_extra5_focus",
};

static const char *const SLOT_CFG_KEY_FOLLOW_PC[DBG_EXTRA_DISASM_COUNT] = {
    "disasm_extra2_follow_pc",
    "disasm_extra3_follow_pc",
    "disasm_extra4_follow_pc",
    "disasm_extra5_follow_pc",
};

static const char *const SLOT_CFG_KEY_SHOW_TSTATES[DBG_EXTRA_DISASM_COUNT] = {
    "disasm_extra2_show_tstates",
    "disasm_extra3_show_tstates",
    "disasm_extra4_show_tstates",
    "disasm_extra5_show_tstates",
};


/* Persistovaná focus_addr - cfgmain bind targets. Zápis: cfgmain při
 * loadu .ini, čtení: cfgmain při shutdownu .ini. Mezi tím sync s
 * instancemi viz update_persisted_focus(). */
static unsigned s_persisted_focus[DBG_EXTRA_DISASM_COUNT] = { 0, 0, 0, 0 };

/* Persistované per-instance dynamic flagy. Default: follow_pc=0 (sekundární
 * okna nesledují PC), show_tstates=0 (sloupec T-states off). Při lazy-create
 * instance se aplikuje na DisassembledView; v každém framu se naopak
 * aktuální hodnota čte zpět z instance pro shutdown save. */
static unsigned s_persisted_follow_pc[DBG_EXTRA_DISASM_COUNT] = { 0, 0, 0, 0 };
static unsigned s_persisted_show_tstates[DBG_EXTRA_DISASM_COUNT] = { 0, 0, 0, 0 };


/* Per-instance runtime state. */
static ExtraDisasmSlot s_slots[DBG_EXTRA_DISASM_COUNT] = {
    { NULL, false, NULL, NULL, NULL },
    { NULL, false, NULL, NULL, NULL },
    { NULL, false, NULL, NULL, NULL },
    { NULL, false, NULL, NULL, NULL },
};


/**
 * @brief V9.3 pattern: per-slot pending flag pro bring-to-front request.
 *
 * Nastaven přes `dbg_extra_disasm_request_focus(idx)` (typicky volaný z
 * `dbg_disasm_show_in_slot` při kliku na decode adresu ve stack monitoru
 * / bookmarks / CPU panelu). Konzumován v `render_one_slot` dual-step
 * patternem - viz V9.3 analýza v stack_window.cpp.
 */
static bool s_focus_pending[DBG_EXTRA_DISASM_COUNT] = { false, false, false, false };


extern "C" void dbg_extra_disasm_request_focus(int slot_idx)
{
    if (slot_idx < 0 || slot_idx >= DBG_EXTRA_DISASM_COUNT)
        return;
    s_focus_pending[slot_idx] = true;
}


/**
 * @brief Vykreslí jedno sekundární okno (= jeden slot).
 *
 * Pri prvnim otevření vytvoří DisassembledView (history=false,
 * animation=false), aplikuje s_persisted_focus[idx] na focus_addr.
 * Pri zavření okna (uživatel klikne na X v okně NEBO toggle z menu
 * vypne flag) instanci uvolní.
 *
 * Při každém framu okna aktualizuje s_persisted_focus[idx] aktuální
 * hodnotou focus_addr - aby cfgmain při shutdownu zapsal poslední stav.
 *
 * @param idx slot index (0..DBG_EXTRA_DISASM_COUNT-1)
 */
static void render_one_slot(int idx)
{
    bool *p_open = &g_gui->showDisasmExtraWindow[idx];
    ExtraDisasmSlot *slot = &s_slots[idx];

    /* Focus-on-open: detekce false -> true transition per slot. Při
     * otevření sekundárního disasm okna (dbg_extra_disasm_show_window)
     * jednorázově nastavíme focus, aby se vykreslilo nad ostatními.
     * Standardní z-order = klik na jiné okno funguje normálně. */
    static bool s_prev_open[DBG_EXTRA_DISASM_COUNT] = { false, false, false, false };
    bool focus_on_open = (*p_open && !s_prev_open[idx]);
    s_prev_open[idx] = *p_open;

    if (!*p_open)
    {
        /* Okno zavřeno: pokud existuje instance, uvolnit. */
        if (slot->view)
        {
            /* Před destrukcí ulož focus do persist (kdyby cfgmain shutdown
             * proběhl s instancí už uvolněnou, hodnota by se ztratila). */
            s_persisted_focus[idx] = (unsigned) dbg_disasm_view_get_focus_addr(slot->view);
            dbg_disasm_view_destroy(slot->view);
            slot->view = NULL;
            slot->focus_applied = false;
        };
        return;
    };

    /* Lazy create. */
    if (!slot->view)
    {
        slot->view = dbg_disasm_view_create(false, SLOT_WINDOW_ID[idx]);
        if (!slot->view)
        {
            /* Alokace selhala - vypneme flag a tichý no-op. */
            *p_open = false;
            return;
        };
        slot->focus_applied = false;
    };

    /* Aplikace persistovaných hodnot (jen jednou per existence instance):
     *   - focus_addr (uint16),
     *   - follow_pc (bool),
     *   - show_tstates (bool).
     * Tyto se přepisují přes setters API DisassembledView, aby vyvolaly
     * případný refresh request. */
    if (!slot->focus_applied)
    {
        uint16_t addr = (uint16_t)(s_persisted_focus[idx] & 0xFFFFu);
        dbg_disasm_view_set_focus_addr(slot->view, addr);
        dbg_disasm_view_set_follow_pc(slot->view, s_persisted_follow_pc[idx] != 0);
        dbg_disasm_view_set_show_tstates(slot->view, s_persisted_show_tstates[idx] != 0);
        slot->focus_applied = true;
    };

    /*
     * Window title - lokalizovaný "Disassembly #N" + stabilní suffix
     * "###disasm_extra_N" zaručující unikátní + překlad-invariant ImGui ID.
     */
    char title[96];
    /* Pozn.: _("Disassembly #%d") prochází gettext klíčem - v anglickém
     * zdroji je "Disassembly #2" / "#3" / atd. registrováno explicit
     * jako menu položky, použijeme tvar bez čísla a number doplníme po. */
    snprintf(title, sizeof(title), "%s #%d%s",
             _("Disassembly"), idx + 2, SLOT_IMGUI_ID_FULL[idx]);

    /* Vychozi velikost - dynamicky vypocet sirky podle obsahu (CalcTextSize
     * + style). FirstUseEver = ulozeny layout v imgui.ini ma prednost.
     * include_table_extras=false: pro sekundarni okna T-states default OFF,
     * tj. tabulka je uzsi. Vyska 600 px = rozumny default.
     *
     * Auto-layout dodá pozici + zachovanou velikost při fresh open. Velikost
     * následně přepíše níže přes SetNextWindowSize (auto-layout setne pos
     * + size obojí FirstUseEver, druhý SetNextWindowSize obnoví správnou
     * šířku dle obsahu). Pozice z auto-layout zůstává. */
    {
        float w = dbg_disasm_view_compute_default_width(false /* without T-states */);
        auto_layout_first_use_portrait(title, w, 600.0f);
        ImGui::SetNextWindowSize(ImVec2(w, 600.0f), ImGuiCond_FirstUseEver);
    }

    /* V9.3 dual-step bring-to-front (krok 1 ze 2): konzumace per-slot
     * pending flagu z `dbg_extra_disasm_request_focus()` PŘED Begin.
     * ImGui-internal z-order zvedne, ale v multi-viewport módu nestačí -
     * krok 2 následuje po Begin (= explicit Platform_SetWindowFocus na
     * cílový viewport). focus_on_open (jednorázový focus při otevření)
     * se aplikuje paralelně. */
    bool was_focus_pending = s_focus_pending[idx];
    if (focus_on_open || s_focus_pending[idx])
        ImGui::SetNextWindowFocus();
    s_focus_pending[idx] = false;

    if (ImGui::Begin(title, p_open, ImGuiWindowFlags_NoCollapse))
    {
        /* V9.3 dual-step bring-to-front (krok 2 ze 2): po Begin víme,
         * do kterého platform viewportu okno patří. Pokud separate
         * viewport (multi-viewport mode + okno mimo main obrys), volej
         * Platform_SetWindowFocus = SDL_RaiseWindow pro OS-level raise.
         * Bez tohoto by SetNextWindowFocus jen blikl title bez reálného
         * raise OS okna nad ostatní.
         * focus_on_open NEpotřebuje krok 2 (ImGui sám platform raise při
         * otevření okna provede); krok 2 je čistě pro request_focus cestu. */
        if (was_focus_pending)
        {
            ImGuiViewport *vp = ImGui::GetWindowViewport();
            ImGuiViewport *main_vp = ImGui::GetMainViewport();
            ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
            if (vp && vp != main_vp && vp->PlatformWindowCreated
                && pio.Platform_SetWindowFocus)
            {
                pio.Platform_SetWindowFocus(vp);
            };
        };

        float h = ImGui::GetContentRegionAvail().y;
        if (h < 1.0f)
            h = 1.0f;
        dbg_disasm_view_render(slot->view, h);
    };
    ImGui::End();

    /*
     * Sync persistovaných hodnot z instance zpět do cfgmain bind targetu,
     * aby shutdown uložil aktuální stav. Cheap (uint16 read + 2× bool).
     */
    if (slot->view)
    {
        s_persisted_focus[idx] = (unsigned) dbg_disasm_view_get_focus_addr(slot->view);
        s_persisted_follow_pc[idx] = dbg_disasm_view_get_follow_pc(slot->view) ? 1 : 0;
        s_persisted_show_tstates[idx] = dbg_disasm_view_get_show_tstates(slot->view) ? 1 : 0;
    };
}


extern "C" void dbg_extra_disasm_show_window(int n, uint16_t addr)
{
    if (n < 2 || n > (1 + DBG_EXTRA_DISASM_COUNT))
        return;

    int idx = n - 2;
    ExtraDisasmSlot *slot = &s_slots[idx];
    bool is_paused = EMULATOR_TEST_PAUSED ? true : false;

    /* Pokud běžící instance, nastav focus přímo. */
    if (slot->view)
    {
        /* Auto-disable Follow PC pokud emu běží - jinak by frame
         * okamžitě přepsal focus_addr na PC, a user by neviděl
         * cílovou adresu. */
        if (!is_paused && dbg_disasm_view_get_follow_pc(slot->view))
        {
            dbg_disasm_view_set_follow_pc(slot->view, false);
            s_persisted_follow_pc[idx] = 0;
        };
        dbg_disasm_view_set_focus_addr(slot->view, addr);
    }
    else
    {
        /* Lazy: otevři okno a nastav persistovaný focus tak, aby ho
         * lazy-create v render_one_slot aplikoval na novou instanci.
         * Pro lazy case není potřeba měnit follow_pc - default je 0
         * pro sekundární okna; pokud user měl persistovanou hodnotu 1,
         * zachovat ji (= měl zapnuté při uložení a chce ji zpět). */
        s_persisted_focus[idx] = (unsigned) addr;
        slot->focus_applied = false;
    };

    /* Vždy zajistíme viditelnost. */
    g_gui->showDisasmExtraWindow[idx] = true;
}


extern "C" void dbg_extra_disasm_render_all(void)
{
    /*
     * Per-frame init UI debuggeru (state init + refresh_tick). Idempotentní
     * v rámci jednoho framu - pokud už proběhl z imgui_debugger_window
     * (= hlavní okno je otevřené), je tohle volání no-op. Pokud hlavní
     * okno není otevřené, sekundární okna by jinak nikdy neměla
     * `g_dbg_ui.refresh.should_refresh = true` a jejich disasm cache by
     * zůstala nulová (= zobrazování samých 0x00). Volá se vždy, i když
     * žádné sekundární okno není otevřené - cena minimální (jeden tick
     * + frame guard check).
     */
    dbg_disassembled_frame_init();

    for (int i = 0; i < DBG_EXTRA_DISASM_COUNT; i++)
    {
        render_one_slot(i);
    };
}


extern "C" void dbg_extra_disasm_register_cfg(void *cmod_void)
{
    if (!cmod_void)
        return;

    st_CFGMODULE *cmod = (st_CFGMODULE *) cmod_void;

    for (int i = 0; i < DBG_EXTRA_DISASM_COUNT; i++)
    {
        /* focus_addr per slot */
        st_CFGELEMENT *elm = cfgmodule_register_new_element(
            cmod, (char *) SLOT_CFG_KEY[i],
            CFGENTYPE_UNSIGNED, 0, 0, 0xFFFFu);
        cfgelement_set_handlers(elm,
                                (void *) &s_persisted_focus[i],
                                (void *) &s_persisted_focus[i]);

        /* follow_pc per slot - default 0 (vypnuto pro sekundární okna) */
        elm = cfgmodule_register_new_element(
            cmod, (char *) SLOT_CFG_KEY_FOLLOW_PC[i],
            CFGENTYPE_BOOL, 0);
        cfgelement_set_handlers(elm,
                                (void *) &s_persisted_follow_pc[i],
                                (void *) &s_persisted_follow_pc[i]);

        /* show_tstates per slot - default 0 (sloupec T-states off) */
        elm = cfgmodule_register_new_element(
            cmod, (char *) SLOT_CFG_KEY_SHOW_TSTATES[i],
            CFGENTYPE_BOOL, 0);
        cfgelement_set_handlers(elm,
                                (void *) &s_persisted_show_tstates[i],
                                (void *) &s_persisted_show_tstates[i]);
    };
}

#else  /* MZ800EMU_CFG_DEBUGGER_ENABLED */

/* Kdyz debugger není zapnutý, poskytujeme prazdne stuby aby linker
 * nereklamoval - main_window.cpp / debugger.c smí volat bezpodmínečně. */
extern "C" void dbg_extra_disasm_render_all(void) {}
extern "C" void dbg_extra_disasm_register_cfg(void *cmod) { (void) cmod; }
extern "C" void dbg_extra_disasm_show_window(int n, uint16_t addr)
{
    (void) n; (void) addr;
}
extern "C" void dbg_extra_disasm_request_focus(int slot_idx) { (void) slot_idx; }

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
