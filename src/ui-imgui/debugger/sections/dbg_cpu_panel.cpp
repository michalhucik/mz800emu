/*
 * dbg_cpu_panel.cpp - implementace CPU registr panelu v debuggeru.
 *
 * Variant A compact layout - pravý sloupec hlavního debug okna.
 *
 * Layout (cca 280-320 px sirka):
 *
 *   +--------------- CPU ---------------+
 *   | > PC: 4242    > SP: FFEC          |
 *   | > AF: 0042   > AF': 0000          |
 *   | > BC: 1234   > BC': 0000          |
 *   | > DE: 5678   > DE': 0000          |
 *   | > HL: 9ABC   > HL': 0000          |
 *   | > IX: 0000   > IY: 0000           |
 *   |                                   |
 *   | F: [S][Z][5][H][3][P][N][C]       |
 *   |     0  1  0  0  0  0  1  0        |
 *   |                                   |
 *   | Special                           |
 *   |   I: 00   R: 7F  IM: 1            |
 *   |   IFF1: ON  IFF2: ON              |
 *   |   HALT: -                         |
 *   +-----------------------------------+
 *
 * Refresh: pres g_dbg_ui.refresh.should_refresh - panel ctve dbgapi
 * (GET_ALL_REGS + GET_CPU_FLAGS) jen kdyz je tick. Mezi tiky pouziva
 * cache (panel_state.regs[], panel_state.flags).
 *
 * Stable ID strategie: kazda interaktivni komponenta ma "###suffix"
 * (tj. PushID per komponenta neni potreba, vyhneme se label-as-ID
 * problemu pri dynamic label).
 *
 * V0 rozsah:
 *   - Registr file read-only (zlate fade pri zmene)
 *   - Flag rozpis F klikatelny (paused: toggle bit; running: autopauza)
 *   - Special sekce read-only
 *   - > focus button - levy klik focus primary disasm, pravy klik popup
 *     (V0: jen "Focus to Disassembly #1" + Copy hex/dec/bin)
 *   - Klik na hodnotu registru -> request autopauza
 *
 * V1 rozsireni:
 *   - In-place edit hodnoty registru: single-click v running zaroven
 *     pauzne A otevre InputText (Alt A flow z planu, otazka 11).
 *     Enter = parse + SET_REG, Esc = cancel, Tab = apply + skok na dalsi
 *     editovatelny registr.
 *   - Per-register format toggle (hex/dec/bin), session-only (nepersistuje).
 *     Prepinani pres polozku "Toggle format" v popup menu.
 *   - Rozsireny RMB popup: "Add to Watch" (= bp_var_set, vytvori static
 *     snapshot $REG = value), "Add BPT at <reg>" (= dbg_ui_bp_add na
 *     hodnotu 16-bit registru).
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
#include <string.h>
#include <stdint.h>

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "dbg_cpu_panel.h"
#include "dbg_disassembled.h"
#include "ui-imgui/debugger/debugger_state.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"

extern "C" {
#include "emulator/debugger/dbgapi_cmdrq.h"
#include "emulator/debugger/dbgapi_ui.h"
#include "emulator/debugger/bp_vars.h"
#include "emulator/emulator.h"
#include "emulator/hw-generic/memory/memory.h"
#include "libs/cpu-z80/z80.h"
}

/* g_dbgapi_cmdrq_queue je definovany v emulator/debugger/dbgapi.c. */
extern "C" st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


/* ========================================================================= */
/*  Panel state                                                              */
/* ========================================================================= */

namespace {

/**
 * @brief Format zobrazeni hodnoty registru pro per-register cyklus.
 *
 * Cyklus: HEX -> DEC -> BIN -> HEX (V1). Session-only (nepersistuje
 * do .ini). Konfigurace per-registr v g_cpu.format[reg_id].
 */
enum CPURegFormat {
    CPU_FMT_HEX = 0,
    CPU_FMT_DEC = 1,
    CPU_FMT_BIN = 2,
    CPU_FMT_COUNT_
};

/**
 * @brief Virtualni reg IDs pro I a R 8-bit registry (V3).
 *
 * z80_reg_t enum ma jen 16-bit Z80_REG_IR (= slozeny I:R). Pro edit
 * 8-bit I a R samostatne potrebujeme virtualni reg IDs mimo
 * DBGAPI_REG_COUNT rozsah. Edit flow detekuje tyto IDs a misto
 * SET_REG(IR) posila SET_CPU_FLAGS s odpovidajicim update_mask bitem.
 *
 * Hodnoty zacinaji od 100 aby byla jasna separace od skutecnych
 * z80_reg_t indexu (0..DBGAPI_REG_COUNT-1 = 0..13).
 */
enum CPUVirtualReg {
    CPU_VREG_I = 100,
    CPU_VREG_R = 101,
    /* V3.3: PIO-Z80 interrupt vector + ISR target. 16-bit, custom edit
     * commit (= ne SET_REG/SET_CPU_FLAGS, ale 2-step VEC = SET_CPU_FLAGS.I
     * + SET_PIOZ80_INTERRUPT_VECTOR resp. MEM_WRITE_CHECKED pro ISR). */
    CPU_VREG_VECA = 200,
    CPU_VREG_VECB = 201,
    CPU_VREG_ISRA = 202,
    CPU_VREG_ISRB = 203
};

/** @brief Test zda reg_id je virtualni 8-bit pseudo-registr (I/R). */
static inline bool cpu_reg_is_virtual_8bit(int reg_id)
{
    return reg_id == CPU_VREG_I || reg_id == CPU_VREG_R;
}

/** @brief Test zda reg_id je virtualni 16-bit pseudo-registr PIO-Z80
 *  (VECA/VECB/ISRA/ISRB). */
static inline bool cpu_reg_is_virtual_pio16(int reg_id)
{
    return reg_id == CPU_VREG_VECA || reg_id == CPU_VREG_VECB
        || reg_id == CPU_VREG_ISRA || reg_id == CPU_VREG_ISRB;
}

/** @brief Test zda reg_id je některý z VEC* pseudo-registrů. */
static inline bool cpu_reg_is_vec(int reg_id)
{
    return reg_id == CPU_VREG_VECA || reg_id == CPU_VREG_VECB;
}

/** @brief Test zda reg_id je některý z ISR* pseudo-registrů. */
static inline bool cpu_reg_is_isr(int reg_id)
{
    return reg_id == CPU_VREG_ISRA || reg_id == CPU_VREG_ISRB;
}

/** @brief Mapování VEC/ISR pseudo-reg na port_id PIO-Z80 (0 = A, 1 = B). */
static inline int cpu_reg_to_pio_port(int reg_id)
{
    if (reg_id == CPU_VREG_VECA || reg_id == CPU_VREG_ISRA) return 0;
    if (reg_id == CPU_VREG_VECB || reg_id == CPU_VREG_ISRB) return 1;
    return -1;
}

/**
 * @brief Persistentni stav CPU panelu (drzeny mezi framy).
 *
 * Cache poslednich precteni z dbgapi + per-register casy zmen pro
 * change highlighting + V1 edit/format state.
 *
 * @invariant regs[] je naplnen vzdy kdyz panel_state.regs_valid == true.
 * @invariant flags je naplnen vzdy kdyz panel_state.flags_valid == true.
 * @invariant editing_reg in [-1, DBGAPI_REG_COUNT). -1 = neprobiha edit.
 */
struct CPUPanelState {
    /* Hodnoty Z80 registru indexovane dle z80_reg_t (Z80_REG_AF..Z80_REG_IR). */
    uint16_t regs[DBGAPI_REG_COUNT];

    /* Doplnkovy CPU stav (IFF, IM, HALT, ...). */
    st_DBGAPI_CPU_FLAGS flags;

    /* Predchozi stav pro diff vs zmena (golden fade) - V0 placeholder. */
    uint16_t prev_regs[DBGAPI_REG_COUNT];

    /* Cas posledni zmeny per registru (ImGui::GetTime() v sekundach).
     * 0.0 = nikdy nezmeneno. V0 placeholder - pouziva se v commitu
     * "change highlighting". */
    double last_change_time[DBGAPI_REG_COUNT];

    /* Valid flags - prvni refresh tick je naplni. Dokud nejsou true,
     * panel zobrazuje "----" placeholder. */
    bool regs_valid;
    bool flags_valid;

    /* Cilovy registr pro RMB popup (focus button >). Indexovany dle
     * z80_reg_t. Nastavi se v render row, otevre v parent scope. */
    int rmb_popup_reg_id;
    bool rmb_popup_open;

    /* === V1: in-place edit === */
    /* Aktualne editovany registr (-1 = zadny). Pri startu edit se naplni
     * edit_buf hodnotou registru v aktualnim formatu. */
    int  editing_reg;
    /* Buffer pro InputText. Velikost staci na 16-bit binarni reprezentaci
     * vc. mezery a terminatoru ("0000 0000 0000 0000\0"). */
    char edit_buf[20];
    /* Flag pro one-shot SetKeyboardFocusHere() pri otevreni editoru. */
    bool edit_focus_pending;
    /* Pokud true, po commit (Enter/Tab) skoc na dalsi registr v poradi
     * (pouziva Tab klavesa). */
    bool edit_advance_next;

    /* === V1: per-register format (session-only) === */
    uint8_t format[DBGAPI_REG_COUNT];

    /* === V2: IM2 ISR sekce (jen MZ-800/MZ-1500, auto-hide na MZ-700) === */
    st_DBGAPI_IM2_VECTOR im2;
    bool                 im2_valid;

    /* === V2: Cycles & raster sekce === */
    st_DBGAPI_RASTER_POS raster;
    bool                 raster_valid;

    /* V3: Last instruction sekce odstranena z UI (Michalova zpetna
     * vazba 2026-05-10). Handler DBGAPI_CMD_GET_LAST_INSTR v dbgapi.c
     * zustal jako dead code (= neni volan z UI, ale infrastruktura
     * existuje pro pripadny budouci tracer / overlay). */

    /* RMB popup pro IM2 ISR target (Focus DA #1-5). Drzime samostatne
     * od register popupu - target je 16-bit adresa, ne reg_id. */
    uint16_t im2_rmb_addr;
    bool     im2_rmb_open;

    /* === V2.1 perf: per-section open tracking pro gating batch refresh ===
     *
     * Render funkce sekci (im2/raster) na zacatku zjisti stav
     * CollapsingHeader a zapise sem (= true pokud expanded v aktualnim
     * framu). Pristi refresh tick podle techto flagu sestavi which-mask
     * pro DBGAPI_CMD_GET_CPU_PANEL_BATCH. Sekce zustane prazdna 1 frame
     * po expand (= prijatelne, fade hned naskoci pri prvnim batch). */
    bool section_im2_open;
    bool section_raster_open;

    /* === V3.1: core fieldy z batche (vzdy fetchovane) === */
    /* Posledni snimek video paprsku z batche (= g_gdg.total_elapsed.screens).
     * UI z neho detekuje prechod na novy frame a snapshot total_cycles
     * pro Frame cyc display. */
    uint32_t frame_number;
    bool     frame_number_valid;

    /* Snapshot cpu->total_cycles v okamziku, kdy frame_number naposledy
     * zmenil hodnotu (= zacatek aktualniho snimku). Frame cyc display
     * = total_cycles - total_cycles_at_frame_start. */
    uint32_t total_cycles_at_frame_start;
    bool     total_cycles_snapshot_valid;

    /* User cycle origin z g_debugger (= emu side storage). UI display
     * = total_cycles - user_cycle_origin. */
    uint32_t user_cycle_origin;

    /* === V3.1: edit stav pro User cyc value === */
    bool  editing_user_cyc;
    char  user_cyc_edit_buf[16];
    bool  user_cyc_edit_focus_pending;

    /* === V3.3: PIO-Z80 interrupt vector + ISR target cache ===
     *
     * Naplněno batch fetchem (has_pioz80=1 pro MZ-800/MZ-1500). UI sekce
     * (2 řádky VECA/ISRA + VECB/ISRB) se zobrazuje jen pokud has_pioz80.
     *
     * pio_int_vec_a/b je raw hodnota registru interrupt_vector portu
     * (bez & 0xFE) - používá se při edit commit ISR pro spočtení VEC
     * adresy ze cache, kdyby refresh nepřišel mezi tím.
     *
     * pio_valid je true po prvním úspěšném batch fetch s has_pioz80=1. */
    uint8_t  has_pioz80;
    uint8_t  pio_int_vec_a;
    uint8_t  pio_int_vec_b;
    uint16_t veca;
    uint16_t vecb;
    uint16_t isra;
    uint16_t isrb;
    bool     pio_valid;

