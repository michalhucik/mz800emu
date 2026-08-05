/*
 * dbgapi_cmdrq.h — CMDRQ kanál: příkazy z UI do Emulátoru
 *
 * Synchronní request/response komunikace přes kruhový buffer (frontu).
 * UI vlákno vkládá příkazy, emulátorové vlákno je zpracovává a vrací odpovědi.
 *
 * Vlastnictví paměti:
 * - data_ptr a result_ptr alokuje a vlastní volající (UI strana)
 * - emulátor pouze čte z data_ptr a zapisuje do result_ptr
 * - po návratu z submit_cmd_sync() je klient zodpovědný za uvolnění
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
#ifndef DBGAPI_CMDRQ_H
#define DBGAPI_CMDRQ_H

#include <stdint.h>
#include <stdbool.h>
#include "app/app_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PŘÍKAZY (CMDRQ)
 *
 * Plochý enum — bez hierarchie. Nové příkazy se přidávají na konec.
 * Horní bit (31) je rezervovaný pro BLOCKING flag.
 * ============================================================================ */

typedef enum en_DBGAPI_CMD
{
    /* --- Řízení emulace --- */
    DBGAPI_CMD_NONE = 0,            /* Bez efektu — ping */
    DBGAPI_CMD_IS_DEBUGGER_ACTIVE,  /* Dotaz na stav debuggeru — result_ptr: bool* */
    DBGAPI_CMD_DEBUGGER_ACTIVATE,   /* Aktivovat debugger (začne se zaznamenávat historie) */
    DBGAPI_CMD_DEBUGGER_DEACTIVATE, /* Deaktivovat debugger */
    DBGAPI_CMD_PAUSE,               /* Pozastavit emulaci */
    DBGAPI_CMD_FORCE_PAUSE,         /* Vynuceně pozastavit (nepřeskočitelné) */
    DBGAPI_CMD_RUN,                 /* Spustit emulaci */
    DBGAPI_CMD_IS_RUNNING,          /* Dotaz na stav emulace — result_ptr: bool* */
    DBGAPI_CMD_STEP_INTO,           /* Jeden krok (Step Into) */
    DBGAPI_CMD_STEP_OVER,           /* Step Over (přes CALL, blokové instrukce) */
    DBGAPI_CMD_RUN_TO,              /* Běh do adresy — data_ptr: uint16_t* (cílová adresa) */
    DBGAPI_CMD_RESET,               /* Reset CPU */

    /* --- CPU registry --- */
    DBGAPI_CMD_GET_REG,      /* Čtení registru — data_ptr: uint8_t* (reg_id), result_ptr: uint16_t* */
    DBGAPI_CMD_SET_REG,      /* Zápis registru — data_ptr: st_DBGAPI_REG_PARAM* */
    DBGAPI_CMD_GET_ALL_REGS, /* Čtení všech registrů — result_ptr: uint16_t[DBGAPI_REG_COUNT] */

    /* --- Paměť --- */
    DBGAPI_CMD_MEM_READ,  /* Čtení bloku paměti — data_ptr: st_DBGAPI_MEM_PARAM*, result_ptr: uint8_t* */
    DBGAPI_CMD_MEM_WRITE, /* Zápis bloku paměti — data_ptr: st_DBGAPI_MEM_PARAM* (včetně dat) */

    /* --- Breakpointy --- */
    DBGAPI_CMD_BP_ADD,    /* Přidání breakpointu — data_ptr: st_DBGAPI_BP_PARAM* */
    DBGAPI_CMD_BP_REMOVE, /* Odebrání breakpointu — data_ptr: st_DBGAPI_BP_PARAM* */
    DBGAPI_CMD_BP_LIST,   /* Seznam breakpointů — result_ptr: st_DBGAPI_BP_LIST_RESULT* */
    /* --- Breakpointy CRUD (V1.7+ migrace UI -> dbgapi) --- */
    DBGAPI_CMD_BP_UPDATE,            /* Selektivní update polí existujícího BP — data_ptr: st_DBGAPI_BP_UPDATE_PARAM* (update_mask řídí co přepsat) */
    DBGAPI_CMD_BP_SET_ENABLED,       /* Quick toggle enabled/disabled — data_ptr: st_DBGAPI_BP_SET_ENABLED_PARAM* */
    DBGAPI_CMD_BP_SET_PARENT,        /* Quick reparent (drag-drop) — data_ptr: st_DBGAPI_BP_SET_PARENT_PARAM* */
    DBGAPI_CMD_BP_CREATE_WITH_INIT,  /* Atomický create + init polí (= add_auto + UPDATE) — data_ptr: st_DBGAPI_BP_UPDATE_PARAM* (id=-1 vstup, naplní handler) */

    /* --- Breakpoint groups CRUD (V1.7+ migrace UI -> dbgapi) --- */
    DBGAPI_CMD_BPGRP_ADD,            /* Přidání nové skupiny — data_ptr: st_DBGAPI_BPGRP_ADD_PARAM* (name + parent, handler naplní id) */
    DBGAPI_CMD_BPGRP_REMOVE,         /* Odebrání skupiny podle ID — data_ptr: st_DBGAPI_BPGRP_REMOVE_PARAM* */
    DBGAPI_CMD_BPGRP_UPDATE,         /* Selektivní update polí skupiny — data_ptr: st_DBGAPI_BPGRP_UPDATE_PARAM* (update_mask) */

    /* --- Disassembly --- */
    DBGAPI_CMD_DASM,        /* Disassembly na adrese — data_ptr: st_DBGAPI_DASM_PARAM*, result_ptr: st_DBGAPI_DASM_RESULT* */
    DBGAPI_CMD_HISTORY_GET, /* Čtení historie instrukcí — result_ptr: buffer pro historii */

    /* --- CPU rozšířený stav (CPU window) --- */
    DBGAPI_CMD_GET_CPU_FLAGS,   /* Čtení doplňkového CPU stavu — result_ptr: st_DBGAPI_CPU_FLAGS* */
    DBGAPI_CMD_SET_CPU_FLAGS,   /* Selektivní zápis CPU stavu (IFF1/IFF2/IM/I/R) — data_ptr: st_DBGAPI_CPU_FLAGS* */
    DBGAPI_CMD_GET_IM2_VECTOR,  /* Čtení IM2 ISR vektoru (PIO-Z80) — result_ptr: st_DBGAPI_IM2_VECTOR* */
    DBGAPI_CMD_GET_RASTER_POS,  /* Čtení pozice rastru a frame counteru — result_ptr: st_DBGAPI_RASTER_POS* */
    DBGAPI_CMD_GET_LAST_INSTR,  /* Poslední dokončená instrukce z history ringu — result_ptr: st_DBGAPI_LAST_INSTR* */
    DBGAPI_CMD_GET_CPU_PANEL_BATCH, /* Agregovaný batch pro CPU panel — data_ptr: uint32_t* (which-mask), result_ptr: st_DBGAPI_CPU_PANEL_BATCH* */
    DBGAPI_CMD_SET_USER_CYCLE_ORIGIN, /* Nastavení počátku User cycle counteru — data_ptr: uint32_t* (= absolutní total_cycles snapshot) */
    DBGAPI_CMD_SET_PIOZ80_INTERRUPT_VECTOR, /* Zápis PIO-Z80 interrupt_vector — data_ptr: st_DBGAPI_PIOZ80_VEC_PARAM* */
    DBGAPI_CMD_MEM_WRITE_CHECKED,   /* Zápis paměti s region check — data_ptr: st_DBGAPI_MEM_WRITE_CHECKED_PARAM* */

    /* --- Stack monitor --- */
    DBGAPI_CMD_STACK_DUMP,          /* Hex dump paměti zásobníku kolem zadané adresy — data_ptr: st_DBGAPI_STACK_DUMP_PARAM* */
    DBGAPI_CMD_STACK_REGIONS_LIST,            /* Snapshot definovaných stack regionů — data_ptr: st_DBGAPI_STACK_REGIONS_LIST_PARAM* */
    DBGAPI_CMD_STACK_REGIONS_ADD,             /* Přidat region — data_ptr: st_DBGAPI_STACK_REGIONS_ADD_PARAM* */
    DBGAPI_CMD_STACK_REGIONS_REMOVE,          /* Odebrat region — data_ptr: st_DBGAPI_STACK_REGIONS_REMOVE_PARAM* */
    DBGAPI_CMD_STACK_REGIONS_RESET_WATERMARK, /* Reset watermark + counters — data_ptr: st_DBGAPI_STACK_REGIONS_REMOVE_PARAM* (sdílí pole index) */
    DBGAPI_CMD_STACK_REGIONS_EDIT,            /* V7: edit existujícího regionu (name/base/limit) — data_ptr: st_DBGAPI_STACK_REGIONS_EDIT_PARAM* */
    DBGAPI_CMD_STACK_HISTORY_ENABLE, /* Zapnout/vypnout SP history recording — data_ptr: st_DBGAPI_STACK_HISTORY_ENABLE_PARAM* */
    DBGAPI_CMD_STACK_HISTORY_GET,    /* Bulk snapshot SP history ring bufferu — data_ptr: st_DBGAPI_STACK_HISTORY_GET_PARAM* */
    DBGAPI_CMD_STACK_HISTORY_RESET,  /* Vyprázdnit ring buffer (recording flag zachován) — bez paramu */

    /* --- Event Viewer (mutant event-viewer, Vlna 1) --- */
    DBGAPI_CMD_EVENTLOG_START,       /* Spustit recording — bez paramu, result: rq->success */
    DBGAPI_CMD_EVENTLOG_STOP,        /* Zastavit recording — bez paramu */
    DBGAPI_CMD_EVENTLOG_CLEAR,       /* Vyprázdnit ring — bez paramu */
    DBGAPI_CMD_EVENTLOG_SET_CAPACITY,/* Změnit capacity ringu — data_ptr: st_DBGAPI_EVENTLOG_CAPACITY_PARAM* */
    DBGAPI_CMD_EVENTLOG_SET_MASK,    /* Změnit categories bitmask — data_ptr: st_DBGAPI_EVENTLOG_MASK_PARAM* */
    DBGAPI_CMD_EVENTLOG_GET_EVENT,   /* Načíst event[idx] z ringu — data_ptr: st_DBGAPI_EVENTLOG_GET_EVENT_PARAM* */

    /* --- Callstack (mutant callstack, Fáze 2B) --- */
    DBGAPI_CMD_GET_CALLSTACK,        /* Snapshot shadow stacku + stats — data_ptr: st_DBGAPI_CALLSTACK_GET_PARAM* */

    /* --- Profiler (mutant profiler V1, fáze F2) --- */
    DBGAPI_CMD_GET_PROFILER,         /* Snapshot agregátoru + stats - data_ptr: st_DBGAPI_PROFILER_GET_PARAM* */
    DBGAPI_CMD_PROFILER_SET_ACTIVE,  /* Set active flag - data_ptr: st_DBGAPI_PROFILER_SET_ACTIVE_PARAM* */
    DBGAPI_CMD_PROFILER_RESET,       /* Reset agregátoru - data_ptr: NULL (no payload) */
    DBGAPI_CMD_PROFILER_EXPORT,      /* Export agregátoru do souboru (mutant mcp-server V1.A.7) - data_ptr: st_DBGAPI_PROFILER_EXPORT_PARAM* */

    /* --- Snapshot (mutant mcp-server V1.A.1) ---
     * MCP klient (= AI agent) potřebuje uložit/načíst .mzs jak na disk, tak
     * inline (= bytes_b64 v JSONL). Dbgapi vrstva delegátem snapshot.h API,
     * volání musí proběhnout v emu vlákně (= safe-point, snapshot vyžaduje
     * paused state).
     *
     * Všechny 4 příkazy sdílí st_DBGAPI_SNAPSHOT_PARAM payload (= rozlišení
     * file vs buffer dělá handler v dbgapi.c podle CMD). */
    DBGAPI_CMD_SNAPSHOT_SAVE_FILE,   /* Uložit snapshot do souboru - data_ptr: st_DBGAPI_SNAPSHOT_PARAM* (filepath + description) */
    DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER, /* Uložit snapshot do bufferu - data_ptr: st_DBGAPI_SNAPSHOT_PARAM* (description, handler vyplní buffer + buffer_size) */
    DBGAPI_CMD_SNAPSHOT_LOAD_FILE,   /* Načíst snapshot ze souboru - data_ptr: st_DBGAPI_SNAPSHOT_PARAM* (filepath) */
    DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER, /* Načíst snapshot z bufferu - data_ptr: st_DBGAPI_SNAPSHOT_PARAM* (buffer + buffer_size) */

    /* --- Symbol DB (mutant mcp-server V1.A.2) ---
     * MCP klient (= AI agent) vystavuje sym_db CRUD přes Tools. Handlery
     * delegují na sym_db_add_user_label / sym_db_remove_user_label /
     * sym_db_lookup_by_name / sym_db_lookup_by_addr / sym_db_iter_*
     * z src/emulator/debugger/symbols/sym_db.h.
     *
     * Reálné sym_db API (V1.7+) pracuje výhradně s SYM_SOURCE_LBL při
     * user-driven přidávání (= žádný kind/source enum se v MCP neukládá).
     * Pole `kind` v MCP payloadu je echo-only (= klient ho dostane zpět
     * v lookup/list response jako "LABEL"), neovlivňuje storage. */
    DBGAPI_CMD_SYMBOL_ADD,           /* Přidat user-defined symbol - data_ptr: st_DBGAPI_SYMBOL_PARAM* */
    DBGAPI_CMD_SYMBOL_REMOVE,        /* Odebrat symbol (by name nebo by addr) - data_ptr: st_DBGAPI_SYMBOL_PARAM* */
    DBGAPI_CMD_SYMBOL_LOOKUP,        /* Vyhledat symbol (by name nebo by addr) - data_ptr: st_DBGAPI_SYMBOL_PARAM* */
    DBGAPI_CMD_SYMBOL_LIST,          /* Iterovat symbols s prefix filterem - data_ptr: st_DBGAPI_SYMBOL_PARAM* */

    /* --- Step out (mutant mcp-server V1.A.3) ---
     * Vyhledá top frame v shadow callstacku (callstack.h API), získá
     * return_addr a nastaví temporary breakpoint + run_to. Asynchronní:
     * po úspěšném submit emu běží, klient pollí get_state. Pokud
     * callstack tracking neaktivní (g_callstack_active == 0) nebo
     * current_depth == 0, handler vrátí success=false a vyplní
     * out_status pro diagnostiku.
     *
     * data_ptr = st_DBGAPI_STEP_OUT_PARAM* (IN: max_cycles informativní,
     * OUT: return_addr + status). */
    DBGAPI_CMD_STEP_OUT,             /* Run until RET z aktuální subroutine - data_ptr: st_DBGAPI_STEP_OUT_PARAM* */

    /* === mutant mcp-server V1.A.5: chip-level Tools ============================
     *
     * Doplnění fault-injection / state-manipulation cmd pro MCP klienta.
     * Volání jdou výhradně přes dbgapi frontu (= EMU thread vykoná), takže
     * nedochází k race condition s běžící Z80 emulací. Z80 IRQ / NMI
     * injekce využívá public API knihovny `libs/cpu-z80/z80.h`.
     */
    DBGAPI_CMD_IO_READ,              /* Z80 IN side-effect read - data_ptr: st_DBGAPI_IO_PARAM* (port IN, value OUT) */
    DBGAPI_CMD_IO_WRITE,             /* Z80 OUT side-effect write - data_ptr: st_DBGAPI_IO_PARAM* */
    DBGAPI_CMD_IRQ_INJECT,           /* Force maskable IRQ z konkrétního zdroje - data_ptr: st_DBGAPI_IRQ_INJECT_PARAM* */
    DBGAPI_CMD_NMI_INJECT,           /* Force NMI - bez parametrů */
    DBGAPI_CMD_MEM_WRITE_FORCE,      /* Raw memory write bez region checku (= ROM override) - data_ptr: st_DBGAPI_MEM_PARAM* */

    /* === mutant mcp-server V1.A.6: Watch + CDL Tools ==========================
     *
     * Watch (4) - wrapper kolem watch.h storage + bp_expr evaluator. Watch je
     * primárně UI-vlákno owned (storage drží UI), proto handlery zde nedělají
     * dlouhé EMU-side operace - pouze proxy. Watch_eval ale potřebuje běžet
     * pod dbgapi sync handlerem, protože konstruuje bp_expr_ctx_t s aktuálním
     * cpu stavem.
     *
     * CDL (4) - wrapper kolem mhmap.{c,h} Memory Heatmap subsystému (FCEUX-style
     * CDL bitmap = counter > 0). Start/Stop modifikuje g_debugger.mhmap_mode,
     * Reset volá mhmap_reset, Export volá mhmap_export(meta_path).
     */
    DBGAPI_CMD_WATCH_ADD,            /* Přidat watch řádek (mode=ADDRESS nebo EXPR_*) - data_ptr: st_DBGAPI_WATCH_ADD_PARAM* */
    DBGAPI_CMD_WATCH_REMOVE,         /* Odebrat watch podle indexu - data_ptr: st_DBGAPI_WATCH_REMOVE_PARAM* */
    DBGAPI_CMD_WATCH_LIST,           /* Vypsat všechny watche - data_ptr: st_DBGAPI_WATCH_LIST_PARAM* (caller alokuje pole) */
    DBGAPI_CMD_WATCH_EVAL,           /* Vyhodnotit watch nebo ad-hoc výraz - data_ptr: st_DBGAPI_WATCH_EVAL_PARAM* */

    DBGAPI_CMD_CDL_START,            /* Spustit CDL recording (mhmap_mode = ALWAYS) - bez paramu */
    DBGAPI_CMD_CDL_STOP,             /* Zastavit CDL recording (mhmap_mode = OFF) - bez paramu */
    DBGAPI_CMD_CDL_RESET,            /* Vynulovat CDL countery (mhmap_reset) - bez paramu */
    DBGAPI_CMD_CDL_EXPORT,           /* Exportovat CDL data do souborů - data_ptr: st_DBGAPI_CDL_EXPORT_PARAM* */

    /* === mutant mcp-server V1.B.1: Media Tools =========================
     *
     * Sjednocený přístup k media operacím (CMT pásek, FDC disk, QD disk,
     * IDE8 HDD obraz). Handler v dbgapi.c dispatchuje podle slot stringu
     * na příslušné hw-generic API:
     *   - cmt   -> cmt_open_file_by_extension / cmt_eject
     *   - cmthack (load_mzf) -> cmthack_load_mzf_filename (instant load)
     *   - fdc0/fdc1 -> fdc_mount_dskfile / fdc_umount
     *   - qd    -> qdisk_open (přes CFGELM) / qdisk_umount
     *   - ide8  -> ide8_drive_open_image / ide8_drive_close_image
     *
     * Pro buffer (bytes_b64) load: MCP dispatch dekóduje base64 do dočasného
     * tmp souboru a předá filepath. Media subsystem nemá in-memory load API.
     */
    DBGAPI_CMD_MEDIA_LOAD_MZF,       /* CMT hack instant load MZF do RAM - data_ptr: st_DBGAPI_MEDIA_PARAM* (slot ignorován, filepath povinný) */
    DBGAPI_CMD_MEDIA_LOAD_BINARY,    /* Raw bytes do Z80 paměti na addr - data_ptr: st_DBGAPI_MEDIA_PARAM* (filepath + load_addr) */
    DBGAPI_CMD_MEDIA_INSERT,         /* Insert obraz do slotu - data_ptr: st_DBGAPI_MEDIA_PARAM* (slot + filepath + ro) */
    DBGAPI_CMD_MEDIA_EJECT,          /* Eject obraz ze slotu - data_ptr: st_DBGAPI_MEDIA_PARAM* (slot) */
    DBGAPI_CMD_MEDIA_STATE,          /* Snapshot stavu všech slotů - data_ptr: st_DBGAPI_MEDIA_STATE_PARAM* */

    /* === mutant mcp-server V1.B.2: Platform + Config Tools =============
     *
     * SETTINGS_GET / SETTINGS_SET vystavují cfgmain INI registry. Klíč
     * má tvar "MODULE/element" (case sensitive). Handler v dbgapi.c
     * dispatchuje na cfgroot_get_module_by_name +
     * cfgmodule_get_element_by_name + cfgelement_get/set_*_value.
     *
     * Whitelist live-settable klíčů a coercion stringu na typ elementu
     * (UNSIGNED/BOOL/TEXT/KEYWORD/FLOAT) řeší MCP dispatch vrstva, ne
     * handler v dbgapi.c (= dbgapi pouze provede operaci na známém
     * elementu typu).
     *
     * PLATFORM_SET záměrně vrací error - mz800/mz700/mz1500 jsou
     * separátní binárky (= compile-time MZARCH). Runtime switch
     * vyžaduje restart s jinou binárkou. Handler v dbgapi.c naplní
     * out_result = -10 a out_active_kind ze stávajícího
     * g_mzarch_platform_numeric.
     *
     * PERIPH_ATTACH / DETACH zapisují cfgmain INI klíče pro periferii.
     * Pro plnou aplikaci je nutný restart emulátoru s novým .ini
     * (= out_requires_restart=1). V1.B.2 neaktivuje hot-swap.
     */
    DBGAPI_CMD_SETTINGS_GET,         /* Čte INI element - data_ptr: st_DBGAPI_SETTINGS_PARAM* (key vstup, out_value výstup) */
    DBGAPI_CMD_SETTINGS_SET,         /* Zapíše INI element - data_ptr: st_DBGAPI_SETTINGS_PARAM* (key + value vstup, out_value zachytí předchozí hodnotu) */
    DBGAPI_CMD_PLATFORM_SET,         /* Pokus o runtime platform switch - data_ptr: st_DBGAPI_PLATFORM_PARAM* (out_result vždy -10 pro V1.B.2) */
    DBGAPI_CMD_PERIPH_ATTACH,        /* Připojit periferii (zápis INI + reset request) - data_ptr: st_DBGAPI_PERIPH_PARAM* */
    DBGAPI_CMD_PERIPH_DETACH,        /* Odpojit periferii (zápis INI + reset request) - data_ptr: st_DBGAPI_PERIPH_PARAM* */

    /* === mutant mcp-server V1.C.1: HID Tools ===========================
     *
     * Injekce klávesnice + joysticku přes PIO8255 vkbd_matrix a g_joy
     * subsystém. Klávesnice press/release operuje nad VIRTUAL keyboard
     * matrix (paralelní k fyzické), takže nekoliduje s reálným SDL
     * scan v iface_keyboard.c. Joystick state se nastavuje přímo do
     * g_joy.dev[].state byte (Sharp MZ active-LOW konvence interně,
     * MCP rozhraní používá user-friendly active-HIGH masku).
     *
     * INPUT_PRESS_KEY a INPUT_RELEASE_KEY drží klávesu trvale (až do
     * dalšího release). INPUT_RELEASE_ALL vyplní celou vkbd matrix
     * 0xff (= vše uvolněné). INPUT_JOY_SET nastaví state, INPUT_JOY_CLEAR
     * uvolní vše. Frame timing a sekvence (= send_keys s delay) řeší
     * MCP dispatch vrstva nad těmito primitivy.
     */
    DBGAPI_CMD_INPUT_PRESS_KEY,      /* Press klávesy ve vkbd matrix - data_ptr: st_DBGAPI_HID_KEY_PARAM* */
    DBGAPI_CMD_INPUT_RELEASE_KEY,    /* Release klávesy - data_ptr: st_DBGAPI_HID_KEY_PARAM* */
    DBGAPI_CMD_INPUT_RELEASE_ALL,    /* Release všech kláves - bez paramu */
    DBGAPI_CMD_INPUT_JOY_SET,        /* Nastavit joystick state - data_ptr: st_DBGAPI_HID_JOY_PARAM* */
    DBGAPI_CMD_INPUT_JOY_CLEAR,      /* Uvolnit joystick - data_ptr: st_DBGAPI_HID_JOY_PARAM* (jen port) */

    /* === mutant mcp-server V1.D.1: Core + CPU extras Resources =========
     *
     * Read-only snapshoty pro Resource endpointy. Všechny jsou pure read
     * (= žádný side effect na emu state). Handlery v dbgapi.c čtou
     * g_emulator / g_memory / g_memext / z80 register přímo se safe-point
     * záruky (= sync submit z UI vlákna do emu vlákna). MCP dispatch
     * vrstva pak data zabalí do JSON odpovědi pro Resource read.
     */
    DBGAPI_CMD_GET_CPU_IM2_VECTOR,   /* Z80 IM2 vector snapshot - data_ptr: st_DBGAPI_CPU_IM2_VECTOR_PARAM* */
    DBGAPI_CMD_GET_CPU_INTERRUPT_BUS,/* Z80 + chip IRQ bus snapshot - data_ptr: st_DBGAPI_CPU_IRQ_BUS_PARAM* */
    DBGAPI_CMD_GET_MEMORY_MAP,       /* Per-platform banking snapshot - data_ptr: st_DBGAPI_MEMORY_MAP_PARAM* */
    DBGAPI_CMD_GET_MEMEXT_INFO,      /* Memory expansion info - data_ptr: st_DBGAPI_MEMEXT_INFO_PARAM* */

    /* === mutant mcp-server V1.D.2.B: Medium debug Resources backing ====
     *
     * Snímky bp_vars a bookmarks pro Resource read. Handlery alokuje
     * caller (= dispatch.c), backend kopíruje obsah úložiště do
     * predalokovaného pole. Truncated flag říká, zda se nevešlo vše.
     *
     * Pozn. bookmarks: backend bookmarks_snapshot() interně serializuje
     * volání pod GMutexem, takže CMD lze submitovat z dispatch (= MCP
     * I/O thread) bezpečně i přes to, že bookmark CRUD storage byl
     * single-thread UI guarantee (viz devdoc/mcp-server/resources-v1d2b).
     */
    DBGAPI_CMD_BP_VARS_LIST,         /* Snapshot bp_vars storage - data_ptr: st_DBGAPI_BP_VARS_LIST_PARAM* */
    DBGAPI_CMD_BOOKMARKS_LIST,       /* Snapshot bookmarks storage - data_ptr: st_DBGAPI_BOOKMARKS_LIST_PARAM* */

    /* === mutant mcp-server V1.D.3.A: IRQ chip Resources backing ========
     *
     * Read-only snapshoty stavu tří IRQ-relevantních chipů: Intel 8255
     * PPI (klávesnice + CMT + PSG audio gate), Intel 8253 CTC (3 časovače,
     * audio + interrupt zdroj) a Zilog Z80 PIO (jen u MZ-800 a MZ-1500;
     * joystick + parallel + IM2 daisy chain). Handlery jen kopírují
     * fixní fields z globálních struktur, žádný side effect.
     *
     * Z80 PIO není přítomen na MZ-700 (HAVE_PIOZ80 == 0); handler v
     * takovém případě vyplní available=false + ostatní fields = 0 a
     * vrátí success=true (klient dostane validní JSON s flagem).
     */
    DBGAPI_CMD_GET_PERIPH_I8255,     /* PPI snapshot - data_ptr: st_DBGAPI_PERIPH_I8255_PARAM* */
    DBGAPI_CMD_GET_PERIPH_I8253,     /* CTC snapshot - data_ptr: st_DBGAPI_PERIPH_I8253_PARAM* */
    DBGAPI_CMD_GET_PERIPH_Z80_PIO,   /* Z80 PIO snapshot - data_ptr: st_DBGAPI_PERIPH_Z80_PIO_PARAM* */

    /* === mutant mcp-server V1.D.3.B: audio chip Resources backing ======
     *
     * Read-only snapshoty audio chipů: SN76489 PSG (mono u MZ-700 -
     * platforma chip nemá; mono nebo stereo u MZ-800 podle runtime
     * stereo flagu; nativně stereo u MZ-1500), AY-3-8910 (= placeholder,
     * chip v emulátoru aktuálně neimplementován) a beeper (= audio
     * cesta CTC0 OUT přes GATE0 + PC0 hradla; není to dedikovaný
     * 1-bit chip jako u ZX Spectra, jen agregovaný snapshot).
     *
     * AY-3-8910 vrátí available=false napříč platformami; klient musí
     * check available před čtením registrů. Beeper vrátí available=true
     * vždy (= signální cesta existuje u všech platforem) plus level a
     * raw bity ctc0_out / gate0 / pc0.
     */
    DBGAPI_CMD_GET_PERIPH_SN76489,   /* PSG snapshot - data_ptr: st_DBGAPI_PERIPH_SN76489_PARAM* */
    DBGAPI_CMD_GET_PERIPH_AY3_8910,  /* AY snapshot (placeholder, chip not implemented) - data_ptr: st_DBGAPI_PERIPH_AY3_8910_PARAM* */
    DBGAPI_CMD_GET_PERIPH_BEEPER,    /* Beeper snapshot - data_ptr: st_DBGAPI_PERIPH_BEEPER_PARAM* */

    /* === mutant mcp-server V1.D.3.C: storage + display Resources backing ===
     *
     * Read-only snapshoty GDG (= per-platforma struct: MZ-800 má 16-color
     * palette přes regPALGRP/regPAL0..3; MZ-700 a MZ-1500 mají 8-entry
     * palette přes mode_color[8]), WD279x FDC (status,
     * regs, 4 drives s mount metadaty), CMT (motor/play state, image
     * filename, polarita) a Quick Disk (motor, virt status, header/body
     * sizes).
     *
     * Per-platforma availability:
     *   - GDG: dostupný u všech tří platforem, ale různý layout palette
     *     a chybějící regBOR/regPALGRP u MZ-700/MZ-1500.
     *   - FDC: CFG_HWEXT_HAVE_FDC=1 u všech tří platforem (default),
     *     ale lze runtime detach.
     *   - CMT: vždy dostupný u všech tří platforem.
     *   - QDisk: CFG_HWEXT_HAVE_QDISK=1 u všech tří platforem (default),
     *     ale lze runtime detach.
     *
     * Image path (CMT + FDC + QD) se vrací jen jako basename (= bez
     * adresářové cesty), security per V1.D.1 precedent.
     */
    DBGAPI_CMD_GET_PERIPH_GDG,       /* GDG snapshot - data_ptr: st_DBGAPI_PERIPH_GDG_PARAM* */
    DBGAPI_CMD_GET_PERIPH_WD1793,    /* WD1793 FDC snapshot - data_ptr: st_DBGAPI_PERIPH_WD1793_PARAM* */
    DBGAPI_CMD_GET_PERIPH_CMT,       /* CMT snapshot - data_ptr: st_DBGAPI_PERIPH_CMT_PARAM* */
    DBGAPI_CMD_GET_PERIPH_QD,        /* Quick Disk snapshot - data_ptr: st_DBGAPI_PERIPH_QD_PARAM* */

    /* === mutant mcp-server V1.D.4: input + frame Resources backing ===
     *
     * Read-only snapshoty input subsystému (klávesnice, joystick) a video
     * framebufferu. Klávesnice: real_matrix (HW scan) + virtual_matrix
     * (vkbd injection) + effective (AND obou) + decode pressed_keys.
     * Joystick: per-port (0 a 1) connected + state bits. Framebuffer:
     * shape metadata (width/height/frame counter/state). Screenshot:
     * raw RGBA buffer (= dispatch expanduje INDEX8 -> RGBA z palety) a
     * volitelný PNG (= aktuálně defer, není PNG knihovna). Text dump:
     * VRAM read pro MZ-700 mode (D000-D3FF text + D800-DBFF attr).
     *
     * Per-platforma availability:
     *   - keyboard_state: vždy dostupný (klávesová matrix existuje
     *     u všech platforem).
     *   - keyboard_matrix_info: vždy dostupný (statický popis tabulky).
     *   - joystick_state: per-port available = (type != JOY_TYPE_NONE);
     *     na MZ-700 jsou joystick porty obvykle NONE.
     *   - framebuffer_info / screenshot_raw: vždy dostupné (framebuffer
     *     existuje u všech platforem).
     *   - screenshot (PNG): aktuálně vždy available=false (= chybí PNG
     *     encoder dependency). Klient používá screenshot_raw.
     *   - video_text_dump: jen pro MZ-700 / MZ-1500 v textovém režimu
     *     (kompilační i runtime detekce); pro MZ-800 v 800 mode
     *     available=false.
     */
    DBGAPI_CMD_GET_INPUT_KEYBOARD_STATE,       /* data_ptr: st_DBGAPI_INPUT_KBD_STATE_PARAM* */
    DBGAPI_CMD_GET_INPUT_KEYBOARD_MATRIX_INFO, /* data_ptr: st_DBGAPI_INPUT_KBD_MATRIX_INFO_PARAM* */
    DBGAPI_CMD_GET_INPUT_JOYSTICK_STATE,       /* data_ptr: st_DBGAPI_INPUT_JOY_STATE_PARAM* */
    DBGAPI_CMD_GET_FRAME_FRAMEBUFFER_INFO,     /* data_ptr: st_DBGAPI_FRAME_FB_INFO_PARAM* */
    DBGAPI_CMD_GET_FRAME_SCREENSHOT_RAW,       /* data_ptr: st_DBGAPI_FRAME_SCREENSHOT_RAW_PARAM* */
    DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG,       /* data_ptr: st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM* */
    DBGAPI_CMD_GET_VIDEO_TEXT_DUMP,            /* data_ptr: st_DBGAPI_VIDEO_TEXT_DUMP_PARAM* */

    /* === mutant mcp-server V1.D.2.C: per-watch snapshot Resource =========
     *
     * Lookup statistik jednoho watch řádku (snap baseline, current, delta,
     * min/max, change_count) podle jména. Handler nečte storage primárně -
     * čte EMU-side thread-safe zrcadlo `watch_emu_cache`, do kterého UI
     * vlákno publikuje jednou per frame. 1-frame stale akceptováno.
     */
    DBGAPI_CMD_GET_WATCH_SNAPSHOT,             /* data_ptr: st_DBGAPI_WATCH_SNAPSHOT_PARAM* */

    /* === mutant mcp-server mzdos-support 0007: Direct memory region read ==
     *
     * Tenké MCP wrappery existujících dbgapi_regions_* funkcí. Klient
     * (= AI agent debug session) potřebuje non-destructive read všech
     * fyzických pamětí (ROM, VRAM, CG-ROM, CG-RAM, MemExt banks, ...)
     * bypass aktuálního Z80 banking. Backend `dbgapi_regions_enumerate`
     * + `dbgapi_regions_read` jsou no-side-effect a bezpečné z emu vlákna.
     */
    DBGAPI_CMD_REGIONS_ENUMERATE,              /* data_ptr: st_DBGAPI_REGIONS_ENUM_PARAM* */
    DBGAPI_CMD_REGIONS_READ,                   /* data_ptr: st_DBGAPI_REGIONS_READ_PARAM* */
    DBGAPI_CMD_REGIONS_WRITE,                  /* data_ptr: st_DBGAPI_REGIONS_WRITE_PARAM* */

    /* === mutant mcp-server (BACKLOG D): emulation speed control =========
     *
     * Read + write emulační rychlosti (= warp pro rychlý boot/load).
     * Speed funkce (emulator_*, customspeed_*) jsou emu-thread owned,
     * proto MUSÍ projít CMDRQ frontou (nikoliv přímé volání z dispatch).
     *
     * GET_SPEED je pure read (= žádný side effect). SET_SPEED je viditelná
     * změna chování (= MCP_ACTION broadcast do Activity logu).
     */
    DBGAPI_CMD_GET_SPEED,                      /* Snapshot rychlosti - data_ptr: st_DBGAPI_GET_SPEED_PARAM* */
    DBGAPI_CMD_SET_SPEED,                      /* Nastavit rychlost - data_ptr: st_DBGAPI_SET_SPEED_PARAM* */

    /* === mutant mcp-server (BACKLOG B): bookmark write =================
     *
     * Write přístup k bookmark storage přes MCP. Dosud existoval jen
     * DBGAPI_CMD_BOOKMARKS_LIST (= read-only snapshot). ADD vytvoří novou
     * záložku a propaguje cmd_origin (= MCP) do storage; REMOVE smaže
     * záložku podle ID. Obě jsou viditelná změna stavu -> MCP_ACTION
     * broadcast do Activity logu (pro cmd_origin == MCP).
     *
     * Bookmark storage CRUD je sice interně mutex-chráněný (viz
     * bookmarks.h Threading), ale CMDRQ pattern držíme kvůli konzistenci
     * s ostatními write cmd a kvůli cmd_origin propagaci + MCP_ACTION.
     */
    DBGAPI_CMD_BOOKMARK_ADD,                   /* Přidat záložku - data_ptr: st_DBGAPI_BOOKMARK_WRITE_PARAM* */
    DBGAPI_CMD_BOOKMARK_REMOVE,                /* Smazat záložku - data_ptr: st_DBGAPI_BOOKMARK_WRITE_PARAM* */

    /* === mutant mcp-server CMT-A: CMT transport + recording + hack ====
     *
     * Ovládání reálné páskové emulace (transport, WAV recording) a
     * okrajového cmthack ROM-patch instant-load toggle. Všechny tři
     * cmd modifikují stav -> MCP_ACTION broadcast pro origin == MCP
     * (Activity log). Funkce cmt_ a cmthack_ běží na emu vlákně, proto
     * jdou výhradně přes CMDRQ frontu.
     *
     * Transport sdružuje play/play_paused/stop/pause/eject do jednoho
     * cmd s action enum (= méně cmd). Record a hack_set samostatně,
     * protože nesou odlišný payload (path / enabled).
     */
    DBGAPI_CMD_CMT_TRANSPORT,                  /* Transport pásky - data_ptr: st_DBGAPI_CMT_TRANSPORT_PARAM* */
    DBGAPI_CMD_CMT_RECORD,                     /* Zahájit WAV nahrávání - data_ptr: st_DBGAPI_CMT_RECORD_PARAM* */
    DBGAPI_CMD_CMT_HACK_SET,                   /* Zapnout/vypnout cmthack ROM patch - data_ptr: st_DBGAPI_CMT_HACK_SET_PARAM* */

    /* === mutant mcp-server CMT-B: vlastnosti CMT + práce s páskou =====
     *
     * Vlastnosti (rychlost, polarita, cpu boost, mzfsize check) sjednoceny
     * do jednoho SET_PROPERTY cmd s property enum (= méně boilerplate).
     * Open je CMT-specifický (= respektuje play_immediately). Práce s
     * páskou (seek, per-blok speed) pro SIMPLE_TAPE multi-blok containery.
     * Tape list je read-only (= backing pro resource emulator://periph/
     * cmt/tape). Všechny funkce cmt a cmtext běží na emu vlákně, proto
     * jdou výhradně přes CMDRQ frontu.
     */
    DBGAPI_CMD_CMT_SET_PROPERTY,               /* Nastavit vlastnost CMT - data_ptr: st_DBGAPI_CMT_SET_PROPERTY_PARAM* */
    DBGAPI_CMD_CMT_OPEN,                       /* Otevřít CMT soubor (+ play) - data_ptr: st_DBGAPI_CMT_OPEN_PARAM* */
    DBGAPI_CMD_CMT_TAPE_SEEK,                  /* Seek na blok pásky - data_ptr: st_DBGAPI_CMT_TAPE_SEEK_PARAM* */
    DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED,           /* Per-blok cmt speed - data_ptr: st_DBGAPI_CMT_TAPE_BLOCK_SPEED_PARAM* */
    DBGAPI_CMD_CMT_TAPE_LIST,                  /* Výpis bloků pásky (read) - data_ptr: st_DBGAPI_CMT_TAPE_LIST_PARAM* */

    /* === mutant mcp-debug-control 0017 FÁZE 1: Tracking lifecycle ======
     *
     * Zrcadlí DBGAPI_CMD_CDL_* pro trace-suite subsystémy (cputrack/iorqlog/
     * intlog/hwlog). Volba kanálu je v st_DBGAPI_TRACE_PARAM.channel. START
     * nastaví mode kanálu na ALWAYS a triggerne callback recompute (analogie
     * mhmap_set_mode), STOP nastaví OFF, RESET uzavře+znovuotevře segment
     * (+ collapse reset u cputrack), SAVE uzavře/přesměruje segment na path.
     * Přidáno na KONEC enumu kvůli stabilitě číselných hodnot existujících
     * příkazů. */
    DBGAPI_CMD_TRACE_START,                    /* Spustit trace recording kanálu - data_ptr: st_DBGAPI_TRACE_PARAM* */
    DBGAPI_CMD_TRACE_STOP,                     /* Zastavit trace recording kanálu - data_ptr: st_DBGAPI_TRACE_PARAM* */
    DBGAPI_CMD_TRACE_RESET,                    /* Vynulovat trace segment kanálu - data_ptr: st_DBGAPI_TRACE_PARAM* */
    DBGAPI_CMD_TRACE_SAVE,                     /* Uložit/přesměrovat trace segment - data_ptr: st_DBGAPI_TRACE_PARAM* */

    /* === mutant mcp-debug-control request 0021: deterministický frame-bounded run ===
     *
     * Spustí emulaci s cílem zastavit ji DETERMINISTICKY přesně na N-té frame
     * hranici. Na rozdíl od dvojice RUN + async PAUSE z dispatch vlákna (= emu
     * mezi "counter dosáhl N" a zpracováním PAUSE běžel dál o wall-clock-závislý
     * počet instrukcí -> nedeterministický cycle bod) se emu pausne sám v hot
     * loopu, jakmile g_gdg.total_elapsed.screens dosáhne cíle. Vzor je
     * g_debugger.step_call (deterministický stop po N instrukcích).
     *
     * Handler nastaví g_debugger.run_frames_target = screens + N a
     * run_frames_active = 1, pak unpausne emulaci. Přidáno na KONEC enumu kvůli
     * stabilitě číselných hodnot existujících příkazů. */
    DBGAPI_CMD_RUN_FRAMES,                     /* Frame-bounded run (deterministický stop po N framech) - data_ptr: int* (N >= 1) */

    /* Přepočet debugger callbacků + active flagů (= mzarch_platform_fn_debugger_state_changed)
     * na EMU vlákně (per-frame safe-point), aby ho UI vlákno nevolalo přímo.
     * Nutné pro trace-suite: stop kanálu uvolní writer buffer (tlog_writer_close);
     * pokud to běží na UI vlákně souběžně s emu-thread tlog_writer_append, vznikne
     * use-after-free race (memcpy do uvolněné paměti). UI si předem nastaví
     * mode/flagy (atomický int zápis) a pak submitne tento příkaz. Bez parametru. */
    DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE,

} en_DBGAPI_CMD;

