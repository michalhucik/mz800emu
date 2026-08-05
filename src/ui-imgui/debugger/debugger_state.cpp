/*
 * debugger_state.cpp — Sdílený stav UI debuggeru
 *
 * Správa globálního stavu UI debuggeru. Tento modul zajišťuje inicializaci,
 * reset a synchronizaci UI stavu s emulátorem (registry CPU).
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

#include "debugger_state.h"
#include "debugger/debugger.h"
#include "emulator/emulator.h"
#include "mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"
#include "libs/imgui/imgui.h"
#include "dbgapi_helpers.h"

/* Globální instance stavu UI debuggeru */
DebuggerUIState g_dbg_ui;


void debugger_ui_state_init(void)
{
    if (g_dbg_ui.initialized)
        return;

    g_dbg_ui.focus_addr = 0;
    g_dbg_ui.selected_row = 0;
    g_dbg_ui.selected_addr = 0;   /* updatuje se v render funkci dbg_disassembled */
    g_dbg_ui.visible_rows = 30; /* výchozí hodnota, přepočítá se při prvním renderingu */
    g_dbg_ui.history_split_ratio = 0.15f; /* přepočítá se při prvním renderingu na 3 řádky */
    g_dbg_ui.paused_by_debugger_click = false;
    g_dbg_ui.show_pause_info_modal = false;
    dbg_refresh_init();
    g_dbg_ui.initialized = true;
}


void debugger_ui_state_reset(void)
{
    g_dbg_ui.initialized = false;
    debugger_ui_state_init();
}


/*
 * Synchronizace UI stavu s aktuálním stavem CPU.
 * Volá se při přechodu do editačního režimu (pauza) —
 * nastaví focus na aktuální regPC a selectuje první řádek.
 */
void debugger_ui_state_refresh_from_cpu(void)
{
    g_dbg_ui.focus_addr = g_mzarch_main.cpu->pc;
    g_dbg_ui.selected_row = 0;
}

/*
 * dbg_autopause_on_interaction — automatická pauza při interakci v animačním režimu.
 *
 * Volají ji interaktivní prvky debuggeru (disassembly Selectable, budoucí
 * editory registrů atd.), když detekují klik uživatele.
 *
 * Pokud emulace běží, pozastaví ji a nastaví modální info okno.
 * Vrací true = pauza právě proběhla (volající přeskočí normální zpracování),
 *        false = emulace už byla v pauze (volající zpracuje klik normálně).
 */
bool dbg_autopause_on_interaction(void)
{
    if (EMULATOR_TEST_PAUSED)
        return false;

    dbg_ui_pause();
    g_dbg_ui.paused_by_debugger_click = true;
    g_dbg_ui.show_pause_info_modal = true;
    return true;
}


/*
 * dbg_autopause_silent — tichá auto-pauza bez info modálu.
 *
 * Stejná sémantika jako dbg_autopause_on_interaction(), ale NEnastavuje
 * show_pause_info_modal. Použití: in-place edit hodnoty registru v CPU
 * panelu, kde by modální okno přebilo InputText a po kliknutí OK by ho
 * předčasně zavřelo (ImGui by ztratil focus na InputText).
 *
 * paused_by_debugger_click se nastavuje stejně jako u verze s modalem -
 * sémanticky pauznul debugger, jen UI feedback nedáváme.
 */
bool dbg_autopause_silent(void)
{
    if (EMULATOR_TEST_PAUSED)
        return false;

    dbg_ui_pause();
    g_dbg_ui.paused_by_debugger_click = true;
    return true;
}


/*
 * dbg_refresh_init — inicializace refresh kontroleru.
 *
 * Nastaví force_refresh = true, aby se při prvním otevření okna debuggeru
 * vždy provedl refresh dat. Vynuluje last_refresh_time (nastaví se při
 * prvním dbg_refresh_tick()).
 */
void dbg_refresh_init(void)
{
    g_dbg_ui.refresh.last_refresh_time = 0.0;
    g_dbg_ui.refresh.force_refresh = true;
    g_dbg_ui.refresh.should_refresh = false;
}


/*
 * dbg_refresh_request — vyžádání okamžitého refreshe dat.
 *
 * Nastaví one-shot flag force_refresh = true. Při příštím volání
 * dbg_refresh_tick() se should_refresh nastaví na true bez ohledu
 * na časový interval.
 *
 * Typická volání: změna focus_addr (slider, navigace, context menu),
 * přechod do pauzy (step/breakpoint hit).
 */
void dbg_refresh_request(void)
{
    g_dbg_ui.refresh.force_refresh = true;
}


/*
 * dbg_refresh_tick — vyhodnocení pravidel refreshe pro aktuální frame.
 *
 * Volá se 1× na začátku framu z debugger_window.cpp, před renderingem sekcí.
 * Nastaví should_refresh na true/false podle pravidel:
 *
 * 1. force_refresh == true → okamžitý refresh (one-shot, resetuje se)
 * 2. Uplynulo >= DBG_REFRESH_INTERVAL_MS od posledního refreshe
 * 3. Jinak → should_refresh = false (sekce použijí cache)
 *
 * Čas se měří přes ImGui::GetTime() (wall-clock sekundy), nikoliv přes
 * počítání framů — frekvence renderování GUI není konstantní.
 */
void dbg_refresh_tick(void)
{
    double now = ImGui::GetTime();

    /* Pravidlo 1: explicitní požadavek — okamžitý refresh */
    if (g_dbg_ui.refresh.force_refresh)
    {
        g_dbg_ui.refresh.should_refresh = true;
        g_dbg_ui.refresh.force_refresh = false;
        g_dbg_ui.refresh.last_refresh_time = now;
        return;
    };

    /*
     * Pravidlo 2: časový interval — data starší než DBG_REFRESH_INTERVAL_MS.
     * Globální animated_updates flag byl odstraněn jako nadbytečný, takže
     * časový refresh běží vždy když je debugger aktivní (= levná operace,
     * sekce mají vlastní cache).
     */
    {
        double elapsed_ms = (now - g_dbg_ui.refresh.last_refresh_time) * 1000.0;
        if (elapsed_ms >= DBG_REFRESH_INTERVAL_MS)
        {
            g_dbg_ui.refresh.should_refresh = true;
            g_dbg_ui.refresh.last_refresh_time = now;
            return;
        };
    };

    /* Žádné pravidlo nesplněno — sekce použijí cache */
    g_dbg_ui.refresh.should_refresh = false;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