    /* === V3.3: modal warning pro ISR write fail (region read-only) ===
     *
     * Po nepovedeném MEM_WRITE_CHECKED nastaví edit commit modal_addr +
     * modal_kind a otevře modal s informací o adrese a typu regionu.
     * Modal_pending true triggeruje OpenPopup v render fázi mimo
     * BeginPopupModal scope. */
    bool     pio_isr_modal_pending;
    bool     pio_isr_modal_open;
    uint16_t pio_isr_modal_addr;
    uint8_t  pio_isr_modal_kind;
};

static CPUPanelState g_cpu = {
    {0}, {0}, {0}, {0.0}, false, false, -1, false,
    -1, "", false, false, {0},
    /* V2: im2, im2_valid, raster, raster_valid */
    {0}, false, {0}, false,
    /* im2_rmb_addr, im2_rmb_open */
    0, false,
    /* V2.1 perf: section_im2_open, section_raster_open
     * default true aby prvni refresh nasahnul vsechna data (= prevention
     * of "section expanded but data not loaded" first-frame artefactu).
     * V3.1: IM2 sekce odstranena, section_im2_open defaultne false. */
    false, true,
    /* V3.1: frame_number, frame_number_valid, total_cycles_at_frame_start,
     * total_cycles_snapshot_valid, user_cycle_origin */
    0, false, 0, false, 0,
    /* V3.1: editing_user_cyc, user_cyc_edit_buf, user_cyc_edit_focus_pending */
    false, "", false,
    /* V3.3 PIO-Z80 cache: has_pioz80, pio_int_vec_a/b, veca, vecb, isra, isrb, pio_valid */
    0, 0, 0, 0, 0, 0, 0, false,
    /* V3.3 modal: pio_isr_modal_pending, pio_isr_modal_open, addr, kind */
    false, false, 0, 0
};

} /* anon namespace */


/* ========================================================================= */
/*  Refresh dat z emulátoru                                                  */
/* ========================================================================= */


/**
 * @brief Doba zvyrazneni zmenene hodnoty (golden fade) v sekundach.
 *
 * Po zmene hodnoty se prislusny registr renderuje zlate (DBG_CPU_FADE_COLOR)
 * a behem teto doby linearne prechazi zpet na default barvu (= mix(zlata,
 * default, t)). Reference: design plan, sekce 5 "Change highlighting"
 * - DBG_HIGHLIGHT_FADE_MS = 1500.
 */
#define DBG_CPU_FADE_SEC 1.5


/**
 * @brief Provede agregovany fetch CPU panel dat pres single dbgapi call.
 *
 * Pred V2.1 perf: 5 separatnich GET_* sync prikazu, kazdy cekal na
 * safepoint emu vlakna -> ~5x drain interval (~100 ms pri bezici emulaci).
 * Po V2.1: jeden DBGAPI_CMD_GET_CPU_PANEL_BATCH s which-mask sestavenou
 * z section_*_open flagu (Fix B - per-section gating: skip data sekci,
 * ktere jsou v UI collapsed).
 *
 * Vola se v okamzicich kdy g_dbg_ui.refresh.should_refresh == true.
 * Mezi refreshi panel pouziva cache. Timeout 5 ms (Fix C) - pokud emu
 * vlakno neni responsive za tu dobu, refresh skipuje a sekce zustanou
 * na cache hodnotach z minuleho ticku.
 *
 * Diff vs prev_regs: po uspesnem batch update aktualizujeme
 * last_change_time[reg] pro vsechny registry, jejichz hodnota se zmenila.
 * Aktualni hodnoty se pak prepisi do prev_regs (= baseline pro pristi diff).
 * Pri prvnim refreshi (regs_valid == false vstup) nedochazi k vytvareni
 * "umelych" zmen - prev_regs se naplni stejnymi hodnotami jako regs a
 * last_change_time zustane 0.
 *
 * Side effect: aktualizuje g_cpu.regs/flags/im2/raster/last_*, prev_regs,
 * last_change_time a *_valid flagy.
 */
static void cpu_panel_refresh(void)
{
    /* Self-rate-limit: pokud je CMDRQ fronta plná (= emu vlákno blokuje,
     * např. CMT hack čeká na FileBrowser, nebo dělá nějakou dlouhou
     * operaci), nemá smysl posílat další request - jen by se zahodil
     * a vygenerovaly debug warningy. Skipneme tento refresh tick, příští
     * (za 100 ms) zase zkusíme. Cache zachová poslední fetchne data. */
    if (dbgapi_ui_queue_is_full(&g_dbgapi_cmdrq_queue))
    {
        return;
    };

    /* Sestavit which-masku z section open flagu. Core (regs+flags) se
     * naplnuje vzdy automaticky, samostatny WANT_ flag pro ne nemame. */
    uint32_t which = 0;
    if (g_cpu.section_raster_open) which |= DBGAPI_CPU_PANEL_WANT_RASTER;
    /* V3: Last instruction sekce odstranena - WANT_LAST_INSTR se neptame.
     * V3.1: IM2 ISR sekce odstranena - WANT_IM2 se neptame.
     * Handlery v emu zustaly jako dead code (= konzervativni cleanup,
     * struktura st_DBGAPI_CPU_PANEL_BATCH zachovava ABI). */

    /* Timeout 50 ms: emu safepoint je per-frame (~20 ms pri 50 Hz), takze
     * realny round trip je obvykle 1-20 ms. Kratsi timeout (5 ms) byl
     * casto kratsi nez safepoint -> vetsina batchu timeout-ovala, UI
     * panel zaseknul cache az do priste, ze ktere zase casto timeout
     * (= viditelne "2s refresh, 2s spi" cykly). 50 ms = bezpecna rezerva
     * pri zachovani full UI responsivity (1 sync call max 50 ms blokace
     * na 100 ms tick = >= 50% UI). */
    st_DBGAPI_CPU_PANEL_BATCH batch;
    if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                    DBGAPI_CMD_GET_CPU_PANEL_BATCH,
                                    &which, &batch, 50))
    {
        /* Timeout / queue full / emu ending - ponechat cache. */
        return;
    };

    /* === Regs (vzdy validni v batch) === */
    if (batch.regs_valid)
    {
        double now = ImGui::GetTime();
        bool first_load = !g_cpu.regs_valid;

        for (int i = 0; i < DBGAPI_REG_COUNT; i++)
        {
            if (!first_load && batch.regs[i] != g_cpu.regs[i]) {
                g_cpu.last_change_time[i] = now;
            };
            g_cpu.prev_regs[i] = g_cpu.regs[i];
            g_cpu.regs[i]      = batch.regs[i];
        };
        g_cpu.regs_valid = true;
    };

    /* === Flags (vzdy validni v batch) === */
    if (batch.flags_valid)
    {
        g_cpu.flags = batch.flags;
        g_cpu.flags_valid = true;
    };

    /* === V3.1 core fieldy (vzdy plnene v batchi) ===
     *
     * Detekce prechodu na novy frame: pokud batch.frame_number != predchozi
     * cache -> nove snapshot total_cycles do total_cycles_at_frame_start.
     * Pri prvnim refreshi (frame_number_valid == false) inicializuj
     * snapshot na aktualni total_cycles - Frame cyc bude od ted ukazovat
     * cycles od prvniho videneho framu (= aproximace, prvni hodnota muze
     * byt mensi nez skutecny pocet do konce daneho framu).
     *
     * Note: refresh tick je ~100 ms (= 5 framu PAL 50 Hz), takze mezi
     * tiky padly 5 framu - sledujeme jen frame_number AT THE TICK TIME,
     * ne kazdy fyzicky frame. Frame cyc display tedy reprezentuje pocet
     * T-states v ramci snimku ve kterem byl batch precten - dostacujici
     * granularita pro UI (jiny tick = jiny snimek). */
    if (batch.flags_valid)
    {
        uint32_t cur_total = batch.flags.total_cycles;
        if (!g_cpu.frame_number_valid)
        {
            g_cpu.total_cycles_at_frame_start = cur_total;
            g_cpu.total_cycles_snapshot_valid = true;
        }
        else if (batch.frame_number != g_cpu.frame_number)
        {
            /* Novy snimek - snapshot total_cycles. Realny pocet cyklu
             * v predchozim framu byl (cur_total - snapshot_pred), ale
             * uzivatele zajima aktualni snimek -> ulozime hned cur_total
             * jako novy start. */
            g_cpu.total_cycles_at_frame_start = cur_total;
            g_cpu.total_cycles_snapshot_valid = true;
        };
        g_cpu.frame_number       = batch.frame_number;
        g_cpu.frame_number_valid = true;
        g_cpu.user_cycle_origin  = batch.user_cycle_origin;
    };

    /* === IM2 (jen pokud sekce expanded a batch ji naplnil) === */
    if (batch.im2_valid)
    {
        g_cpu.im2 = batch.im2;
        g_cpu.im2_valid = true;
    };
    /* Pokud sekce collapsed, ponechame cache - nebudeme znevalidovavat,
     * abychom pri rozbaleni meli okamzite co zobrazit (1-frame stara data). */

    /* === Raster (jen pokud sekce expanded) === */
    if (batch.raster_valid)
    {
        g_cpu.raster = batch.raster;
        g_cpu.raster_valid = true;
    };

    /* V3: Last instruction sekce odstranena z UI - batch nikdy nevraci
     * last_instr_valid (which mask nemam bit set). */

    /* === V3.3: PIO-Z80 interrupt vector + ISR target (core fieldy) ===
     *
     * Batch je vždy plní pokud HAVE_PIOZ80 - pro MZ-700 batch.has_pioz80
     * zůstane 0 (= UI sekci pak nezobrazí). */
    g_cpu.has_pioz80    = batch.has_pioz80;
    g_cpu.pio_int_vec_a = batch.pio_int_vec_a;
    g_cpu.pio_int_vec_b = batch.pio_int_vec_b;
    g_cpu.veca          = batch.veca;
    g_cpu.vecb          = batch.vecb;
    g_cpu.isra          = batch.isra;
    g_cpu.isrb          = batch.isrb;
    g_cpu.pio_valid     = (batch.has_pioz80 != 0);
}


/**
 * @brief Vrati barvu pro hodnotu registru s fade efektem dle zmeny.
 *
 * Pokud byla hodnota registru zmenena pred dobou kratsi nez DBG_CPU_FADE_SEC,
 * vrati barvu mezi DBG_CPU_FADE_COLOR (zlata) a default text colorem,
 * linearne interpolovanou podle uplynule doby. Po DBG_CPU_FADE_SEC vrati
 * default ImGui text color.
 *
 * @param reg_id  Index registru (Z80_REG_*).
 * @return        ImU32 barva pro ImGui::PushStyleColor(ImGuiCol_Text, ..).
 */
static ImU32 cpu_panel_get_value_color(int reg_id)
{
    ImU32 default_col = ImGui::GetColorU32(ImGuiCol_Text);
    if (reg_id < 0 || reg_id >= DBGAPI_REG_COUNT) return default_col;

    double last = g_cpu.last_change_time[reg_id];
    if (last <= 0.0) return default_col;

    double age = ImGui::GetTime() - last;
    if (age < 0.0 || age >= DBG_CPU_FADE_SEC) return default_col;

    /* Linearni interpolace zlata -> default. t=0 zlata, t=1 default. */
    float t = (float)(age / DBG_CPU_FADE_SEC);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    ImVec4 gold(1.0f, 0.78f, 0.20f, 1.0f);
    ImVec4 dflt = ImGui::ColorConvertU32ToFloat4(default_col);
    ImVec4 mix(
        gold.x + (dflt.x - gold.x) * t,
        gold.y + (dflt.y - gold.y) * t,
        gold.z + (dflt.z - gold.z) * t,
        gold.w + (dflt.w - gold.w) * t);
    return ImGui::ColorConvertFloat4ToU32(mix);
}


/* ========================================================================= */
/*  V1: Format / parse helpers                                               */
/* ========================================================================= */


/**
 * @brief Vrati pocet bitu pro dany registr (8 nebo 16).
 *
 * Hlavni registr file je 16-bit (AF..HL2, IX/IY, SP, PC, WZ, IR).
 * IR ma sice 2x 8-bit polovinu, ale dbgapi vraci jako 16-bit slozenou
 * hodnotu - editujeme tedy jako 16-bit (= I:R).
 */
static int cpu_panel_reg_bits(int reg_id)
{
    /* V3: virtualni 8-bit I a R registry (pres SET_CPU_FLAGS). */
    if (cpu_reg_is_virtual_8bit(reg_id)) return 8;
    /* V3.3: PIO-Z80 VEC a ISR jsou 16-bit. */
    if (cpu_reg_is_virtual_pio16(reg_id)) return 16;
    /* Vsechny ostatni registry pristupne pres GET_ALL_REGS jsou 16-bit */
    return 16;
}


/**
 * @brief Zformatuje hodnotu registru do retezce dle aktualniho formatu.
 *
 * @param value   Hodnota registru (16-bit).
 * @param format  CPU_FMT_HEX / DEC / BIN.
 * @param bits    Pocet bitu (8 nebo 16) - pro bin a hex sirku padu.
 * @param out     Vystupni buffer.
 * @param out_sz  Velikost vystupniho bufferu.
 *
 * Hex: 4 znaky velkym pismem ("FFFF"), 8-bit varianta 2 znaky.
 * Dec: unsigned (0..65535 / 0..255).
 * Bin: 16-bit oddeleny na dve 8-tici mezerou ("01010101 11001100"),
 *      8-bit jen "01010101".
 */