/* ============================================================================
 * BLOCKING FLAG
 *
 * Pokud je nastaven v horním bitu příkazu, emulátor po zpracování tohoto
 * příkazu nepřechází zpět do normálního režimu, ale okamžitě kontroluje
 * frontu na další příkaz. Umožňuje transakční dávky:
 *
 *   CMD_GET_REG | BLOCKING  →  CMD_GET_REG | BLOCKING  →  CMD_GET_REG
 *
 * Poslední příkaz v dávce nemá BLOCKING flag — emulátor se vrátí do normálu.
 * ============================================================================ */

#define DBGAPI_CMDFLAG_BLOCKING (1 << 31)

/* Maska pro extrakci příkazu bez flagů */
#define DBGAPI_CMD_MASK 0x0000FFFF

/* ============================================================================
 * STAVY ZPRACOVÁNÍ PŘÍKAZU
 * ============================================================================ */

typedef enum en_DBGAPI_CMDSTATE
{
    DBGAPI_CMDSTATE_NONE = 0,  /* Slot je volný */
    DBGAPI_CMDSTATE_PENDING,   /* Příkaz čeká na zpracování emulátorem */
    DBGAPI_CMDSTATE_PROCESSED, /* Příkaz byl zpracován — odpověď je připravena */
} en_DBGAPI_CMDSTATE;

/* ============================================================================
 * STAV ODPOVĚDI — ochranný příznak
 *
 * Emulátor nastaví ENDING, když se chystá ukončit.
 * UI vlákno pak ví, že nemá smysl posílat další příkazy.
 * ============================================================================ */

typedef enum en_DBGAPI_CMDREPLY_STATE
{
    DBGAPI_CMDREPLY_STATE_NONE = 0,
    DBGAPI_CMDREPLY_STATE_ENDING, /* Emulátor se ukončuje - nekomunikovat */
} en_DBGAPI_CMDREPLY_STATE;

/* ============================================================================
 * ZDROJ PŘÍKAZU (CMD_ORIGIN) - audit + cooperative UX
 *
 * Identifikace odesílatele konkrétního CMDRQ. Pro budoucí cooperative UX
 * vrstvu mezi GUI uživatelem a MCP klientem (= AI agent / external tool),
 * která musí rozlišit kdo akci inicioval (zobrazit notifikaci, logovat
 * do action logu, podléhá policy ohledně auto-pause atd.).
 *
 * Pro pouhý dbgapi refactor v V-1.3 stačí přidat field a propagovat ho
 * frontou; samotná akce (= broadcast MSG_MCP_ACTION) se dnes triggeruje
 * jen pro origin = MCP. Ostatní origin jsou dnes equivalentní (= bez
 * vedlejšího efektu), ale GUI panel "Activity Log" v budoucnosti smí
 * je rozlišit.
 *
 * Hodnoty:
 *   USER     - GUI user interakce (klik, hotkey, menu) - default backward compat
 *   MCP      - MCP klient přes wrapper (= AI agent / external tool)
 *   TEST     - Test framework (ctest, integration tests)
 *   INTERNAL - Emu sám (= reset z fatal error, auto-init, post-load fixup)
 * ============================================================================ */

typedef enum en_DBGAPI_CMD_ORIGIN
{
    DBGAPI_CMD_ORIGIN_USER = 0,     /* Default - GUI user akce (klik, hotkey, menu) */
    DBGAPI_CMD_ORIGIN_MCP = 1,      /* MCP klient přes wrapper (= AI agent, external tool) */
    DBGAPI_CMD_ORIGIN_TEST = 2,     /* Test framework (ctest, integration tests) */
    DBGAPI_CMD_ORIGIN_INTERNAL = 3, /* Emu sám (reset z fatal error, auto-init, post-load fixup) */
} en_DBGAPI_CMD_ORIGIN;

/* ============================================================================
 * SLOT CMDRQ — jeden příkaz ve frontě
 *
 * Každý slot má vlastní mutex a condition variable pro synchronizaci
 * mezi UI vláknem (čekající na odpověď) a emulačním vláknem (zpracovávající
 * příkaz).
 *
 * Životní cyklus:
 * 1. UI zamkne slot->mutex, nastaví cmd/data_ptr/result_ptr, cmd_state=PENDING
 * 2. UI čeká na slot->cond (blokuje se)
 * 3. EMU zpracuje příkaz, zapíše result_ptr/success, cmd_state=PROCESSED
 * 4. EMU signalizuje slot->cond → UI se probudí
 * 5. UI přečte výsledek, nastaví cmd_state=NONE → slot volný
 * ============================================================================ */

typedef struct st_DBGAPI_CMDRQ
{
    en_DBGAPI_CMDSTATE cmd_state;     /* Stav zpracování příkazu */
    en_DBGAPI_CMD cmd;                /* Příkaz + případný BLOCKING flag v horním bitu */
    en_DBGAPI_CMD_ORIGIN cmd_origin;  /* Zdroj příkazu (USER/MCP/TEST/INTERNAL) - audit + cooperative UX */
    void *data_ptr;                   /* Vstupní data od klienta (vlastní klient) */
    void *result_ptr;                 /* Buffer pro odpověď (vlastní klient) */
    bool success;                     /* Výsledek: true = úspěch, false = chyba */
    app_mutex_t *mutex;               /* Per-slot mutex */
    app_cond_t *cond;                 /* Per-slot condition variable */
} st_DBGAPI_CMDRQ;

/* ============================================================================
 * FRONTA CMDRQ — kruhový buffer
 *
 * Implementace: ring buffer s pevnou velikostí.
 * Přístup k head/tail je chráněn queue_mutex.
 * Čekající emulátor (v pause) se probudí přes queue_cond.
 *
 * head = pozice dalšího příkazu ke zpracování (EMU čte)
 * tail = pozice pro vložení nového příkazu (UI zapisuje)
 * Fronta je prázdná pokud head == tail.
 * Fronta je plná pokud (tail + 1) % SIZE == head.
 * ============================================================================ */

#define DBGAPI_CMDRQ_QUEUE_SIZE 16

typedef struct st_DBGAPI_CMDRQ_QUEUE
{
    st_DBGAPI_CMDRQ cmdrq[DBGAPI_CMDRQ_QUEUE_SIZE]; /* Pole slotů */
    int head;                                       /* Index čtení (EMU) */
    int tail;                                       /* Index zápisu (UI) */
    app_mutex_t *queue_mutex;                       /* Mutex pro přístup k head/tail */
    app_cond_t *queue_cond;                         /* Signalizace: nový příkaz ve frontě */
    en_DBGAPI_CMDREPLY_STATE reply_state;           /* Ochranný příznak pro ukončení */
} st_DBGAPI_CMDRQ_QUEUE;

/* ============================================================================
 * PARAMETRICKÉ STRUKTURY PRO PŘÍKAZY
 *
 * Struktury, které klient alokuje a předává přes data_ptr/result_ptr.
 * Definovány zde, protože je potřebují obě strany (UI i EMU).
 * ============================================================================ */

/**
 * @brief Počet Z80 registrů v poli pro CMD_GET_ALL_REGS.
 *
 * Odpovídá počtu hodnot enum z80_reg_t (Z80_REG_AF..Z80_REG_IR).
 * Pořadí v poli je dle z80_reg_t (= 0=AF, 1=BC, 2=DE, 3=HL,
 * 4=AF', 5=BC', 6=DE', 7=HL', 8=IX, 9=IY, 10=SP, 11=PC, 12=WZ,
 * 13=IR). Caller musí alokovat uint16_t[DBGAPI_REG_COUNT].
 */
#define DBGAPI_REG_COUNT 14

/* Parametr pro CMD_SET_REG */
typedef struct st_DBGAPI_REG_PARAM
{
    uint8_t reg_id; /* Identifikátor registru */
    uint16_t value; /* Nová hodnota */
} st_DBGAPI_REG_PARAM;

/* Parametr pro CMD_MEM_READ / CMD_MEM_WRITE */
typedef struct st_DBGAPI_MEM_PARAM
{
    uint16_t addr; /* Počáteční adresa */
    uint16_t len;  /* Délka bloku v bajtech */
    uint8_t *buf;  /* Buffer pro data (pro WRITE: vstupní data, pro READ: výstup) */
} st_DBGAPI_MEM_PARAM;

/* Parametr pro CMD_BP_ADD / CMD_BP_REMOVE */
typedef struct st_DBGAPI_BP_PARAM
{
    uint16_t addr; /* Adresa breakpointu */
    int id;        /* ID breakpointu (kladné číslo) */
} st_DBGAPI_BP_PARAM;

/* Parametr pro CMD_DASM */
typedef struct st_DBGAPI_DASM_PARAM
{
    uint16_t addr; /* Počáteční adresa disassembly */
    int count;     /* Počet instrukcí k disassemblování */
} st_DBGAPI_DASM_PARAM;

/* Výsledek CMD_DASM — jedna instrukce */
typedef struct st_DBGAPI_DASM_RESULT
{
    uint16_t addr;     /* Adresa instrukce */
    uint8_t bytes[4];  /* Bajty instrukce */
    int num_bytes;     /* Délka instrukce (1–4) */
    char mnemonic[64]; /* Textová mnemonika */
} st_DBGAPI_DASM_RESULT;

/**
 * @brief Doplňkový stav CPU pro CPU window (over GET_ALL_REGS).
 *
 * Drží interrupt flip-flops, IM mód, HALT stav, čekající přerušení,
 * EI delay flag, interní Q registr a cycle countery. Pole nejsou
 * pokryta v Z80_REG_* enumu - GET_ALL_REGS je nedoručuje.
 *
 * Update_mask je rezervováno pro budoucí SET_CPU_FLAGS (selektivní zápis
 * vybraných fieldů). V V0 (read-only refresh) se neevaluuje.
 *
 * @invariant im je vždy 0, 1 nebo 2 po dispatch.
 * @invariant iff1, iff2, halted, int_pending, nmi_pending, ei_delay
 *            jsou 0 nebo 1.
 */
typedef struct st_DBGAPI_CPU_FLAGS
{
    uint8_t iff1;          /**< Master interrupt enable (0/1) */
    uint8_t iff2;          /**< Shadow IFF1 - kopie pro NMI/RETN (0/1) */
    uint8_t im;            /**< Interrupt mode (0/1/2) */
    uint8_t halted;        /**< CPU v HALT instrukci (0/1) */
    uint8_t int_pending;   /**< Čekající INT (0/1) */
    uint8_t nmi_pending;   /**< Čekající NMI (0/1) */
    uint8_t ei_delay;      /**< EI delay flag (po EI 1 instrukce odklad) */
    uint8_t q;             /**< Interní Q registr (F z poslední ALU operace) */
    uint32_t total_cycles; /**< Celkové T-stavy od resetu */
    uint32_t cycles;       /**< T-stavy v aktuálním frame */
    int32_t op_tstate;     /**< T-stavy od začátku aktuální instrukce */
    uint16_t update_mask;  /**< Bity DBGAPI_CPU_FLAGS_UM_* - selektivní SET */
    uint8_t i_reg;         /**< Interrupt Vector register I (8-bit) */
    uint8_t r_reg;         /**< Memory Refresh register R (8-bit) */
} st_DBGAPI_CPU_FLAGS;

/**
 * @brief Bity update_mask pro DBGAPI_CMD_SET_CPU_FLAGS.
 *
 * Caller v UI naplní jen ty fieldy struct, které chce zapsat, a v
 * update_mask nastaví odpovídající bity. Handler v dbgapi.c projde mask
 * a aplikuje jen vyžádané změny. Ostatní fieldy struktury jsou ignorovány.
 *
 * Bezpečnost: zápis IFF/IM/I/R během běžící emulace je v principu race
 * (modifikuje CPU stav mezi instrukcemi). Caller (= UI) ručí za pause
 * stav (typicky přes dbg_autopause_silent před commit). Handler sám
 * neguarduje - běží v emu vlákně v safepointu mezi instrukcemi, takže
 * atomicita zápisu je zajištěna implicitně.
 */
#define DBGAPI_CPU_FLAGS_UM_IFF1 (1u << 0)  /**< Zapsat iff1 (0/1) */
#define DBGAPI_CPU_FLAGS_UM_IFF2 (1u << 1)  /**< Zapsat iff2 (0/1) */
#define DBGAPI_CPU_FLAGS_UM_IM   (1u << 2)  /**< Zapsat im (0/1/2 - jine hodnoty rejected) */
#define DBGAPI_CPU_FLAGS_UM_I    (1u << 3)  /**< Zapsat i_reg (8-bit) */
#define DBGAPI_CPU_FLAGS_UM_R    (1u << 4)  /**< Zapsat r_reg (8-bit) */

/**
 * @brief IM2 ISR vektor pro CPU window (jen platformy s PIO-Z80).
 *
 * Drží stav potřebný k dekódování, kam by Z80 skočil v případě IM 2
 * interruptu obslouženého PIO-Z80 (MZ-800, MZ-1500). Platforma bez
 * PIO-Z80 (MZ-700) vrátí `available = 0` a ostatní fieldy nejsou
 * definované.
 *
 * Composition v IM 2:
 *   - I  ............ horní byte adresy
 *   - vector_byte ... dolní byte (PIO-Z80 dá na bus přes IORQ/INT)
 *   - isr_table_addr = (I << 8) | vector_byte
 *   - isr_target_addr = MEM[isr_table_addr] | (MEM[isr_table_addr+1] << 8)
 *
 * Vector_byte je definován jen pokud `pio_irq_pending == 1`. Bez
 * čekajícího interruptu PIO-Z80 sice vrátí 0x00 (viz pioz80.c
 * `pioz80_interrupt_ack_im2_cb`), ale je to implementačně specifické -
 * UI to musí signalizovat ("no IRQ pending").
 *
 * PIO-Z80 IRQ chain priorita: port A nad port B (= pioz80.c iteruje
 * `for port_id = PORT_A; port_id < PORT_COUNT`). pio_source signalizuje,
 * který port by vektor poskytl.
 *
 * @invariant Pokud available == 0, ostatní pole se nezkoumají.
 * @invariant im in {0, 1, 2}.
 * @invariant pio_source in {0=PIO-A, 1=PIO-B}; význam jen při
 *            pio_irq_pending == 1.
 */
typedef struct st_DBGAPI_IM2_VECTOR
{
    uint8_t  available;        /**< 0 = arch nemá PIO-Z80 (MZ-700) */
    uint8_t  im;               /**< Aktuální IM mód (0/1/2) */
    uint8_t  i_register;       /**< Horní byte vektoru = registr I */
    uint8_t  vector_byte;      /**< Dolní byte = co by PIO-Z80 dal na bus */
    uint16_t isr_table_addr;   /**< (I << 8) | vector_byte */
    uint16_t isr_target_addr;  /**< Dereferenced = MEM[isr_table_addr] little-endian */
    uint8_t  pio_irq_pending;  /**< 0/1 - PIO-Z80 IRQ pending? */
    uint8_t  pio_source;       /**< 0=PIO-A, 1=PIO-B (jen pokud pending) */
} st_DBGAPI_IM2_VECTOR;