static void cpu_panel_format_value(uint16_t value, int format, int bits,
                                    char *out, size_t out_sz)
{
    if (bits == 8) {
        uint8_t v8 = (uint8_t)(value & 0xFF);
        switch (format) {
            case CPU_FMT_DEC:
                snprintf(out, out_sz, "%u", (unsigned)v8);
                break;
            case CPU_FMT_BIN: {
                char buf[16];
                for (int i = 0; i < 8; i++)
                    buf[i] = ((v8 >> (7 - i)) & 1) ? '1' : '0';
                buf[8] = '\0';
                snprintf(out, out_sz, "%s", buf);
                break;
            };
            case CPU_FMT_HEX:
            default:
                snprintf(out, out_sz, "%02X", v8);
                break;
        };
    } else {
        switch (format) {
            case CPU_FMT_DEC:
                snprintf(out, out_sz, "%u", (unsigned)value);
                break;
            case CPU_FMT_BIN: {
                char buf[20];
                for (int i = 0; i < 8; i++)
                    buf[i] = ((value >> (15 - i)) & 1) ? '1' : '0';
                buf[8] = ' ';
                for (int i = 0; i < 8; i++)
                    buf[9 + i] = ((value >> (7 - i)) & 1) ? '1' : '0';
                buf[17] = '\0';
                snprintf(out, out_sz, "%s", buf);
                break;
            };
            case CPU_FMT_HEX:
            default:
                snprintf(out, out_sz, "%04X", value);
                break;
        };
    };
}


/**
 * @brief Parsuje retezec v danem formatu na 16-bit hodnotu.
 *
 * Tolerance:
 *   - HEX: akceptuje volitelny "0x" / "$" prefix, hex cifry (case-insens).
 *   - DEC: akceptuje volitelne znamenko (`-` -> two's complement pro 8/16 bit).
 *   - BIN: akceptuje volitelny "0b" prefix, ignoruje mezery (oddelovac 8-tic).
 *
 * Range check: 16-bit max 0xFFFF (unsigned) nebo -32768..32767 (signed).
 *               8-bit  max 0xFF   (unsigned) nebo -128..127 (signed).
 *
 * @param str         Vstupni retezec (NULL = parse fail).
 * @param format      CPU_FMT_*.
 * @param bits        8 nebo 16.
 * @param out_value   Vystupni hodnota (pouze pri uspechu).
 * @return            true pri uspesnem parse v range, false jinak.
 */
static bool cpu_panel_parse_value(const char *str, int format, int bits,
                                   uint16_t *out_value)
{
    if (!str || !out_value) return false;

    /* Skip leading whitespace */
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '\0') return false;

    uint32_t max_uns = (bits == 8) ? 0xFFu : 0xFFFFu;
    int32_t  min_sig = (bits == 8) ? -128 : -32768;
    int32_t  max_sig = (bits == 8) ?  127 :  32767;

    switch (format) {
        case CPU_FMT_HEX: {
            /* Volitelny prefix */
            if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
            else if (str[0] == '$') str += 1;
            if (*str == '\0') return false;
            uint32_t acc = 0;
            int digits = 0;
            for (; *str; str++) {
                if (*str == ' ' || *str == '\t') continue;
                int d;
                if (*str >= '0' && *str <= '9') d = *str - '0';
                else if (*str >= 'a' && *str <= 'f') d = 10 + (*str - 'a');
                else if (*str >= 'A' && *str <= 'F') d = 10 + (*str - 'A');
                else return false;
                acc = (acc << 4) | (uint32_t)d;
                digits++;
                if (acc > max_uns) return false;
            };
            if (digits == 0) return false;
            *out_value = (uint16_t)acc;
            return true;
        };

        case CPU_FMT_DEC: {
            bool negative = false;
            if (*str == '+') str++;
            else if (*str == '-') { negative = true; str++; }
            if (*str < '0' || *str > '9') return false;
            uint32_t acc = 0;
            for (; *str; str++) {
                if (*str == ' ' || *str == '\t') continue;
                if (*str < '0' || *str > '9') return false;
                acc = acc * 10u + (uint32_t)(*str - '0');
                if (acc > 0xFFFFFFFFu / 10u) return false; /* overflow guard */
            };
            if (negative) {
                int64_t signed_val = -(int64_t)acc;
                if (signed_val < min_sig || signed_val > max_sig) return false;
                /* Two's complement do unsigned reprezentace pro vraceni */
                if (bits == 8)
                    *out_value = (uint16_t)((uint8_t)(int8_t)signed_val);
                else
                    *out_value = (uint16_t)((int16_t)signed_val);
            } else {
                if (acc > max_uns) return false;
                *out_value = (uint16_t)acc;
            };
            return true;
        };

        case CPU_FMT_BIN: {
            if (str[0] == '0' && (str[1] == 'b' || str[1] == 'B')) str += 2;
            uint32_t acc = 0;
            int digits = 0;
            for (; *str; str++) {
                if (*str == ' ' || *str == '\t' || *str == '_') continue;
                if (*str == '0') acc = (acc << 1);
                else if (*str == '1') acc = (acc << 1) | 1u;
                else return false;
                digits++;
                if (acc > max_uns) return false;
            };
            if (digits == 0) return false;
            *out_value = (uint16_t)acc;
            return true;
        };
    };
    return false;
}


/**
 * @brief Cyklus formatu hex -> dec -> bin -> hex.
 *
 * @param current  Aktualni format.
 * @return         Nasledujici format v cyklu.
 */
static int cpu_panel_next_format(int current)
{
    return (current + 1) % (int)CPU_FMT_COUNT_;
}


/**
 * @brief Pocet znaku potrebnych pro vykresleni hodnoty v danem formatu.
 *
 * Pouziva se pro CalcTextSize pri vypoctu sirky Selectable/InputText
 * (= musime mit stabilni column layout napric registry s ruznymi formaty).
 *
 * @param format  CPU_FMT_*.
 * @param bits    8 nebo 16.
 * @return        Pocet znaku (worst-case sirka).
 */
static int cpu_panel_format_width(int format, int bits)
{
    switch (format) {
        case CPU_FMT_DEC: return (bits == 8) ? 3 : 5;
        case CPU_FMT_BIN: return (bits == 8) ? 8 : 17; /* "FFFFFFFF FFFFFFFF" */
        case CPU_FMT_HEX:
        default:          return (bits == 8) ? 2 : 4;
    };
}


/**
 * @brief Maximalni pocet znaku ktery edit InputText akceptuje.
 *
 * Limit pro `buf_size` parametr `ImGui::InputText` - omezuje kolik znaku
 * uzivatel muze do pole napsat. Bez tohoto limitu by `sizeof(edit_buf)`
 * dovolil napsat libovolny garbage string.
 *
 * Hodnoty odpovidaji presne worst-case sirce zobrazene hodnoty bez
 * prefixu (HEX 16-bit "FFFF" = 4, DEC 16-bit "65535" = 5, BIN 16-bit
 * "FFFFFFFF FFFFFFFF" = 17). Tolerantni parser zvlada prefixy `0x` /
 * `0b` / `$` a znaminka, ale uzivatel je nemuze napsat - pokud chce,
 * pouzije Copy/Paste z popup menu (= nepsat rucne).
 *
 * @param format  CPU_FMT_*.
 * @param bits    8 nebo 16.
 * @return        Maximalni pocet uzivatelskych znaku (bez null terminatoru).
 */
static int cpu_panel_max_input_chars(int format, int bits)
{
    return cpu_panel_format_width(format, bits);
}


/* ========================================================================= */
/*  V1: SET_REG helper                                                       */
/* ========================================================================= */


/**
 * @brief Posle SET_REG (16-bit) nebo SET_CPU_FLAGS (8-bit I/R) s novou hodnotou.
 *
 * @param reg_id     Cilovy registr (z80_reg_t nebo CPU_VREG_I/R).
 * @param new_value  Nova hodnota (16-bit) - pro virtual I/R se vezme low byte.
 * @return           true pri uspechu (cmd akceptovan + odpoved OK).
 *
 * Po uspesnem zapisu vyzada okamzity refresh (= UI hned ukaze novou
 * hodnotu + spusti golden fade).
 *
 * V3: pro CPU_VREG_I a CPU_VREG_R posila SET_CPU_FLAGS s update_mask
 * UM_I resp. UM_R. Pro klasicky z80_reg_t (0..DBGAPI_REG_COUNT-1)
 * posila puvodni SET_REG.
 */
static bool cpu_panel_write_reg(int reg_id, uint16_t new_value)
{
    /* V3: virtualni 8-bit registry I a R - jdou pres SET_CPU_FLAGS. */
    if (cpu_reg_is_virtual_8bit(reg_id))
    {
        st_DBGAPI_CPU_FLAGS p;
        memset(&p, 0, sizeof(p));
        if (reg_id == CPU_VREG_I)
        {
            p.update_mask = DBGAPI_CPU_FLAGS_UM_I;
            p.i_reg = (uint8_t)(new_value & 0xFF);
        }
        else
        {
            p.update_mask = DBGAPI_CPU_FLAGS_UM_R;
            p.r_reg = (uint8_t)(new_value & 0xFF);
        };
        bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                             DBGAPI_CMD_SET_CPU_FLAGS,
                                             &p, NULL, 50);
        if (ok) dbg_refresh_request();
        return ok;
    };

    /* V3.3: VEC* zápis = 2-step (SET_CPU_FLAGS UM_I + SET_PIOZ80_INTERRUPT_VECTOR).
     * Nová hodnota:
     *   - hi byte (new_value >> 8) = nový I register
     *   - lo byte (new_value & 0xFE) = nový PIO-Z80 interrupt_vector portu
     *
     * Pokud první krok selže (timeout / arch nepodporuje), 2. krok
     * neposíláme - zachováváme atomicity at API úrovni. */
    if (cpu_reg_is_vec(reg_id))
    {
        int port = cpu_reg_to_pio_port(reg_id);
        if (port < 0) return false;

        /* Krok 1: SET_CPU_FLAGS UM_I (= horní byte VEC). */
        st_DBGAPI_CPU_FLAGS fp;
        memset(&fp, 0, sizeof(fp));
        fp.update_mask = DBGAPI_CPU_FLAGS_UM_I;
        fp.i_reg = (uint8_t)((new_value >> 8) & 0xFF);
        if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_SET_CPU_FLAGS,
                                        &fp, NULL, 50))
        {
            return false;
        };

        /* Krok 2: SET_PIOZ80_INTERRUPT_VECTOR (= dolní byte & 0xFE). */
        st_DBGAPI_PIOZ80_VEC_PARAM vp;
        vp.port_id = (uint8_t)port;
        vp.vector_byte = (uint8_t)(new_value & 0xFE);
        if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_SET_PIOZ80_INTERRUPT_VECTOR,
                                        &vp, NULL, 50))
        {
            /* Krok 1 sa stihl projet, krok 2 selhal. UI dostane refresh
             * a uvidí nekonzistenci - acceptable, alternativně bychom
             * museli umět rollback I registru (= příliš složité pro
             * okrajový případ). */
            dbg_refresh_request();
            return false;
        };

        dbg_refresh_request();
        return true;
    };

    /* V3.3: ISR* zápis přes MEM_WRITE_CHECKED. Vyžaduje region check
     * - při fail zobrazujeme modal warning v render fázi. */
    if (cpu_reg_is_isr(reg_id))
    {
        int port = cpu_reg_to_pio_port(reg_id);
        if (port < 0) return false;

        /* Cílová adresa = VEC* z cache (= odpovídá aktuálnímu I + PIO vec). */
        uint16_t target_addr = (port == 0) ? g_cpu.veca : g_cpu.vecb;

        /* Little-endian 2 bajty na adresu target_addr. */
        uint8_t data[2] = {
            (uint8_t)(new_value & 0xFF),
            (uint8_t)((new_value >> 8) & 0xFF)
        };
        st_DBGAPI_MEM_WRITE_CHECKED_PARAM mp;
        memset(&mp, 0, sizeof(mp));
        mp.addr = target_addr;
        mp.length = 2;
        mp.data = data;
        if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_MEM_WRITE_CHECKED,
                                        &mp, NULL, 50))
        {
            /* Cmd dispatch selhal (= timeout / queue full) - neuspěch
             * bez modal warning. */
            return false;
        };

        if (!mp.success)
        {
            /* Region check selhal - nastav modal warning pro render fázi. */
            g_cpu.pio_isr_modal_addr    = mp.first_failed_addr;
            g_cpu.pio_isr_modal_kind    = mp.first_failed_kind;
            g_cpu.pio_isr_modal_pending = true;
            return false;
        };

        dbg_refresh_request();
        return true;
    };

    if (reg_id < 0 || reg_id >= DBGAPI_REG_COUNT) return false;
    st_DBGAPI_REG_PARAM p = { (uint8_t)reg_id, new_value };
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                         DBGAPI_CMD_SET_REG,
                                         &p, NULL, 50);
    if (ok) dbg_refresh_request();
    return ok;
}