/**
 * @brief Pozice rastru a frame counter pro CPU window "Cycles & raster".
 *
 * Drží aktuální stav GDG rasteru a Z80 cycle counterů, aby UI mohlo
 * vykreslit kde právě paprsek stojí a kolik T-states uplynulo.
 *
 * @field frame_number  Pořadové číslo aktuálního snímku
 *                       (= g_gdg.total_elapsed.screens).
 * @field scanline      Aktuální raster row = g_gdg.beam_row
 *                       (0..VIDEO_SCREEN_HEIGHT-1, pro MZ-800 PAL 312).
 * @field column_pixel  Pixel sloupec v aktuálním scanline
 *                       (= VIDEO_GET_SCREEN_COL z `g_gdg.total_elapsed.ticks`).
 * @field total_cycles  Kumulativní T-stavy Z80 od resetu
 *                       (= cpu->total_cycles).
 * @field frame_cycles  T-stavy v rámci aktuálního snímku
 *                       (= cpu->cycles).
 */
typedef struct st_DBGAPI_RASTER_POS
{
    uint32_t frame_number;
    uint16_t scanline;
    uint16_t column_pixel;
    uint32_t total_cycles;
    uint32_t frame_cycles;
} st_DBGAPI_RASTER_POS;


/**
 * @brief Poslední dokončená Z80 instrukce z debugger history ringu.
 *
 * Vrací nejnovější záznam g_debugger_history.row[position & POSMASK] +
 * délku instrukce dopočítanou disassemblerem. Pokud history není
 * aktivní (= TEST_DEBUGGER_CPUHIST_ACTIVE = 0) nebo zatím nezaznamenala
 * žádnou instrukci, vrací valid=0.
 *
 * @field valid        1 = záznam je platný, 0 = history prázdná/neaktivní
 * @field addr         Adresa instrukce (PC v okamžiku M1 startu)
 * @field bytes        Bajty instrukce (až 4) - obsah row.byte[]
 * @field length       Délka instrukce (1..4), dopočítaná z disassembleru
 */
typedef struct st_DBGAPI_LAST_INSTR
{
    uint8_t  valid;
    uint8_t  length;
    uint16_t addr;
    uint8_t  bytes[4];
} st_DBGAPI_LAST_INSTR;


/**
 * @brief Bitová pole pro CMD_GET_CPU_PANEL_BATCH which-mask.
 *
 * Caller alokuje uint32_t mask, OR'd flagy podle toho které sekce panelu
 * potřebují aktuální data, a předá pres data_ptr. Emu handler vrací jen
 * vyžádané fieldy do st_DBGAPI_CPU_PANEL_BATCH (per-section valid flagy
 * indikují co bylo skutečně naplněno).
 *
 * Účel: per-section gating - pokud je v UI sekce (IM2/raster/last_instr)
 * collapsed, nemusí jít do baseline batch payloadu, čímž se zredukuje
 * práce na emu straně.
 *
 * Registry + flags se ptají vždy (= core panel, levný read), proto pro
 * ně samostatný flag neexistuje - jsou součástí každého batch volání.
 */
#define DBGAPI_CPU_PANEL_WANT_IM2          (1u << 0) /**< IM2 ISR vector */
#define DBGAPI_CPU_PANEL_WANT_RASTER       (1u << 1) /**< Cycles & raster pos */
#define DBGAPI_CPU_PANEL_WANT_LAST_INSTR   (1u << 2) /**< Last instruction */


/**
 * @brief Agregovaný snapshot dat pro CPU panel v jediném round-tripu.
 *
 * Eliminuje 5 separátních sync requestů (= 5 čekání na emu safepoint
 * při běžící emulaci) za 1 batch call. UI vlákno tak ztratí jen 1× čas
 * na safepoint místo 5×, což odstraňuje viditelný lag CPU panelu při
 * běžícím emulátoru a zrychluje paint i v pauze (= drain zpoždění
 * proti UI tickeru se kumuluje pomaleji).
 *
 * Caller:
 *  - Alokuje strukturu (typicky stack lokál)
 *  - Předá pres result_ptr
 *  - Předá uint32_t mask přes data_ptr (DBGAPI_CPU_PANEL_WANT_*)
 *  - Po návratu čte regs + flags vždy, ostatní jen pokud
 *    odpovídající *_valid je 1
 *
 * Per-section valid flagy reflektují obě podmínky:
 *  1. Sekce byla vyžádána ve which-mask
 *  2. Handler ji úspěšně naplnil (analogie current per-cmd success
 *     samostatných GET_* příkazů)
 *
 * @invariant regs[] je vždy naplněn, regs_valid = 1.
 * @invariant flags je vždy naplněn, flags_valid = 1.
 * @invariant Pokud byl bit DBGAPI_CPU_PANEL_WANT_X = 0 ve which masce,
 *            x_valid bude 0 a obsah x je nedefinovaný (zachová caller
 *            cache).
 */
typedef struct st_DBGAPI_CPU_PANEL_BATCH
{
    uint32_t which;                 /**< Kopie which-masky (echo pro debug) */

    /* Vždy naplněné (core panel) */
    uint16_t regs[14];              /**< Z80 registry, indexace dle z80_reg_t. Velikost = DBGAPI_REG_COUNT */
    st_DBGAPI_CPU_FLAGS flags;      /**< IFF/IM/HALT/.../cycles */
    uint8_t  regs_valid;            /**< 1 = regs naplněny */
    uint8_t  flags_valid;           /**< 1 = flags naplněny */

    /* V3.1 core fieldy (vždy naplněné, levné čtení). Umístěné mimo
     * st_DBGAPI_RASTER_POS aby UI mohlo počítat Frame cyc / User cyc
     * i pokud "Cycles & raster" sekce není expandovaná (bez ní by
     * raster substruct zůstal valid=0 a frame_number nedostupný). */
    uint32_t frame_number;          /**< Pořadové číslo aktuálního snímku (g_gdg.total_elapsed.screens) */
    uint32_t user_cycle_origin;     /**< Snapshot total_cycles z g_debugger.user_cycle_origin pro UI Frame/User cyc */

    /* Volitelné (per-section gating) */
    st_DBGAPI_IM2_VECTOR im2;       /**< Naplněno jen pokud WANT_IM2 a im2_valid=1 */
    uint8_t              im2_valid;
    st_DBGAPI_RASTER_POS raster;    /**< Naplněno jen pokud WANT_RASTER a raster_valid=1 */
    uint8_t              raster_valid;
    st_DBGAPI_LAST_INSTR last_instr;/**< Naplněno jen pokud WANT_LAST_INSTR a last_instr_valid=1 */
    uint8_t              last_instr_valid;

    /* === V3.3 core fieldy: PIO-Z80 interrupt vector + ISR target ===
     *
     * Vždy plněné při HAVE_PIOZ80 (= MZ-800, MZ-1500). Pro MZ-700
     * (HAVE_PIOZ80 == 0) zůstává has_pioz80 = 0 a ostatní fieldy = 0.
     *
     * Účel: CPU panel zobrazuje pod IFF2 dva řádky VECA/ISRA a VECB/ISRB.
     * UI sekce existuje jen pro architektury s PIO-Z80 - has_pioz80
     * řídí viditelnost.
     *
     * Composition (pro každý port):
     *   - VEC*  = (cpu->i << 8) | (port[*].interrupt_vector & 0xFE)
     *   - ISR*  = MEM[VEC*] | (MEM[VEC*+1] << 8)
     *
     * pio_int_vec_a/b drží surovou hodnotu interrupt_vector registru
     * jednotlivých portů (bez & 0xFE maskování) - UI ji nepotřebuje k
     * zobrazení (VEC* už je hotová), ale slouží jako baseline při edit
     * commitu k otestování že se hodnota nezměnila mezi refreshem a
     * submitem (zatím nepoužité, rezerva). */
    uint8_t  has_pioz80;            /**< 1 = arch má PIO-Z80, plní VEC/ISR */
    uint8_t  pio_int_vec_a;         /**< Raw g_pioz80.port[A].interrupt_vector */
    uint8_t  pio_int_vec_b;         /**< Raw g_pioz80.port[B].interrupt_vector */
    uint16_t veca;                  /**< (i_reg<<8) | (pio_int_vec_a & 0xFE) */
    uint16_t vecb;                  /**< (i_reg<<8) | (pio_int_vec_b & 0xFE) */
    uint16_t isra;                  /**< MEM[veca] little-endian */
    uint16_t isrb;                  /**< MEM[vecb] little-endian */
} st_DBGAPI_CPU_PANEL_BATCH;


/**
 * @brief Parametr pro CMD_SET_PIOZ80_INTERRUPT_VECTOR.
 *
 * Zápis hodnoty interrupt_vector registru pro vybraný port PIO-Z80
 * (A nebo B). Handler v dbgapi.c maskuje bit 0 ven (= IVW spec: bit 0
 * vždy 0). Pokud port_id mimo {0, 1}, handler vrátí success = false.
 *
 * Na MZ-700 (HAVE_PIOZ80 == 0) handler vrátí success = false (= PIO-Z80
 * v této architektuře neexistuje).
 *
 * @field port_id      0 = PIOZ80_PORT_A, 1 = PIOZ80_PORT_B
 * @field vector_byte  Nová hodnota (bit 0 se vynuluje handlerem)
 */
typedef struct st_DBGAPI_PIOZ80_VEC_PARAM
{
    uint8_t port_id;
    uint8_t vector_byte;
} st_DBGAPI_PIOZ80_VEC_PARAM;

/**
 * @brief Parametr pro CMD_MEM_WRITE_CHECKED.
 *
 * Zápis bloku do paměti s ověřením, že každá dotčená adresa je v
 * zapisovatelném regionu. Pokud kterákoliv adresa padne do ROM
 * (ROM_LOW/HIGH), CG-ROM, prohibited nebo unmapped (případně MZ-800
 * native VRAM_I/II - viz handler), žádný bajt se nezapíše a handler
 * vrátí success = 0 + first_failed_addr.
 *
 * Použití: editor ISR target v CPU panelu chce zapsat 2 bajty na adresu
 * VEC*. Pokud VEC* ukazuje do ROM (typické tabulky vektorů!), zápis by
 * tiše propadl - banking memory_write_byte ROM regiony ignoruje. Místo
 * toho UI dostane explicit failure a může uživateli ukázat warning.
 *
 * @field addr               Počáteční adresa zápisu
 * @field length             Počet bajtů
 * @field data               Vstupní data (caller vlastní; handler jen čte)
 * @field success            (OUT) 1 = celý blok zapsán, 0 = žádný bajt zapsán
 * @field first_failed_addr  (OUT) Adresa, na které check selhal (pouze při success=0)
 * @field first_failed_kind  (OUT) en_MEMMAP_REGION_KIND padlé adresy
 *                                 (uint8_t, pouze při success=0)
 */
typedef struct st_DBGAPI_MEM_WRITE_CHECKED_PARAM
{
    uint16_t addr;
    uint16_t length;
    const uint8_t *data;
    uint8_t  success;
    uint16_t first_failed_addr;
    uint8_t  first_failed_kind;
} st_DBGAPI_MEM_WRITE_CHECKED_PARAM;

/**
 * @brief Typ disasm-back dekódování pro jeden slot stacku (V3).
 *
 * Určuje, jakou instrukci handler předpokládá těsně před danou
 * 16-bit hodnotou, tedy zda by hodnota mohla být návratovou adresou
 * po CALL/RST. Heuristika - bez záruky, že před adresou opravdu CALL
 * leží (= mohou to být náhodná data).
 */
typedef enum en_DBGAPI_STACK_DECODE_TYPE
{
    DBGAPI_STACK_DECODE_NONE    = 0, /* Žádný match (= prázdná decode buňka) */
    DBGAPI_STACK_DECODE_CALL    = 1, /* CALL nn (opcode 0xCD) na W-3 */
    DBGAPI_STACK_DECODE_CALL_CC = 2, /* CALL cc,nn (opcody C4/CC/D4/DC/E4/EC/F4/FC) na W-3 */
    DBGAPI_STACK_DECODE_RST     = 3, /* RST n (opcody C7/CF/D7/DF/E7/EF/F7/FF) na W-1 */
} en_DBGAPI_STACK_DECODE_TYPE;


/**
 * @brief Informace o jednom decoded slotu stacku (V3, disasm-back heuristika).
 *
 * Vyplněno handlerem CMD_STACK_DUMP, pokud byl předán `decode_buf` pole
 * o velikosti `decode_count`. Pole je indexované per řádek hex dump
 * tabulky (= shoda s buf semantikou: index 0 = nejvyšší adresa).
 *
 * Decode platí jen pro word-oriented zobrazení (lichý SP = byte mode,
 * decode_buf se nevyplňuje, type zůstane NONE).
 *
 * @field type    Druh detekované call/rst instrukce (viz enum). NONE = ne.
 * @field target  Cílová adresa CALL/RST (= operand 16-bit nn nebo RST n*8).
 *                Platí jen při type != NONE.
 * @field opcode  Hodnota opcode bajtu nalezeného před návratovou adresou
 *                (= W-3 pro CALL, W-1 pro RST). Slouží UI pro tooltipy.
 */
typedef struct st_DBGAPI_STACK_DECODE_INFO
{
    uint8_t  type;
    uint8_t  opcode;
    uint16_t target;
} st_DBGAPI_STACK_DECODE_INFO;


/**
 * @brief Parametr pro CMD_STACK_DUMP - hex dump paměti zásobníku.
 *
 * Handler vyplní `buf` čtením paměti přes debugger_memory_read_byte
 * (banking-aware, bez side effects).
 *
 * Adresování okna - dva režimy podle `lines_above`:
 *  - `lines_above > 0` (= SP-anchored mode, preferovaný pro stack panel):
 *    Handler ignoruje vstupní `addr` a sám spočítá
 *    `base = sp + lines_above * 2`, takže okno je vždy konzistentní
 *    s aktuálním SP v témž ticku (= žádný 1-tick lag oproti `sp_now`).
 *    Vypočtenou base zapíše zpět do `addr` jako OUT hodnotu - UI tak
 *    má jistotu kde okno začíná. Buffer je naplněn DESC: `buf[0]` je
 *    bajt na adrese `base`, `buf[i]` na adrese `base - i`. Velikost
 *    okna je dána `len`.
 *  - `lines_above == 0` (= absolute mode, legacy): Handler použije
 *    `addr` jak ji caller předal a naplní buf ASC od této adresy
 *    (`buf[i] = read(addr + i)`). Vhodné pro fixní inspekci konkrétní
 *    adresy bez návaznosti na SP.
 *
 * Dodatečně handler do `sp_now` zapíše aktuální hodnotu SP v okamžiku
 * zpracování (= UI nemusí dělat zvláštní GET_REG round-trip). Bit
 * `sp_odd` říká, zda je SP lichý (= UI přepne na byte-oriented zobrazení
 * místo word-oriented default).
 *
 * Buffer `buf` vlastní caller (= UI alokuje pole o velikosti `len`).
 * Handler ho jen naplňuje, neuvolňuje.
 *
 * Limity:
 *  - len bývá 256 B (= V0 hex dump 40 řádků × 2 B + rezerva). Větší
 *    hodnoty handler neodmítá, ale UI by si měla cap nastavit (= zbytečně
 *    drahá round-trip data).
 *  - addr je 16-bit, adresování přes banking respektuje aktuální mapping.
 *    Wrap kolem 0xFFFF (= addr+len > 0xFFFF) přečte z 0x0000 (= dáno
 *    debugger_memory_read_byte cast na uint16_t).
 *
 * @field addr        (INOUT) Počáteční adresa okna (= nejvyšší zobrazená;
 *                    stack roste dolů, tabulka řazena addr DESC). V SP-anchored
 *                    mode (lines_above > 0) je hodnota přepsána handlerem.
 *                    V absolute mode (lines_above == 0) je vstupní hodnota
 *                    použita beze změny.
 * @field len         Počet bajtů ke čtení.
 * @field lines_above (IN) Pokud > 0, SP-anchored mode: handler spočítá
 *                    `addr = sp + lines_above * 2`. Pokud == 0, absolute
 *                    mode (= legacy chování).
 * @field buf         Buffer alokovaný caller-em pro výstupní data.
 * @field sp_now      (OUT) Aktuální hodnota SP v okamžiku zpracování.
 * @field sp_odd      (OUT) 1 = SP je lichý (UI fallback na byte-oriented).
 * @field decode_buf  (INOUT) Volitelné pole st_DBGAPI_STACK_DECODE_INFO o
 *                    velikosti `decode_count`. Pokud != NULL a SP je sudý
 *                    (word-mode), handler pro každý řádek tabulky (index
 *                    odpovídá indexu v `buf` v krocích step=2) zjistí
 *                    word kandidát `W = mem[addr] | (mem[addr-1] << 8)` a
 *                    udělá disasm-back lookup (W-3 pro CALL, W-1 pro RST).
 *                    Při NONE se vyplní type=NONE. Pokud NULL, handler
 *                    decode přeskočí. Caller vlastní buffer.
 * @field decode_count (IN) Velikost pole `decode_buf` (= obvykle počet řádků
 *                    hex dump tabulky). Pokud > len/2, handler omezí na len/2.
 */
typedef struct st_DBGAPI_STACK_DUMP_PARAM
{
    uint16_t addr;
    uint16_t len;
    uint16_t lines_above;
    uint8_t *buf;
    uint16_t sp_now;
    uint8_t  sp_odd;
    st_DBGAPI_STACK_DECODE_INFO *decode_buf;
    uint16_t decode_count;
} st_DBGAPI_STACK_DUMP_PARAM;


/**
 * @brief Snapshot info o jednom stack regionu pro UI.
 *
 * Cache friendly POD - UI alokuje pole o velikosti DBGAPI_STACK_REGIONS_MAX
 * a handler v dbgapi.c ho naplní podle aktuálního g_stack_regions[].
 *
 * @field name                 Jméno regionu ('\0'-terminated, max 32 B).
 * @field base                 Vrchol (nejvyšší adresa).
 * @field limit                Dno (nejnižší adresa).
 * @field watermark            Nejnižší SP zaznamenaný v regionu.
 * @field push_count           Počet PUSH-like událostí v regionu.
 * @field pop_count            Počet POP-like událostí v regionu.
 * @field current_sp_in_region 1 = aktuální SP padá do <limit..base>.
 */
typedef struct st_DBGAPI_STACK_REGION_INFO
{
    char     name[ 32 ];
    uint16_t base;
    uint16_t limit;
    uint16_t watermark;
    uint64_t push_count;
    uint64_t pop_count;
    uint8_t  current_sp_in_region;
} st_DBGAPI_STACK_REGION_INFO;


/**
 * @brief Maximální počet regionů přenosný v jednom LIST snapshotu.
 *
 * Musí odpovídat STACK_REGIONS_MAX v stack_regions.h (= 8). Duplikace
 * tady kvůli izolaci dbgapi_cmdrq.h od EMU-specifických headerů
 * (caller UI nemusí includovat stack_regions.h jen kvůli velikosti
 * pole).
 */
#define DBGAPI_STACK_REGIONS_MAX 8


/**
 * @brief Parametr pro CMD_STACK_REGIONS_LIST.
 *
 * Caller alokuje strukturu a předá pres data_ptr. Handler v dbgapi.c
 * naplní `count` (= aktuální počet platných regionů) a `regions[0..count-1]`
 * snapshot daty z g_stack_regions[]. Pole nad count je nedotčené.
 *
 * Navíc handler vyplní `sp_now` (= aktuální SP) - UI s tím dopočte
 * "current_sp_in_region" markery v dropdownu.
 *
 * @field count      (OUT) Počet platných regionů (0..DBGAPI_STACK_REGIONS_MAX).
 * @field sp_now     (OUT) Aktuální SP v okamžiku snapshotu.
 * @field regions    (OUT) Pole snapshotů jednotlivých regionů.
 */
typedef struct st_DBGAPI_STACK_REGIONS_LIST_PARAM
{
    int                          count;
    uint16_t                     sp_now;
    st_DBGAPI_STACK_REGION_INFO  regions[ DBGAPI_STACK_REGIONS_MAX ];
} st_DBGAPI_STACK_REGIONS_LIST_PARAM;


/**
 * @brief Parametr pro CMD_STACK_REGIONS_ADD.
 *
 * Validace name + base/limit dělá stack_regions_add (= core API).
 * Handler předá výsledek (index nebo -1) zpět přes `result_index`.
 *
 * @field name             (IN)  Jméno regionu, '\0'-terminated.
 * @field base             (IN)  Vrchol regionu.
 * @field limit            (IN)  Dno regionu (base > limit).
 * @field result_index     (OUT) Index 0..MAX-1 při úspěchu, -1 při chybě.
 */
typedef struct st_DBGAPI_STACK_REGIONS_ADD_PARAM
{
    char     name[ 32 ];
    uint16_t base;
    uint16_t limit;
    int      result_index;
} st_DBGAPI_STACK_REGIONS_ADD_PARAM;


/**
 * @brief Parametr pro CMD_STACK_REGIONS_EDIT (V7).
 *
 * Edituje existující region na indexu @c idx. Validace + overlap detekce
 * proti ostatním regionům probíhá v stack_regions_edit (= core API).
 * Při úspěšné editaci handler resetuje watermark + counters (= staré stats
 * neplatí pro nový rozsah).
 *
 * @field idx     (IN)  Index editovaného regionu (0..count-1).
 * @field name    (IN)  Nový label, '\0'-terminated.
 * @field base    (IN)  Nový vrchol regionu.
 * @field limit   (IN)  Nové dno (base > limit).
 * @field success (OUT) true = editace OK, false = invalid args / duplicate
 *                      name / overlap / idx mimo rozsah. Stejnou informaci
 *                      handler signalizuje i přes rq->success (= konzistentní
 *                      s ostatními STACK_REGIONS_* příkazy).
 */
typedef struct st_DBGAPI_STACK_REGIONS_EDIT_PARAM
{
    uint8_t  idx;
    char     name[ 32 ];
    uint16_t base;
    uint16_t limit;
    bool     success;
} st_DBGAPI_STACK_REGIONS_EDIT_PARAM;


/**
 * @brief Parametr pro CMD_STACK_REGIONS_REMOVE a CMD_STACK_REGIONS_RESET_WATERMARK.
 *
 * Sdílená struktura - oba příkazy potřebují jen index. Úspěch handler
 * signalizuje přes rq->success.
 *
 * @field index  (IN) Index regionu k odebrání / resetu.
 */
typedef struct st_DBGAPI_STACK_REGIONS_REMOVE_PARAM
{
    int index;
} st_DBGAPI_STACK_REGIONS_REMOVE_PARAM;


/**
 * @brief Vzorek SP history pro přenos UI <-> EMU.
 *
 * Stejný layout jako st_STACK_HISTORY_SAMPLE v stack_history.h, ale
 * duplikovaný tady aby UI nemuselo includovat emu-specifický header.
 *
 * @field cycles  Snapshot cpu->total_cycles v okamžiku samplování.
 * @field sp      Hodnota SP.
 */
typedef struct st_DBGAPI_STACK_HISTORY_SAMPLE
{
    uint32_t cycles;
    uint16_t sp;
    uint16_t _pad;
} st_DBGAPI_STACK_HISTORY_SAMPLE;


/**
 * @brief Maximální počet vzorků přenosný v jednom GET snapshotu.
 *
 * Musí odpovídat STACK_HISTORY_SIZE v stack_history.h (= 4096). Duplikace
 * tady kvůli izolaci dbgapi_cmdrq.h od EMU-specifických headerů.
 */
#define DBGAPI_STACK_HISTORY_MAX 4096


/**
 * @brief Parametr pro CMD_STACK_HISTORY_ENABLE.
 *
 * Při enable = 0 handler navíc vyprázdní ring buffer (= clean state pro
 * další zapnutí). Při enable = 1 jen nastaví flag.
 *
 * @field enable  (IN) 0 = vypnout recording, 1 = zapnout.
 */
typedef struct st_DBGAPI_STACK_HISTORY_ENABLE_PARAM
{
    uint8_t enable;
} st_DBGAPI_STACK_HISTORY_ENABLE_PARAM;


/**
 * @brief Parametr pro CMD_STACK_HISTORY_GET - bulk snapshot ring bufferu.
 *
 * Caller alokuje pole `samples` o velikosti `max_count`, předá pres
 * data_ptr a po návratu čte `count` (= kolik vzorků handler skutečně
 * naplnil). Vzorky jsou v pořadí "oldest first" (= samples[0] je nejstarší
 * zaznamenaný vzorek v okénku, samples[count-1] nejnovější).
 *
 * Doplňkové výstupy:
 *  - `active`  = aktuální stav recording flagu (= UI ho používá pro
 *               synchronizaci s checkboxem v sticky headeru).
 *  - `slope`   = lineární regrese SP/cycle přes posledních `slope_window`
 *               vzorků (= "stack creep" indikátor). Záporná hodnota = SP
 *               v čase klesá.
 *  - `slope_window` (IN) = velikost okénka pro slope; pokud > count, použije
 *               se count.
 *
 * @field max_count     (IN)  Velikost samples pole.
 * @field slope_window  (IN)  Window pro slope (typicky 256).
 * @field count         (OUT) Počet skutečně naplněných vzorků (0..max_count).
 * @field active        (OUT) 1 = recording aktivní, 0 = vypnutý.
 * @field slope         (OUT) Slope (SP/cycle). 0 = nedostatek dat.
 * @field samples       (OUT) Pole vzorků, caller-allocated.
 */
typedef struct st_DBGAPI_STACK_HISTORY_GET_PARAM
{
    uint32_t max_count;
    uint32_t slope_window;
    uint32_t count;
    uint8_t  active;
    float    slope;
    st_DBGAPI_STACK_HISTORY_SAMPLE *samples;
} st_DBGAPI_STACK_HISTORY_GET_PARAM;


/**
 * @brief Plochý snapshot st_BPT pro CMD_BP_UPDATE / CMD_BP_CREATE_WITH_INIT.
 *
 * Selektivní zápis: caller naplní jen ty fieldy, které chce přepsat, a v
 * `update_mask` nastaví odpovídající bity DBGAPI_BP_UM_*. Handler v
 * dbgapi.c projde mask a aplikuje jen vyžádané změny voláním existujících
 * `breakpoints_set_*()` setterů (= single source of truth pro mutaci
 * `g_breakpoints`).
 *
 * Pro CMD_BP_UPDATE: `id` musí ukazovat na existující BP. Pokud
 * `breakpoints_find_by_id(id) == NULL`, handler vrátí success = false a
 * žádnou změnu neaplikuje.
 *
 * Pro CMD_BP_CREATE_WITH_INIT: caller předává `id = -1` na vstupu, handler
 * volá `breakpoints_add_auto(addr, name, parent)` a po úspěchu naplní `id`
 * přiděleným ID + aplikuje update_mask. Pokud `breakpoints_add_auto`
 * selže, handler vrátí success = false a id zůstane -1.
 *
 * String lifetime: `name`, `event_name`, `expr`, `action` jako `const
 * char*` mají lifetime trvání sync cmd (= UI strana drží alokaci do
 * návratu `dbgapi_ui_submit_cmd_sync`). Handler uvnitř setterů provede
 * `g_strdup`. NULL = "nastav prázdnou hodnotu" (= legitimní pro
 * action/expr/name = clear).
 *
 * Enum fieldy uloženy jako uint8_t pro minimalizaci závislosti
 * dbgapi_cmdrq.h na breakpoints.h. Handler valid-checkuje rozsah +
 * castuje na konkrétní enum při volání setteru. Mimo rozsah = success
 * false (= nemodifikuje, vrací chybu).
 *
 * Color fieldy bg_rgb / fg_rgb jsou aplikovány společně přes
 * `breakpoints_set_colors(bg, fg)` - 1 bit UM_COLORS pokrývá oba.
 *
 * Match modes a IM filter fieldy uplatňují stejnou logiku co UI: aplikuj
 * všechny vyžádané pole bez ohledu na BPT_TYPE (= zachová hodnoty při
 * přepnutí typu - viz `working_copy_apply` komentář v bpt_edit_panel.cpp).
 *
 * @invariant Pokud update_mask == 0, handler nic neaplikuje a vrátí
 *            success = true (= no-op je validní).
 */