/**
 * @brief Posle SET_CPU_FLAGS s IFF nebo IM zmenou.
 *
 * Wrapper kolem dbgapi sync submit. Po uspechu vyzada refresh.
 *
 * @param um_bit  Jedna z DBGAPI_CPU_FLAGS_UM_IFF1/IFF2/IM.
 * @param value   Nova hodnota (0/1 pro IFF, 0..2 pro IM).
 * @return        true pri uspechu.
 */
static bool cpu_panel_write_flags(uint16_t um_bit, uint8_t value)
{
    st_DBGAPI_CPU_FLAGS p;
    memset(&p, 0, sizeof(p));
    p.update_mask = um_bit;
    if (um_bit == DBGAPI_CPU_FLAGS_UM_IFF1) p.iff1 = value;
    else if (um_bit == DBGAPI_CPU_FLAGS_UM_IFF2) p.iff2 = value;
    else if (um_bit == DBGAPI_CPU_FLAGS_UM_IM) p.im = value;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                         DBGAPI_CMD_SET_CPU_FLAGS,
                                         &p, NULL, 50);
    if (ok) dbg_refresh_request();
    return ok;
}


/* ========================================================================= */
/*  V1: Edit flow                                                            */
/* ========================================================================= */


/**
 * @brief Poradi registru pro Tab navigaci v edit modu.
 *
 * Logicke poradi pro Tab cyklus (V3): obi hlavni paty, alt set, indexovy,
 * SP, PC, R, I. Tabulka je vyhradne pro UI navigaci - nesouvisi s
 * z80_reg_t enum poradim.
 *
 * I a R jsou virtualni 8-bit registry (CPU_VREG_I/R), zapis pres
 * SET_CPU_FLAGS (= ne z80_reg_t IR slot ktery je 16-bit).
 */
static const int k_edit_tab_order[] = {
    Z80_REG_AF, Z80_REG_BC, Z80_REG_DE, Z80_REG_HL,
    Z80_REG_IX, Z80_REG_IY, Z80_REG_SP, Z80_REG_PC,
    Z80_REG_AF2, Z80_REG_BC2, Z80_REG_DE2, Z80_REG_HL2,
    CPU_VREG_R, CPU_VREG_I
};
static const int k_edit_tab_order_count = (int)(sizeof(k_edit_tab_order)
                                                 / sizeof(k_edit_tab_order[0]));


/**
 * @brief Najde index v k_edit_tab_order pro dany reg_id.
 *
 * @return Index nebo -1 pokud reg_id neni v Tab cyklu.
 */
static int cpu_panel_tab_index_of(int reg_id)
{
    for (int i = 0; i < k_edit_tab_order_count; i++)
        if (k_edit_tab_order[i] == reg_id) return i;
    return -1;
}


/**
 * @brief Vrati aktualni hodnotu registru z cache (16-bit) nebo 8-bit
 *        pro virtualni I/R registry z g_cpu.flags.
 *
 * @param reg_id  z80_reg_t nebo CPU_VREG_I/R.
 * @return        Hodnota (high byte = 0 pro 8-bit virtual reg).
 */
static uint16_t cpu_panel_reg_value(int reg_id)
{
    if (reg_id == CPU_VREG_I) return (uint16_t)g_cpu.flags.i_reg;
    if (reg_id == CPU_VREG_R) return (uint16_t)g_cpu.flags.r_reg;
    /* V3.3: PIO-Z80 pseudo-registry (16-bit). */
    if (reg_id == CPU_VREG_VECA) return g_cpu.veca;
    if (reg_id == CPU_VREG_VECB) return g_cpu.vecb;
    if (reg_id == CPU_VREG_ISRA) return g_cpu.isra;
    if (reg_id == CPU_VREG_ISRB) return g_cpu.isrb;
    if (reg_id < 0 || reg_id >= DBGAPI_REG_COUNT) return 0;
    return g_cpu.regs[reg_id];
}


/**
 * @brief Vrati aktualni format zobrazeni pro registr.
 *
 * Pro virtualni 8-bit I/R vraci vzdy HEX (= nepouzivaji format[] pole,
 * to ma jen DBGAPI_REG_COUNT prvku).
 */
static int cpu_panel_reg_format(int reg_id)
{
    if (cpu_reg_is_virtual_8bit(reg_id)) return CPU_FMT_HEX;
    if (cpu_reg_is_virtual_pio16(reg_id)) return CPU_FMT_HEX;
    if (reg_id < 0 || reg_id >= DBGAPI_REG_COUNT) return CPU_FMT_HEX;
    return (int)g_cpu.format[reg_id];
}


/**
 * @brief Vrati cas posledni zmeny pro registr (pro golden fade).
 *
 * Pro virtualni 8-bit I/R fade nedrzime (= zatim).
 */
static double cpu_panel_reg_last_change(int reg_id)
{
    if (cpu_reg_is_virtual_8bit(reg_id)) return 0.0;
    if (reg_id < 0 || reg_id >= DBGAPI_REG_COUNT) return 0.0;
    return g_cpu.last_change_time[reg_id];
}


/**
 * @brief Naplni g_cpu.edit_buf hodnotou registru v aktualnim formatu.
 *
 * Volat pri otevreni edit modu (editing_reg <- reg_id).
 */
static void cpu_panel_fill_edit_buf(int reg_id)
{
    int fmt  = cpu_panel_reg_format(reg_id);
    int bits = cpu_panel_reg_bits(reg_id);
    uint16_t val = cpu_panel_reg_value(reg_id);
    cpu_panel_format_value(val, fmt, bits,
                            g_cpu.edit_buf, sizeof(g_cpu.edit_buf));
}


/**
 * @brief Pozadavek na otevreni in-place editu hodnoty registru.
 *
 * Alt A flow (Michal 2026-05-10 plan): single-click v running mode
 * SOUCASNE pauzne A otevre edit (= zadny "second click required" bug).
 *   - V running: vyvola dbg_autopause_silent() (= request pause BEZ
 *     modalniho okna) a soucasne otevre editor s aktualni cache hodnotou.
 *   - V paused: rovnou otevre editor.
 *
 * Pouzivame TICHOU variantu autopauzy - modalni info okno
 * "Emulation was paused..." by se prekrylo pres InputText a klik OK
 * by zavrel editor jeste nez uzivatel stihne zacit psat (Bug #2).
 *
 * Idempotency: pokud uz editujeme tento samy registr, neresetujeme
 * edit_buf - opakovany klik jen znovu vyzada focus (defensive proti
 * Bug #3 edge case kdy by stary edit_buf zustal po nejake tranzici).
 *
 * Po otevreni je nutne v render fazi InputText volat SetKeyboardFocusHere()
 * (= edit_focus_pending = true).
 *
 * @param reg_id  Cilovy registr (z80_reg_t).
 */
static void cpu_panel_request_edit(int reg_id)
{
    /* Akceptujeme bud z80_reg_t (0..DBGAPI_REG_COUNT-1) nebo virtualni
     * 8-bit reg (CPU_VREG_I, CPU_VREG_R) nebo virtualni 16-bit PIO reg
     * (CPU_VREG_VECA, VECB, ISRA, ISRB). Bez teto vetve byly VEC a ISR
     * needitovatelne (Michal 2026-05-11). */
    if (!cpu_reg_is_virtual_8bit(reg_id)
        && !cpu_reg_is_virtual_pio16(reg_id)
        && (reg_id < 0 || reg_id >= DBGAPI_REG_COUNT))
    {
        return;
    };

    /* Idempotency guard: opakovany klik na uz editovany registr je no-op
     * (krome obnoveni focus pendingu pro pripad ze ho neco rozbilo). */
    if (g_cpu.editing_reg == reg_id) {
        g_cpu.edit_focus_pending = true;
        return;
    };

    /* Pokud emulator bezi, zazadame TICHOU pauzu (= bez modalniho info
     * okna). Modalni okno by prekrylo InputText a nasledny klik OK by
     * editor zavrel. Request je idempotentni - UI v dalsim framu uvidi
     * paused state. */
    (void)dbg_autopause_silent();

    g_cpu.editing_reg        = reg_id;
    g_cpu.edit_focus_pending = true;
    g_cpu.edit_advance_next  = false;
    cpu_panel_fill_edit_buf(reg_id);
}


/**
 * @brief Zrusi probihajici edit (Esc nebo klik mimo).
 */
static void cpu_panel_cancel_edit(void)
{
    g_cpu.editing_reg        = -1;
    g_cpu.edit_focus_pending = false;
    g_cpu.edit_advance_next  = false;
    g_cpu.edit_buf[0]        = '\0';
}


/**
 * @brief Provede submit aktualne editovane hodnoty.
 *
 * Parsuje edit_buf dle aktualniho formatu editovaneho registru. Pri
 * uspesnem parse posle SET_REG. Pokud parse selze, zustane v edit modu
 * (uzivatel muze opravit). Pri uspechu (+ advance_next true) prepne na
 * dalsi registr v k_edit_tab_order.
 *
 * @return true pokud bylo submit uspesne (parse + SET_REG OK).
 */
static bool cpu_panel_commit_edit(void)
{
    int reg_id = g_cpu.editing_reg;
    bool is_vreg8 = cpu_reg_is_virtual_8bit(reg_id);
    bool is_vreg_pio = cpu_reg_is_virtual_pio16(reg_id);
    bool is_vreg = is_vreg8 || is_vreg_pio;
    if (!is_vreg && (reg_id < 0 || reg_id >= DBGAPI_REG_COUNT)) {
        cpu_panel_cancel_edit();
        return false;
    };

    int fmt  = cpu_panel_reg_format(reg_id);
    int bits = cpu_panel_reg_bits(reg_id);
    uint16_t parsed = 0;
    if (!cpu_panel_parse_value(g_cpu.edit_buf, fmt, bits, &parsed)) {
        /* Parse fail: zustan v edit modu, znovu fokus na InputText. */
        g_cpu.edit_focus_pending = true;
        return false;
    };

    /* Zapis hodnoty pres SET_REG (normalni 16-bit) nebo SET_CPU_FLAGS
     * (virtualni 8-bit I/R). cpu_panel_write_reg rozhoduje sam podle
     * reg_id. */
    if (!cpu_panel_write_reg(reg_id, parsed)) {
        /* fail (= timeout / emu neresponsive): zustan v edit. */
        g_cpu.edit_focus_pending = true;
        return false;
    };

    /* Optimisticky uloz do cache - nez prijde refresh, panel zobrazi
     * novou hodnotu. dbg_refresh_request() v cpu_panel_write_reg zaridi
     * read-back z dbgapi pri pristim ticku. */
    if (is_vreg8)
    {
        if (reg_id == CPU_VREG_I)
            g_cpu.flags.i_reg = (uint8_t)(parsed & 0xFF);
        else
            g_cpu.flags.r_reg = (uint8_t)(parsed & 0xFF);
    }
    else if (is_vreg_pio)
    {
        /* V3.3: optimistický update VEC a ISR cache. VEC update musí
         * promítnout i do flags.i_reg (= horní byte) a pio_int_vec_a/b
         * (= dolní byte & 0xFE), aby bylo všechno konzistentní. ISR
         * update jenom isra/b - paměť se přečte při příštím refreshi.
         *
         */
        switch (reg_id)
        {
            case CPU_VREG_VECA:
                g_cpu.veca = parsed;
                g_cpu.flags.i_reg = (uint8_t)((parsed >> 8) & 0xFF);
                g_cpu.pio_int_vec_a = (uint8_t)(parsed & 0xFE);
                /* I se změnil = vecb se taky implicitně mění (sdílí horní
                 * byte) - přepočítáme. */
                g_cpu.vecb = (uint16_t)(((uint16_t)g_cpu.flags.i_reg << 8)
                                         | (uint8_t)(g_cpu.pio_int_vec_b & 0xFE));
                break;
            case CPU_VREG_VECB:
                g_cpu.vecb = parsed;
                g_cpu.flags.i_reg = (uint8_t)((parsed >> 8) & 0xFF);
                g_cpu.pio_int_vec_b = (uint8_t)(parsed & 0xFE);
                g_cpu.veca = (uint16_t)(((uint16_t)g_cpu.flags.i_reg << 8)
                                         | (uint8_t)(g_cpu.pio_int_vec_a & 0xFE));
                break;
            case CPU_VREG_ISRA:
                g_cpu.isra = parsed;
                break;
            case CPU_VREG_ISRB:
                g_cpu.isrb = parsed;
                break;
        };
    }
    else
    {
        g_cpu.regs[reg_id] = parsed;
    };

    if (g_cpu.edit_advance_next) {
        /* Skok na dalsi editovatelny registr v Tab cyklu. */
        int idx = cpu_panel_tab_index_of(reg_id);
        if (idx >= 0) {
            int next_idx = (idx + 1) % k_edit_tab_order_count;
            int next_reg = k_edit_tab_order[next_idx];
            g_cpu.editing_reg        = next_reg;
            g_cpu.edit_focus_pending = true;
            g_cpu.edit_advance_next  = false;
            cpu_panel_fill_edit_buf(next_reg);
            return true;
        };
    };

    cpu_panel_cancel_edit();
    return true;
}


/* ========================================================================= */
/*  V1: Register name mapping                                                */
/* ========================================================================= */


/**
 * @brief Vrati symbolicke jmeno registru pro UI a externi vazby
 *        (Watch promenna, jmeno BPT).
 *
 * Apostrofy ve jmenech AF', BC' atd. nelze pouzit jako BP_VAR ident
 * (= regex `[a-zA-Z_][a-zA-Z0-9_]*`). Pro vars proto pouzivame suffix
 * `_alt` (viz cpu_panel_reg_var_name).
 *
 * @param reg_id  Z80_REG_*.
 * @return        Konstantni retezec ("AF", "AF'", "BC'", ..., "IR").
 */
static const char *cpu_panel_reg_label(int reg_id)
{
    switch (reg_id) {
        case Z80_REG_AF:  return "AF";
        case Z80_REG_BC:  return "BC";
        case Z80_REG_DE:  return "DE";
        case Z80_REG_HL:  return "HL";
        case Z80_REG_AF2: return "AF'";
        case Z80_REG_BC2: return "BC'";
        case Z80_REG_DE2: return "DE'";
        case Z80_REG_HL2: return "HL'";
        case Z80_REG_IX:  return "IX";
        case Z80_REG_IY:  return "IY";
        case Z80_REG_SP:  return "SP";
        case Z80_REG_PC:  return "PC";
        case Z80_REG_WZ:  return "WZ";
        case Z80_REG_IR:  return "IR";
        case CPU_VREG_I:  return "I";
        case CPU_VREG_R:  return "R";
        case CPU_VREG_VECA: return "VECA";
        case CPU_VREG_VECB: return "VECB";
        case CPU_VREG_ISRA: return "ISRA";
        case CPU_VREG_ISRB: return "ISRB";
        default:          return "?";
    };
}


/**
 * @brief Vrati jmeno bezpecne pouzitelne jako $vars identifikator.
 *
 * BP_VARS pravidla (bp_var_name_is_valid): regex `[a-zA-Z_][a-zA-Z0-9_]*`,
 * max BP_VAR_NAME_MAX znaku. Mapovani:
 *   - Z80_REG_AF2 -> "AF_alt" (= bez apostrofu)
 *   - ostatni     -> stejne jako cpu_panel_reg_label.
 *
 * @param reg_id  Z80_REG_*.
 * @return        Konstantni retezec.
 */
static const char *cpu_panel_reg_var_name(int reg_id)
{
    switch (reg_id) {
        case Z80_REG_AF2: return "AF_alt";
        case Z80_REG_BC2: return "BC_alt";
        case Z80_REG_DE2: return "DE_alt";
        case Z80_REG_HL2: return "HL_alt";
        default:          return cpu_panel_reg_label(reg_id);
    };
}


/* ========================================================================= */
/*  Focus button >                                                           */
/* ========================================================================= */


/**
 * @brief Vykresli tlacitko ">" (focus button) pro 16-bit registr.
 *
 * Pattern z Bookmarks (bm_window.cpp):
 *   - Levy klik = focus primarni Disassembly (dbg_disasm_show_in_slot(0, ..)).
 *     NEpauzuje - navigacni akce zustava live.
 *   - Pravy klik = otevre popup menu (Focus to DA #1-#5, Copy hex/dec/bin).
 *     Take navigacni / kopiruje do clipboard - NEpauzuje.
 *
 * Stable ID pres "###cpu_focus_<suffix>" - dynamic label by jinak resetoval
 * ID pri zmene hodnoty.
 *
 * @param id_suffix  Stable ID suffix (napr. "af", "pc").
 * @param reg_id     Z80_REG_* hodnota - pouziva se pro lookup hodnoty v
 *                   g_cpu.regs a nastaveni cile popup menu (rmb_popup_reg_id).
 * @param valid      Pokud false, tlacitko je disabled.
 */
static void cpu_panel_draw_focus_button(const char *id_suffix,
                                         int reg_id, bool valid)
{
    char label[24];
    snprintf(label, sizeof(label), ">###cpu_focus_%s", id_suffix);

    ImGui::BeginDisabled(!valid);
    if (ImGui::SmallButton(label)) {
        /* Levy klik = focus primary disasm na hodnotu registru. NEpauzuje.
         * Pro 8-bit virtual reg (I/R) bereme low byte jako adresu - i to
         * ma smysl pro rychlou navigaci. */
        uint16_t target = cpu_panel_reg_value(reg_id);
        dbg_disasm_show_in_slot(0, target);
    };
    /* RMB - nastav cil popup, vlastni OpenPopup se vola mimo aktualni scope
     * v dbg_cpu_panel_render (po vsech buttonech). */
    if (valid && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_cpu.rmb_popup_reg_id = reg_id;
        g_cpu.rmb_popup_open = true;
    };
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered() && valid) {
        ImGui::SetTooltip(
            _("Left-click: focus primary Disassembly at $%04X.\n"
              "Right-click: more focus targets and copy."),
            cpu_panel_reg_value(reg_id));
    };
}


/**
 * @brief Vykresli RMB popup pro focus button (V0 obsah).
 *
 * Polozky:
 *   - "Focus in Disassembly #1..#5" - rozdeleno na main + extra
 *   - separator
 *   - "Copy hex" / "Copy dec" / "Copy bin" - kopirovani do clipboardu
 *
 * Cilovy registr je g_cpu.rmb_popup_reg_id, hodnota se vezme z
 * g_cpu.regs[reg_id]. ImGui clipboard pres SetClipboardText.
 *
 * Platni cilovi reg_id: skutecne 16-bit Z80 registry (0..DBGAPI_REG_COUNT-1),
 * I/R 8-bit pseudo-registry (CPU_VREG_I/R - jen Copy, hodnota neni adresa)
 * a VEC/ISR 16-bit PIO-Z80 pseudo-registry (CPU_VREG_VECA..ISRB - plne menu
 * vc. Focus/Watch/BPT, hodnota je adresa).
 */
static void cpu_panel_render_rmb_popup(void)
{
    if (!ImGui::BeginPopup("cpu_focus_popup")) return;

    int reg_id = g_cpu.rmb_popup_reg_id;
    /* is_vreg8 = I/R (8-bit pseudo-registry, jen Copy - hodnota neni adresa).
     * is_pio16 = VEC/ISR (16-bit PIO-Z80 pseudo-registry, plne menu jako
     * skutecne 16-bit registry - hodnota je adresa v Z80 adresnim prostoru).
     * Bez is_pio16 vetve padaly VEC/ISR (reg_id 200..203 >= DBGAPI_REG_COUNT)
     * do guardu a popup se zaviral prazdny - RMB menu se nezobrazilo. */
    bool is_vreg8 = cpu_reg_is_virtual_8bit(reg_id);
    bool is_pio16 = cpu_reg_is_virtual_pio16(reg_id);
    if (!is_vreg8 && !is_pio16 && (reg_id < 0 || reg_id >= DBGAPI_REG_COUNT)) {
        ImGui::EndPopup();
        return;
    };

    uint16_t value = cpu_panel_reg_value(reg_id);

    /* V3.2 (Michal 2026-05-11): pro 8-bit virtual registry (I, R) nedava
     * Focus / Watch / Add BPT smysl - hodnota I a R neni adresa v Z80
     * adresnim prostoru (R je refresh counter, I je high byte IM2 vectoru,
     * ne primy pointer). Tj. tyto polozky se skryvaji a popup pro I/R
     * obsahuje pouze Copy hex/dec/bin. Pro 16-bit registry (vc. VEC/ISR
     * pseudo-registru, ktere drzi adresu) je layout beze zmeny. */
    if (!is_vreg8)
    {
        /* Focus to Disassembly #1..#5 */
        for (int slot = 0; slot < 5; slot++) {
            char label[64];
            if (slot == 0) {
                snprintf(label, sizeof(label),
                          "%s", _("Focus in Disassembly (main)"));
            } else {
                snprintf(label, sizeof(label),
                          "%s #%d", _("Focus in Disassembly"), slot + 1);
            };
            if (ImGui::MenuItem(label)) {
                dbg_disasm_show_in_slot(slot, value);
            };
        };

        ImGui::Separator();

        /* Add to Watch (V1): vytvori novou $name promennou ve sdilenem
         * bp_vars storage s aktualni snapshot hodnotou registru. Watch okno
         * je sdilene s $vars (smart BP) - var se objevi automaticky.
         *
         * POZN: Hodnota je STATIC snapshot - bp_vars netracuje zive CPU
         * registry. Pro zive sledovani uzivatel pouzije expressionu typu
         * `$AF == 0x1234` v BP condition, ne $AF jako live read.
         * Pro V1 staci snapshot - re-add prepise hodnotu. */
        {
            const char *vname = cpu_panel_reg_var_name(reg_id);
            char label[80];
            snprintf(label, sizeof(label), "%s $%s = $%04X",
                      _("Add to Watch"), vname, value);
            if (ImGui::MenuItem(label)) {
                bp_var_set(vname, (int32_t)value);
                dbg_refresh_request();
            };
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                    _("Creates/updates a $vars entry as a static snapshot.\n"
                      "Open Vars window to view and edit."));
            };
        };

        /* Add BPT at <reg>: vytvori execution breakpoint na adrese rovnu
         * hodnote 16-bit registru. Pouziva dbg_ui_bp_add helper (= prochazi
         * pres dbgapi CMD_BP_ADD, race-safe vs EMU vlakno).
         *
         * Pokud na adrese uz BP existuje, dbg_ui_bp_add vrati false (= no-op
         * z pohledu UI; ridici disasm si pak na dane adrese ukaze existing
         * BPT). */
        {
            const char *rlabel = cpu_panel_reg_label(reg_id);
            char label[80];
            snprintf(label, sizeof(label), "%s %s ($%04X)",
                      _("Add breakpoint at"), rlabel, value);
            if (ImGui::MenuItem(label)) {
                int new_id = -1;
                if (dbg_ui_bp_add(value, &new_id)) {
                    dbg_refresh_request();
                };
            };
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                    _("Creates an execution breakpoint at the address held\n"
                      "in the register. No-op if an existing BP occupies it."));
            };
        };

        ImGui::Separator();
    };

    /* Copy hex / dec / bin */
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%04X", value);
        char menu_label[48];
        snprintf(menu_label, sizeof(menu_label), "%s (%s)",
                  _("Copy hex"), buf);
        if (ImGui::MenuItem(menu_label)) {
            ImGui::SetClipboardText(buf);
        };
    };
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%u", (unsigned)value);
        char menu_label[48];
        snprintf(menu_label, sizeof(menu_label), "%s (%s)",
                  _("Copy dec"), buf);
        if (ImGui::MenuItem(menu_label)) {
            ImGui::SetClipboardText(buf);
        };
    };
    {
        char buf[24];
        /* 16-bit binary, MSB first, oddeleny do dvou 8-bitu pro
         * citelnost. */
        for (int i = 0; i < 8; i++)
            buf[i] = ((value >> (15 - i)) & 1) ? '1' : '0';
        buf[8] = ' ';
        for (int i = 0; i < 8; i++)
            buf[9 + i] = ((value >> (7 - i)) & 1) ? '1' : '0';
        buf[17] = '\0';
        char menu_label[48];
        snprintf(menu_label, sizeof(menu_label), "%s", _("Copy bin"));
        if (ImGui::MenuItem(menu_label)) {
            ImGui::SetClipboardText(buf);
        };
    };

    ImGui::EndPopup();
}


/* ========================================================================= */
/*  Render helpers                                                           */
/* ========================================================================= */


/**
 * @brief Vykresli pouze hodnotovou bunku registru (view nebo edit mode).
 *
 * Pomocna funkce pro tabulkovy layout (V3). Renderuje obsah jedne bunky -
 * bud Selectable s hex hodnotou (view mode), nebo InputText (edit mode).
 *
 * Edit flow je identicky s puvodni cpu_panel_draw_reg16_row, ale bez
 * focus buttonu / labelu / dvojtecky - ty se vykresluji v separatnich
 * tabulkovych bunkach.
 *
 * @param id_suffix  Stable ID suffix (napr. "af", "i").
 * @param reg_id     Z80_REG_* nebo CPU_VREG_I/R.
 * @param valid      Pokud false, zobrazi placeholder.
 */