typedef struct st_DBGAPI_BP_UPDATE_PARAM
{
    /* === Cíl / výsledek === */
    int id;                  /**< UPDATE: vstupní ID existujícího BP. CREATE: -1 vstup, naplní handler. */
    uint64_t update_mask;    /**< Bity DBGAPI_BP_UM_* - které fieldy aplikovat */

    /* === Identifikace (5 bitů) === */
    bool enabled;            /**< UM_ENABLED */
    bool auto_name;          /**< UM_AUTO_NAME */
    const char *name;        /**< UM_NAME (NULL = clear) */
    uint32_t bg_rgb;         /**< UM_COLORS - aplikuje s fg_rgb společně */
    uint32_t fg_rgb;         /**< UM_COLORS */
    int parent;              /**< UM_PARENT (-1 = root, jinak group ID) */

    /* === Smart core (14 bitů) === */
    uint8_t type;            /**< UM_TYPE - cast na en_BPT_TYPE */
    uint16_t addr;           /**< UM_ADDR */
    uint16_t addr_end;       /**< UM_ADDR_END */
    uint8_t zone;            /**< UM_ZONE - cast na en_BP_ZONE */
    uint8_t bank_id;         /**< UM_BANK_ID */
    uint16_t port;           /**< UM_PORT */
    const char *event_name;  /**< UM_EVENT_NAME (NULL = clear) */
    uint8_t event_trigger;   /**< UM_EVENT_TRIGGER - cast na en_BP_EVENT_TRIGGER */
    uint16_t sp_threshold;   /**< UM_SP_THRESHOLD */
    const char *expr;        /**< UM_EXPR (NULL = clear) */
    const char *action;      /**< UM_ACTION (NULL = clear) */
    uint32_t hit_count;      /**< UM_HIT_COUNT */
    uint32_t skip_count;     /**< UM_SKIP_COUNT */
    bool edge_triggered;     /**< UM_EDGE_TRIGGERED */

    /* === Match modes (11 bitů) === */
    uint8_t addr_match_mode; /**< UM_ADDR_MATCH_MODE - cast na en_BP_MATCH_MODE */
    uint16_t addr_mask;      /**< UM_ADDR_MASK */
    uint8_t port_match_mode; /**< UM_PORT_MATCH_MODE - cast na en_BP_MATCH_MODE */
    uint16_t port_end;       /**< UM_PORT_END */
    uint16_t port_mask;      /**< UM_PORT_MASK */
    uint8_t port_mode;       /**< UM_PORT_MODE - cast na en_BP_PORT_MODE */
    uint8_t bank_match_mode; /**< UM_BANK_MATCH_MODE - cast na en_BP_MATCH_MODE */
    uint8_t bank_id_end;     /**< UM_BANK_ID_END */
    uint8_t bank_id_mask;    /**< UM_BANK_ID_MASK */
    uint8_t bp_addr_space;   /**< UM_ADDR_SPACE - cast na en_BP_ADDR_SPACE (feature D) */
    uint8_t sp_mode;         /**< UM_SP_MODE - cast na en_BP_SP_MODE */
    uint16_t sp_upper;       /**< UM_SP_UPPER */

    /* === IRQ A8 vector / ISR filter (8 bitů) === */
    bool im2_vector_enabled;        /**< UM_IM2_VECTOR_FILTER s im2_vector_addr */
    uint16_t im2_vector_addr;       /**< UM_IM2_VECTOR_FILTER */
    uint8_t im2_vector_match_mode;  /**< UM_IM2_VECTOR_MATCH_MODE - cast na en_BP_MATCH_MODE */
    uint16_t im2_vector_addr_end;   /**< UM_IM2_VECTOR_ADDR_END */
    uint16_t im2_vector_mask;       /**< UM_IM2_VECTOR_MASK */
    bool im2_isr_enabled;           /**< UM_IM2_ISR_FILTER s im2_isr_addr */
    uint16_t im2_isr_addr;          /**< UM_IM2_ISR_FILTER */
    uint8_t im2_isr_match_mode;     /**< UM_IM2_ISR_MATCH_MODE - cast na en_BP_MATCH_MODE */
    uint16_t im2_isr_addr_end;      /**< UM_IM2_ISR_ADDR_END */
    uint16_t im2_isr_mask;          /**< UM_IM2_ISR_MASK */

    /* === IRQ A8.5 IM discriminator + RST filter (4 bity) === */
    bool im0_enabled;        /**< UM_IM0_ENABLED */
    bool im1_enabled;        /**< UM_IM1_ENABLED */
    bool im2_enabled;        /**< UM_IM2_ENABLED */
    uint8_t im0_rst_mask;    /**< UM_IM0_RST_MASK */

    /* === IRQ_SIG (1 bit) === */
    uint8_t irq_sig_source_mask; /**< UM_IRQ_SIG_SOURCE_MASK */

    /* === 0019 vrstva 2 - per-BP rate-limit override (2 bity) ===
     * Override implicitní ochrany proti saturaci diskem těžkými forward
     * akcemi (snapshot / trace_save). Aplikuje se jen na FWD_SNAPSHOT a
     * FWD_TRACE_SAVE; ostatní pole BP se nedotýkají. */
    uint32_t fwd_min_interval_ms; /**< UM_FWD_MIN_INTERVAL_MS (0 = global/built-in default) */
    uint32_t fwd_max_fires;       /**< UM_FWD_MAX_FIRES (0 = neomezeno) */
} st_DBGAPI_BP_UPDATE_PARAM;

/* ============================================================================
 * Bity update_mask pro CMD_BP_UPDATE / CMD_BP_CREATE_WITH_INIT.
 *
 * Caller OR-uje vybrané bity do update_mask. Handler iteruje bity v
 * pořadí a volá odpovídající breakpoints_set_*() setter. Nezávisle aplikované
 * pole (= 1 bit, 1 setter) má 1:1 mapping. Compound bits (COLORS,
 * IM2_VECTOR_FILTER, IM2_ISR_FILTER) volají setter beroucí 2 argumenty.
 *
 * Bitová pozice je stabilní součást ABI - nové fieldy přidávat na konec
 * (= bit 43+). NEpřemapovávat existující bity.
 * ============================================================================ */

/* Identifikace */
#define DBGAPI_BP_UM_ENABLED            (UINT64_C(1) << 0)
#define DBGAPI_BP_UM_AUTO_NAME          (UINT64_C(1) << 1)
#define DBGAPI_BP_UM_NAME               (UINT64_C(1) << 2)
#define DBGAPI_BP_UM_COLORS             (UINT64_C(1) << 3)  /**< bg_rgb + fg_rgb */
#define DBGAPI_BP_UM_PARENT             (UINT64_C(1) << 4)

/* Smart core */
#define DBGAPI_BP_UM_TYPE               (UINT64_C(1) << 5)
#define DBGAPI_BP_UM_ADDR               (UINT64_C(1) << 6)
#define DBGAPI_BP_UM_ADDR_END           (UINT64_C(1) << 7)
#define DBGAPI_BP_UM_ZONE               (UINT64_C(1) << 8)
#define DBGAPI_BP_UM_BANK_ID            (UINT64_C(1) << 9)
#define DBGAPI_BP_UM_PORT               (UINT64_C(1) << 10)
#define DBGAPI_BP_UM_EVENT_NAME         (UINT64_C(1) << 11)
#define DBGAPI_BP_UM_EVENT_TRIGGER      (UINT64_C(1) << 12)
#define DBGAPI_BP_UM_SP_THRESHOLD       (UINT64_C(1) << 13)
#define DBGAPI_BP_UM_EXPR               (UINT64_C(1) << 14)
#define DBGAPI_BP_UM_ACTION             (UINT64_C(1) << 15)
#define DBGAPI_BP_UM_HIT_COUNT          (UINT64_C(1) << 16)
#define DBGAPI_BP_UM_SKIP_COUNT         (UINT64_C(1) << 17)
#define DBGAPI_BP_UM_EDGE_TRIGGERED     (UINT64_C(1) << 18)

/* Match modes */
#define DBGAPI_BP_UM_ADDR_MATCH_MODE    (UINT64_C(1) << 19)
#define DBGAPI_BP_UM_ADDR_MASK          (UINT64_C(1) << 20)
#define DBGAPI_BP_UM_PORT_MATCH_MODE    (UINT64_C(1) << 21)
#define DBGAPI_BP_UM_PORT_END           (UINT64_C(1) << 22)
#define DBGAPI_BP_UM_PORT_MASK          (UINT64_C(1) << 23)
#define DBGAPI_BP_UM_PORT_MODE          (UINT64_C(1) << 24)
#define DBGAPI_BP_UM_BANK_MATCH_MODE    (UINT64_C(1) << 25)
#define DBGAPI_BP_UM_BANK_ID_END        (UINT64_C(1) << 26)
#define DBGAPI_BP_UM_BANK_ID_MASK       (UINT64_C(1) << 27)
#define DBGAPI_BP_UM_SP_MODE            (UINT64_C(1) << 28)
#define DBGAPI_BP_UM_SP_UPPER           (UINT64_C(1) << 29)

/* IRQ A8 */
#define DBGAPI_BP_UM_IM2_VECTOR_FILTER     (UINT64_C(1) << 30)  /**< im2_vector_enabled + im2_vector_addr */
#define DBGAPI_BP_UM_IM2_VECTOR_MATCH_MODE (UINT64_C(1) << 31)
#define DBGAPI_BP_UM_IM2_VECTOR_ADDR_END   (UINT64_C(1) << 32)
#define DBGAPI_BP_UM_IM2_VECTOR_MASK       (UINT64_C(1) << 33)
#define DBGAPI_BP_UM_IM2_ISR_FILTER        (UINT64_C(1) << 34)  /**< im2_isr_enabled + im2_isr_addr */
#define DBGAPI_BP_UM_IM2_ISR_MATCH_MODE    (UINT64_C(1) << 35)
#define DBGAPI_BP_UM_IM2_ISR_ADDR_END      (UINT64_C(1) << 36)
#define DBGAPI_BP_UM_IM2_ISR_MASK          (UINT64_C(1) << 37)

/* IRQ A8.5 */
#define DBGAPI_BP_UM_IM0_ENABLED        (UINT64_C(1) << 38)
#define DBGAPI_BP_UM_IM1_ENABLED        (UINT64_C(1) << 39)
#define DBGAPI_BP_UM_IM2_ENABLED        (UINT64_C(1) << 40)
#define DBGAPI_BP_UM_IM0_RST_MASK       (UINT64_C(1) << 41)

/* IRQ_SIG */
#define DBGAPI_BP_UM_IRQ_SIG_SOURCE_MASK (UINT64_C(1) << 42)

/* 0019 vrstva 2 - per-BP rate-limit override těžkých FWD akcí (snapshot/trace) */
#define DBGAPI_BP_UM_FWD_MIN_INTERVAL_MS (UINT64_C(1) << 43)  /**< fwd_min_interval_ms (0 = global/built-in default) */
#define DBGAPI_BP_UM_FWD_MAX_FIRES       (UINT64_C(1) << 44)  /**< fwd_max_fires (0 = neomezeno) */
#define DBGAPI_BP_UM_ADDR_SPACE          (UINT64_C(1) << 45)  /**< bp_addr_space (feature D: cpu_view / bank_offset) */

/**
 * @brief Parametr pro CMD_BP_SET_ENABLED - quick toggle enabled flag.
 *
 * Forwarder na `breakpoints_set_enabled(id, enabled)`. Použití: BP list
 * checkbox, disasm right-click toggle. Atomic, žádný side-effect na
 * ostatní fieldy. Pokud BP s id neexistuje -> success = false.
 */
typedef struct st_DBGAPI_BP_SET_ENABLED_PARAM
{
    int id;        /**< ID existujícího BP */
    bool enabled;  /**< Nový stav */
} st_DBGAPI_BP_SET_ENABLED_PARAM;

/**
 * @brief Parametr pro CMD_BP_SET_PARENT - quick reparent (drag-drop).
 *
 * Forwarder na `breakpoints_set_parent(id, parent_id)`. Použití:
 * drag-drop přesun BP mezi skupinami v BP tree, "Clear parent" v context
 * menu. `parent_id = -1` = root (= bez group).
 */
typedef struct st_DBGAPI_BP_SET_PARENT_PARAM
{
    int id;         /**< ID existujícího BP */
    int parent_id;  /**< -1 = root, jinak ID existující skupiny */
} st_DBGAPI_BP_SET_PARENT_PARAM;

/**
 * @brief Parametr pro CMD_BPGRP_ADD - přidání nové skupiny.
 *
 * Forwarder na `breakpoints_group_add(name, parent)`. Po úspěchu handler
 * naplní `id` přiděleným ID nové skupiny. Pokud add selže (= name NULL,
 * cycle, parent neexistuje), vrátí success = false a `id` zůstane -1.
 *
 * String lifetime: `name` const char* musí být platný do návratu sync cmd.
 * Handler interně provede g_strdup ve `breakpoints_group_add`.
 */
typedef struct st_DBGAPI_BPGRP_ADD_PARAM
{
    const char *name;  /**< Jméno skupiny */
    int parent;        /**< -1 = root, jinak ID existující rodičovské skupiny */
    int id;            /**< Výstup: přidělené ID (vstup ignored) */
} st_DBGAPI_BPGRP_ADD_PARAM;

/**
 * @brief Parametr pro CMD_BPGRP_REMOVE - odebrání skupiny podle ID.
 *
 * Forwarder na `breakpoints_group_remove(id)`. Existující děti (BPs +
 * sub-skupiny) jsou hendlovány backendovou logikou
 * (cascading delete / reparent to root - viz breakpoints.c).
 */
typedef struct st_DBGAPI_BPGRP_REMOVE_PARAM
{
    int id;  /**< ID existující skupiny */
} st_DBGAPI_BPGRP_REMOVE_PARAM;

/**
 * @brief Plochý snapshot st_BPTGROUP pro CMD_BPGRP_UPDATE.
 *
 * Selektivní zápis: caller naplní jen fieldy které chce přepsat a v
 * `update_mask` nastaví odpovídající bity DBGAPI_BPGRP_UM_*. Handler v
 * dbgapi.c volá existující `breakpoints_group_set_*()` settery.
 *
 * Pokud `breakpoints_group_find_by_id(id) == NULL`, handler vrátí
 * success = false a žádnou změnu neaplikuje.
 *
 * String `name` má lifetime trvání sync cmd (= UI strana drží alokaci do
 * návratu). NULL je legitimní pro UM_NAME = clear (= setter uloží
 * prázdný string).
 *
 * Color fieldy bg_rgb / fg_rgb se aplikují společně přes
 * `breakpoints_group_set_colors(bg, fg)` - 1 bit UM_COLORS pokrývá oba.
 */
typedef struct st_DBGAPI_BPGRP_UPDATE_PARAM
{
    int id;                /**< ID existující skupiny */
    uint64_t update_mask;  /**< Bity DBGAPI_BPGRP_UM_* */

    bool enabled;          /**< UM_ENABLED */
    const char *name;      /**< UM_NAME (NULL = clear) */
    uint32_t bg_rgb;       /**< UM_COLORS - aplikuje s fg_rgb společně */
    uint32_t fg_rgb;       /**< UM_COLORS */
    int parent;            /**< UM_PARENT (-1 = root, jinak group ID) */
} st_DBGAPI_BPGRP_UPDATE_PARAM;

/* ============================================================================
 * Bity update_mask pro CMD_BPGRP_UPDATE.
 *
 * Stabilní součást ABI - nové fieldy přidávat na konec (bit 4+),
 * NEpřemapovávat existující.
 * ============================================================================ */

#define DBGAPI_BPGRP_UM_ENABLED         (UINT64_C(1) << 0)
#define DBGAPI_BPGRP_UM_NAME            (UINT64_C(1) << 1)
#define DBGAPI_BPGRP_UM_COLORS          (UINT64_C(1) << 2)  /**< bg_rgb + fg_rgb */
#define DBGAPI_BPGRP_UM_PARENT          (UINT64_C(1) << 3)


/* ============================================================================
 * Event Viewer (mutant event-viewer, Vlna 1) - dbgapi parametry
 *
 * Paralelní k existujícím trace-suite a stack history kanálům. Capacity
 * a mask jsou uložené ve struct kvůli budoucímu rozšíření (= snadné
 * přidávat fieldy bez breakování ABI).
 *
 * Layout @c st_EVENTLOG_EVENT je veřejný součást API (24 B, viz
 * eventlog.h). Caller alokuje buffer ve své paměti, handler ho jen
 * vyplní.
 * ============================================================================ */

/**
 * @brief Parametr pro CMD_EVENTLOG_SET_CAPACITY.
 *
 * Caller předává požadovanou velikost ringu. Handler hodnotu interně
 * clampuje do @c [EVENTLOG_MIN_CAPACITY..EVENTLOG_MAX_CAPACITY] a
 * vrátí výslednou (post-clamp) hodnotu v @c capacity_after.
 *
 * @field capacity        (IN)  Požadovaná velikost ringu.
 * @field capacity_after  (OUT) Skutečně nastavená velikost.
 */
typedef struct st_DBGAPI_EVENTLOG_CAPACITY_PARAM
{
    uint32_t capacity;
    uint32_t capacity_after;
} st_DBGAPI_EVENTLOG_CAPACITY_PARAM;

/**
 * @brief Parametr pro CMD_EVENTLOG_SET_MASK.
 *
 * @field mask  (IN) Nová bitmask povolených kategorií (bit i = kategorie i).
 */
typedef struct st_DBGAPI_EVENTLOG_MASK_PARAM
{
    uint64_t mask;
} st_DBGAPI_EVENTLOG_MASK_PARAM;

/**
 * @brief Parametr pro CMD_EVENTLOG_GET_EVENT.
 *
 * Caller předává logický index (0 = oldest) a alokovaný buffer pro
 * 24 B event záznam. Handler vyplní @c found = 1 a obsah @c event,
 * nebo @c found = 0 pokud @c idx >= count.
 *
 * @field idx     (IN)  Logický index v ringu.
 * @field found   (OUT) 1 = event nalezen, 0 = idx mimo rozsah.
 * @field event   (OUT) Vyplněný record (validní pokud found == 1).
 */
typedef struct st_DBGAPI_EVENTLOG_GET_EVENT_PARAM
{
    uint32_t idx;
    uint8_t  found;
    uint8_t  _pad[ 3 ];
    /* 24 B raw record - layout shodný s st_EVENTLOG_EVENT
     * (eventlog.h). Caller si může pole castnout, případně přečíst
     * jednotlivé bajty (offsety jsou stabilní součást API). */
    uint64_t pxclk_total;
    uint32_t screens_total;
    uint32_t pxclk_in_screen;
    uint8_t  category;
    uint8_t  subtype;
    uint16_t pc;
    uint32_t payload;
} st_DBGAPI_EVENTLOG_GET_EVENT_PARAM;

/**
 * @brief Parametr pro CMD_GET_CALLSTACK - snapshot shadow stacku + statistiky.
 *
 * Snapshot pattern: handler (= emu vlákno) alokuje pole entries přes
 * @c callstack_snapshot_get() (= g_malloc) a předá pointer + count zpět
 * caller-ovi. UI po vykreslení MUSÍ uvolnit pole přes
 * @c callstack_snapshot_free() (= g_free). Statistiky jsou kopírovány
 * inline do struct (= žádná dodatečná alokace).
 *
 * Ownership pravidla:
 *  - Při návratu z dbgapi_ui_submit_cmd_sync s success == true vlastní
 *    UI pointer @c entries (i pokud count == 0 -> entries == NULL).
 *  - UI MUSÍ entries uvolnit přes callstack_snapshot_free, ne přes
 *    g_free / free přímo. (Implementace dnes interně volá g_free; helper
 *    chrání před změnou alokátoru v budoucnu.)
 *  - Při success == false handler garantuje entries == NULL a count == 0.
 *
 * @field entries  (OUT) Pole zkopírovaných entries (callee-allocated).
 *                       NULL pokud count == 0 nebo při chybě.
 * @field count    (OUT) Počet entries (0..CALLSTACK_MAX_DEPTH).
 * @field stats    (OUT) Statistiky (current_depth, max_depth_reached,
 *                       divergence_count, sp_swap_count, overflow_count).
 *                       Layout shodný s st_CALLSTACK_STATS - typu-pun přes
 *                       int/uint32_t pole.
 * @field active     (OUT) g_callstack_active snapshot (= UI sync s checkboxem).
 * @field cycles_now (OUT) cpu->total_cycles v okamžiku snapshotu. UI pak
 *                         spočítá Cyc-in = cycles_now - entry.cycles_at_entry
 *                         (= reálné cycles uvnitř každého frame). 0 pokud
 *                         g_mzarch_main.cpu == NULL.
 */
typedef struct st_DBGAPI_CALLSTACK_GET_PARAM
{
    /* OUT: pointer na pole struktur st_CALLSTACK_ENTRY (callstack.h).
     * Typován jako void* aby dbgapi_cmdrq.h nemusel includovat
     * callstack.h (= cyklická závislost prevence). Caller cast na
     * st_CALLSTACK_ENTRY*. */
    void   *entries;
    int     count;
    /* Inline statistiky (= layout st_CALLSTACK_STATS): */
    int     current_depth;
    int     max_depth_reached;
    uint32_t divergence_count;    /* total = trampoline + longjmp + mismatch */
    uint32_t diverg_trampoline;
    uint32_t diverg_longjmp;
    uint32_t diverg_mismatch;
    uint32_t sp_swap_count;
    uint32_t overflow_count;
    uint32_t stack_discard_count;
    uint8_t active;
    uint8_t _pad[ 3 ];
    uint64_t cycles_now;
} st_DBGAPI_CALLSTACK_GET_PARAM;


/**
 * @brief Payload pro DBGAPI_CMD_GET_PROFILER.
 *
 * Sync handler v EMU vlákně alokuje pole entries přes
 * profiler_snapshot_get (callee-allocated, g_malloc). Caller (UI)
 * vlastní vrácený pointer a po dokončení renderování ho MUSÍ
 * uvolnit zpětnou cestou (= zrekonstruovat st_PROF_SNAPSHOT z
 * entries + entry_count a zavolat profiler_snapshot_free).
 *
 * Statistiky kopírujeme inline (= žádná dodatečná alokace nad
 * rámec entries pole). Layout zbývajících polí kopíruje
 * st_PROF_STATS (profiler.h) bez entry_count (= ten je už
 * vyjádřený dedikovaným fieldem entry_count nahoře).
 *
 * @field entries     (OUT) Pole st_PROF_ENTRY (callee-allocated).
 *                          NULL pokud entry_count == 0 nebo při chybě.
 *                          Typován void* aby dbgapi_cmdrq.h nemusel
 *                          includovat profiler.h (prevence cyklické
 *                          závislosti). Caller cast na st_PROF_ENTRY*.
 * @field entry_count (OUT) Počet entries (0..N).
 * @field active             (OUT) g_profiler_active snapshot (= UI sync s
 *                          checkboxem).
 * @field total_cycles_64    (OUT) 64-bit extended cycles counter od resetu.
 * @field total_calls        (OUT) Celkový počet on_enter eventů.
 * @field irq_entries        (OUT) Z toho IRQ accept events.
 * @field unmatched_returns  (OUT) DIVERGENT exit nebo pop nad prázdným.
 * @field max_depth_reached  (OUT) Nejvyšší g_prof_depth od resetu.
 * @field overflow_count     (OUT) Push pokus nad PROFILER_MAX_DEPTH.
 */
typedef struct st_DBGAPI_PROFILER_GET_PARAM
{
    /* OUT: pointer na pole st_PROF_ENTRY (profiler.h). Typován void*
     * aby dbgapi_cmdrq.h nemusel includovat profiler.h
     * (prevence cyklické závislosti). Caller cast na st_PROF_ENTRY*. */
    void   *entries;
    int     entry_count;
    /* Inline statistiky (= layout st_PROF_STATS bez entry_count): */
    uint8_t  active;
    uint8_t  _pad[ 7 ];
    uint64_t total_cycles_64;
    uint64_t total_calls;
    uint32_t irq_entries;
    uint32_t unmatched_returns;
    uint32_t max_depth_reached;
    uint32_t overflow_count;
} st_DBGAPI_PROFILER_GET_PARAM;


/**
 * @brief Payload pro DBGAPI_CMD_PROFILER_SET_ACTIVE.
 *
 * Sync handler v EMU vlákně volá profiler_set_active(p->active != 0).
 * V handleru je volání bezpečné (= safe-point, listener slot
 * manipulace mimo hot path).
 *
 * @field active (IN) 0 = vypnout profiler, !=0 = zapnout.
 */
typedef struct st_DBGAPI_PROFILER_SET_ACTIVE_PARAM
{
    uint8_t  active;        /**< IN: 0/1 = vypnout/zapnout profiler. */
    uint8_t  _pad[ 7 ];     /**< Padding (zarovnání na 8 bajtů). */
} st_DBGAPI_PROFILER_SET_ACTIVE_PARAM;


/**
 * @brief Payload pro DBGAPI_CMD_PROFILER_EXPORT (mutant mcp-server V1.A.7).
 *
 * Sync handler v EMU vlákně volá profiler_export_to_file. Export je
 * čistě read-only nad agregátorem (= snapshot + format-specific writer),
 * takže nezávisí na paused state a může běžet i během run.
 *
 * Formáty (= @c format hodnoty):
 *   - 0 = CSV (UTF-8, '\n' line ending, locale-safe číselné formátování,
 *         header row + jedna entry per řádek)
 *   - 1 = JSON (objekt s `stats` agregovanými countery + `entries` polem)
 *
 * @field filepath  (IN) Cílová cesta. NULL nebo prázdný řetězec = chyba.
 *                       Caller vlastní řetězec - handler ho jen čte.
 * @field format    (IN) 0 = CSV, 1 = JSON. Jiná hodnota → handler vrátí
 *                       chybu (= rq->success = false, result = -2).
 * @field result    (OUT) 0 = success; -1 = open/write chyba (errno čitelný
 *                       hned po návratu); -2 = neplatný format / parametry;
 *                       -3 = alokační chyba snapshot.
 * @field entry_count (OUT) Počet zapsaných entries (0..N).
 */
typedef struct st_DBGAPI_PROFILER_EXPORT_PARAM
{
    const char *filepath;   /**< IN: cílová cesta. */
    int         format;     /**< IN: 0=CSV, 1=JSON. */
    int         result;     /**< OUT: výsledek (viz výše). */
    int         entry_count;/**< OUT: počet zapsaných entries. */
    int         _pad;       /**< Padding (zarovnání). */
} st_DBGAPI_PROFILER_EXPORT_PARAM;


/**
 * @brief Payload pro DBGAPI_CMD_SNAPSHOT_SAVE_FILE / SAVE_BUFFER /
 *        LOAD_FILE / LOAD_BUFFER (mutant mcp-server V1.A.1).
 *
 * Sdílená struktura pro všechny 4 snapshot příkazy. Rozlišení file vs
 * buffer dělá handler v dbgapi.c podle CMD, nikoliv přes pole struktury.
 *
 * @field filepath    (IN) Cesta k .mzs souboru. Význam:
 *                          - SAVE_FILE/LOAD_FILE: povinný non-NULL.
 *                          - SAVE_BUFFER/LOAD_BUFFER: ignorován (může být NULL).
 *                          Ownership: ukazatel patří volajícímu, handler
 *                          ho nesmí uvolnit.
 * @field description (IN) Popis snapshotu vložený do metadat. Pouze pro
 *                          SAVE_*. Smí být NULL (= prázdný popis).
 *                          Ownership: volajícího.
 * @field buffer      (IN/OUT) Buffer s .mzs daty.
 *                          - SAVE_BUFFER (OUT): handler alokuje přes
 *                            g_malloc / g_free-compatible alokátor a uloží
 *                            sem ukazatel. Volající uvolní přes g_free.
 *                            Pokud handler selže, *buffer = NULL.
 *                          - LOAD_BUFFER (IN): non-NULL, ukazatel na .mzs
 *                            data v paměti, vlastnictví volajícího.
 *                          - SAVE_FILE/LOAD_FILE: nepoužívá se (NULL).
 * @field buffer_size (IN/OUT) Velikost dat v buffer.
 *                          - SAVE_BUFFER (OUT): handler vyplní; 0 při chybě.
 *                          - LOAD_BUFFER (IN): povinná velikost dat.
 *                          - SAVE_FILE/LOAD_FILE: nepoužívá se (0).
 * @field result      (OUT) Návratový kód z snapshot.h API (cast int z
 *                          en_SNAPSHOT_RESULT). 0 = SNAPSHOT_OK.
 *                          Caller ho může zobrazit klientovi jako detail.
 *
 * @note Všechny snapshot operace vyžadují pauzu emulátoru (= vrátí
 *       SNAPSHOT_ERR_NOT_PAUSED jinak). MCP dispatch volá tento příkaz
 *       skrz dbgapi_ui_submit_cmd_sync, tedy z UI vlákna - emu vlákno
 *       handler zpracuje v safe-pointu.
 */
typedef struct st_DBGAPI_SNAPSHOT_PARAM
{
    const char *filepath;     /**< IN: cesta k .mzs souboru (file varianty). */
    const char *description;  /**< IN: popis (jen save), může být NULL. */
    uint8_t    *buffer;       /**< IN/OUT: buffer s .mzs daty (buffer varianty). */
    size_t      buffer_size;  /**< IN/OUT: velikost buffer v bajtech. */
    int         result;       /**< OUT: en_SNAPSHOT_RESULT (0 = OK). */
} st_DBGAPI_SNAPSHOT_PARAM;


/**
 * @brief Jedna položka v out array pro DBGAPI_CMD_SYMBOL_LOOKUP a
 *        DBGAPI_CMD_SYMBOL_LIST (mutant mcp-server V1.A.2).
 *
 * Caller alokuje pole st_DBGAPI_SYMBOL_ENTRY[out_max] a předá ho přes
 * st_DBGAPI_SYMBOL_PARAM::out_entries. Handler vyplní out_count položek.
 *
 * @field addr     CPU 16-bit adresa (sym_db ji ukládá jako uint32_t,
 *                  ale wire-level API limituje na 0..65535).
 * @field name     Heap kopie jména symbolu (g_strdup). Caller uvolňuje
 *                  přes g_free po zpracování.
 * @field comment  Heap kopie komentáře (g_strdup), nebo NULL pokud
 *                  symbol komentář nemá. Caller uvolňuje přes g_free.
 * @field source   Hodnota en_SYM_SOURCE (0=SJASMPLUS, 1=NOI, 2=MAP,
 *                  3=LBL). Pro user-added symbols vždy 3 (LBL).
 */
typedef struct st_DBGAPI_SYMBOL_ENTRY
{
    uint16_t  addr;
    char     *name;
    char     *comment;
    uint8_t   source;
    uint8_t   _pad[ 7 ];
} st_DBGAPI_SYMBOL_ENTRY;


/**
 * @brief Payload pro DBGAPI_CMD_SYMBOL_ADD / REMOVE / LOOKUP / LIST
 *        (mutant mcp-server V1.A.2).
 *
 * Sdílená struktura pro všechny 4 symbol příkazy. Rozlišení akce dělá
 * handler v dbgapi.c podle CMD, ne přes pole struktury. Reálné sym_db
 * API (sym_db_add_user_label / sym_db_remove_user_label / lookup_by_name
 * / lookup_by_addr / iter_init+next) pracuje vždy s SYM_SOURCE_LBL při
 * user-driven modifikacích - parametr `kind` z MCP wire je echo-only,
 * neovlivňuje storage.
 *
 * @field addr        (IN) CPU 16-bit adresa.
 *                       - ADD: cílová adresa pro nový symbol.
 *                       - REMOVE: použito pokud name == NULL (= by addr).
 *                       - LOOKUP: použito pokud name == NULL (= by addr).
 *                       - LIST: ignoruje se.
 * @field name        (IN) Jméno symbolu (heap, ownership volajícího).
 *                       - ADD: povinné, non-NULL.
 *                       - REMOVE: pokud non-NULL, remove by name; jinak
 *                         remove by addr.
 *                       - LOOKUP: pokud non-NULL, lookup by name; jinak
 *                         lookup by addr.
 *                       - LIST: ignoruje se.
 * @field comment     (IN) Komentář (může být NULL). Jen pro ADD.
 * @field prefix      (IN) Prefix filter pro LIST (NULL nebo "" = vše).
 * @field out_entries (OUT) Pole, do kterého handler zapíše výsledky pro
 *                       LOOKUP (max 1 záznam) a LIST (až out_max záznamů).
 *                       Caller alokuje, handler vyplní out_count položek.
 *                       Položky vlastní heap stringy (name, comment) -
 *                       caller je uvolní přes g_free.
 * @field out_max     (IN) Velikost pole out_entries (počet slotů).
 * @field out_count   (OUT) Počet skutečně vyplněných záznamů (0..out_max).
 *                       Pro LOOKUP s nenalezeným symbolem = 0.
 * @field source      (OUT) Echo en_SYM_SOURCE pro úspěšný ADD (= vždy
 *                       SYM_SOURCE_LBL = 3); pro ostatní akce nepoužívá.
 *
 * @note Threading: handler běží v emu vlákně, sym_db API je single-thread
 *       (UI vlákno), ale v MCP cestě je emu vlákno bouncováno na safe-point
 *       takže sym_db volání je bezpečné (= žádné souběžné UI volání během
 *       safe-pointu).
 */
typedef struct st_DBGAPI_SYMBOL_PARAM
{
    uint16_t                addr;
    const char             *name;
    const char             *comment;
    const char             *prefix;
    st_DBGAPI_SYMBOL_ENTRY *out_entries;
    size_t                  out_max;
    size_t                  out_count;
    uint8_t                 source;
    uint8_t                 _pad[ 7 ];
} st_DBGAPI_SYMBOL_PARAM;


/**
 * @brief Parametr pro CMD_STEP_OUT (V1.A.3 mcp-server mutant).
 *
 * Handler v emu vláknu vyhledá top frame v shadow callstacku, získá
 * z něj return_addr (= adresa, kam se RET vrátí), nastaví temporary
 * breakpoint a spustí run-to. Asynchronní: po úspěchu emu běží a
 * klient pollí get_state.
 *
 * Status kódy:
 *  - 0 = OK (success=true, temp BP nastaven, emu poběží)
 *  - 1 = callstack tracking neaktivní (g_callstack_active == 0)
 *  - 2 = callstack prázdný (current_depth == 0, "jsme v main")
 *  - 3 = emu už běží (caller musí dřív pause)
 *  - 4 = vnitřní chyba snapshot alokace
 *
 * @field max_cycles    (IN)  Informativní timeout v T-states (V1.A.3
 *                            ne-enforced; pro budoucí rozšíření).
 * @field return_addr   (OUT) Adresa cílového RET (validní jen pokud
 *                            status == 0).
 * @field status        (OUT) Diagnostický kód (viz výše).
 */
typedef struct st_DBGAPI_STEP_OUT_PARAM
{
    uint32_t max_cycles;
    uint16_t return_addr;
    uint8_t  status;
    uint8_t  _pad[ 1 ];
} st_DBGAPI_STEP_OUT_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_IO_READ a DBGAPI_CMD_IO_WRITE.
 *
 * Mutant mcp-server V1.A.5: chip-level fault injection.
 *
 * @field port  16bit Z80 I/O adresa (= bus low byte typicky kóduje port).
 * @field value (IN pro WRITE, OUT pro READ) - 8bit hodnota.
 *
 * @invariant Side effecty na chipech (PSG, FDC, GDG, PIO, CTC) probíhají
 *            přesně jak by je vyvolala instrukce Z80 IN/OUT v EMU vlákně.
 */
typedef struct st_DBGAPI_IO_PARAM
{
    uint16_t port;
    uint8_t  value;
    uint8_t  _pad[ 1 ];
} st_DBGAPI_IO_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_IRQ_INJECT.
 *
 * Mutant mcp-server V1.A.5: fault injection. Force maskable IRQ jako by
 * ho vyvolal hw zdroj. Pokud `vector_valid != 0`, použije se `vector` pro
 * IM0/IM2 (= z80_irq(cpu, vector)); jinak default IRQ s callbackem
 * intread_cb (= z80_int(cpu)).
 *
 * @field source       Textový label zdroje (jen audit logy, ne enforce).
 *                     Vlastní caller; handler ho nevolá ani neuvolňuje.
 * @field vector       8bit IM2 vektor (jen pokud vector_valid != 0).
 * @field vector_valid 0 = bez vektoru (default intread_cb), 1 = použij `vector`.
 */
typedef struct st_DBGAPI_IRQ_INJECT_PARAM
{
    const char *source;
    uint8_t     vector;
    uint8_t     vector_valid;
    uint8_t     _pad[ 2 ];
} st_DBGAPI_IRQ_INJECT_PARAM;


/* ============================================================================
 * Mutant mcp-server V1.A.6 - Watch + CDL Tools payload struktury
 * ============================================================================ */


/**
 * @brief Mód watch řádku pro DBGAPI_CMD_WATCH_ADD.
 *
 * Mapování 1:1 na watch.h en_WATCH_MODE (= explicit hodnoty stabilní,
 * dbgapi_cmdrq.h neincluduje watch.h aby šel zkompilovat i v testu bez
 * debuggeru).
 */
typedef enum en_DBGAPI_WATCH_MODE
{
    DBGAPI_WATCH_MODE_ADDRESS = 0,     /**< Literal addr + type (legacy L0/L1) */
    DBGAPI_WATCH_MODE_EXPR_SCALAR = 1, /**< Výraz -> int32 scalar */
    DBGAPI_WATCH_MODE_EXPR_DEREF  = 2, /**< Výraz -> uint16 addr, read podle type */
} en_DBGAPI_WATCH_MODE;


/**
 * @brief Typ hodnoty watch (mapování na watch.h en_WATCH_TYPE).
 *
 * Stabilní enum hodnoty (stejné jako watch.h). Defaultem je U8 (= 0).
 */
typedef enum en_DBGAPI_WATCH_TYPE
{
    DBGAPI_WATCH_TYPE_U8 = 0,
    DBGAPI_WATCH_TYPE_I8 = 1,
    DBGAPI_WATCH_TYPE_U16LE = 2,
    DBGAPI_WATCH_TYPE_U16BE = 3,
    DBGAPI_WATCH_TYPE_I16LE = 4,
    DBGAPI_WATCH_TYPE_I16BE = 5,
    DBGAPI_WATCH_TYPE_U32LE = 6,
    DBGAPI_WATCH_TYPE_U32BE = 7,
    DBGAPI_WATCH_TYPE_I32LE = 8,
    DBGAPI_WATCH_TYPE_I32BE = 9,
    DBGAPI_WATCH_TYPE_BIT = 10,
    DBGAPI_WATCH_TYPE_ASCII = 11,
    DBGAPI_WATCH_TYPE_MZASCII = 12,
    DBGAPI_WATCH_TYPE_BYTES = 13,
} en_DBGAPI_WATCH_TYPE;


/**
 * @brief Parametr pro DBGAPI_CMD_WATCH_ADD.
 *
 * Vstup:
 *   - mode: ADDRESS/EXPR_SCALAR/EXPR_DEREF
 *   - name: volitelné jméno (NULL nebo prázdné = anonymní)
 *   - addr: jen pro mode=ADDRESS (jinak ignorováno)
 *   - expr_text: jen pro mode=EXPR_* (jinak NULL)
 *   - type: typ hodnoty (default U8)
 *
 * Výstup:
 *   - out_index: index nově přidaného řádku (0..count-1) nebo -1 při chybě
 *
 * Vlastnictví: caller vlastní `name` a `expr_text` (handler si dělá vlastní
 * g_strdup kopii uvnitř watch storage).
 */
typedef struct st_DBGAPI_WATCH_ADD_PARAM
{
    en_DBGAPI_WATCH_MODE mode;
    const char          *name;
    uint16_t             addr;
    uint8_t              _pad1[ 2 ];
    const char          *expr_text;
    en_DBGAPI_WATCH_TYPE type;
    int                  out_index;
} st_DBGAPI_WATCH_ADD_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_WATCH_REMOVE.
 *
 * Lze odebrat buď přes `name` (= match první watch řádek s tímto jménem),
 * nebo přes `index` přímo. Pokud `name != NULL`, handler ho vyhledá a
 * naplní `index`. Pokud `name == NULL`, použije se `index` přímo.
 *
 * @field name        Jméno watche (NULL = použij index).
 * @field index       Index řádku (0..count-1), -1 = nenalezeno.
 * @field out_removed 1 = OK, 0 = nenalezeno / out-of-range.
 */
typedef struct st_DBGAPI_WATCH_REMOVE_PARAM
{
    const char *name;
    int         index;
    int         out_removed;
} st_DBGAPI_WATCH_REMOVE_PARAM;


/**
 * @brief Jeden záznam výstupu pro DBGAPI_CMD_WATCH_LIST.
 *
 * Stringy `name`, `expr_text` a `value_str` jsou heap-allocated (g_strdup),
 * caller (= dispatch handler) je MUSÍ uvolnit přes g_free před návratem.
 */
typedef struct st_DBGAPI_WATCH_LIST_ENTRY
{
    int                  index;
    char                *name;        /**< Heap kopie (g_strdup) nebo NULL. */
    en_DBGAPI_WATCH_MODE mode;
    en_DBGAPI_WATCH_TYPE type;
    uint16_t             addr;
    uint8_t              _pad[ 2 ];
    char                *expr_text;   /**< Heap kopie nebo NULL. */
    char                *value_str;   /**< Formátovaná hodnota (vždy non-NULL). */
} st_DBGAPI_WATCH_LIST_ENTRY;


/**
 * @brief Parametr pro DBGAPI_CMD_WATCH_LIST.
 *
 * Caller alokuje pole `out_entries` o velikosti `out_max`; handler vyplní
 * první `out_count` polí. Pokud `out_count == out_max`, mohly být další
 * řádky odříznuty (= caller by měl velikost zvětšit).
 *
 * Stringy uvnitř `out_entries[i]` jsou g_strdup kopie - caller je MUSÍ
 * uvolnit přes g_free.
 */
typedef struct st_DBGAPI_WATCH_LIST_PARAM
{
    st_DBGAPI_WATCH_LIST_ENTRY *out_entries;
    int                         out_max;
    int                         out_count;
} st_DBGAPI_WATCH_LIST_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_WATCH_EVAL.
 *
 * Dva režimy:
 *   1. Eval existujícího watche přes `name` nebo `index` (expr_text == NULL):
 *      přečte aktuální hodnotu a vyplní `out_value_str`.
 *   2. Ad-hoc eval výrazu: `expr_text != NULL`, parsuje + vyhodnotí výraz
 *      bez perzistentního přidání. `name` a `index` se ignorují.
 *
 * Výstup `out_value_str` je heap-alokovaný (g_strdup), caller MUSÍ uvolnit.
 * `out_value_int` je raw int32 výsledek (pro EXPR_SCALAR / EXPR_DEREF deref
 * read).
 */
typedef struct st_DBGAPI_WATCH_EVAL_PARAM
{
    const char *name;
    int         index;
    const char *expr_text;
    int32_t     out_value_int;
    char       *out_value_str;
    char       *out_error;          /**< Heap chybová hláška (NULL pokud OK). */
} st_DBGAPI_WATCH_EVAL_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_CDL_EXPORT.
 *
 * @field meta_path  Cílová cesta k meta JSON souboru. Region soubory
 *                   `*_bus.cdl`, `*_ram.cdl` atd. budou vytvořeny v
 *                   parent adresáři s prefixem odvozeným z basename.
 *                   Pokud parent adresář neexistuje, vytvoří se přes
 *                   g_mkdir_with_parents.
 * @field out_result 0 = OK, -1 = I/O chyba.
 * @field out_region_count Počet exportovaných region souborů (informativní).
 */
typedef struct st_DBGAPI_CDL_EXPORT_PARAM
{
    const char *meta_path;
    int         out_result;
    int         out_region_count;
} st_DBGAPI_CDL_EXPORT_PARAM;


/**
 * @brief Výběr trace-suite kanálu pro DBGAPI_CMD_TRACE_*.
 *
 * Identifikuje, na který trace subsystém se lifecycle příkaz aplikuje.
 * Mapování string -> enum dělá dispatch vrstva (mcp/dispatch.c).
 */
typedef enum en_DBGAPI_TRACE_CHANNEL
{
    DBGAPI_TRACE_CHANNEL_CPUTRACK = 0,  /**< CPU instrukční tracking (cputrack). */
    DBGAPI_TRACE_CHANNEL_IORQLOG,       /**< I/O port log (iorqlog). */
    DBGAPI_TRACE_CHANNEL_INTLOG,        /**< Interrupt/PIO log (intlog). */
    DBGAPI_TRACE_CHANNEL_HWLOG,         /**< HW signál log (hwlog). */
} en_DBGAPI_TRACE_CHANNEL;


/**
 * @brief Parametr pro DBGAPI_CMD_TRACE_{START,STOP,RESET,SAVE}.
 *
 * @field channel  Cílový trace kanál (cputrack/iorqlog/intlog/hwlog).
 * @field path     Cílová cesta segmentu pro TRACE_SAVE (NULL = jen flush+
 *                 restart na stávající dir/name). Ignorováno u START/STOP/RESET.
 * @field out_result  Výstup: 0 = OK, -1 = chyba (např. restart writeru selhal).
 *
 * @invariant @c channel musí být platná en_DBGAPI_TRACE_CHANNEL hodnota;
 *            dispatch vrstva odmítne neznámý channel string jako INVALID_PARAMS.
 */
typedef struct st_DBGAPI_TRACE_PARAM
{
    en_DBGAPI_TRACE_CHANNEL channel;
    const char             *path;
    int                     out_result;
} st_DBGAPI_TRACE_PARAM;


/**
 * @brief Lifecycle operace nad trace kanálem.
 *
 * Sdílené jádro pro DBGAPI_CMD_TRACE_* handlery i pro forwarding z BP-action
 * DSL (trace_start/stop/save). Hodnoty 1:1 odpovídají příslušným
 * DBGAPI_CMD_TRACE_* příkazům - viz @ref dbgapi_trace_lifecycle.
 */
typedef enum en_DBGAPI_TRACE_OP
{
    DBGAPI_TRACE_OP_START = 0,  /**< Mode kanálu na ALWAYS + recompute (analogie cdl_start). */
    DBGAPI_TRACE_OP_STOP,       /**< Mode kanálu na OFF + recompute. */
    DBGAPI_TRACE_OP_RESET,      /**< fn_reset + flush/restart segmentu na stávající dir/name. */
    DBGAPI_TRACE_OP_SAVE,       /**< Uložit/přesměrovat segment na @p path (NULL = jen flush+restart). */
} en_DBGAPI_TRACE_OP;


/**
 * @brief Provede lifecycle operaci nad jedním trace-suite kanálem.
 *
 * Sdílené jádro extrahované z DBGAPI_CMD_TRACE_* handlerů (0017 FÁZE 1),
 * aby tutéž logiku mohl beze změny chování volat i BP-action DSL forwarding
 * (trace_start/stop/save). Mapuje kanál na deskriptor (mode pole + reset/save
 * funkce) a podle @p op:
 *   - START/STOP: nastaví mode kanálu (ALWAYS/OFF) a triggerne
 *     @c mzarch_platform_fn_debugger_state_changed (recompute všech kanálů +
 *     swap CPU callbacků), analogicky @c mhmap_set_mode.
 *   - RESET: zavolá fn_reset (pokud existuje) + fn_save(NULL) (flush+restart).
 *   - SAVE: zavolá fn_save(@p path).
 *
 * @param channel  Cílový trace kanál (musí být platná en_DBGAPI_TRACE_CHANNEL).
 * @param op       Lifecycle operace.
 * @param path     Cesta pro SAVE (NULL = jen flush+restart); ignorováno
 *                 u START/STOP/RESET.
 * @return 0 při úspěchu; -1 při neznámém kanálu nebo selhání save/restart
 *         writeru.
 *
 * Side effects: mění mode kanálu, swapuje CPU debug callbacky, zapisuje
 * trace segment soubory na disk. Threading: musí běžet na EMU vlákně
 * (mutuje emu stav). Volá se z dbgapi dispatch i z bp_action_execute.
 */
extern int dbgapi_trace_lifecycle ( en_DBGAPI_TRACE_CHANNEL channel,
                                    en_DBGAPI_TRACE_OP op,
                                    const char *path );


/**
 * @brief Identifikátor slotu pro Media Tools (mutant mcp-server V1.B.1).
 *
 * Whitelistované hodnoty - dispatch v dbgapi.c mapuje na konkrétní hw API.
 * Hodnoty MUSÍ odpovídat stringům v MCP JSON payloadu (= validace probíhá
 * v MCP dispatch.c před vložením do fronty).
 */
typedef enum en_DBGAPI_MEDIA_SLOT
{
    DBGAPI_MEDIA_SLOT_NONE = 0,   /**< Neuvedeno (pro LOAD_MZF / LOAD_BINARY / STATE). */
    DBGAPI_MEDIA_SLOT_CMT,        /**< CMT pásek. */
    DBGAPI_MEDIA_SLOT_FDC0_FD0,   /**< Primární FDC (FDC0, porty 0xD8-0xDF), mechanika 0. */
    DBGAPI_MEDIA_SLOT_FDC0_FD1,   /**< FDC0, mechanika 1. */
    DBGAPI_MEDIA_SLOT_FDC0_FD2,   /**< FDC0, mechanika 2. */
    DBGAPI_MEDIA_SLOT_FDC0_FD3,   /**< FDC0, mechanika 3. */
    DBGAPI_MEDIA_SLOT_FDC1_FD0,   /**< Sekundární FDC (FDC1, porty 0x58-0x5F), mechanika 0. */
    DBGAPI_MEDIA_SLOT_FDC1_FD1,   /**< FDC1, mechanika 1. */
    DBGAPI_MEDIA_SLOT_FDC1_FD2,   /**< FDC1, mechanika 2. */
    DBGAPI_MEDIA_SLOT_FDC1_FD3,   /**< FDC1, mechanika 3. */
    DBGAPI_MEDIA_SLOT_QD,         /**< Quick Disk. */
    DBGAPI_MEDIA_SLOT_IDE8,       /**< IDE8 master (drive 0). */
} en_DBGAPI_MEDIA_SLOT;


/**
 * @brief Společný parametr pro Media Tools CMD (V1.B.1).
 *
 * Sdílen mezi DBGAPI_CMD_MEDIA_LOAD_MZF / LOAD_BINARY / INSERT / EJECT.
 * Jednotlivé CMD používají jen relevantní pole - viz docstring per CMD.
 *
 * Vlastnictví: caller (MCP dispatch.c) alokuje stringy přes g_strdup
 * a po návratu z submit je uvolňuje. Handler v dbgapi.c stringy POUZE čte.
 *
 * @field slot         IN: cílový slot. Pro LOAD_MZF / LOAD_BINARY = NONE.
 * @field filepath     IN: cesta k souboru. Při insert/eject prázdná =
 *                     ekvivalent eject. Pro buffer variantu MCP vrstva
 *                     předá tmp file path.
 * @field load_addr    IN: Z80 adresa pro LOAD_BINARY (0-65535). Ignorováno
 *                     pro ostatní CMD.
 * @field read_only    IN: 1 = R/O mount (pokud subsystem podporuje, jinak
 *                     ignorováno).
 * @field out_size     OUT: velikost nahraných dat v bajtech. Pro LOAD_MZF
 *                     = fsize z MZF hlavičky (= délka body bloku), pro
 *                     LOAD_BINARY = počet zapsaných bajtů. 0 pokud neznámé.
 * @field out_load_addr OUT: pro LOAD_MZF = fstrt z MZF hlavičky (= adresa
 *                     kam se body uložilo). Nedefinováno pro ostatní CMD.
 * @field out_exec_addr OUT: pro LOAD_MZF = fexec z MZF hlavičky (= adresa
 *                     spuštění, informativní pro caller / composite run).
 *                     Nedefinováno pro ostatní CMD.
 * @field out_result   OUT: 0 = OK, jinak hw-subsystem specifický kód.
 *                     Caller ho použije pro error detail v response.
 *                     Pro LOAD_MZF: -1 = neplatný parametr, -2 = fáze 1
 *                     (hlavička) selhala (soubor/header), -3 = fáze 2
 *                     (tělo) selhala (= CARRY z cmthack_result).
 */
typedef struct st_DBGAPI_MEDIA_PARAM
{
    en_DBGAPI_MEDIA_SLOT slot;
    const char          *filepath;
    uint16_t             load_addr;
    uint8_t              read_only;
    uint32_t             out_size;
    uint16_t             out_load_addr;
    uint16_t             out_exec_addr;
    int                  out_result;
} st_DBGAPI_MEDIA_PARAM;


/**
 * @brief Jedna položka v state snapshotu (DBGAPI_CMD_MEDIA_STATE).
 */
typedef struct st_DBGAPI_MEDIA_SLOT_INFO
{
    en_DBGAPI_MEDIA_SLOT slot;        /**< Identifikace slotu. */
    uint8_t              inserted;    /**< 1 = aktuálně něco vloženo. */
    uint8_t              read_only;   /**< 1 = R/O mount (informativní). */
    char                 filepath[1024]; /**< Aktuálně mountnutá cesta (prázdné = nic). */
} st_DBGAPI_MEDIA_SLOT_INFO;


/**
 * @brief Parametr pro DBGAPI_CMD_MEDIA_STATE.
 *
 * Pole `slots[]` má pevnou velikost 11 (= počet podporovaných slotů:
 * CMT, FDC0 mechaniky 0-3, FDC1 mechaniky 0-3, QD, IDE8).
 * Handler vyplní `count` a obsah `slots[]` v pořadí CMT, fdc0_fd0..3,
 * fdc1_fd0..3, QD, IDE8.
 */
typedef struct st_DBGAPI_MEDIA_STATE_PARAM
{
    int                       count;     /**< Počet validních záznamů v slots[]. */
    st_DBGAPI_MEDIA_SLOT_INFO slots[11];
} st_DBGAPI_MEDIA_STATE_PARAM;


/* ============================================================================
 * Platform + Config Tools (V1.B.2 - mutant mcp-server)
 * ============================================================================ */

/**
 * @brief Typový kód INI elementu (vrácený v st_DBGAPI_SETTINGS_PARAM).
 *
 * Hodnoty odpovídají interním CFGENTYPE_* z libs/cfgfile, ale jsou
 * zde duplikovány, aby dbgapi nemusel includovat cfgfile hlavičky.
 * Konzistence se ověřuje runtime v dbgapi handleru přes switch nad
 * skutečným typem elementu.
 */
typedef enum en_DBGAPI_SETTINGS_TYPE
{
    DBGAPI_SETTINGS_TYPE_UNKNOWN  = 0, /**< Klíč neexistuje nebo nepodporovaný typ. */
    DBGAPI_SETTINGS_TYPE_UNSIGNED = 1, /**< CFGENTYPE_UNSIGNED. */
    DBGAPI_SETTINGS_TYPE_BOOL     = 2, /**< CFGENTYPE_BOOL. */
    DBGAPI_SETTINGS_TYPE_TEXT     = 3, /**< CFGENTYPE_TEXT. */
    DBGAPI_SETTINGS_TYPE_KEYWORD  = 4, /**< CFGENTYPE_KEYWORD. */
    DBGAPI_SETTINGS_TYPE_FLOAT    = 5, /**< CFGENTYPE_FLOAT. */
} en_DBGAPI_SETTINGS_TYPE;


/**
 * @brief Společný parametr pro DBGAPI_CMD_SETTINGS_GET / SETTINGS_SET.
 *
 * SETTINGS_GET vstupy: module + element. Výstup naplní handler v
 * out_value (heap-alokovaný g_strdup, uvolňuje caller přes g_free)
 * + out_type + out_result.
 *
 * SETTINGS_SET vstupy: module + element + new_value (string,
 * type-coerce dle out_type). Před zápisem naplní handler předchozí
 * hodnotu do out_value (audit / rollback support).
 *
 * Hodnoty out_result:
 *   0   = OK
 *   -1  = neplatné parametry (NULL field)
 *   -2  = modul neexistuje
 *   -3  = element neexistuje v modulu
 *   -4  = type-coerce hodnoty selhal (= např. "abc" do UNSIGNED)
 *   -5  = klíč není live-settable (pouze SET; check je v MCP vrstvě,
 *         ale handler ho také používá pro defense in depth)
 *
 * Vlastnictví: caller (MCP dispatch.c) alokuje vstupní stringy přes
 * g_strdup a po návratu z submit je uvolňuje. Handler stringy POUZE
 * čte. out_value handler alokuje přes g_strdup, caller uvolňuje.
 */