static void cpu_panel_draw_reg_value_cell(const char *id_suffix,
                                           int reg_id, bool valid)
{
    if (!valid) {
        ImGui::TextDisabled("%s", "----");
        return;
    };

    int fmt  = cpu_panel_reg_format(reg_id);
    int bits = cpu_panel_reg_bits(reg_id);
    uint16_t value = cpu_panel_reg_value(reg_id);

    /* Sirka view-mode bunky: v3 vzdy HEX, tj. 4 znaky pro 16-bit, 2 znaky
     * pro 8-bit. Pro padding pri editaci pouzivame max sirku 4 znaky. */
    int width_chars = cpu_panel_format_width(fmt, bits);
    char wbuf[8];
    for (int i = 0; i < width_chars && i < 7; i++) wbuf[i] = 'F';
    wbuf[width_chars < 7 ? width_chars : 7] = '\0';
    ImVec2 sz = ImGui::CalcTextSize(wbuf);

    /* === Edit mode === */
    if (g_cpu.editing_reg == reg_id)
    {
        char input_id[32];
        snprintf(input_id, sizeof(input_id), "###cpu_edit_%s", id_suffix);

        if (g_cpu.edit_focus_pending) {
            ImGui::SetKeyboardFocusHere();
            g_cpu.edit_focus_pending = false;
        };

        /* Sirka edit pole pro 16-bit ("FFFF") staci i pro 8-bit ("FF"). */
        char ewbuf[8] = "FFFF";
        ImVec2 esz = ImGui::CalcTextSize(ewbuf);
        ImGui::SetNextItemWidth(esz.x + ImGui::GetStyle().FramePadding.x * 2.0f);

        ImGuiInputTextFlags flags =
            ImGuiInputTextFlags_EnterReturnsTrue
            | ImGuiInputTextFlags_AutoSelectAll
            | ImGuiInputTextFlags_CallbackCharFilter;

        /* V3: format je vzdy HEX (jednoduchy char filter). */
        auto char_filter = [](ImGuiInputTextCallbackData *data) -> int {
            ImWchar ch = data->EventChar;
            if (ch == ' ' || ch == '_') return 0;
            if ((ch >= '0' && ch <= '9') ||
                (ch >= 'a' && ch <= 'f') ||
                (ch >= 'A' && ch <= 'F') ||
                ch == 'x' || ch == 'X' || ch == '$') return 0;
            return 1;
        };

        int max_chars = cpu_panel_max_input_chars(fmt, bits);
        size_t buf_size = (size_t)(max_chars + 1);
        if (buf_size > sizeof(g_cpu.edit_buf)) buf_size = sizeof(g_cpu.edit_buf);

        bool submit = ImGui::InputText(input_id, g_cpu.edit_buf,
                                        buf_size,
                                        flags, char_filter, NULL);
        bool tab_pressed = ImGui::IsKeyPressed(ImGuiKey_Tab, false);

        if (submit) {
            g_cpu.edit_advance_next = tab_pressed;
            (void)cpu_panel_commit_edit();
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            cpu_panel_cancel_edit();
        }
        else if (ImGui::IsItemDeactivated() && !submit) {
            cpu_panel_cancel_edit();
        };
        return;
    };

    /* === View mode === */
    char view_buf[20];
    cpu_panel_format_value(value, fmt, bits, view_buf, sizeof(view_buf));
    char sel_label[40];
    snprintf(sel_label, sizeof(sel_label), "%s###cpu_reg_%s",
              view_buf, id_suffix);

    /* Golden fade jen pro normalni 16-bit registry (virtual I/R zatim
     * fade netracuji). */
    ImU32 col = cpu_reg_is_virtual_8bit(reg_id)
                 ? ImGui::GetColorU32(ImGuiCol_Text)
                 : cpu_panel_get_value_color(reg_id);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    /* size.x = 0 => Selectable vyplni celou sirku tabulkove bunky
     * (= sloupec font_size * 5 z V3.3). Bez toho byla klikatelna oblast
     * jen sirka textu "FFFF" a uzivatel videl prazdnou mezeru za hodnotou
     * (Michal 2026-05-11). */
    bool clicked = ImGui::Selectable(sel_label, false,
                                       ImGuiSelectableFlags_AllowDoubleClick,
                                       ImVec2(0, 0));
    ImGui::PopStyleColor();
    if (clicked)
    {
        cpu_panel_request_edit(reg_id);
    };

    if (ImGui::IsItemHovered())
    {
        if (EMULATOR_TEST_PAUSED)
        {
            ImGui::SetTooltip("%s", _("Click to edit. Right-click for options."));
        }
        else
        {
            ImGui::SetTooltip("%s", _("Click to pause and edit"));
        };
    };
}


/**
 * @brief Vykresli 4 nasledujici bunky tabulky pro jeden registr.
 *
 * Bunky: focus button ">", label "AF", ":", hodnota. Vola se uvnitr
 * BeginTable - kazda bunka oddelena ImGui::TableNextColumn().
 *
 * Pro prazdnou polovinu radku (= tabulka 8 sloupcu, jen leva polovina
 * naplnena) staci nevolat tuto funkci a misto toho zavolat
 * TableNextColumn 4x bez obsahu.
 *
 * @param id_suffix  Stable ID suffix (musi byt unique v ramci panelu).
 * @param reg_id     Z80_REG_* nebo CPU_VREG_I/R.
 * @param valid      Pokud false, hodnotova bunka je placeholder + focus disabled.
 */
static void cpu_panel_draw_reg_row_cells(const char *id_suffix,
                                          int reg_id, bool valid)
{
    /* Bunka 1: focus button ">" */
    ImGui::TableNextColumn();
    cpu_panel_draw_focus_button(id_suffix, reg_id, valid);

    /* Bunka 2: label registru */
    ImGui::TableNextColumn();
    ImGui::Text("%s", cpu_panel_reg_label(reg_id));

    /* Bunka 3: dvojtecka */
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(":");

    /* Bunka 4: hodnota (Selectable / InputText) */
    ImGui::TableNextColumn();
    cpu_panel_draw_reg_value_cell(id_suffix, reg_id, valid);
}


/**
 * @brief Vyplni 4 prazdne bunky tabulky (pro mezeru v levem/pravem sloupci).
 *
 * Vola se pro polovinu radku (levou nebo pravou), ktera neobsahuje
 * registr - typicky kdyz pravy sloupec je v posledni radce prazdny.
 * Tabulka ma 9 sloupcu celkem: 4 left + 1 separator + 4 right.
 * Separator sloupec je 20 px sirky (V3.1 - Michalova zpetna vazba
 * 2026-05-10), takze nevyzaduje obsah - prazdny radek mu nevadi.
 */
static void cpu_panel_draw_empty_row_cells(void)
{
    for (int i = 0; i < 4; i++) ImGui::TableNextColumn();
}


/**
 * @brief Vykresli IFF1/IFF2 toggle button do bunky tabulky.
 *
 * SmallButton se stable ID "###cpu_iff<n>_btn". Label "ON" / "OFF" dle
 * hodnoty. Klik = silent autopauza + toggle bit pres SET_CPU_FLAGS.
 *
 * @param iff_idx  1 nebo 2 (IFF1 nebo IFF2).
 */
static void cpu_panel_draw_iff_cell(int iff_idx)
{
    if (!g_cpu.flags_valid)
    {
        ImGui::TextDisabled("%s", "--");
        return;
    };

    bool on = (iff_idx == 1) ? (g_cpu.flags.iff1 != 0)
                              : (g_cpu.flags.iff2 != 0);
    char label[32];
    snprintf(label, sizeof(label), "%s###cpu_iff%d_btn",
              on ? "ON" : "OFF", iff_idx);

    /* Tlacitko ma fixnich znaku sirku ("ON" vs "OFF" by jinak mirne
     * resizovalo. Pevna sirka = "OFF" + frame padding. */
    char wbuf[] = "OFF";
    ImVec2 sz = ImGui::CalcTextSize(wbuf);
    float btn_w = sz.x + ImGui::GetStyle().FramePadding.x * 2.0f;
    /* SmallButton ma vlastni padding - rozpracovat na regularni Button
     * by bylo lepsi pro pevnou sirku, ale SmallButton drzi sjednoceny
     * vizual s ostatnimi panel buttony. */
    (void)btn_w;

    if (ImGui::SmallButton(label))
    {
        (void)dbg_autopause_silent();
        uint8_t new_val = on ? 0 : 1;
        uint16_t um = (iff_idx == 1) ? DBGAPI_CPU_FLAGS_UM_IFF1
                                      : DBGAPI_CPU_FLAGS_UM_IFF2;
        if (cpu_panel_write_flags(um, new_val))
        {
            /* Optimisticky uloz do cache. */
            if (iff_idx == 1) g_cpu.flags.iff1 = new_val;
            else              g_cpu.flags.iff2 = new_val;
        };
    };
    if (ImGui::IsItemHovered())
    {
        if (EMULATOR_TEST_PAUSED)
            ImGui::SetTooltip("%s", _("Click to toggle"));
        else
            ImGui::SetTooltip("%s", _("Click to pause and toggle"));
    };
}


/**
 * @brief Vykresli IM combo (jen ciselne polozky 0 / 1 / 2) do bunky tabulky.
 *
 * V3.1 (Michal 2026-05-10): label "IM" + ":" je vykresleny okolnim radkem
 * tabulky (R_name + R_colon), tj. combo zde obsahuje jen ciselnou hodnotu
 * bez "IM " prefixu - konzistentni s ostatnimi value cells.
 *
 * Po zmene posila SET_CPU_FLAGS s update_mask UM_IM. Silent autopauza
 * se vyvola na zmene polozky (combo nepauzuje na otevreni dropdownu, jen
 * na committed change).
 */
static void cpu_panel_draw_im_cell(void)
{
    if (!g_cpu.flags_valid)
    {
        ImGui::TextDisabled("%s", "-");
        return;
    };

    static const char *items[3] = { "0", "1", "2" };
    int cur = (int)g_cpu.flags.im;
    if (cur < 0 || cur > 2) cur = 0;

    /* SetNextItemWidth na "0" + frame padding + arrow padding */
    char wbuf[] = "0 ";
    ImVec2 sz = ImGui::CalcTextSize(wbuf);
    float w = sz.x + ImGui::GetStyle().FramePadding.x * 2.0f
            + ImGui::GetFrameHeight();  /* arrow button */
    ImGui::SetNextItemWidth(w);

    if (ImGui::Combo("###cpu_im_combo", &cur, items, 3))
    {
        if (cur >= 0 && cur <= 2)
        {
            (void)dbg_autopause_silent();
            if (cpu_panel_write_flags(DBGAPI_CPU_FLAGS_UM_IM, (uint8_t)cur))
            {
                /* Optimistic cache. */
                g_cpu.flags.im = (uint8_t)cur;
            };
        };
    };
    if (ImGui::IsItemHovered())
    {
        if (EMULATOR_TEST_PAUSED)
            ImGui::SetTooltip("%s", _("Select interrupt mode"));
        else
            ImGui::SetTooltip("%s", _("Click to pause and change IM"));
    };
}


/**
 * @brief Vykresli kompletni tabulku registru (V3 layout, V3.1 separator).
 *
 * Tabulka 9 sloupcu: 4 levy slot (focus, name, ":", value) + 1 separator
 * sloupec (20 px fixni sirka) + 4 pravy slot. Pres 9 radku. Posledni 2
 * radky obsahuji IFF1/IFF2 buttony (leva polovina) a IM combo se svym
 * vlastnim labelem "IM:" (prava polovina).
 *
 * V3.1 (Michal 2026-05-10): separator sloupec ma garantovanou minimalni
 * sirku 20 px - jinak ho SizingFixedFit zhroutil na ~0 px a leve/prave
 * registry vypadaly slepene.
 *
 *  Row 1: >  AF  :  hex  | |  >  AF' :  hex
 *  Row 2: >  HL  :  hex  | |  >  HL' :  hex
 *  Row 3: >  DE  :  hex  | |  >  DE' :  hex
 *  Row 4: >  BC  :  hex  | |  >  BC' :  hex
 *  Row 5: >  PC  :  hex  | |  >  IX  :  hex
 *  Row 6: >  SP  :  hex  | |  >  IY  :  hex
 *  Row 7: >  R   :  hex  | |  >  I   :  hex
 *  Row 8:    IFF1:  ON   | |     IM  : [0/1/2]
 *  Row 9:    IFF2:  ON   | |
 *
 * (V druhe svisle care "| |" je 20 px separator sloupec.)
 */
static void cpu_panel_draw_reg_table(void)
{
    /* SizingFixedFit + NoBordersInBody = sloupce se prizpusobi obsahu
     * a nemaji separator cary uvnitr (= cisty look podobny SameLine).
     * NoPadOuterX zmensi vlevo/vpravo margin. */
    ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit
                          | ImGuiTableFlags_NoBordersInBody
                          | ImGuiTableFlags_NoPadOuterX;

    /* V3.1: 9 sloupcu (= 4 + 1 sep + 4). Sep sloupec je explicitne
     * WidthFixed 20 px - SizingFixedFit by ho jinak zmensil na 0. */
    if (!ImGui::BeginTable("cpu_regs_table", 9, flags)) return;

    /* Sloupce 0..2 a 5..7 (focus btn, name, colon) automaticky dimenzovane
     * podle obsahu. Hodnotove sloupce L_val/R_val maji EXPLICITNI pevnou
     * sirku - bez toho by se sloupec menil dle obsahu bunky (16-bit hex
     * "FFFF" vs 8-bit "FF" vs IFF button "ON"/"OFF" vs IM Combo), takze
     * tabulka pri prepnuti IFF ON->OFF poskakovala (Michal 2026-05-11).
     *
     * Sirka = 5 znaku v aktualnim fontu (= 4 hex znaky + 1 padding znak),
     * Michal 2026-05-11. CalcTextSize "FFFFF" je presna sirka 5 monospace
     * glyfu - drive jsem omylem pouzil GetFontSize() ktere vraci height
     * fontu (= ~2x sirsi nez char width v monospace), takze sloupec byl
     * cca 2.5x sirsi nez hodnota. */
    float val_w = ImGui::CalcTextSize("FFFFF").x;

    ImGui::TableSetupColumn("##L_focus", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##L_name",  ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##L_colon", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##L_val",   ImGuiTableColumnFlags_WidthFixed, val_w);
    ImGui::TableSetupColumn("##sep",     ImGuiTableColumnFlags_WidthFixed, 20.0f);
    ImGui::TableSetupColumn("##R_focus", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##R_name",  ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##R_colon", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##R_val",   ImGuiTableColumnFlags_WidthFixed, val_w);

    bool valid = g_cpu.regs_valid;

    /* Helper lambda pro pridani 20 px separator bunky mezi levou
     * a pravou polovinu radku. cpu_panel_draw_reg_row_cells vola
     * 4x TableNextColumn (L_focus..L_val); nasleduje sep bunka,
     * pak 4x bunka prave poloviny. */

    /* Row 1: AF | AF' */
    ImGui::TableNextRow();
    cpu_panel_draw_reg_row_cells("af",  Z80_REG_AF,  valid);
    ImGui::TableNextColumn();                       /* sep */
    cpu_panel_draw_reg_row_cells("af2", Z80_REG_AF2, valid);

    /* Row 2: HL | HL' */
    ImGui::TableNextRow();
    cpu_panel_draw_reg_row_cells("hl",  Z80_REG_HL,  valid);
    ImGui::TableNextColumn();
    cpu_panel_draw_reg_row_cells("hl2", Z80_REG_HL2, valid);

    /* Row 3: DE | DE' */
    ImGui::TableNextRow();
    cpu_panel_draw_reg_row_cells("de",  Z80_REG_DE,  valid);
    ImGui::TableNextColumn();
    cpu_panel_draw_reg_row_cells("de2", Z80_REG_DE2, valid);

    /* Row 4: BC | BC' */
    ImGui::TableNextRow();
    cpu_panel_draw_reg_row_cells("bc",  Z80_REG_BC,  valid);
    ImGui::TableNextColumn();
    cpu_panel_draw_reg_row_cells("bc2", Z80_REG_BC2, valid);

    /* Row 5: PC | IX */
    ImGui::TableNextRow();
    cpu_panel_draw_reg_row_cells("pc",  Z80_REG_PC,  valid);
    ImGui::TableNextColumn();
    cpu_panel_draw_reg_row_cells("ix",  Z80_REG_IX,  valid);

    /* Row 6: SP | IY */
    ImGui::TableNextRow();
    cpu_panel_draw_reg_row_cells("sp",  Z80_REG_SP,  valid);
    ImGui::TableNextColumn();
    cpu_panel_draw_reg_row_cells("iy",  Z80_REG_IY,  valid);

    /* Row 7: R | I (8-bit virtualni regs). flags_valid musi byt true
     * (= GET_CPU_FLAGS plni i_reg/r_reg). */
    ImGui::TableNextRow();
    cpu_panel_draw_reg_row_cells("r",  CPU_VREG_R, g_cpu.flags_valid);
    ImGui::TableNextColumn();
    cpu_panel_draw_reg_row_cells("i",  CPU_VREG_I, g_cpu.flags_valid);

    /* Row 8: leva polovina IFF1, prava polovina "IM:" + combo.
     * V3.1: prava strana ma vlastni label "IM" v R_name a ":" v R_colon
     * (= konzistentni s ostatnimi radky); v R_val je samotne combo. */
    ImGui::TableNextRow();
    /* L1: empty focus */
    ImGui::TableNextColumn();
    /* L2: label "IFF1" */
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("IFF1");
    /* L3: ":" */
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(":");
    /* L4: button ON/OFF */
    ImGui::TableNextColumn();
    cpu_panel_draw_iff_cell(1);
    /* sep */
    ImGui::TableNextColumn();
    /* R1: empty focus */
    ImGui::TableNextColumn();
    /* R2: "IM" label */
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("IM");
    /* R3: ":" */
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(":");
    /* R4: IM combo (jen ciselne hodnoty 0/1/2 - V3.1) */
    ImGui::TableNextColumn();
    cpu_panel_draw_im_cell();

    /* Row 9: leva polovina IFF2, prava polovina prazdna */
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("IFF2");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(":");
    ImGui::TableNextColumn();
    cpu_panel_draw_iff_cell(2);
    /* sep + prava polovina prazdna */
    ImGui::TableNextColumn();
    cpu_panel_draw_empty_row_cells();

    /* V3.3: Row 10 VECA / ISRA a Row 11 VECB / ISRB - jen pro arch s
     * PIO-Z80 (MZ-800, MZ-1500). MZ-700 has_pioz80 = 0, řádky se neukáží. */
    if (g_cpu.has_pioz80 && g_cpu.pio_valid)
    {
        bool pio_valid = g_cpu.pio_valid;

        /* Row 10: VECA | ISRA */
        ImGui::TableNextRow();
        cpu_panel_draw_reg_row_cells("veca", CPU_VREG_VECA, pio_valid);
        ImGui::TableNextColumn(); /* sep */
        cpu_panel_draw_reg_row_cells("isra", CPU_VREG_ISRA, pio_valid);

        /* Row 11: VECB | ISRB */
        ImGui::TableNextRow();
        cpu_panel_draw_reg_row_cells("vecb", CPU_VREG_VECB, pio_valid);
        ImGui::TableNextColumn();
        cpu_panel_draw_reg_row_cells("isrb", CPU_VREG_ISRB, pio_valid);
    };

    ImGui::EndTable();
}


/* ========================================================================= */
/*  Flag rozpis F                                                            */
/* ========================================================================= */


/**
 * @brief Bitove pozice a labely pro flag rozpis F (Z80 SZ5H3PNC).
 *
 * Poradi v poli odpovida vizualnimu poradi zleva doprava (= bit 7 -> 0,
 * tj. S Z 5 H 3 P/V N C). Symbol "P" zahrnuje P i V (= kontextove
 * parity/overflow). Symboly "5" a "3" jsou nedokumentovane kopie bitu
 * vysledku - viditelne ale ne klikatelne (jen vizualne).
 */
static const struct {
    int  bit_index;
    char symbol;
    const char *tooltip_key;
} k_flag_bits[8] = {
    { 7, 'S', N_("S = Sign flag (bit 7 of last result)") },
    { 6, 'Z', N_("Z = Zero flag (result == 0)") },
    { 5, '5', N_("Undocumented copy of bit 5 of result") },
    { 4, 'H', N_("H = Half carry flag (carry from bit 3 to 4, used by DAA)") },
    { 3, '3', N_("Undocumented copy of bit 3 of result") },
    { 2, 'P', N_("P/V = Parity (logic ops) / Overflow (arithmetic)") },
    { 1, 'N', N_("N = Negative flag (last op was subtract, used by DAA)") },
    { 0, 'C', N_("C = Carry flag") },
};


/**
 * @brief Odeslat SET_REG AF s novou hodnotou (toggle bitu flagu).
 *
 * Sestavi parametr struct na stacku a posle synchronni CMD. Po uspechu
 * vyzada okamzity refresh aby UI hned ukazalo zmenu.
 *
 * @param new_af  Nova hodnota AF (low byte = F po toggle).
 */
static void cpu_panel_write_af(uint16_t new_af)
{
    st_DBGAPI_REG_PARAM p = { (uint8_t)Z80_REG_AF, new_af };
    if (dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                  DBGAPI_CMD_SET_REG,
                                  &p, NULL, 50))
    {
        dbg_refresh_request();
    };
}


/**
 * @brief Vykresli radku flag rozpisu F (8 bunek S/Z/5/H/3/P/N/C).
 *
 * Layout: "F: [S][Z][5][H][3][P][N][C]" s hodnotou bitu pod symbolem
 * (0/1). Klik na bunku v paused mode toggle prislusny bit ve F registru
 * (= zapis AF pres SET_REG). V running mode klik trigger auto-pauzu.
 *
 * Hover ukazuje tooltip s vysvetlenim flagu.
 */
static void cpu_panel_draw_flag_row(void)
{
    ImGui::Text("F: ");
    ImGui::SameLine();

    if (!g_cpu.regs_valid) {
        ImGui::TextDisabled("%s", "--------");
        return;
    };

    uint8_t f = (uint8_t)(g_cpu.regs[Z80_REG_AF] & 0xFF);

    /* Bunky bitu - SmallButton se stable ID "###cpu_flag_<bit>". Toggle
     * v paused, autopauza v running. Velikost bunky je urcena 1 znakem +
     * FramePadding (= konstantni napric stavem bitu). */
    for (int i = 0; i < 8; i++)
    {
        if (i > 0) ImGui::SameLine(0.0f, 2.0f);

        int bit = k_flag_bits[i].bit_index;
        bool is_set = (f & (1u << bit)) != 0;
        char label[16];
        /* Label: pismeno bitu vetsi pri is_set, mensi pri clear (vizualni
         * rozlisitelnost). Stable ID per bit, ne per stav. */
        snprintf(label, sizeof(label), "%c###cpu_flag_%d",
                  is_set ? k_flag_bits[i].symbol
                         : (char)(k_flag_bits[i].symbol | 0x20),
                  bit);

        if (is_set) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                   IM_COL32(220, 220, 80, 255));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                   IM_COL32(120, 120, 120, 255));
        };
        bool clicked = ImGui::SmallButton(label);
        ImGui::PopStyleColor();

        if (clicked)
        {
            /* Klik na flag bit = edit-attempt (zmeni F registr).
             * Tichá autopauza (BEZ info modalu, stejne jako edit value)
             * + okamzity toggle bitu = jeden klik = pauza + zmena.
             * Konzistentni s Alt A flow value editu. */
            (void)dbg_autopause_silent();
            uint16_t af = g_cpu.regs[Z80_REG_AF];
            af ^= (uint16_t)(1u << bit);
            cpu_panel_write_af(af);
        };

        if (ImGui::IsItemHovered() && k_flag_bits[i].tooltip_key) {
            ImGui::SetTooltip("%s", _(k_flag_bits[i].tooltip_key));
        };
    };
}


/* V3.1: IM2 ISR sekce odstranena (Michalova zpetna vazba 2026-05-10).
 * V V2 byla sekce klikatelna pro IM2 vector decode na MZ-800/MZ-1500.
 * Render funkce cpu_panel_draw_im2_section a cpu_panel_render_im2_rmb_popup
 * odstraneny. Datovy handler DBGAPI_CMD_GET_IM2_VECTOR + struct
 * st_DBGAPI_IM2_VECTOR zustavaji v dbgapi pro potencialni budouci pouziti
 * (= ABI stable, batch ji neptava). */


/* ========================================================================= */
/*  V2: Cycles & raster sekce                                                */
/* ========================================================================= */


/**
 * @brief Posle DBGAPI_CMD_SET_USER_CYCLE_ORIGIN s aktualni hodnotou.
 *
 * @param new_origin  Nova absolutni hodnota total_cycles uzivatelskeho
 *                    pocatku (= co se ulozi do g_debugger.user_cycle_origin).
 *                    UI display je nasledne (total_cycles - new_origin).
 * @return  true pri uspechu, false pri timeoutu/queue full.
 */
static bool cpu_panel_set_user_cycle_origin(uint32_t new_origin)
{
    uint32_t v = new_origin;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                         DBGAPI_CMD_SET_USER_CYCLE_ORIGIN,
                                         &v, NULL, 50);
    if (ok) {
        /* Optimisticky aktualizuj cache, nez priste refresh donese
         * authoritative hodnotu z batche. */
        g_cpu.user_cycle_origin = new_origin;
        dbg_refresh_request();
    };
    return ok;
}