typedef struct st_DBGAPI_SETTINGS_PARAM
{
    const char              *module;       /**< IN: CFGMODULE jméno (= např. "AUDIO"). */
    const char              *element;      /**< IN: element jméno (= např. "volume_8253"). */
    const char              *new_value;    /**< IN (pro SET): string s novou hodnotou, type-coerce dle out_type. */
    char                    *out_value;    /**< OUT: stringifikovaná hodnota (před zápisem u SET, aktuální u GET). */
    en_DBGAPI_SETTINGS_TYPE  out_type;     /**< OUT: typový kód elementu. */
    int                      out_result;   /**< OUT: 0 = OK, jinak chybový kód (viz výše). */
} st_DBGAPI_SETTINGS_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_PLATFORM_SET.
 *
 * V1.B.2 záměrně NEPODPORUJE runtime platform switch - hodnota
 * out_result je vždy -10 (= "requires restart with different
 * binary"). Handler pouze ověří, že parametr odpovídá build platformě
 * (= no-op pro stejnou) nebo signalizuje incompatibility (= jiná).
 *
 * Vstup target_kind:
 *   1 = "mz700"
 *   2 = "mz800"
 *   3 = "mz1500"
 *
 * Výstup out_active_kind = aktuální compile-time platform (= odvozeno
 * z g_mzarch_platform_numeric).
 */
typedef struct st_DBGAPI_PLATFORM_PARAM
{
    int  target_kind;        /**< IN: cílová platforma (1=mz700, 2=mz800, 3=mz1500). */
    int  out_active_kind;    /**< OUT: aktuální platforma (compile-time). */
    int  out_result;         /**< OUT: 0 = OK (jen pro target_kind==active), -10 = nepodporováno. */
} st_DBGAPI_PLATFORM_PARAM;


/**
 * @brief Typový kód periferie (PERIPH_ATTACH / DETACH).
 *
 * Whitelistované hodnoty - dispatch v dbgapi.c mapuje na konkrétní
 * cfgmain modul + reset request.
 */
typedef enum en_DBGAPI_PERIPH_KIND
{
    DBGAPI_PERIPH_KIND_UNKNOWN = 0,
    DBGAPI_PERIPH_KIND_MEMEXT  = 1, /**< Memory expansion (Luftner / PEHU). */
    DBGAPI_PERIPH_KIND_FDC     = 2, /**< WD279x FDC. */
    DBGAPI_PERIPH_KIND_QD      = 3, /**< Quick Disk. */
    DBGAPI_PERIPH_KIND_IDE8    = 4, /**< 8-bit IDE. */
    DBGAPI_PERIPH_KIND_GAL5    = 5, /**< Geneve 5 adapter. */
} en_DBGAPI_PERIPH_KIND;


/**
 * @brief Parametr pro DBGAPI_CMD_PERIPH_ATTACH / DETACH.
 *
 * V1.B.2 minimální implementace - nastaví cfgmain INI flag (= např.
 * MEMEXT/active=true) a vrátí out_requires_restart=1 (= aplikace až
 * po restartu emulátoru). Hot-attach (live re-init) je V1.B.3+.
 *
 * Hodnoty out_result:
 *   0   = OK (INI změněno, restart vyžadován)
 *   -1  = neplatné parametry
 *   -10 = periferie není v arch sestavě (CFG_HWEXT_HAVE_*=0)
 *   -11 = periferie není podporována ani jako INI v této platformě
 *
 * Vlastnictví: caller alokuje option_value přes g_strdup, handler
 * pouze čte.
 */
typedef struct st_DBGAPI_PERIPH_PARAM
{
    en_DBGAPI_PERIPH_KIND  kind;                 /**< IN: typ periferie. */
    const char            *option_value;         /**< IN (optional): typ/variant (= např. "luftner4k" pro memext). */
    int                    out_requires_restart; /**< OUT: 1 = pro plnou aplikaci nutný restart, 0 = aplikováno. */
    int                    out_result;           /**< OUT: 0 = OK, jinak chybový kód. */
} st_DBGAPI_PERIPH_PARAM;


/**
 * @brief Výsledek CMD_BP_LIST - seznam breakpointů s plnými atributy.
 *
 * Caller alokuje strukturu s flexibilním polem bp[max_count] a nastaví
 * max_count. Handler v dbgapi.c naplní count a bp[] až do max_count
 * (= overflow ořízne, ale úspěch).
 *
 * Vlastnictví: handler g_strdup() naplní bp[i].condition (heap, smí být
 * NULL pokud BP nemá condition expr). Caller je POVINEN po použití
 * každý bp[i].condition uvolnit g_free() (i < count). V dispatch.c k
 * tomu slouží sdílený helper _free_bp_list_result(), který uvolní celé
 * pole condition + samotnou strukturu - každý nový caller jej musí
 * použít místo holého g_free(result). Ostatní pole jsou by-value.
 */
typedef struct st_DBGAPI_BP_LIST_RESULT
{
    int count;     /* Počet breakpointů */
    int max_count; /* Velikost pole bp[] (musí nastavit klient) */
    struct
    {
        uint16_t addr;      /* Primární adresa (PC / MEM / IRQ vector) */
        int id;             /* ID */
        bool enabled;       /* Aktivní? */
        uint8_t type;       /* en_BPT_TYPE jako int (PC_EXEC/MEM_R/...) */
        uint8_t zone;       /* en_BP_ZONE jako int (CPU_VIEW/RAM/...) */
        uint8_t bank_id;    /* Bank index pro BP_ZONE_MMEXT_BANK */
        uint64_t hits;      /* Počítadlo aktivací (display only) */
        char *condition;    /* Heap g_strdup() expr (NULL = unconditional) */
    } bp[];                 /* Flexibilní pole */
} st_DBGAPI_BP_LIST_RESULT;


/* ============================================================================
 * V1.C.1 - HID Tools parameter structs
 * ============================================================================ */

/**
 * @brief Parametr pro DBGAPI_CMD_INPUT_PRESS_KEY / RELEASE_KEY.
 *
 * Caller resolvuje key name nebo ASCII znak na (col, bit, needs_shift)
 * přes hid_keymap_resolve / hid_keymap_resolve_ascii. Handler v dbgapi.c
 * pouze provede VKBD bit operaci.
 *
 * Pole col=-1 značí "neresolvovaná klávesa" - handler vrátí success=false.
 * Pole needs_shift=true způsobí, že handler dodatečně press/release i
 * SHIFT (col 8 bit 0). Pro release_key s needs_shift=true SHIFT uvolní
 * jen pokud nejsou další klávesy aktivní (= implementace v handleru
 * volí jednoduchou strategii: SHIFT vždy uvolnit pokud caller žádá).
 */
typedef struct st_DBGAPI_HID_KEY_PARAM
{
    int  col;          /* IN: 0..9 sloupec matrix, -1 = invalid (handler nic neudělá). */
    int  bit;          /* IN: 0..7 bit v sloupci. */
    bool needs_shift;  /* IN: true = handler navíc operuje na SHIFT (col 8 bit 0). */
} st_DBGAPI_HID_KEY_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_INPUT_JOY_SET / JOY_CLEAR.
 *
 * Port 0..1 (= JOY_DEVID_0 / 1). Pro CMD_INPUT_JOY_SET určuje
 * `mcp_mask` active-HIGH bitmasku (bit 0 = UP, 1 = DOWN, 2 = LEFT,
 * 3 = RIGHT, 4 = FIRE1, 5 = FIRE2). Pro CMD_INPUT_JOY_CLEAR se
 * `mcp_mask` ignoruje (= ekvivalent SET s mask=0).
 *
 * V testovacím MCP stub buildu (MZ800EMU_MCP_TEST_BUILD) obsluhu
 * dodává test stub (zachytává parametry do g_stub_state).
 */
typedef struct st_DBGAPI_HID_JOY_PARAM
{
    int     port;     /* IN: 0..1. */
    uint8_t mcp_mask; /* IN: aktivní-HIGH 8-bit maska (jen bity 0..5). */
} st_DBGAPI_HID_JOY_PARAM;


/* ============================================================================
 * V1.D.1 - Last user action tracker API
 * ============================================================================ */

/**
 * @brief Vrátí poslední CMDRQ s origin == USER (= GUI akce).
 *
 * Slouží AI klientovi přes emulator://state Resource ke zjištění, co
 * naposledy human user v GUI udělal. Thread-safe (interně mutex);
 * volání je levné (= jeden lock/unlock).
 *
 * @param[out] out_cmd          Poslední USER CMD (validní jen pokud return true).
 * @param[out] out_timestamp_us Monotonic timestamp v mikrosekundách.
 * @return true pokud byla aspoň jedna USER akce zaznamenána, false pokud
 *         emu byl právě restartován / startup.
 */
bool dbgapi_get_last_user_action ( en_DBGAPI_CMD *out_cmd,
                                    uint64_t *out_timestamp_us );


/**
 * @brief Vrátí lidsky čitelný název DBGAPI_CMD (statický řetězec).
 *
 * Použití: dispatch / log / GUI activity log / Resource last_user_action.
 * Pro neznámý CMD vrátí "unknown". Vrací statický řetězec - volající
 * NESMÍ uvolnit.
 *
 * @param cmd Příkaz bez BLOCKING flag (= caller masknul DBGAPI_CMD_MASK).
 * @return Statický řetězec, nikdy NULL.
 */
const char *dbgapi_cmd_to_str ( en_DBGAPI_CMD cmd );


/* ============================================================================
 * V1.D.1 - Core + CPU extras Resources parameter structs
 * ============================================================================ */

/**
 * @brief Parametr pro DBGAPI_CMD_GET_CPU_IM2_VECTOR (read-only).
 *
 * Vrátí snapshot stavu Z80 pro Interrupt Mode 2 - hodnota IM registru,
 * I registru (high byte vektoru) a poslední acknowledgovaný vector byte
 * pokud byl IM2 IRQ právě servisován. Pro `isr_addr` vypočítává
 * (I << 8) | vec; `isr_target` načítá 16-bit little-endian hodnotu z
 * paměti na isr_addr (= cíl vektoru). Pro IM != 2 vyplní isr_addr=0
 * a vec=0 + nastaví available=0.
 *
 * Žádný side effect na emu state.
 */
typedef struct st_DBGAPI_CPU_IM2_VECTOR_PARAM
{
    uint8_t  im;             /**< OUT: Z80 IM register hodnota (0/1/2). */
    uint8_t  i_reg;          /**< OUT: Z80 I register (high byte vektoru). */
    uint8_t  last_vec;       /**< OUT: poslední ACK vector byte (= 0 pokud neznámý). */
    uint8_t  available;      /**< OUT: 1 = im2_vector platné, 0 = IM != 2 nebo nedostupné. */
    uint16_t isr_addr;       /**< OUT: (i_reg << 8) | last_vec. */
    uint16_t isr_target;     /**< OUT: 16-bit hodnota z paměti na isr_addr (LE). */
} st_DBGAPI_CPU_IM2_VECTOR_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_CPU_INTERRUPT_BUS (read-only).
 *
 * Plný snapshot IRQ subsystému: Z80 core flags (IFF1/IFF2, IM, halted,
 * int_line, nmi_line, I register) + per-platform notes. Detailní per-chip
 * IRQ state (daisy chain pro PIO, non-chain GDG/CTC/FDC, NMI sources)
 * jsou v V1.D.1 placeholder - každá sub-sekce má `*_available` flag.
 * Pokud chip API není přístupné nebo chip není v sestavě, sekce zůstane
 * available=0 + důvod v `*_reason` textu.
 *
 * V1.D.2 / pozdější fáze rozšíří o `recent_acks` ring buffer a per-chip
 * detail.
 */
typedef struct st_DBGAPI_CPU_IRQ_BUS_PARAM
{
    /* Z80 core state (vždy dostupné) */
    uint8_t  iff1;            /**< OUT: Z80 IFF1 flip-flop. */
    uint8_t  iff2;            /**< OUT: Z80 IFF2 flip-flop. */
    uint8_t  im;              /**< OUT: Interrupt mode (0/1/2). */
    uint8_t  halted;          /**< OUT: 1 = CPU v HALT, 0 = běží. */
    uint8_t  int_line;        /**< OUT: aktuální stav INT linky (1 = asserted). */
    uint8_t  nmi_line;        /**< OUT: aktuální stav NMI linky (1 = asserted). */
    uint8_t  i_reg;           /**< OUT: Z80 I register. */
    uint8_t  ei_pending;      /**< OUT: 1 = EI delay slot (1 instruction window). */

    /* Per-platform popis (= statický text per build) */
    char     platform_note[128];  /**< OUT: stručný popis IRQ topologie platformy. */

    /* Daisy chain (Z80 PIO) - V1.D.1 placeholder */
    uint8_t  daisy_chain_available;
    char     daisy_chain_reason[128];

    /* Non-chain IRQ sources (GDG raster, CTC, FDC ...) - V1.D.1 placeholder */
    uint8_t  non_chain_available;
    char     non_chain_reason[128];

    /* NMI sources (memext PEHU atd.) - V1.D.1 placeholder */
    uint8_t  nmi_sources_available;
    char     nmi_sources_reason[128];
} st_DBGAPI_CPU_IRQ_BUS_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_MEMORY_MAP (read-only).
 *
 * Per-platform snapshot 16 KB / 4 KB / 8 KB slotů Z80 address space podle
 * aktuálního banking stavu. V1.D.1 implementuje 16 slotů × 4 KB granulárně
 * (= odpovídá memext map granularitě i Sharp ROM/VRAM banking pravidlům
 * jsou ve většině případů zarovnaná na 4 KB).
 *
 * Source kódy:
 *   0 = unknown / unmapped
 *   1 = ROM (mz800 / mz700 monitor)
 *   2 = CGROM (character ROM, jen MZ-700/MZ-1500)
 *   3 = SRAM (work / video RAM)
 *   4 = VRAM (graphics, MZ-800)
 *   5 = MEMEXT_RAM
 *   6 = MEMEXT_FLASH (Luftner only)
 *
 * `slot_offset` = offset v source bance (= byte offset uvnitř RAM/ROM
 * pole). Pro MEMEXT je to absolutní offset v g_memext.RAM nebo .FLASH.
 *
 * ro_rw: 0 = read-only (ROM, FLASH read-only), 1 = read-write (RAM,
 * VRAM, MEMEXT_RAM).
 */
typedef struct st_DBGAPI_MEMORY_MAP_SLOT
{
    uint16_t addr_start;    /**< Z80 adresa začátku slotu (0x0000, 0x1000, ...). */
    uint16_t addr_end;      /**< Z80 adresa konce slotu (inclusive, 0x0FFF, 0x1FFF, ...). */
    uint8_t  source;        /**< 0..6 source kód viz výše. */
    uint8_t  ro_rw;         /**< 0 = read-only, 1 = read-write. */
    uint32_t slot_offset;   /**< Offset v source bance (informativní). */
} st_DBGAPI_MEMORY_MAP_SLOT;


typedef struct st_DBGAPI_MEMORY_MAP_PARAM
{
    char    platform[32];                     /**< OUT: "mz800" / "mz700" / "mz1500". */
    char    mode_note[64];                    /**< OUT: stručný popis aktivního módu. */
    int     slot_count;                       /**< OUT: počet validních záznamů. */
    st_DBGAPI_MEMORY_MAP_SLOT slots[16];      /**< OUT: 16 × 4 KB slotů. */
} st_DBGAPI_MEMORY_MAP_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_MEMEXT_INFO (read-only).
 *
 * Memory expansion adapter info (Luftner / PEHU / none). Pokud memext
 * není v build sestavě nebo není připojen, vrátí connected=0 + type
 * naplněn jako "none".
 *
 * `current_map[]` zrcadlí g_memext.map[16] (= raw bank index aktuálně
 * mapovaný do 16 slotů × 4 KB). Pro PEHU má relevantní jen sudé indexy
 * (každá PEHU bank = 8 KB = 2 ze 4 KB slotů).
 *
 * Pole pro non-Luftner typ jako flash_banks/flash_bank_size jsou nastavena
 * na 0 (= "neaplikovatelné").
 */
typedef struct st_DBGAPI_MEMEXT_INFO_PARAM
{
    char     type[16];          /**< OUT: "luftner" / "pehu" / "none". */
    uint8_t  connected;         /**< OUT: 1 = memext připojen, 0 = ne. */
    uint32_t ram_banks;         /**< OUT: počet RAM bank. */
    uint32_t ram_bank_size;     /**< OUT: velikost jedné RAM banky v bajtech. */
    uint32_t flash_banks;       /**< OUT: počet FLASH bank (0 pokud N/A). */
    uint32_t flash_bank_size;   /**< OUT: velikost FLASH banky (0 pokud N/A). */
    uint32_t current_map[16];   /**< OUT: aktuální mapování 16 slotů × 4 KB (raw bank idx). */
    uint8_t  map_available;     /**< OUT: 1 = current_map naplněno, 0 = memext odpojen. */
} st_DBGAPI_MEMEXT_INFO_PARAM;


/* ============================================================================
 * V1.D.2.B: Medium debug Resources backing payloady
 * ============================================================================
 */

/**
 * @brief Maximální délka name+comment polí v snapshotu bp_var entry.
 *
 * Fixed-size buffer s '\0' terminátorem - storage drží heap-alokované
 * stringy, snapshot je překopíruje do fixní velikosti aby nemusel řešit
 * ownership přes hranici threadů. Pokud původní jméno přesahuje, ořežou
 * se trailing znaky (snapshot je read-only view, název v storage zůstává
 * původní).
 *
 * BP_VAR_NAME_MAX = 31 (= bp_vars.h limit) + terminátor = 32.
 * Comment limit (256) je velký - kopírujeme jen prvních 128 znaků
 * (= postačující pro snapshot list, plnou hodnotu klient získá přes
 * dedicated Tool až bude existovat).
 */
#define DBGAPI_BP_VAR_NAME_MAX     32
#define DBGAPI_BP_VAR_COMMENT_MAX  128


/**
 * @brief Jeden záznam výstupu pro DBGAPI_CMD_BP_VARS_LIST.
 *
 * Fixed-size buffer kopie z bp_var_t. Stringy jsou '\0'-terminated.
 * Storage bp_var_t.value je signed 32-bit (int32_t).
 */
typedef struct st_DBGAPI_BP_VAR_ENTRY
{
    char    name[DBGAPI_BP_VAR_NAME_MAX];        /**< Identifier bez '$' prefixu. */
    int32_t value;                                /**< Aktuální hodnota. */
    char    comment[DBGAPI_BP_VAR_COMMENT_MAX];   /**< Komentář (může být ""). */
    uint8_t has_comment;                          /**< 1 pokud byl uložen non-NULL/non-empty comment. */
    uint8_t persist_value;                        /**< 1 = value persistuje do .vars souboru. */
} st_DBGAPI_BP_VAR_ENTRY;


/**
 * @brief Parametr pro DBGAPI_CMD_BP_VARS_LIST.
 *
 * Caller alokuje pole `entries` o velikosti `capacity`; handler vyplní
 * první `out_count` polí. Pokud bylo v storage víc než capacity záznamů,
 * nastaví `truncated = 1`. Žádné heap-allokované stringy uvnitř entries
 * (= fixed buffers), proto caller jen uvolní samotné pole.
 */
typedef struct st_DBGAPI_BP_VARS_LIST_PARAM
{
    st_DBGAPI_BP_VAR_ENTRY *entries;   /**< (IN) Caller-allocated pole. */
    size_t                  capacity;  /**< (IN) Velikost pole. */
    size_t                  out_count; /**< (OUT) Počet zapsaných záznamů. */
    uint8_t                 truncated; /**< (OUT) 1 = storage > capacity. */
} st_DBGAPI_BP_VARS_LIST_PARAM;


/**
 * @brief Maximální délka snapshot polí pro bookmark entry.
 *
 * BOOKMARK_INPUT_MAX = 63 v bookmarks.h, snapshot kopíruje 64 znaků (+ '\0').
 * Comment limit má bookmarks.h 256, snapshot kopíruje 128 znaků (viz
 * DBGAPI_BP_VAR_COMMENT_MAX rationale).
 */
#define DBGAPI_BOOKMARK_INPUT_MAX    64
#define DBGAPI_BOOKMARK_COMMENT_MAX  128


/**
 * @brief Jeden záznam výstupu pro DBGAPI_CMD_BOOKMARKS_LIST.
 *
 * Fixed-size kopie z bookmark_t. Adresa není v storage - bookmark v
 * původní formě drží user_input string (= hex literál nebo symbol jméno),
 * adresa se odvozuje runtime přes bookmarks_resolve_addr. Snapshot zde
 * resolve dělá za nás a do pole `addr` zapíše resolved hodnotu; pokud
 * resolve selhal, `addr_resolved` = 0 a `addr` = 0.
 */
typedef struct st_DBGAPI_BOOKMARK_ENTRY
{
    uint32_t             id;                                       /**< Monotonic ID >= 1. */
    char                 user_input[DBGAPI_BOOKMARK_INPUT_MAX];    /**< Hex literál nebo symbol jméno. */
    char                 comment[DBGAPI_BOOKMARK_COMMENT_MAX];     /**< Komentář (může být ""). */
    uint8_t              has_comment;                              /**< 1 pokud non-NULL/non-empty comment. */
    uint16_t             addr;                                     /**< Resolved 16-bit adresa (0 pokud nelze). */
    uint8_t              addr_resolved;                            /**< 1 = resolve OK, 0 = unresolved. */
    en_DBGAPI_CMD_ORIGIN cmd_origin;                               /**< Kdo záložku vytvořil (V1.C.3). */
} st_DBGAPI_BOOKMARK_ENTRY;


/**
 * @brief Parametr pro DBGAPI_CMD_BOOKMARKS_LIST.
 *
 * Caller alokuje pole. Handler interně volá bookmarks_snapshot() který
 * pod GMutexem zkopíruje aktuální obsah storage; tím je read bezpečný
 * z libovolného threadu (= MCP I/O thread, EMU thread, UI thread).
 */
typedef struct st_DBGAPI_BOOKMARKS_LIST_PARAM
{
    st_DBGAPI_BOOKMARK_ENTRY *entries;   /**< (IN) Caller-allocated pole. */
    size_t                    capacity;  /**< (IN) Velikost pole. */
    size_t                    out_count; /**< (OUT) Počet zapsaných záznamů. */
    uint8_t                   truncated; /**< (OUT) 1 = storage > capacity. */
} st_DBGAPI_BOOKMARKS_LIST_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_BOOKMARK_ADD a DBGAPI_CMD_BOOKMARK_REMOVE.
 *
 * Sdílená struktura pro obě write operace:
 *   - ADD používá `user_input` (povinné) + `comment` (volitelné, NULL =
 *     žádný komentář). Po úspěchu handler vyplní `out_id` (= nové ID >= 1)
 *     a `out_addr` / `out_addr_resolved` (= výsledek resolve user_input).
 *   - REMOVE používá `remove_id` (= ID existující záložky). `out_id` se
 *     nastaví na `remove_id` (echo); ostatní out pole nejsou relevantní.
 *
 * Stringy `user_input` a `comment` vlastní caller (= dispatch.c je
 * g_strdup z JSON a uvolní po návratu). Handler si v storage dělá vlastní
 * kopii přes bookmarks_add (g_strdup), takže nevzniká aliasing.
 */
typedef struct st_DBGAPI_BOOKMARK_WRITE_PARAM
{
    const char *user_input;       /**< (IN ADD) Hex literál nebo symbol jméno. */
    const char *comment;          /**< (IN ADD) Komentář nebo NULL. */
    uint32_t    remove_id;        /**< (IN REMOVE) ID záložky k odebrání. */
    uint32_t    out_id;           /**< (OUT) Nové ID (ADD) nebo echo remove_id. */
    uint16_t    out_addr;         /**< (OUT ADD) Resolved 16-bit adresa. */
    uint8_t     out_addr_resolved;/**< (OUT ADD) 1 = resolve OK, 0 = unresolved. */
} st_DBGAPI_BOOKMARK_WRITE_PARAM;


/* ============================================================================
 * V1.D.3.A: IRQ chip Resources backing payloady
 * ============================================================================
 */


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_I8255.
 *
 * Read-only snapshot stavu Intel 8255 PPI. Handler kopíruje z globálu
 * `g_pio8255` (= keyboard matrix scan + PC řízení CMT/PSG). 8255 hardware
 * není fully introspectable - Control Word je sequencer registr, který
 * 8255 sám neumí přečíst. Emu si proto drží mirror v g_pio8255.last_cw_byte
 * (poslední CPU write na DEF_PIO8255_MASTER); fields mode_group_a /
 * mode_group_b / pa_dir / pb_dir / pc_upper_dir / pc_lower_dir handler
 * dekóduje z mirror bytu podle 8255 datasheet (= sekce CW Mode Set).
 *
 * Pokud bit 7 mirror byte = 0 (= Bit Set/Reset operace), Mode skupiny
 * a directions zůstávají odvozené z posledního Mode Set; emu zatím
 * neudržuje samostatný shadow Mode Set bytu, takže handler vrátí
 * cw_decoded = 0 a klient nesmí mode_group_a, mode_group_b ani
 * pa_dir / pb_dir / pc_upper_dir / pc_lower_dir považovat za platné.
 * Klidový stav po HW init (last_cw_byte = 0x8A) je validní Mode Set
 * CW: cw_decoded = 1.
 */
typedef struct st_DBGAPI_PERIPH_I8255_PARAM
{
    uint8_t  port_a;            /**< OUT: PA výstupní hodnota (g_pio8255.signal_PA). */
    uint8_t  port_b;            /**< OUT: PB - 8255 nemá pro MZ samostatný PB read, vrací 0. */
    uint8_t  port_c;            /**< OUT: PC celý byte (g_pio8255.signal_PC). */
    uint8_t  control_word;      /**< OUT: poslední Control Word zapsaný na PPI (mirror). */
    uint8_t  cw_decoded;        /**< OUT: 1 = control_word je Mode Set (= mode/dir fields valid), 0 = Bit Set/Reset. */
    uint8_t  mode_group_a;      /**< OUT: Mode Group A (0..2) z CW bits 6-5. */
    uint8_t  mode_group_b;      /**< OUT: Mode Group B (0..1) z CW bit 2. */
    uint8_t  pa_dir;            /**< OUT: 0 = output, 1 = input (CW bit 4). */
    uint8_t  pb_dir;            /**< OUT: 0 = output, 1 = input (CW bit 1). */
    uint8_t  pc_upper_dir;      /**< OUT: 0 = output, 1 = input (CW bit 3). */
    uint8_t  pc_lower_dir;      /**< OUT: 0 = output, 1 = input (CW bit 0). */
    uint8_t  signal_pc00;       /**< OUT: PC0 - CTC0 audio gate (1 = audio enabled), MZ-700 vždy 1. */
    uint8_t  signal_pc01;       /**< OUT: PC1 - CMT data out. */
    uint8_t  signal_pc02;       /**< OUT: PC2 - CTC2 IRQ enable (0 = IRQ zakázán). */
    uint8_t  signal_pc03;       /**< OUT: PC3 - CMT motor control. */
    uint8_t  signal_pc04;       /**< OUT: PC4 - CMT motor status read. */
    uint8_t  pa_keyboard_column;/**< OUT: aktivní sloupec klávesnice (PA bits 0-3). */
    uint8_t  pa_joy1_enabled;   /**< OUT: PA4 - JOY1 enable (jen MZ-800/1500). */
    uint8_t  pa_joy2_enabled;   /**< OUT: PA5 - JOY2 enable (jen MZ-800/1500). */
} st_DBGAPI_PERIPH_I8255_PARAM;


/**
 * @brief Per-kanál snapshot 8253 CTC.
 *
 * Mirror fields odpovídají běžně použitým hodnotám st_CTC8253 v ctc8253.h.
 * Stav state je en_CTC_STATE (např. INIT, COUNTDOWN, LOAD_DONE); klient
 * dostane raw integer, mapování na stringy řeší dispatch.c.
 */
typedef struct st_DBGAPI_PERIPH_I8253_CHANNEL
{
    uint16_t value;                  /**< Aktuální counter (16-bit, live countdown). */
    uint16_t preset_value;           /**< Aktivní preset (divisor). */
    uint16_t preset_latch;           /**< Queued preset (čeká na LOAD_DONE). */
    uint16_t read_latch;             /**< Read latch snapshot (pokud latch_op=1). */
    uint8_t  out;                    /**< Aktuální output bit (0/1). */
    uint8_t  gate;                   /**< Poslední známá úroveň GATE (0/1). */
    uint8_t  mode;                   /**< en_CTC_MODE (0..5). */
    uint8_t  bcd;                    /**< 0 = binary, 1 = BCD. */
    uint8_t  rlf;                    /**< en_CTC_RLF (1..3). */
    uint8_t  state;                  /**< en_CTC_STATE - interní stav sequenceru. */
    uint8_t  load_done;              /**< 1 = LOAD sequence dokončena. */
    uint8_t  latch_op;               /**< 1 = read_latch drží snapshot. */
    uint8_t  rl_byte;                /**< Počet bajtů už zapsaných/přečtených v aktuálním R/L. */
    uint8_t  _pad;                   /**< Padding pro zarovnání. */
} st_DBGAPI_PERIPH_I8253_CHANNEL;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_I8253.
 *
 * Snapshot všech tří kanálů 8253. Pole `last_cw_byte` zrcadlí poslední
 * Control Word zapsaný CPU na CWREG (= debug mirror; 8253 hardware CW
 * neumí přečíst).
 */
typedef struct st_DBGAPI_PERIPH_I8253_PARAM
{
    st_DBGAPI_PERIPH_I8253_CHANNEL ch[3];  /**< OUT: CTC0, CTC1, CTC2 snapshoty. */
    uint8_t                        last_cw_byte; /**< OUT: poslední CW byte (mirror). */
    uint8_t                        _pad[3];      /**< Zarovnání. */
} st_DBGAPI_PERIPH_I8253_PARAM;


/**
 * @brief Per-port snapshot Z80 PIO.
 *
 * Fields odpovídají st_PIOZ80_PORT v pioz80.h. ICW / IDW / IOMCW shadow
 * v této verzi exponujeme pouze přes odvozené fields (mode, io_mask,
 * int_vec, icmask, icena, icfnc, iclvl). Bit-by-bit interpretace
 * `last_ctrl_byte` mirror je sequencer-závislá (Mode Set, IO_MASK byte,
 * ICW, IDW) a v této fázi nejde do snapshotu - poskytuje ji budoucí
 * dedicated decoder Resource (V2).
 */
typedef struct st_DBGAPI_PERIPH_Z80_PIO_PORT
{
    uint8_t data_output;        /**< Data Output Register. */
    uint8_t masked_input;       /**< Masked input snapshot. */
    uint8_t io_mask;            /**< I/O Select (Mode 3 directional mask; 0=out, 1=in per bit). */
    uint8_t mode;               /**< en_PIOZ80_PORT_MODE (0..3). */
    uint8_t int_vec;            /**< Interrupt vector (7 bit + LSB 0). */
    uint8_t icmask;             /**< Interrupt Control mask. */
    uint8_t icena;              /**< Interrupt Enable (0=disabled, 1=enabled). */
    uint8_t icfnc;              /**< Interrupt logic (0=OR, 1=AND). */
    uint8_t iclvl;              /**< Interrupt active level (0=LOW, 1=HIGH). */
    uint8_t port_int;           /**< en_PIOZ80_PORT_INT (NONE/PENDING/RECEIVED/REPENDING). */
    uint8_t last_ctrl_byte;     /**< Mirror posledního CPU write na control port. */
    uint8_t _pad;               /**< Zarovnání. */
} st_DBGAPI_PERIPH_Z80_PIO_PORT;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_Z80_PIO.
 *
 * Snapshot dvou portů Z80 PIO + agregátní pole IRQ stavu. Na MZ-700,
 * kde HAVE_PIOZ80 = 0, handler vyplní available = 0 a ostatní pole
 * nulami; rq->success je v takovém případě true (= validní "není k
 * dispozici" odpověď, klient nemá failed request).
 */
typedef struct st_DBGAPI_PERIPH_Z80_PIO_PARAM
{
    uint8_t                       available;          /**< OUT: 1 = chip přítomen (MZ-800/1500), 0 = MZ-700. */
    uint8_t                       interrupt;          /**< OUT: en_PIOZ80_INTERRUPT (NONE/PENDING/NEXTPRIO/RECEIVED). */
    uint8_t                       interrupt_port_id;  /**< OUT: 0/1 který port drží pending int; 0xFF = none. */
    uint8_t                       _pad;               /**< Zarovnání. */
    st_DBGAPI_PERIPH_Z80_PIO_PORT port_a;             /**< OUT: snapshot Port A. */
    st_DBGAPI_PERIPH_Z80_PIO_PORT port_b;             /**< OUT: snapshot Port B. */
} st_DBGAPI_PERIPH_Z80_PIO_PARAM;


/**
 * @brief Per-kanál snapshot SN76489 PSG.
 *
 * Fields odpovídají hodnotám z `psg_mirror_channel_*` getterů. Kanály
 * 0..2 jsou TONE (square wave, tone_divider relevantní), kanál 3 je
 * NOISE (noise_div_type + noise_type relevantní; tone_divider field
 * obsahuje raw `.tone.divider` ale nemá audio význam).
 *
 * Klient by měl interpretovat `tone_divider` jen pro type=TONE a
 * `noise_div_type` / `noise_type` jen pro type=NOISE.
 */
typedef struct st_DBGAPI_PERIPH_SN76489_CHANNEL
{
    uint8_t  type;             /**< en_PSG_CHTYPE: 0=TONE, 1=NOISE. */
    uint8_t  attenuation;      /**< 0..15 (0 = max volume, 15 = silent / OFF). */
    uint16_t tone_divider;     /**< 10-bit tone divider (TONE kanály); pro NOISE raw .tone.divider. */
    uint8_t  noise_div_type;   /**< en_NOISE_DIV_TYPE 0..3 (jen NOISE kanál relevantní). */
    uint8_t  noise_type;       /**< en_NOISE_TYPE: 0=periodic, 1=white (jen NOISE kanál). */
    uint8_t  _pad[2];          /**< Zarovnání. */
} st_DBGAPI_PERIPH_SN76489_CHANNEL;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_SN76489.
 *
 * Snapshot PSG modulu (`g_psg_module`). Modul drží 1 nebo 2 PSG instance:
 *   - MZ-700: HAVE_PSG=0 → chip není přítomen, handler vrátí
 *     available=0, psg_count=0, žádné kanály nejsou validní.
 *   - MZ-800: HAVE_PSG=2; runtime `g_psg_module.stereo` rozhoduje, zda
 *     `psg_count=1` (mono, jen psg[0]) nebo `psg_count=2` (stereo, oba).
 *   - MZ-1500: HAVE_PSG=2 + stereo trvale true → `psg_count=2`.
 *
 * Per-PSG snímáme `latch_cs` (= aktuální channel select bit 6-5
 * posledního LATCH bytu) a `latch_attn` (= bit 4 - "další DATA byte je
 * attn"). Per-kanál (4 kanály) snímáme type, attenuation, tone_divider,
 * noise_div_type, noise_type. Handler používá `psg_mirror_*()` API
 * (= side-effect free, snapshot z EMU vlákna).
 *
 * Pole `psg1_*` je validní jen pokud `psg_count >= 2`. Při psg_count=1
 * je psg1_latch_cs / psg1_latch_attn = 0 a `psg1_ch[]` je nuly (klient
 * nesmí pole interpretovat).
 */
typedef struct st_DBGAPI_PERIPH_SN76489_PARAM
{
    uint8_t  available;                            /**< OUT: 1 = aspoň 1 PSG přítomen, 0 = MZ-700 (HAVE_PSG=0). */
    uint8_t  psg_count;                            /**< OUT: počet aktivních PSG instancí (0/1/2). */
    uint8_t  stereo;                               /**< OUT: 1 = `g_psg_module.stereo` flag. */
    uint8_t  _pad;                                 /**< Zarovnání. */
    uint8_t  psg0_latch_cs;                        /**< OUT: aktuální latched channel select (0..3) PSG0. */
    uint8_t  psg0_latch_attn;                      /**< OUT: 1 = další DATA byte updatuje attenuation (PSG0). */
    uint8_t  psg1_latch_cs;                        /**< OUT: latched CS PSG1 (jen pokud psg_count >= 2). */
    uint8_t  psg1_latch_attn;                      /**< OUT: latched attn flag PSG1 (jen pokud psg_count >= 2). */
    st_DBGAPI_PERIPH_SN76489_CHANNEL psg0_ch[4];   /**< OUT: 4 kanály PSG0 (ch0..ch2 TONE, ch3 NOISE). */
    st_DBGAPI_PERIPH_SN76489_CHANNEL psg1_ch[4];   /**< OUT: 4 kanály PSG1 (jen pokud psg_count >= 2). */
} st_DBGAPI_PERIPH_SN76489_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_AY3_8910.
 *
 * Placeholder Resource. AY-3-8910 NENÍ v současné verzi emulátoru
 * implementován (grep ay/AY8910/AY3_8910 v `src/emulator/hw-generic/` =
 * žádné výsledky). Handler vrátí available=0 napříč platformami a
 * žádná pole nemají validní obsah. Struktura existuje pro forward
 * compatibility - pokud někdo v budoucnu chip přidá, layout rozšíříme
 * v zachované struktuře (jen přidáme readback registrů, neovlivní
 * stávající wire protokol).
 */
typedef struct st_DBGAPI_PERIPH_AY3_8910_PARAM
{
    uint8_t  available;        /**< OUT: vždy 0 v aktuálním buildu (chip neimplementován). */
    uint8_t  _pad[3];          /**< Zarovnání. */
} st_DBGAPI_PERIPH_AY3_8910_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_BEEPER.
 *
 * Beeper na Sharp MZ NENÍ samostatný 1-bit chip jako u ZX Spectra. Audio
 * cesta z CTC0 OUT (channel 0 timeru 8253) prochází přes dvě AND hradla
 * GATE0 a PC0 (= bit 0 portu C 8255). Slyšitelný signál:
 *
 *   audible = ctc0_out AND gate0 AND pc0
 *
 * GATE0 řízení:
 *   - MZ-800 v 800 módu (DMD3=0): GATE0 trvale 1, nelze měnit (= HW).
 *   - MZ-800/MZ-700/MZ-1500 v 700 módu: GATE0 = `g_gdg.regct53g7` bit 0
 *     (zápis na port 0xE008 bit 0).
 *
 * Handler kopíruje raw signály pro debugging. Audio modulace
 * (= square wave z CTC0 OUT) generuje slyšitelný tón, nikoliv pulsně
 * řízený 1-bit reproduktor - "beeper" je tedy pracovní termín, ne HW
 * pojem ze Sharp dokumentace.
 *
 * Reference: mz800-knowledge/reference/agent/hw/06-ctc-8253.md
 * (sekce CTC0 OUT, GATE0), hw/05-pio-8255.md (sekce PC0 audio gate).
 */
typedef struct st_DBGAPI_PERIPH_BEEPER_PARAM
{
    uint8_t  available;        /**< OUT: vždy 1 (= signální cesta existuje u všech platforem). */
    uint8_t  level;            /**< OUT: výsledná audible úroveň 0/1 (= ctc0_out AND gate0 AND pc0). */
    uint8_t  ctc0_out;         /**< OUT: raw výstup CTC0 (`g_ctc8253[0].out`). */
    uint8_t  gate0;            /**< OUT: GATE0 signál (= `g_gdg.regct53g7` bit 0; MZ-800 v 800 módu trvale 1). */
    uint8_t  pc0;              /**< OUT: PC0 z 8255 (`g_pio8255.signal_pc00`; audio gate). */
    uint8_t  source[3];        /**< OUT: ASCII identifikátor "PC0" (klient může logovat). */
} st_DBGAPI_PERIPH_BEEPER_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_GDG.
 *
 * Snapshot GDG custom video LSI. Sharp MZ-800, MZ-700 i MZ-1500 mají
 * každý vlastní `st_GDG` strukturu - sdílejí společná pole (beam_row,
 * total_elapsed, regDMD, sts_vsync/hsync, vbln/hbln, regct53g7, tempo)
 * ale liší se v palette systému:
 *
 *   - MZ-800: 16-color palette, ovládaná přes `regPALGRP` + 4 byte
 *     `regPAL0..3` (= 16 logických -> fyzických hodnot). Plus `regBOR`
 *     (border color), `vram800_hot_phase_*` pro WAIT model. Nemá
 *     `mode_color[]`.
 *   - MZ-700: 8-entry palette (`mode_color[8]`), žádný `regBOR`
 *     (hardware border port chybí, vždy 0), žádné `regPAL*` ani
 *     `regPALGRP`.
 *   - MZ-1500: 8-entry palette (`mode_color[8]`), žádný `regBOR`,
 *     žádné `regPAL*` ani `regPALGRP`.
 *
 * Handler vyplní pole `platform` ASCII identifikátorem ("mz800",
 * "mz700", "mz1500"), `palette_count` = 16 (MZ-800) nebo 8 (MZ-700,
 * MZ-1500). `palette[]` obsahuje per-entry GDG hodnotu - pro MZ-800
 * jsou platné entries 0..15 (= regPAL0..3 expandované do 16 položek),
 * pro MZ-700/MZ-1500 entries 0..7 (= mode_color
 * přetypované do uint8_t).
 *
 * Reference: mz800-knowledge/reference/agent/hw/09-video-mz800-modes.md
 * (GDG modes), 10-vram-organization.md (VRAM layout).
 */
typedef struct st_DBGAPI_PERIPH_GDG_PARAM
{
    uint8_t  available;          /**< OUT: vždy 1 (GDG je u všech tří platforem). */
    uint8_t  palette_count;      /**< OUT: 16 (MZ-800) nebo 8 (MZ-700, MZ-1500). */
    uint8_t  has_border_reg;     /**< OUT: 1 = `regBOR` existuje (MZ-800), 0 = ne. */
    uint8_t  has_pal_group;      /**< OUT: 1 = `regPALGRP` + regPAL0..3 (MZ-800), 0 = ne. */
    char     platform[12];       /**< OUT: ASCII "mz800" / "mz700" / "mz1500" + NUL. */
    uint8_t  regDMD;             /**< OUT: Display Mode register (společné napříč). */
    uint8_t  regBOR;             /**< OUT: Border register (MZ-800); 0 u ostatních. */
    uint8_t  regPALGRP;          /**< OUT: Palette Group register (MZ-800); 0 u ostatních. */
    uint8_t  regct53g7;          /**< OUT: GATE0 bit pro CTC0 v 700 módu (společné). */
    uint8_t  palette[16];        /**< OUT: paleta; platné indexy 0..palette_count-1. */
    uint32_t beam_row;           /**< OUT: aktuální raster line (= g_gdg.beam_row). */
    uint32_t total_screens;      /**< OUT: g_gdg.total_elapsed.screens (počet snímků). */
    uint32_t total_ticks;        /**< OUT: g_gdg.total_elapsed.ticks (pixely v aktuálním snímku). */
    uint8_t  sts_vsync;          /**< OUT: STS Vsync bit (status registr mirror). */
    uint8_t  sts_hsync;          /**< OUT: STS Hsync bit (status registr mirror). */
    uint8_t  hbln;               /**< OUT: HBLN: 0 = paprsek mimo screen horizontálně. */
    uint8_t  vbln;               /**< OUT: VBLN: 0 = paprsek mimo screen vertikálně. */
    uint8_t  cksw;               /**< OUT: CKSW (Superimpose) bit; vždy 0 u MZ-700/MZ-1500. */
    uint8_t  has_cksw;           /**< OUT: 1 = MZ-800 (cksw field existuje), 0 = jinde. */
    uint8_t  _pad[2];            /**< Zarovnání. */
    uint32_t tempo;              /**< OUT: tempo počítadlo (společné). */
    uint32_t tempo_divider;      /**< OUT: tempo_divider (společné). */
} st_DBGAPI_PERIPH_GDG_PARAM;


/**
 * @brief Drive entry v `st_DBGAPI_PERIPH_WD1793_PARAM.drives[]`.
 *
 * Mount metadata jedné FDC mechaniky (0..3). `present` = 1 znamená
 * mountnutý DSK obraz. `image_basename` je jen filename (bez adresářové
 * cesty), 63 znaků + NUL (security per V1.D.1 precedent).
 */
typedef struct st_DBGAPI_PERIPH_FDC_DRIVE
{
    uint8_t  present;            /**< OUT: 1 = drive má namountovaný DSK. */
    uint8_t  readonly;           /**< OUT: efektivní R/O (user || fs). */
    uint8_t  user_readonly;      /**< OUT: persistent user R/O preference. */
    uint8_t  fs_readonly;        /**< OUT: runtime auto-detect FS write-protect. */
    uint8_t  storage_mode;       /**< OUT: 0=CACHED, 1=DIRECT, 2=DISCARD. */
    uint8_t  geometry_valid;     /**< OUT: 1 = geometry fields jsou platné. */
    uint16_t tracks;             /**< OUT: počet stop (z DSK geometry). */
    uint16_t sides;              /**< OUT: počet stran (1 nebo 2). */
    uint32_t total_data_bytes;   /**< OUT: velikost data sekce v bajtech. */
    char     image_basename[64]; /**< OUT: jen filename (basename), NUL-terminated. */
} st_DBGAPI_PERIPH_FDC_DRIVE;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_WD1793.
 *
 * Snapshot WD279x FDC chipu plus mount metadat 4 mechanik. Když je
 * FDC subsystém runtime detached (= `g_fdc.connected != CONNECTED`),
 * handler vrátí `available=0` a ostatní pole nejsou platná. Při buildu
 * bez CFG_HWEXT_HAVE_FDC handler vždy vrátí `available=0` (chip není
 * zkompilován).
 *
 * `drives[]` obsahuje per-drive metadata. Image_basename je pouze
 * filename (basename), bez absolutní cesty (security per V1.D.1).
 *
 * Reference: mz800-knowledge/reference/agent/hw/16-floppy.md
 * (Sharp WD1793 specifika).
 */
typedef struct st_DBGAPI_PERIPH_WD1793_PARAM
{
    uint8_t  available;          /**< OUT: 1 = FDC compiled + connected, 0 = ne. */
    uint8_t  bus_xlate_invert;   /**< OUT: 1 = BUS xlate INVERT (Sharp default), 0 = PASSTHROUGH. */
    uint8_t  hd_patch;           /**< OUT: 1 = HD Patch obvod aktivní (port 0xDFh EINT). */
    uint8_t  _pad0;
    /* Chip registry (true-bus hodnoty po inverzi). */
    uint8_t  reg_status;         /**< OUT: regSTATUS. */
    uint8_t  reg_command;        /**< OUT: regCOMMAND. */
    uint8_t  reg_track;          /**< OUT: regTRACK. */
    uint8_t  reg_sector;         /**< OUT: regSECTOR. */
    uint8_t  reg_data;           /**< OUT: regDATA. */
    uint8_t  motor;              /**< OUT: MOTOR/DRIVE port (offset 4, 0xDC). */
    uint8_t  side;               /**< OUT: SIDE port (offset 5, 0xDD). */
    uint8_t  density;            /**< OUT: DENSITY port (offset 6, 0xDE). */
    /* Stav state machine. */
    uint8_t  multiblock_rw;      /**< OUT: m flag z posledního Type II R/W. */
    int8_t   direction_latch;    /**< OUT: +1 = posun dovnitř, -1 = ven. */
    uint8_t  intrq_active;       /**< OUT: 1 = sticky INTRQ. */
    uint8_t  positioned_track;   /**< OUT: aktuální track hlavy. */
    uint8_t  positioned_sector;  /**< OUT: aktuální sektor hlavy. */
    uint8_t  positioned_side;    /**< OUT: aktuální strana hlavy. */
    uint8_t  status_mode;        /**< OUT: en_WD279X_STATUS_MODE (sémantika regSTATUS bitů). */
    uint8_t  _pad1;
    uint16_t buffer_pos;         /**< OUT: pozice v interním I/O bufferu. */
    uint16_t data_counter;       /**< OUT: zbývající bajty pro R/W transfer. */
    uint16_t current_sector_size;/**< OUT: velikost aktuálně zpracovávaného sektoru. */
    st_DBGAPI_PERIPH_FDC_DRIVE drives[4]; /**< OUT: 4 mechaniky. */
} st_DBGAPI_PERIPH_WD1793_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_CMT.
 *
 * Snapshot CMT (Cassette Tape) modulu. CMT je vždy přítomen u všech
 * tří platforem (= cassette interface je standard Sharp MZ HW), proto
 * `available=1` napříč buildy.
 *
 * `state` = 0 (STOP) / 1 (PLAY) / 2 (RECORD). `image_basename` je jen
 * filename bez adresářové cesty (security per V1.D.1). `paused` je
 * orthogonální k `state` (= PLAY + paused = PLAY pozastavený).
 *
 * Reference: mz800-knowledge/reference/agent/formats/mzf.md (CMT
 * transport format).
 */
typedef struct st_DBGAPI_PERIPH_CMT_PARAM
{
    uint8_t  available;          /**< OUT: vždy 1 (CMT je u všech platforem). */
    uint8_t  state;              /**< OUT: 0=STOP, 1=PLAY, 2=RECORD. */
    uint8_t  paused;             /**< OUT: 1 = PLAY/RECORD pozastaven (paused). */
    uint8_t  filled;             /**< OUT: 1 = je nahrán MZF (g_cmt.ext != NULL). */
    uint8_t  polarity_inverted;  /**< OUT: 1 = inverted polarity dip switch. */
    uint8_t  cmtspeed;           /**< OUT: en_CMTSPEED (rychlost CMT). */
    uint8_t  cpu_boost;          /**< OUT: 1 = CPU boost aktivní. */
    uint8_t  mzfsize_check;      /**< OUT: 1 = MZF size check zapnut. */
    uint8_t  output;             /**< OUT: aktuální výstupní bit (na PIO). */
    uint8_t  playsts;            /**< OUT: en_CMTEXT_BLOCK_PLAYSTS. */
    uint8_t  cmthack_enabled;    /**< OUT: 1 = cmthack ROM patch nainstalován (CMTHACK_TEST_IS_INSTALLED). */
    uint8_t  _pad;               /**< Zarovnání. */
    uint64_t start_time;         /**< OUT: gdg_total_ticks při zahájení PLAY/RECORD. */
    uint64_t paused_time;        /**< OUT: gdg_total_ticks při pauznutí. */
    char     image_basename[64]; /**< OUT: jen filename (basename) MZF, NUL-terminated. */
} st_DBGAPI_PERIPH_CMT_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_PERIPH_QD.
 *
 * Snapshot Quick Disk (MZ-1F11) modulu. CFG_HWEXT_HAVE_QDISK=1 default
 * u všech tří platforem, ale Quick Disk lze runtime detach (= `g_qdisk.
 * connected != CONNECTED`). Build bez CFG_HWEXT_HAVE_QDISK = handler
 * vrátí `available=0`.
 *
 * `type` 0=IMAGE, 1=VIRTUAL, 2=UNICARD. `image_basename` jen pro
 * IMAGE/UNICARD mode (jméno mountnutého .mzq souboru); pro VIRTUAL mode
 * vrátí prázdný string. `vrtsts` je en_QDISK_VRTSTS pro VIRTUAL mode
 * (= state machine pozice v emulaci formátu).
 */
typedef struct st_DBGAPI_PERIPH_QD_PARAM
{
    uint8_t  available;          /**< OUT: 1 = QD compiled + connected, 0 = ne. */
    uint8_t  type;               /**< OUT: 0=IMAGE, 1=VIRTUAL, 2=UNICARD. */
    uint8_t  status;             /**< OUT: g_qdisk.status (QDSTS_* bitfield). */
    uint8_t  readonly;           /**< OUT: efektivní R/O flag. */
    uint8_t  user_readonly;      /**< OUT: persistent user R/O preference. */
    uint8_t  fs_readonly;        /**< OUT: runtime FS write-protect detect. */
    uint8_t  storage_mode;       /**< OUT: en_QDISK_STORAGE_MODE (0/1/2). */
    uint8_t  vrtsts;             /**< OUT: en_QDISK_VRTSTS (jen VIRTUAL mode). */
    uint32_t image_position;     /**< OUT: aktuální pozice hlavy v image. */
    uint32_t virt_files_count;   /**< OUT: virt_files_count (VIRTUAL mode). */
    uint32_t virt_file_num;      /**< OUT: virt_file_num (VIRTUAL mode). */
    uint16_t virt_mzfbody_size;  /**< OUT: aktuální MZF body size (VIRTUAL mode). */
    uint16_t out_crc16;          /**< OUT: out_crc16 z QDSIO. */
    char     image_basename[64]; /**< OUT: jen filename (basename) .mzq, NUL-terminated. */
} st_DBGAPI_PERIPH_QD_PARAM;


/* ============================================================================
 * V1.D.4 - Input + Frame Resources structs
 * ============================================================================ */

/**
 * @brief Jeden záznam stisknuté klávesy pro keyboard state snapshot.
 *
 * Vrácený pressed_keys[] obsahuje co je v `effective` matici (= real
 * AND virtual) v pozici col,bit aktivní (= bit clear, viz logika Sharp
 * matrix). `name` obsahuje UPPERCASE symbolické jméno (RETURN, SPACE,
 * ARROW_UP, ...) z hid_keymap tabulky; pro pozice bez named entry
 * zůstává prázdný string a klient si může jméno odvodit z col,bit.
 */
typedef struct st_DBGAPI_INPUT_KBD_PRESSED_KEY
{
    uint8_t col;                  /**< OUT: 0..9 sloupec matice. */
    uint8_t bit;                  /**< OUT: 0..7 bit ve sloupci. */
    char    name[24];             /**< OUT: jméno klávesy nebo prázdný string. */
} st_DBGAPI_INPUT_KBD_PRESSED_KEY;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_INPUT_KEYBOARD_STATE.
 *
 * Snapshot klávesnice. Tři matice (real, virtual, effective) každá 10
 * sloupců po 8 bitech (= aktivní bit = clear, idle = 1). Effective je
 * pre-počítaný AND obou matric (= co CPU efektivně vidí). Decode
 * pressed_keys obsahuje seznam aktivních pozic s jmenovaným mapováním
 * (max 32 - kdyby uživatel držel cca celou klávesnici); pokud reálně
 * stisk > pole, `pressed_truncated=1`.
 *
 * Klávesová matrix je sjednocená napříč MZ-700/MZ-800/MZ-1500 (= layout
 * v iface_keyboard.c je shodný), proto handler vrací stejnou strukturu
 * bez per-platform větvení.
 */
typedef struct st_DBGAPI_INPUT_KBD_STATE_PARAM
{
    uint8_t real_matrix[10];      /**< OUT: g_pio8255.keyboard_matrix. */
    uint8_t virtual_matrix[10];   /**< OUT: g_pio8255.vkbd_matrix. */
    uint8_t effective[10];        /**< OUT: real & virtual (= co CPU vidí). */
    uint32_t pressed_count;       /**< OUT: počet aktivních pozic v effective. */
    uint8_t  pressed_truncated;   /**< OUT: 1 = aktivních > kapacita pressed_keys. */
    uint8_t  _pad[3];             /**< Zarovnání. */
    st_DBGAPI_INPUT_KBD_PRESSED_KEY pressed_keys[32]; /**< OUT: decode. */
} st_DBGAPI_INPUT_KBD_STATE_PARAM;


/**
 * @brief Jeden záznam v keyboard matrix info tabulce.
 *
 * Pro Resource `emulator://input/keyboard/matrix_info`. Tabulka
 * jména -> (col, bit) je sjednocená napříč platformami. `needs_shift`=1
 * znamená klávesa vyžaduje současný stisk SHIFT (col 8 bit 0).
 */
typedef struct st_DBGAPI_INPUT_KBD_MATRIX_KEY
{
    uint8_t col;                  /**< OUT: 0..9 sloupec matice. */
    uint8_t bit;                  /**< OUT: 0..7 bit. */
    uint8_t needs_shift;          /**< OUT: 1 = klávesa pod SHIFT. */
    uint8_t _pad;
    char    name[24];             /**< OUT: UPPERCASE jméno klávesy. */
} st_DBGAPI_INPUT_KBD_MATRIX_KEY;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_INPUT_KEYBOARD_MATRIX_INFO.
 *
 * Statická popisná tabulka. Caller poskytne pole `keys[80]` (= 10 cols
 * x 8 bits horní limit, ve skutečnosti aktuálně cca 40 entries včetně
 * aliasů). `key_count` udává kolik je validních.
 *
 * `platform` rozliší výstupní target (mz700/mz800/mz1500) i přes to
 * že tabulka aktuálně nemá per-platform varianty. Klient si může na
 * základě tohoto pole zobrazit správný label.
 */
typedef struct st_DBGAPI_INPUT_KBD_MATRIX_INFO_PARAM
{
    char    platform[16];         /**< OUT: "mz700"/"mz800"/"mz1500". */
    uint32_t key_count;           /**< OUT: počet validních entries. */
    st_DBGAPI_INPUT_KBD_MATRIX_KEY keys[80]; /**< OUT: tabulka. */
} st_DBGAPI_INPUT_KBD_MATRIX_INFO_PARAM;


/**
 * @brief Per-port stav joysticku pro snapshot.
 *
 * `connected` odráží runtime config (= JOY_TYPE_NONE -> 0, jinak 1).
 * `state_bits` je active-HIGH bitmask (bit 0=UP, 1=DOWN, 2=LEFT,
 * 3=RIGHT, 4=FIRE1, 5=FIRE2) - mapping ze native active-LOW
 * `g_joy.dev[].state` (= zrcadlení dispatch HID set logiky).
 * `device_name` je "none"/"num_keypad"/"joystick" podle en_JOY_TYPE.
 */
typedef struct st_DBGAPI_INPUT_JOY_PORT
{
    uint8_t connected;            /**< OUT: 1 = type != JOY_TYPE_NONE. */
    uint8_t state_bits;           /**< OUT: active-HIGH bitmask. */
    uint8_t native_state;         /**< OUT: raw g_joy.dev[].state (active-LOW). */
    uint8_t _pad;
    char    device_name[16];      /**< OUT: "none"/"num_keypad"/"joystick". */
} st_DBGAPI_INPUT_JOY_PORT;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_INPUT_JOYSTICK_STATE.
 *
 * Per-port state pro porty 0 a 1. Pro MZ-700 je `g_joy` přítomen ale
 * type bývá NONE (= joystick HW není standardní pro MZ-700, jen MZ-800/
 * MZ-1500 mají dedikované porty). `available_per_port` = 1 znamená
 * connected (= dále má smysl číst `state_bits`).
 */
typedef struct st_DBGAPI_INPUT_JOY_STATE_PARAM
{
    st_DBGAPI_INPUT_JOY_PORT port[2]; /**< OUT: porty 0 a 1. */
} st_DBGAPI_INPUT_JOY_STATE_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_FRAME_FRAMEBUFFER_INFO.
 *
 * Shape metadata aktuálního framebufferu. `pixel_format` 0=INDEX8 (= MZ
 * nativně používá INDEX8 s palette), `bytes_per_pixel` odpovídá. Klient
 * který chce RGBA pixely musí použít `frame/screenshot.raw` Resource
 * (dispatch tam dělá INDEX8 -> RGBA expand).
 *
 * `framebuffer_state` odráží `en_FBSTATE` bitmask (1=SCREEN_CHANGED,
 * 2=BORDER_CHANGED). `dirty` je odvozený `state != FB_STATE_NOT_CHANGED`.
 */
typedef struct st_DBGAPI_FRAME_FB_INFO_PARAM
{
    uint32_t width;               /**< OUT: VIDEO_DISPLAY_WIDTH (např. 928). */
    uint32_t height;              /**< OUT: VIDEO_DISPLAY_HEIGHT (např. 288). */
    uint32_t last_screen_id;      /**< OUT: fbsnapshot_screen_id (frame counter). */
    uint32_t bytes_per_pixel;     /**< OUT: 1 pro INDEX8. */
    uint8_t  framebuffer_state;   /**< OUT: en_FBSTATE bitmask. */
    uint8_t  pixel_format;        /**< OUT: 0=INDEX8. */
    uint8_t  dirty;               /**< OUT: 1 = state != NOT_CHANGED. */
    uint8_t  has_palette;         /**< OUT: 1 = INDEX8 vyžaduje palette pro decode. */
    uint32_t palette_size;        /**< OUT: počet platných entries v palette (16 pro Sharp MZ). */
    uint32_t palette[16];         /**< OUT: RGB 0x00RRGGBB per index (DISPLAY_MZCOLORS). */
} st_DBGAPI_FRAME_FB_INFO_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_FRAME_SCREENSHOT_RAW.
 *
 * Caller poskytne `buffer` o velikosti `buffer_capacity` (= max bytes
 * který umí přijmout). Handler vyplní `width`, `height`, `pixel_format`
 * a do bufferu zapíše pixel data v RGBA8888 layoutu (= 4 bajty per pixel
 * v pořadí R, G, B, A=0xff). Pokud aktuální framebuffer nemá data
 * (`available=0`), buffer zůstane nedotčen.
 *
 * `downscale_factor` 1 = native rozlišení, 2 = polovina (= každý 2.
 * pixel v obou osách), 4 = čtvrtina. Caller nastaví požadovaný faktor
 * před voláním; handler ho respektuje, pokud výsledek vejde do buffer
 * capacity. Pokud ne, handler downscale zvýší a vrátí použitou hodnotu.
 *
 * Threading: handler bere `fbsnapshot_pixels_mutex` a kopíruje (= raw
 * data jsou stabilní jen po dobu lockního okna).
 *
 * V1.E.6.C - headless fallback: pokud `g_iface_video->fbsnapshot_pixels`
 * je NULL (= SDL render thread neběží, typicky `--mcp-pipe` headless
 * mode), handler přepne na GDG live buffer `g_framebuffer.pixels` a
 * vrátí `fallback_source = SCREENSHOT_SRC_GDG_LIVE`. V GUI mode s
 * běžícím SDL render threadem zůstává `SCREENSHOT_SRC_SDL_SNAPSHOT`.
 */
enum
{
    SCREENSHOT_SRC_SDL_SNAPSHOT = 0, /**< Source: iface_video->fbsnapshot_pixels (GUI cesta). */
    SCREENSHOT_SRC_GDG_LIVE     = 1, /**< Source: g_framebuffer.pixels (headless fallback). */
};

typedef struct st_DBGAPI_FRAME_SCREENSHOT_RAW_PARAM
{
    uint8_t  available;           /**< OUT: 1 = data zapsána, 0 = framebuffer prázdný. */
    uint8_t  pixel_format;        /**< OUT: 0=RGBA8888 (po dispatch expandu). */
    uint8_t  downscale_factor;    /**< IN/OUT: 1/2/4. */
    uint8_t  fallback_source;     /**< OUT: 0=sdl_snapshot, 1=gdg_live (V1.E.6.C). */
    uint32_t width;               /**< OUT: width po downscale. */
    uint32_t height;              /**< OUT: height po downscale. */
    uint32_t bytes_per_pixel;     /**< OUT: 4 pro RGBA8888. */
    uint32_t source_screen_id;    /**< OUT: fbsnapshot_screen_id v okamžiku snapshotu. */
    size_t   buffer_capacity;     /**< IN: max bytů které buffer pojme. */
    size_t   buffer_size;         /**< OUT: kolik bytů handler skutečně zapsal. */
    uint8_t *buffer;              /**< IN: caller alokovaný buffer pro pixels. */
} st_DBGAPI_FRAME_SCREENSHOT_RAW_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG.
 *
 * Handler enkóduje aktuální framebuffer (= stejná pixel cesta jako
 * screenshot_raw: INDEX8 buffer expandovaný přes paletu na RGBA8888) do
 * PNG streamu pomocí vendorovaného stb_image_write.h. PNG je vždy plný
 * frame bez downscale (= jiný kontejner než raw, ale identický obsah
 * při raw factor=1).
 *
 * Při úspěchu `available=1`, `buffer` ukazuje na handlerem alokovaný
 * PNG file stream (glib alokace) a `buffer_size` je jeho délka; dispatch
 * ho base64-enkóduje a poté MUSÍ uvolnit přes `g_free`. Při neúspěchu
 * (framebuffer nepřipravený, paleta neinicializovaná, selhání enkódu)
 * `available=0`, `buffer=NULL`, `reason` vyplněno.
 *
 * Pravidla vlastnictví: `buffer` vlastní dispatch po návratu handleru a
 * uvolňuje ho přes `g_free` po base64 enkódování.
 */
typedef struct st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM
{
    uint8_t  available;           /**< OUT: 1 = PNG stream zapsán, 0 = nedostupný. */
    uint8_t  _pad[3];
    uint32_t width;               /**< OUT: šířka obrázku v pixelech. */
    uint32_t height;              /**< OUT: výška obrázku v pixelech. */
    uint8_t *buffer;              /**< OUT: handlerem alokovaný PNG stream (g_free), NULL při chybě. */
    size_t   buffer_size;         /**< OUT: délka PNG streamu v bajtech. */
    char     reason[64];          /**< OUT: důvod proč není dostupný (jen při available=0). */
} st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM;


/**
 * @brief Parametr pro DBGAPI_CMD_GET_VIDEO_TEXT_DUMP.
 *
 * Text mode dump pro MZ-700 (D000-D3FF chars + D800-DBFF attributes, 40
 * cols x 25 rows). MZ-1500 v MZ-700 compat mode má stejný layout. Pro
 * MZ-800 v 800 mode (= GDG grafický mode) `available=0` + důvod.
 *
 * `chars` jsou Sharp ASCII bajty (= mapping na UTF-8 dělá až dispatch v
 * presentation layeru). `attributes` jsou raw bajty atributu char cellu
 * (význam záleží na MZARCH; klient si interpretaci řeší sám).
 */
typedef struct st_DBGAPI_VIDEO_TEXT_DUMP_PARAM
{
    uint8_t  available;           /**< OUT: 1 = data validní. */
    uint8_t  _pad[3];
    char     platform[16];        /**< OUT: "mz700"/"mz800"/"mz1500". */
    char     reason[64];          /**< OUT: vyplněno pokud available=0. */
    uint32_t cols;                /**< OUT: počet sloupců (40 pro MZ-700). */
    uint32_t rows;                /**< OUT: počet řádků (25 pro MZ-700). */
    uint32_t cell_count;          /**< OUT: cols * rows (max 1000 v MZ-700). */
    uint8_t  chars[1000];         /**< OUT: Sharp ASCII bajty, row-major. */
    uint8_t  attributes[1000];    /**< OUT: per-cell attribute bajty. */
} st_DBGAPI_VIDEO_TEXT_DUMP_PARAM;


/**
 * @brief Maximální délka jména watch řádku v request/response paramu.
 *
 * Drží se v sync s `WATCH_EMU_SNAPSHOT_NAME_MAX` (watch_emu_cache.h).
 * Delší jména se ořežou.
 */
#define DBGAPI_WATCH_SNAPSHOT_NAME_MAX 64


/**
 * @brief Parametr pro DBGAPI_CMD_GET_WATCH_SNAPSHOT.
 *
 * Lookup watch řádku v EMU-side thread-safe zrcadle (`watch_emu_cache`)
 * podle jména. Handler nečte storage z UI vlákna - přečte mirror přes
 * `watch_emu_cache_get_by_name`. Mirror je naplňován UI vláknem jednou
 * per frame (= 1-frame stale akceptovatelné per scope V1.D.2.C).
 *
 * Pokud řádek se zadaným jménem v mirror není (= neexistuje nebo je
 * anonymní), `found=0` a ostatní fields jsou 0.
 *
 * Jméno je case-sensitive (= shoda s UI publikací).
 */
typedef struct st_DBGAPI_WATCH_SNAPSHOT_PARAM
{
    char     name[DBGAPI_WATCH_SNAPSHOT_NAME_MAX]; /**< IN: hledané jméno (NUL-terminated). */
    uint8_t  found;                                /**< OUT: 1 = nalezeno, 0 jinak. */
    uint8_t  snapshot_active;                      /**< OUT: 1 = baseline aktivní (delta_int má smysl). */
    uint8_t  min_max_valid;                        /**< OUT: 1 = min/max smysluplné (int typ). */
    uint8_t  _pad;
    int32_t  row_id;                               /**< OUT: stable id watch řádku. */
    int32_t  type_snap;                            /**< OUT: en_WATCH_TYPE v okamžiku publish. */
    int64_t  snap_int;                             /**< OUT: snapshot baseline (sign-extended). */
    int64_t  cur_int;                              /**< OUT: aktuální hodnota (sign-extended). */
    int64_t  delta_int;                            /**< OUT: cur - snap (jen pokud snapshot_active). */
    int64_t  min_int;                              /**< OUT: min od init/reset (sign-extended). */
    int64_t  max_int;                              /**< OUT: max od init/reset (sign-extended). */
    uint64_t change_count;                         /**< OUT: počet změn od init/reset. */
} st_DBGAPI_WATCH_SNAPSHOT_PARAM;


/* ============================================================================
 * mzdos-support 0007 - Direct memory region read
 *
 * Tenké MCP wrappery existujících `dbgapi_regions_enumerate` /
 * `dbgapi_regions_read` z `dbgapi_regions.h`. Žádný refactor backend - jen
 * dispatch pattern + emu vlákno safe-point bounce.
 * ============================================================================ */

/**
 * @brief Parametr pro CMD_REGIONS_ENUMERATE.
 *
 * Caller alokuje pole `out` o velikosti `max_count`. Handler vyplní entries
 * a počet v `out_count`. Per documentation v dbgapi_regions.h - IDs jsou
 * stable v rámci jedné enumerate volání; mezi voláními se po HW
 * reconfigure (= memext/ramdisk attach/detach) může změnit.
 *
 * @field out         (IN/OUT) Pole alokované callerem (min `max_count` položek).
 * @field max_count   (IN) Velikost pole.
 * @field out_count   (OUT) Skutečný počet zapsaných položek.
 */
typedef struct st_DBGAPI_REGIONS_ENUM_PARAM
{
    void *out;       /* st_REGION_DESC* (= dbgapi_regions.h) */
    int max_count;
    int out_count;
} st_DBGAPI_REGIONS_ENUM_PARAM;

/**
 * @brief Parametr pro CMD_REGIONS_READ.
 *
 * Caller předává region_id (= z poslední enumerate volání), offset
 * v rámci regionu, výstupní buffer + požadovanou délku. Handler vyplní
 * out_count s skutečně přečtenou délkou (= clamp pokud offset+len > size).
 *
 * @field region_id   (IN) ID z poslední enumerate.
 * @field offset      (IN) Offset v rámci regionu (0..size-1).
 * @field buf         (IN/OUT) Buffer alokovaný callerem (min `len` bajtů).
 * @field len         (IN) Požadovaná délka.
 * @field out_count   (OUT) Skutečně přečtená délka. -1 při chybě.
 */
typedef struct st_DBGAPI_REGIONS_READ_PARAM
{
    int region_id;
    uint32_t offset;
    uint8_t *buf;
    uint32_t len;
    int out_count;
} st_DBGAPI_REGIONS_READ_PARAM;

/**
 * @brief Parametr pro CMD_REGIONS_WRITE.
 *
 * Caller předává region_id, offset, vstupní data + délku. Handler
 * vyplní out_count s skutečně zapsanou délkou. -1 při chybě (= region
 * disconnected / read-only / unknown id).
 *
 * Pro REGION_KIND_MEMEXT_FLASH a REGION_KIND_PROHIBITED_SHADOW vrátí
 * backend -1 (= read-only z pohledu Memory Browseru, resp. virtual
 * region).
 */
typedef struct st_DBGAPI_REGIONS_WRITE_PARAM
{
    int region_id;
    uint32_t offset;
    const uint8_t *data;
    uint32_t len;
    int out_count;
} st_DBGAPI_REGIONS_WRITE_PARAM;


/**
 * @brief Výstupní parametr pro DBGAPI_CMD_GET_SPEED.
 *
 * Read-only snapshot stavu emulační rychlosti. Handler v dbgapi.c
 * vyplní všechna pole z g_customspeed / g_emulator a vrátí success=true
 * (= pure read bez side efektu).
 *
 * @field current_percent Aktuálně nastavená rychlost v procentech
 *                        (= customspeed_get_current_speed(), 1..4000).
 *                        Význam i při zapnutém max_speed (= poslední
 *                        custom % před warpem), ale při warpu se reálné
 *                        tempo neřídí touto hodnotou.
 * @field max_speed       1 = warp (unthrottled) aktivní, 0 = throttled.
 * @field mode            Textový režim: "max" pokud max_speed,
 *                        jinak "custom" pokud current_percent != 100,
 *                        jinak "normal". NUL-terminated.
 * @field status          Informativní status string z
 *                        emulator_get_speed_status_as_text() (= UI text,
 *                        může být lokalizovaný). NUL-terminated.
 */
typedef struct st_DBGAPI_GET_SPEED_PARAM
{
    uint32_t current_percent;
    uint8_t  max_speed;
    char     mode[16];
    char     status[32];
} st_DBGAPI_GET_SPEED_PARAM;

/**
 * @brief Hodnoty pole `mode` pro st_DBGAPI_SET_SPEED_PARAM.
 *
 * Caller (= MCP dispatch) přeloží string mode z JSON na tento enum.
 * Handler v dbgapi.c podle něj zvolí core funkci.
 */
typedef enum en_DBGAPI_SPEED_MODE
{
    DBGAPI_SPEED_MODE_NORMAL = 0, /**< 100 % (= emulator_switch_to_normal_speed) */
    DBGAPI_SPEED_MODE_CUSTOM = 1, /**< Konkrétní % z pole percent (= customspeed_set_request + warp off) */
    DBGAPI_SPEED_MODE_MAX    = 2, /**< Warp (= emulator_max_speed(true)) */
    DBGAPI_SPEED_MODE_STEP   = 3, /**< Relativní delta z pole step (= customspeed_step_*) */
} en_DBGAPI_SPEED_MODE;

/**
 * @brief Vstupní parametr pro DBGAPI_CMD_SET_SPEED.
 *
 * Handler v dbgapi.c podle `mode` vykoná příslušnou speed operaci na emu
 * vlákně. Po úspěchu vyplní out_* pole aktuálním stavem (= echo pro
 * klienta, ekvivalent GET_SPEED bez druhého round-tripu).
 *
 * Sémantika max speed: zapíná se mode=MAX, vypíná přechodem na
 * NORMAL/CUSTOM (= žádný separátní max_off mode). STEP nemění warp flag,
 * pouze custom %.
 *
 * @field mode      en_DBGAPI_SPEED_MODE - která akce.
 * @field percent   Pro mode=CUSTOM cílové % (1..4000, clamp v core).
 *                  Ignorováno pro ostatní mody.
 * @field step      Pro mode=STEP relativní delta (kladná up, záporná
 *                  down, 0 = no-op). Ignorováno pro ostatní mody.
 * @field out_current_percent Aktuální % po operaci (= echo).
 * @field out_max_speed       Warp flag po operaci (= echo).
 * @field out_mode            Textový režim po operaci ("max"/"custom"/
 *                            "normal"). NUL-terminated.
 */
typedef struct st_DBGAPI_SET_SPEED_PARAM
{
    en_DBGAPI_SPEED_MODE mode;
    int      percent;
    int      step;
    uint32_t out_current_percent;
    uint8_t  out_max_speed;
    char     out_mode[16];
} st_DBGAPI_SET_SPEED_PARAM;


/* ============================================================================
 * CMT-A: transport + recording + cmthack toggle
 * ============================================================================ */

/**
 * @brief Akce transportu pásky pro DBGAPI_CMD_CMT_TRANSPORT.
 *
 * Sjednocuje pět transport operací do jednoho cmd. PAUSE používá navíc
 * pole `pause_value` (= 0 odpauzovat, nenula pauznout); ostatní akce
 * `pause_value` ignorují.
 */
typedef enum en_DBGAPI_CMT_TRANSPORT_ACTION
{
    DBGAPI_CMT_TRANSPORT_PLAY = 0,    /**< cmt_play() */
    DBGAPI_CMT_TRANSPORT_PLAY_PAUSED, /**< cmt_play_paused() */
    DBGAPI_CMT_TRANSPORT_STOP,        /**< cmt_stop() */
    DBGAPI_CMT_TRANSPORT_PAUSE,       /**< cmt_pause(pause_value) */
    DBGAPI_CMT_TRANSPORT_EJECT,       /**< cmt_eject() */
} en_DBGAPI_CMT_TRANSPORT_ACTION;

/**
 * @brief Parametr pro DBGAPI_CMD_CMT_TRANSPORT.
 *
 * Klient zvolí akci a (pro PAUSE) hodnotu pause_value. Handler v dbgapi.c
 * zavolá odpovídající cmt_* funkci na emu vlákně. Transport funkce
 * samy validují stav (= no-op pokud nelze provést), proto out_result
 * je 0 a success true i pro no-op (operace proběhla = byla vykonána
 * příslušná cmt_* funkce, která se sama rozhodla nic nedělat).
 *
 * @invariant action je platná hodnota en_DBGAPI_CMT_TRANSPORT_ACTION,
 *            jinak handler vrátí success=false a out_result=-1.
 */
typedef struct st_DBGAPI_CMT_TRANSPORT_PARAM
{
    en_DBGAPI_CMT_TRANSPORT_ACTION action; /**< IN: zvolená transport akce. */
    uint8_t pause_value;                   /**< IN: hodnota pro PAUSE (0/1). */
    int     out_result;                    /**< OUT: 0 = OK, -1 = neznámá akce. */
} st_DBGAPI_CMT_TRANSPORT_PARAM;

/**
 * @brief Parametr pro DBGAPI_CMD_CMT_RECORD.
 *
 * Zahájí WAV nahrávání do souboru `filepath` přes cmt_record_to_file.
 * Nahrávání startuje v pauze (= cmt_record nastaví paused). Pokud cestu
 * nelze otevřít pro zápis nebo CMT není ve STOP, out_result != 0 a
 * success=false.
 */
typedef struct st_DBGAPI_CMT_RECORD_PARAM
{
    const char *filepath;   /**< IN: cesta k cílovému WAV souboru. */
    int         out_result; /**< OUT: 0 = OK, -1 = neplatný param, -2 = cmt_record_to_file selhal. */
} st_DBGAPI_CMT_RECORD_PARAM;

/**
 * @brief Parametr pro DBGAPI_CMD_CMT_HACK_SET.
 *
 * Zapne/vypne cmthack ROM patch (= instant load) přes
 * cmthack_mzarch_load_rom_patch. Po operaci handler vyplní out_installed
 * z CMTHACK_TEST_IS_INSTALLED (= echo skutečného stavu).
 */
typedef struct st_DBGAPI_CMT_HACK_SET_PARAM
{
    uint8_t enabled;       /**< IN: 1 = zapnout patch, 0 = vypnout. */
    uint8_t out_installed; /**< OUT: stav patche po operaci (CMTHACK_TEST_IS_INSTALLED). */
} st_DBGAPI_CMT_HACK_SET_PARAM;


/* ============================================================================
 * mutant mcp-server CMT-B - vlastnosti CMT + práce s páskou
 * ============================================================================ */

/**
 * @brief Vlastnost CMT pro DBGAPI_CMD_CMT_SET_PROPERTY.
 *
 * Sjednocuje čtyři nastavitelné vlastnosti do jednoho cmd s property
 * selektorem. Význam pole `value` závisí na zvolené property (viz
 * st_DBGAPI_CMT_SET_PROPERTY_PARAM).
 */
typedef enum en_DBGAPI_CMT_PROPERTY
{
    DBGAPI_CMT_PROP_SPEED = 0,    /**< value = en_CMTSPEED (1..9), cmt_change_speed. */
    DBGAPI_CMT_PROP_POLARITY,     /**< value = 0/1, cmt_rear_dip_switch_cmt_inverted_polarity. */
    DBGAPI_CMT_PROP_CPU_BOOST,    /**< value = 0/1, cmt_cpu_boost_set. */
    DBGAPI_CMT_PROP_MZFSIZE_CHECK,/**< value = 0/1, cmt_mzfsize_check_set. */
} en_DBGAPI_CMT_PROPERTY;

/**
 * @brief Parametr pro DBGAPI_CMD_CMT_SET_PROPERTY.
 *
 * Klient zvolí vlastnost a její hodnotu. Pro SPEED je `value` hodnota
 * en_CMTSPEED v rozsahu 1..9 (= CMTSPEED_1_1 .. CMTSPEED_25_14);
 * handler ji validuje přes cmtspeed_is_valid a při neplatné hodnotě
 * vrátí out_result = -1, success = false. Pro POLARITY/CPU_BOOST/
 * MZFSIZE_CHECK je `value` boolean (0/1).
 *
 * @invariant property je platná hodnota en_DBGAPI_CMT_PROPERTY, jinak
 *            handler vrátí success = false a out_result = -1.
 */
typedef struct st_DBGAPI_CMT_SET_PROPERTY_PARAM
{
    en_DBGAPI_CMT_PROPERTY property; /**< IN: zvolená vlastnost. */
    int                    value;    /**< IN: hodnota (význam dle property). */
    int                    out_result; /**< OUT: 0 = OK, -1 = neplatná property/hodnota. */
} st_DBGAPI_CMT_SET_PROPERTY_PARAM;

/**
 * @brief Parametr pro DBGAPI_CMD_CMT_OPEN.
 *
 * Otevře CMT soubor přes cmt_open_file_by_extension (non-UI). Při
 * `play_immediately` handler navíc po úspěšném openu zavolá cmt_play()
 * (= přesně jako cmt_ui_open_cb). Nerozpoznaná přípona nebo selhání
 * cb_open -> out_result != 0, success = false.
 */
typedef struct st_DBGAPI_CMT_OPEN_PARAM
{
    const char *filepath;         /**< IN: cesta k CMT souboru (.mzf/.mzt/.wav/...). */
    uint8_t     play_immediately; /**< IN: 1 = po openu spustit přehrávání. */
    int         out_result;       /**< OUT: 0 = OK, -1 = neplatný param, -2 = open selhal. */
} st_DBGAPI_CMT_OPEN_PARAM;

/**
 * @brief Parametr pro DBGAPI_CMD_CMT_TAPE_SEEK.
 *
 * Seek na blok `block_id` přes container->cb_open_block. Vyžaduje
 * naloženou pásku (g_cmt.ext != NULL) s containerem. Mimo rozsah nebo
 * bez pásky -> out_result != 0, success = false.
 */
typedef struct st_DBGAPI_CMT_TAPE_SEEK_PARAM
{
    int block_id;   /**< IN: cílový blok (0-based). */
    int out_result; /**< OUT: 0 = OK, -1 = bez pásky, -2 = seek selhal. */
} st_DBGAPI_CMT_TAPE_SEEK_PARAM;

/**
 * @brief Parametr pro DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED.
 *
 * Nastaví per-blok cmt rychlost přes cmtext_container_set_block_cmt_speed.
 * Per Michal lze per-blok nastavit JEN cmt speed (= žádné další parametry).
 * `cmtspeed` musí být platná en_CMTSPEED hodnota (1..9). Bez pásky nebo
 * neplatná rychlost -> out_result != 0, success = false.
 */
typedef struct st_DBGAPI_CMT_TAPE_BLOCK_SPEED_PARAM
{
    int block_id;   /**< IN: cílový blok (0-based). */
    int cmtspeed;   /**< IN: en_CMTSPEED hodnota (1..9). */
    int out_result; /**< OUT: 0 = OK, -1 = bez pásky, -2 = neplatná rychlost. */
} st_DBGAPI_CMT_TAPE_BLOCK_SPEED_PARAM;

/** @brief Maximální délka názvu bloku v st_DBGAPI_CMT_TAPE_BLOCK_ENTRY (vč. NUL). */
#define DBGAPI_CMT_TAPE_NAME_MAX 64

/**
 * @brief Záznam jednoho bloku pásky pro DBGAPI_CMD_CMT_TAPE_LIST.
 *
 * Fixní buffer pro `name` (= žádné heap stringy, caller jen uvolní
 * samotné pole entries). `cmtspeed` je en_CMTSPEED hodnota bloku.
 * `type` je en_CMTEXT_BLOCK_TYPE (0=WAV, 1=MZF, 2=TAPHEADER, 3=TAPDATA).
 * `playable`/`recordable` jsou per-extension příznaky (= stejné pro
 * všechny bloky téže pásky), kopírují se do každého záznamu pro pohodlí.
 */
typedef struct st_DBGAPI_CMT_TAPE_BLOCK_ENTRY
{
    int     block_id;                        /**< Index bloku (0-based). */
    char    name[ DBGAPI_CMT_TAPE_NAME_MAX ];/**< Název bloku (clamp na buffer). */
    int     cmtspeed;                        /**< en_CMTSPEED rychlost bloku. */
    uint8_t type;                            /**< en_CMTEXT_BLOCK_TYPE. */
    uint8_t is_current;                      /**< 1 = právě přehrávaný blok. */
    uint8_t playable;                        /**< 1 = páska je playable. */
    uint8_t recordable;                      /**< 1 = páska je recordable. */
} st_DBGAPI_CMT_TAPE_BLOCK_ENTRY;

/**
 * @brief Parametr pro DBGAPI_CMD_CMT_TAPE_LIST (read-only).
 *
 * Caller alokuje pole `entries` o velikosti `capacity`; handler vyplní
 * první `out_count` polí. Pokud má páska víc bloků než capacity, zbytek
 * se odřízne (= out_count == capacity, klient by měl velikost zvětšit).
 *
 * `available` = 0 pokud není naložena páska / chybí container; v tom
 * případě je out_count = 0 a klient čte důvod z available. `container_type`
 * je en_CMTEXT_CONTAINER_TYPE (0=SINGLE, 1=SIMPLE_TAPE). `current_block`
 * je index právě přehrávaného bloku (-1 pokud žádný).
 */
typedef struct st_DBGAPI_CMT_TAPE_LIST_PARAM
{
    st_DBGAPI_CMT_TAPE_BLOCK_ENTRY *entries;       /**< (IN) Caller-allocated pole. */
    size_t                          capacity;      /**< (IN) Velikost pole. */
    size_t                          out_count;     /**< (OUT) Počet zapsaných záznamů. */
    uint8_t                         available;     /**< (OUT) 1 = páska + container k dispozici. */
    uint8_t                         container_type;/**< (OUT) en_CMTEXT_CONTAINER_TYPE. */
    int                             current_block; /**< (OUT) index aktuálního bloku, -1 = žádný. */
} st_DBGAPI_CMT_TAPE_LIST_PARAM;


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DBGAPI_CMDRQ_H */