/**
 * @brief Vykresli Cycles & raster sekci v collapsible headeru.
 *
 * Default collapsed. Read-only s vyjimkou User cyc value (editovatelna)
 * a Reset tlacitka.
 *
 * Layout (V3.1):
 *   Total cyc: 12345678
 *   Frame cyc: 56789                (V3.1 - vraceno)
 *   Frame: 1234  Line: 156  Col: 234
 *   User cyc:  4321  [Reset]        (V3.1 - novy)
 *
 * Frame cyc = total_cycles - total_cycles_at_frame_start (snapshot UI-side
 * v okamziku prechodu na novy frame, viz cpu_panel_refresh).
 *
 * User cyc = total_cycles - user_cycle_origin (origin v g_debugger storage,
 * synchronizovano z batche). Reset tlacitko nastavi origin na aktualni
 * total_cycles -> display 0. Edit hodnoty: parse decimal, vypocti
 * new_origin = total_cycles - parsed, posli SET_USER_CYCLE_ORIGIN.
 *
 * Stable IDs:
 *   "###cpu_raster_header"      - collapsible header
 *   "###cpu_user_cyc_edit"      - InputText edit User cyc value
 *   "###cpu_user_cyc_reset"     - tlacitko Reset
 */
static void cpu_panel_draw_raster_section(void)
{
    /* Default collapsed - sekce 6.5 planu. */
    bool open = ImGui::CollapsingHeader("Cycles & raster###cpu_raster_header");
    /* V2.1 perf: zapsat open stav pro pristi refresh tick (Fix B).
     * Sekce je default collapsed - pri prvnim renderu open=false, pristi
     * refresh nepujde do raster batch sloty -> usetri praci na emu strane. */
    g_cpu.section_raster_open = open;
    if (!open)
    {
        /* Zustane-li sekce collapsed, prerus pripadny rozpracovany User cyc
         * edit aby ImGui state nezustal "ghost focused" na neviditelnem
         * widgetu. */
        g_cpu.editing_user_cyc = false;
        return;
    };

    if (!g_cpu.flags_valid) {
        ImGui::TextDisabled("%s", _("(refresh pending)"));
        return;
    };

    uint32_t total = g_cpu.flags.total_cycles;

    /* === Radek 1: Frame / Line / Col === (V3.2: presunuto nahoru nad
     * tabulku citacu pro lepsi prehlednost - raster pozice je casto
     * sledovana zvlast od cycle counteru.) */
    if (g_cpu.raster_valid) {
        const st_DBGAPI_RASTER_POS *r = &g_cpu.raster;
        ImGui::Text("Frame: %u  Line: %u  Col: %u",
                     (unsigned)r->frame_number,
                     (unsigned)r->scanline,
                     (unsigned)r->column_pixel);
    } else {
        ImGui::TextDisabled("Frame: -  Line: -  Col: -");
    };

    ImGui::Spacing();

    /* === Tabulka cycle counteru === (V3.2: 4 sloupce - label, ':', hodnota,
     * akcni tlacitko - aby Frame/Total/User cyc byly vertikalne zarovnane
     * a Set0 button pro User cyc mel vlastni sloupec. Poradi dle Michala
     * 2026-05-11: Frame cyc, Total cyc, User cyc.) */
    if (ImGui::BeginTable("cpu_cyc_tbl", 4,
                            ImGuiTableFlags_SizingFixedFit
                          | ImGuiTableFlags_NoBordersInBody))
    {
        /* --- Row: Frame cyc --- */
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Frame cyc");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(":");
        ImGui::TableNextColumn();
        if (g_cpu.total_cycles_snapshot_valid) {
            uint32_t frame_cyc = total - g_cpu.total_cycles_at_frame_start;
            ImGui::Text("%u", (unsigned)frame_cyc);
        } else {
            ImGui::TextDisabled("-");
        };
        ImGui::TableNextColumn(); /* 4. sloupec u Frame cyc prazdny */

        /* --- Row: Total cyc --- */
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Total cyc");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(":");
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)total);
        ImGui::TableNextColumn(); /* 4. sloupec u Total cyc prazdny */

        /* --- Row: User cyc + Set0 --- */
        uint32_t user_cyc = total - g_cpu.user_cycle_origin;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("User cyc");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(":");
        ImGui::TableNextColumn();

        if (g_cpu.editing_user_cyc)
        {
            /* InputText mode - parse pri Enter, cancel pri Escape. */
            if (g_cpu.user_cyc_edit_focus_pending) {
                ImGui::SetKeyboardFocusHere();
                g_cpu.user_cyc_edit_focus_pending = false;
            };

            /* Fixni sirka pro 10 znaku (max uint32_t decimal = 4294967295). */
            char wbuf[] = "0000000000";
            ImVec2 sz = ImGui::CalcTextSize(wbuf);
            float w = sz.x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(w);

            ImGuiInputTextFlags itflags = ImGuiInputTextFlags_EnterReturnsTrue
                                        | ImGuiInputTextFlags_AutoSelectAll
                                        | ImGuiInputTextFlags_CharsDecimal;
            bool committed = ImGui::InputText("###cpu_user_cyc_edit",
                                               g_cpu.user_cyc_edit_buf,
                                               sizeof(g_cpu.user_cyc_edit_buf),
                                               itflags);

            bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
            if (!committed && !cancel && ImGui::IsItemDeactivated()) {
                cancel = true;
            };

            if (committed)
            {
                char *endp = NULL;
                unsigned long parsed = strtoul(g_cpu.user_cyc_edit_buf, &endp, 10);
                if (endp != g_cpu.user_cyc_edit_buf) {
                    uint32_t new_display = (uint32_t)parsed;
                    uint32_t new_origin = total - new_display;
                    (void)cpu_panel_set_user_cycle_origin(new_origin);
                };
                g_cpu.editing_user_cyc = false;
            }
            else if (cancel)
            {
                g_cpu.editing_user_cyc = false;
            };
        }
        else
        {
            /* Display mode - klikatelne Selectable. */
            char buf[24];
            snprintf(buf, sizeof(buf), "%u###cpu_user_cyc_label",
                      (unsigned)user_cyc);
            if (ImGui::Selectable(buf, false, ImGuiSelectableFlags_AllowOverlap,
                                   ImVec2(0, 0)))
            {
                snprintf(g_cpu.user_cyc_edit_buf,
                          sizeof(g_cpu.user_cyc_edit_buf),
                          "%u", (unsigned)user_cyc);
                g_cpu.editing_user_cyc = true;
                g_cpu.user_cyc_edit_focus_pending = true;
            };
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", _("Click to edit value"));
            };
        };

        /* Set0 tlacitko ve vlastnim 4. sloupci (V3.3: oddelene od hodnoty
         * pro vizualni zarovnani Reset = "nastav na 0", presnejsi semantika
         * nez "Reset" - viz Michal 2026-05-11). */
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(_L("Set0###cpu_user_cyc_set0")))
        {
            (void)cpu_panel_set_user_cycle_origin(total);
            g_cpu.editing_user_cyc = false;
        };
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                _("Set User cyc to zero (= origin to current total)"));
        };

        ImGui::EndTable();
    };
}


/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */


void dbg_cpu_panel_render(void)
{
    /* Defensivni check (Bug #3): edit ma smysl jen v paused mode.
     * Pokud emulator prejde do running stavu (Play / step / breakpoint
     * resume) zatimco mame otevreny edit, vynutime cancel. Bez tohoto
     * checku zustane editing_reg nastaveny, InputText se ale prestane
     * korektne renderovat (frame-by-frame data se meni) a ImGui se muze
     * dostat do nekonzistentniho focus stavu - aplikace pak vypada jako
     * zamrzla pri dalsi interakci. */
    if (g_cpu.editing_reg != -1 && !EMULATOR_TEST_PAUSED)
    {
        cpu_panel_cancel_edit();
    };

    /* Refresh dat pokud kontroler rekl ze je cas. */
    if (g_dbg_ui.refresh.should_refresh)
    {
        cpu_panel_refresh();
    };

    /* Cely panel ve zmensenem fontu (sjednoceno s ostatnimi debug sekcemi). */
    ImGui::PushFont(NULL, ImGui::GetFontSize() + DBG_CONTENT_FONT_SIZE_OFFSET);

    /* V3: SeparatorText "CPU" odstranen (Michal 2026-05-10 - okno se
     * jmenuje "CPU Registers", separator byl redundantni). */

    /* === Flag rozpis F === (V3.2: prelozeno nahoru nad tabulku registru
     * pro lepsi viditelnost flag status - Michal 2026-05-11) */
    cpu_panel_draw_flag_row();

    ImGui::Spacing();

    /* === V3 Tabulkovy layout registru ===
     *
     * Tabulka 9 sloupcu (8 dat + 1 separator 20 px), 9 radku (vc.
     * IFF1/IFF2/IM cells). Nahrazuje stary SameLine + col2_offset pattern.
     * Stable IDs cells per registr (###cpu_focus_<reg>, ###cpu_reg_<reg>,
     * ###cpu_edit_<reg>). */
    cpu_panel_draw_reg_table();

    /* V3: Special sekce odstranena (Michal 2026-05-10) - I/R/IFF1/IFF2/IM
     * jsou nove primo v tabulce registru (commit "tabulkovy layout"). HALT
     * + INT/NMI pending boli odstraneny zcela. */

    /* V3.1 (Michal 2026-05-10): IM2 ISR sekce odstranena z UI. Render
     * funkce cpu_panel_draw_im2_section + cpu_panel_render_im2_rmb_popup
     * smazany, im2 cache flagy a section_im2_open zustavaji ve strukture
     * pro pripadny budouci re-introduction (ne plytvani pameti, levne).
     * WANT_IM2 bit ve which-mask se nikdy nenastavuje -> batch handler
     * IM2 cast preskoci. */

    /* === V2: Cycles & raster sekce === (default collapsed) */
    ImGui::Spacing();
    cpu_panel_draw_raster_section();

    /* V3: Last instruction sekce odstranena z UI (Michal 2026-05-10). */

    /* RMB popup - OpenPopup musi byt MIMO scope buttons aby BeginPopup
     * nasel shodne ID. Flag se nastavi v cpu_panel_draw_focus_button. */
    if (g_cpu.rmb_popup_open) {
        ImGui::OpenPopup("cpu_focus_popup");
        g_cpu.rmb_popup_open = false;
    };
    cpu_panel_render_rmb_popup();

    /* V3.1: IM2 ISR target popup odstranen (cela IM2 sekce zrusena). */

    /* V3.3: modal warning po neúspěšném ISR write (region read-only).
     * Otevírá se z cpu_panel_write_reg, když MEM_WRITE_CHECKED vrátil
     * success=0 (= cílová adresa VEC* je v ROM, CG-ROM, VRAM v 800
     * native módu, prohibited, unmapped nebo mapped ports). */
    if (g_cpu.pio_isr_modal_pending)
    {
        ImGui::OpenPopup("cpu_isr_write_fail");
        g_cpu.pio_isr_modal_pending = false;
        g_cpu.pio_isr_modal_open    = true;
    };
    if (g_cpu.pio_isr_modal_open)
    {
        /* Modal s pevnou rozumnou šířkou - bez SetNextWindowSize ImGui
         * v kombinaci s AlwaysAutoResize spočítá content size velmi úzce
         * a TextWrapped pak wrapuje text na každé slovo. Pevná šířka
         * cca 30 znaků aktualního fontu zajistí čitelný blok. */
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                 ImGuiCond_Appearing,
                                 ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(
            ImVec2(ImGui::GetFontSize() * 22.0f, 0.0f),
            ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("cpu_isr_write_fail", NULL,
                                    ImGuiWindowFlags_NoResize))
        {
            /* Mapování en_MEMMAP_REGION_KIND na anglický label. */
            const char *kind_label;
            switch ((en_MEMMAP_REGION_KIND)g_cpu.pio_isr_modal_kind)
            {
                case MEMMAP_KIND_ROM_LOW:      kind_label = "ROM (low)"; break;
                case MEMMAP_KIND_ROM_HIGH:     kind_label = "ROM (high)"; break;
                case MEMMAP_KIND_CGROM:        kind_label = "CG-ROM"; break;
                case MEMMAP_KIND_VRAM_I:       kind_label = "VRAM I (MZ-800 native)"; break;
                case MEMMAP_KIND_VRAM_II:      kind_label = "VRAM II (MZ-800 native)"; break;
                case MEMMAP_KIND_MAPPED_PORTS: kind_label = "mapped I/O ports"; break;
                case MEMMAP_KIND_PROHIBITED:   kind_label = "prohibited mode"; break;
                case MEMMAP_KIND_UNMAPPED:     kind_label = "unmapped"; break;
                default:                       kind_label = "read-only region"; break;
            };

            ImGui::TextWrapped(
                _("Cannot write ISR target: address $%04X points to "
                  "read-only memory (%s)."),
                (unsigned)g_cpu.pio_isr_modal_addr,
                kind_label);
            ImGui::Spacing();
            ImGui::TextWrapped("%s",
                _("Change the bank mapping or edit VEC so it points to RAM."));
            ImGui::Spacing();

            if (ImGui::Button(_L("OK"), ImVec2(120, 0)))
            {
                g_cpu.pio_isr_modal_open = false;
                ImGui::CloseCurrentPopup();
            };
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }
        else
        {
            /* Pokud BeginPopupModal vrátí false ale máme open flag,
             * popup byl zavřen jinak - resetuj. */
            g_cpu.pio_isr_modal_open = false;
        };
    };

    ImGui::PopFont();
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
