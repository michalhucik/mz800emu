/**
 * @file dasm_window.cpp
 * @brief Disassembler V1 - implementace samostatného range-based okna.
 *
 * F0 stav: okno + iconbar toggle (placeholder text).
 * F1 stav: range From/To input fields (hex), Disassemble button,
 * smyčka přes z80_dasm() v zadaném rozsahu, ImGui table se 4 sloupci
 * (Addr | Bytes | Label | Mnemonic), status bar dole.
 * F2 stav: auto-label scanner přes dasm_export modul,
 * sloupec Label naplněn S/L/D/W s barvením podle typu, mnemonic
 * substituován jmény labelů (call $C020 → call Sc020) přes lokální
 * z80_symtab_t. Status bar diferencuje instrukce/labely/warningy.
 * F3 stav: use_symdb checkbox (default OFF), per-call gating
 * pro symdb_bridge_get_symtab(), use_cdl placeholder s (F8), info
 * text (N syms in range), Browse... button pro sym_window.
 * F4+F5+F6 stav: Save dialog, 3 dialekty (pasmo / sjasmplus / sdcc-asz80).
 * F7 polish: clipboard copy, IGFD file picker pro Save, granulární
 * error hlášky, status bar enrichment, persistence do mz<arch>emu.ini,
 * context menu nad řádkem, klávesové zkratky Ctrl+S / F5 / Esc.
 *
 * Listing se NEAKTUALIZUJE live; uživatel musí kliknout
 * Disassemble po každé změně rozsahu nebo checkboxů.
 *
 * Paměť se čte přes @c debugger_memory_read_byte() (= banking-aware,
 * stejné chování jako hlavní disasm view). Žádné dbgapi CMDRQ v
 * V1 - z80_dasm engine je čistě UI-thread bezpečný.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "dasm_window.h"

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/debugger/symbols/symdb_bridge.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "libs/dasm-z80/z80_dasm.h"
#include "emulator/debugger/debugger.h"
#include "emulator/debugger/dasm_export.h"
#include "emulator/debugger/mhmap.h"
#include "emulator/debugger/symbols/sym_db.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "libs/cpu-z80/z80.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "baseui/baseui.h"
}


namespace {

/**
 * @brief Jeden řádek listingu cached ve state struktuře.
 *
 * Vyplněn ve @c dasm_window_run() po kliknutí Disassemble.
 * Drží pre-formátovaný mnemonic string, takže render funkce jen
 * vypisuje (žádný re-format per frame).
 *
 * F7: pole @c target_addr drží výsledek @c z80_dasm_target_addr() v
 * době disassembly (= cíl skoku/volání pokud je staticky znám), nebo
 * @c 0xFFFFu jako sentinel. Použito v context menu pro "Focus to target".
 */
struct DasmRow {
    uint16_t addr;          /**< Adresa první instrukce v paměti. */
    uint16_t target_addr;   /**< Statický target nebo 0xFFFF (= žádný). */
    uint8_t  bytes[4];      /**< Surové bajty instrukce (max 4 pro Z80). */
    uint8_t  num_bytes;     /**< Délka instrukce v bajtech (1-4). */
    char     mnemonic[64];  /**< Pre-formátovaný text mnemoniky + operandů
                                 (po substituci jmen labelů). */
};

/**
 * @brief Persistent state Disassembler V1 okna.
 *
 * File-static instance - V1 má jedinou instanci okna. V2+ by
 * mohlo přidat secondary windows (jako Memory Browser #2-#5).
 *
 * @c range_from / @c range_to drží naposledy zadaný rozsah,
 * @c rows je výsledek posledního @c dasm_window_run(), @c labels
 * je výsledek auto-label scannera + případně sym_db merge.
 *
 * @c dirty NEAUTOMATICKY re-spouští disasm - uživatel musí
 * kliknout Disassemble button (V1 design: explicit refresh,
 * žádný live update).
 *
 * @c use_symdb / @c use_cdl jsou per-disassembly gating flagy
 * (default OFF) - rozbor sekce 4.5: implicit "vždy konzultovat
 * sym_db" je zakázané (false hits při snapshot z jiného běhu).
 *
 * F7 poznámka k typům: pole napojená na cfgmain (@c persist_*) jsou
 * unsigned/int dle požadavku CFG API; "živé" UI state pole zachovávají
 * původní typy (uint16_t, bool, dasm_dialect_t). Sync v
 * @ref dasm_window_apply_persisted resp. @ref dasm_window_capture_persist.
 */
struct DasmWindowState {
    uint16_t range_from = 0x0000;  /**< Počáteční adresa rozsahu (včetně). */
    uint16_t range_to   = 0x00FF;  /**< Koncová adresa rozsahu (včetně). */
    bool dirty = true;             /**< true = range/options změněn, čeká re-disasm. */
    std::vector<DasmRow> rows;     /**< Výsledek posledního disassemble. */

    /* F2: auto-label storage + lookup */
    std::vector<dasm_label_t> labels;            /**< Detekované labely. */
    std::unordered_map<uint16_t, int> label_at_addr; /**< addr → idx do labels. */

    /* F3: per-call sym_db a CDL gating */
    bool use_symdb = false;            /**< OFF default - sym_db lookup ON. */
    bool use_cdl   = false;            /**< F8 placeholder, zatím no-op. */
    int  symdb_hits_in_range = 0;      /**< Info-text v topbaru. */
    unsigned symdb_hits_cached_version = (unsigned)~0u; /**< Cache invalidator. */

    /* F4: Save dialog state - persistuje mezi otevřeními okna. */
    dasm_dialect_t save_dialect = DASM_DIALECT_SJASMPLUS;  /**< V1 F5 default
                                                                = sjasmplus
                                                                (preferovaný
                                                                dialekt pro
                                                                MZ-800 scene). */
    bool save_include_org    = true;   /**< Emit ORG direktiva. */
    bool save_include_bytes  = true;   /**< Emit komentář s opcode bytes. */
    bool save_uppercase      = false;  /**< false = ld a,b / true = LD A,B. */
    char save_path[512]      = "disasm.asm";  /**< Cesta k cílovému souboru. */
    bool save_request_open   = false;  /**< Tick = otevřít popup příští frame
                                            (OpenPopup musí být ve stejném
                                            ImGui ID scope jako BeginPopupModal). */
    bool save_browse_open    = false;  /**< IGFD file picker je otevřený. */

    /* F7: Persistence mirroring (mz<arch>emu.ini sekce [DASM_WINDOW]).
     * cfgmain bind potřebuje stabilní pointer na proměnnou; ImGui state
     * (uint16_t / dasm_dialect_t enum) jsou různé typy, tak držíme
     * unsigned/int kopii pro CFG. Sync přes @ref capture_persist /
     * @ref apply_persisted. */
    unsigned persist_range_from   = 0u;
    unsigned persist_range_to     = 0xFFu;
    unsigned persist_use_symdb    = 0u;
    unsigned persist_use_cdl      = 0u;
    unsigned persist_dialect      = (unsigned)DASM_DIALECT_SJASMPLUS;
    unsigned persist_include_org  = 1u;
    unsigned persist_include_bytes = 1u;
    unsigned persist_uppercase    = 0u;
    char    *persist_save_path    = NULL;  /**< Pointer pro cfgelement_bind (TEXT). */
};

static DasmWindowState s_state;


/**
 * @brief Z80 dasm callback pro čtení bajtu z paměti.
 *
 * Bridge mezi z80_dasm engine signaturou
 * (@c z80_dasm_read_fn) a mz800new banking-aware paměťovým
 * přístupem. Stejná cesta jako hlavní inline disasm view -
 * respektuje aktuální banking pohled CPU.
 *
 * @param addr      Adresa bajtu (0x0000-0xFFFF).
 * @param user_data Ignored (V1 nepotřebuje per-call kontext).
 * @return Byte na dané adrese z CPU view.
 */
static uint8_t dasm_read_byte_cb(uint16_t addr, void *user_data)
{
    (void)user_data;
    return debugger_memory_read_byte(addr);
}


/**
 * @brief Vrátí lidsky čitelné jméno dialektu pro status bar.
 *
 * @param d Dialect.
 * @return Statický řetězec ("pasmo" / "sjasmplus" / "sdcc-asz80" / "?").
 */
static const char *dialect_name_str(dasm_dialect_t d)
{
    switch (d) {
        case DASM_DIALECT_PASMO:      return "pasmo";
        case DASM_DIALECT_SJASMPLUS:  return "sjasmplus";
        case DASM_DIALECT_SDCC_ASZ80: return "sdcc-asz80";
        default:                       return "?";
    }
}


/**
 * @brief F8: Mapuje @ref en_DEBUGGER_MHMAP_MODE na krátký label pro UI.
 *
 * @param m Mode.
 * @return Statický anglický string ("OFF" / "with-window" / "always").
 */
static const char *mhmap_mode_name(en_DEBUGGER_MHMAP_MODE m)
{
    switch (m) {
        case DEBUGGER_MHMAP_MODE_OFF:         return "OFF";
        case DEBUGGER_MHMAP_MODE_WITH_WINDOW: return "with-window";
        case DEBUGGER_MHMAP_MODE_ALWAYS:      return "always";
        default:                              return "?";
    }
}


/**
 * @brief Vrátí barvu pro label podle typu (auto-label) nebo zelenou
 *        pro sym_db match.
 *
 * Color scheme:
 *   - S (subroutine)  → cyan
 *   - L (jump)        → yellow
 *   - D (data)        → orange/tan
 *   - W (warning)     → red
 *   - sym_db          → green (overrides type color)
 *
 * @param lbl Label.
 * @return ImVec4 barva pro TextColored.
 */
static ImVec4 label_color_for(const dasm_label_t &lbl)
{
    if (lbl.from_symdb) {
        /* sym_db wins on color (green) - vyjma WARN, kde si WARN
         * zachovává červenou jako signál chyby. */
        if (lbl.type == DASM_LABEL_WARN) {
            return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        }
        return ImVec4(0.55f, 1.0f, 0.55f, 1.0f);
    }
    switch (lbl.type) {
        case DASM_LABEL_SUBROUTINE: return ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
        case DASM_LABEL_JUMP:       return ImVec4(1.0f,  0.95f, 0.55f, 1.0f);
        case DASM_LABEL_DATA:       return ImVec4(1.0f,  0.80f, 0.55f, 1.0f);
        case DASM_LABEL_WARN:       return ImVec4(1.0f,  0.45f, 0.45f, 1.0f);
        default:                    return ImVec4(0.7f,  0.7f,  0.7f,  1.0f);
    }
}


/**
 * @brief Přepočítá počet sym_db záznamů v aktuálním rozsahu.
 *
 * Volá se při změně range from/to nebo pokud sym_db_get_version()
 * posunulo proti cached value. Lineární scan přes sym_db_count() -
 * V1 typicky < 1000 záznamů, výkon zanedbatelný.
 */
static void recount_symdb_hits_in_range(void)
{
    int count = 0;
    size_t n = sym_db_count();
    for (size_t i = 0; i < n; i++) {
        const st_SYMBOL *s = sym_db_get_by_index(i);
        if (!s) continue;
        /* Bank-aware: jen CPU view (bank_id==0) - stejný filtr jako
         * symdb_bridge. */
        if (s->bank_id != 0) continue;
        uint16_t a = (uint16_t)(s->addr & 0xFFFFu);
        if (a >= s_state.range_from && a <= s_state.range_to) {
            count++;
        }
    }
    s_state.symdb_hits_in_range = count;
    s_state.symdb_hits_cached_version = sym_db_get_version();
}


/**
 * @brief Spustí disassemble přes aktuální rozsah a naplní s_state.rows.
 *
 * Sekvence:
 *   1) Zavolá dasm_export_collect_labels() s aktuálním gating
 *      (symdb pokud use_symdb, jinak NULL; cdl vždy NULL pro V1).
 *   2) Postaví dočasný z80_symtab_t naplněný auto-labely (+ sym_db
 *      override jmen pokud jsou ve výsledku).
 *   3) Iteruje rozsah, pro každou instrukci volá z80_dasm() +
 *      z80_dasm_to_str_sym() s tím symtab → mnemonic má substituovaná
 *      jména (call $C020 → call Sc020). Spočte target_addr per row
 *      (= F7 context menu "Focus to target").
 *
 * Output je formátován defaultním z80_dasm_format_default(),
 * tj. styl ekvivalentní z80ex_dasm: hex_style = HASH (#FF),
 * uppercase = 1. Per-dialect rendering (pasmo $/sjasmplus/sdcc)
 * přijde ve F6.
 *
 * @post @c s_state.rows obsahuje 0 až N řádků.
 * @post @c s_state.labels obsahuje 0 až max_labels záznamů.
 * @post @c s_state.label_at_addr je naplněn lookup mapou.
 * @post @c s_state.dirty == false.
 */
static void dasm_window_run(void)
{
    s_state.rows.clear();
    s_state.labels.clear();
    s_state.label_at_addr.clear();

    if (s_state.range_to < s_state.range_from)
    {
        s_state.dirty = false;
        return;
    }

    /* --- Krok 1: collect labels ------------------------------------ */
    /* Per-call gating: NULL pokud checkbox OFF. */
    const z80_symtab_t *symdb_param = NULL;
    if (s_state.use_symdb) {
        symdb_param = symdb_bridge_get_symtab();
    }
    /* F8: CDL/mhmap snapshot - aktivní jen když use_cdl=ON. View je
     * vlastněný heap zdroj, na konci funkce uvolnit. */
    mhmap_view_t *cdl_param = NULL;
    if (s_state.use_cdl) {
        cdl_param = mhmap_view_create_for_range(s_state.range_from,
                                                s_state.range_to);
    }

    /* Pesimistický limit - 2048 typicky stačí i pro velké rozsahy
     * (ROM monitor má pár stovek labelů). Pokud by se to ukázalo malé,
     * F4+ může zvětšit nebo udělat resize loop. */
    s_state.labels.resize(2048);
    int n_lbl = dasm_export_collect_labels(
        s_state.range_from, s_state.range_to,
        dasm_read_byte_cb, NULL,
        symdb_param, cdl_param,
        s_state.labels.data(), (int)s_state.labels.size());
    if (n_lbl < 0) n_lbl = 0;
    s_state.labels.resize((size_t)n_lbl);

    /* Lookup mapa pro O(1) přístup v tabulce. */
    for (int i = 0; i < n_lbl; i++) {
        s_state.label_at_addr[s_state.labels[i].addr] = i;
    }

    /* --- Krok 2: postav lokální symtab z labelů pro substituce ------ */
    z80_symtab_t *local_symtab = z80_symtab_create();
    if (local_symtab != NULL) {
        for (const auto &lbl : s_state.labels) {
            z80_symtab_add(local_symtab, lbl.addr, lbl.name);
        }
    }

    /* --- Krok 3: build rows s substituovaným mnemonic --------------- */
    z80_dasm_format_t fmt;
    z80_dasm_format_default(&fmt);
    /* Default fmt = HASH hex style, uppercase mnemoniky.
     * F6 doplní per-dialect override. */

    uint32_t addr = s_state.range_from;
    const uint32_t end = s_state.range_to;
    const size_t MAX_ROWS = 70000;

    while (addr <= end && s_state.rows.size() < MAX_ROWS)
    {
        z80_dasm_inst_t inst;
        int len = z80_dasm(&inst, dasm_read_byte_cb, NULL, (uint16_t)addr);
        if (len <= 0) break;

        DasmRow r;
        r.addr = (uint16_t)addr;
        r.target_addr = z80_dasm_target_addr(&inst);  /* F7 pro ctx menu */
        r.num_bytes = (uint8_t)((len > 4) ? 4 : len);
        for (int i = 0; i < r.num_bytes; i++)
        {
            r.bytes[i] = dasm_read_byte_cb((uint16_t)(addr + i), NULL);
        }
        /* Mnemonic se symbolovou substitucí - pokud local_symtab je
         * NULL (alloc fail), degraduje na bez-symbol fmt. */
        z80_dasm_to_str_sym(r.mnemonic, sizeof r.mnemonic, &inst, &fmt,
                            local_symtab);
        s_state.rows.push_back(r);

        if (addr + len > 0xFFFF) break;
        addr += (uint32_t)len;
    }

    z80_symtab_destroy(local_symtab);

    /* F8: uvolnit CDL snapshot pokud byl alokován. */
    if (cdl_param != NULL) {
        mhmap_view_destroy(cdl_param);
    }

    s_state.dirty = false;
}


/**
 * @brief Sestaví aktuální export options ze stavu okna.
 *
 * @return Naplněná @c dasm_export_opts_t struktura připravená pro
 *         @c dasm_export_write / @c dasm_export_to_string.
 */
static dasm_export_opts_t build_export_opts(void)
{
    dasm_export_opts_t opts;
    opts.dialect                = s_state.save_dialect;
    opts.include_org            = s_state.save_include_org;
    opts.include_bytes_comment  = s_state.save_include_bytes;
    opts.generate_equ_for_syms  = false;  /* V1 ignored */
    opts.uppercase_mnemonics    = s_state.save_uppercase;
    return opts;
}


/**
 * @brief Mapuje @c dasm_export_result_t na user-friendly hlášku přes
 *        @c baseui_show_message.
 *
 * Pro OK vypíše info ("Disassembly saved to %s" / "copied to clipboard");
 * pro chybové kódy vypíše error variantu (is_error=true). Texty jsou
 * záměrně v angličtině (project i18n konvence - zdroj v @c _() makru
 * by se použil jen v UI vrstvě; @c baseui_show_message bere raw string
 * a vypisuje do message okna).
 *
 * @param rc      Návratová hodnota @c dasm_export_write nebo @c dasm_export_to_string.
 * @param context Volitelný kontext (cesta souboru nebo "clipboard"); může být NULL.
 */
static void map_export_result_to_message(int rc, const char *context)
{
    switch (rc) {
        case DASM_EXPORT_OK:
            if (context) {
                baseui_show_message(false, (char *)"Disassembly saved to %s",
                                    context);
            } else {
                baseui_show_message(false,
                                    (char *)"Disassembly copied to clipboard");
            }
            break;
        case DASM_EXPORT_ERR_OPEN_FAIL:
            baseui_show_message(true,
                (char *)"Cannot open file - check path and permissions: %s",
                context ? context : "");
            break;
        case DASM_EXPORT_ERR_WRITE_FAIL:
            baseui_show_message(true,
                (char *)"Write failed - disk full or I/O error: %s",
                context ? context : "");
            break;
        case DASM_EXPORT_ERR_INVALID_RANGE:
            baseui_show_message(true,
                (char *)"Invalid range (From > To)");
            break;
        case DASM_EXPORT_ERR_UNSUPPORTED_DIALECT:
            baseui_show_message(true,
                (char *)"Selected dialect not supported");
            break;
        case DASM_EXPORT_ERR_OUT_OF_MEMORY:
            baseui_show_message(true,
                (char *)"Out of memory during export");
            break;
        default:
            baseui_show_message(true,
                (char *)"Unknown error during export (rc=%d)", rc);
            break;
    }
}


/**
 * @brief Vykreslí top-bar s range From/To inputs + Disassemble button.
 *
 * Layout:
 *   řádek 1: [From: XXXX] [To: XXXX]  Bank: CPU   [Disassemble]                [Heatmap...]
 *   řádek 2: External sources: [_] use sym_db  (N syms)  [Browse...]  [_] use CDL/mhmap
 *
 * Heatmap button na řádku 1 je vpravo zarovnaný a vždy viditelný
 * (= rychlý přístup nezávisle na stavu use_cdl checkboxu). mhmap
 * status (mode / OFF warning) je v status baru dole, vpravo
 * zarovnaný za "dialect:".
 *
 * Hex inputy s flagem CharsHexadecimal - uživatel může psát
 * 0-9 / A-F bez prefixu. Změna libovolného fieldu nastaví
 * @c s_state.dirty = true (zatím jen informativní; live update
 * NENÍ V1 design).
 */
static void render_top_bar(void)
{
    ImGui::PushItemWidth(60);
    if (ImGui::InputScalar(_L("From###dasm_from"), ImGuiDataType_U16,
                           &s_state.range_from, NULL, NULL, "%04X",
                           ImGuiInputTextFlags_CharsHexadecimal))
    {
        s_state.dirty = true;
        /* Range změna → invalidace cached count */
        s_state.symdb_hits_cached_version = (unsigned)~0u;
    }
    ImGui::SameLine();
    if (ImGui::InputScalar(_L("To###dasm_to"), ImGuiDataType_U16,
                           &s_state.range_to, NULL, NULL, "%04X",
                           ImGuiInputTextFlags_CharsHexadecimal))
    {
        s_state.dirty = true;
        s_state.symdb_hits_cached_version = (unsigned)~0u;
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::TextDisabled("%s", _("Bank: CPU"));

    ImGui::SameLine();
    if (ImGui::Button(_L("Disassemble###dasm_run")))
    {
        dasm_window_run();
    }

    /*
     * Heatmap... button zarovnaný vpravo na 1. řádku (vždy viditelný,
     * nezávisle na stavu use_cdl - umožní rychlý přístup k Memory
     * Heatmap oknu i bez aktivní CDL/mhmap integrace).
     */
    {
        const char *hm_label = _L("Heatmap...###dasm_mhmap");
        float hm_w = ImGui::CalcTextSize(hm_label, NULL, true).x
                     + ImGui::GetStyle().FramePadding.x * 2.0f;
        float right_x = ImGui::GetWindowContentRegionMax().x - hm_w;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < right_x) {
            ImGui::SetCursorPosX(right_x);
        }
        if (ImGui::Button(hm_label)) {
            g_gui->showMemoryHeatmapWindow = true;
        }
    }

    /* --- F3: External sources ------------------------------------- */
    /* Recount sym_db hits pokud verze posunula (lazy invalidate). */
    if (s_state.symdb_hits_cached_version != sym_db_get_version()) {
        recount_symdb_hits_in_range();
    }

    ImGui::Text("%s", _("External sources:"));
    ImGui::SameLine();

    if (ImGui::Checkbox(_L("use sym_db###dasm_use_symdb"),
                        &s_state.use_symdb))
    {
        s_state.dirty = true;
    }
    ImGui::SameLine();
    if (s_state.use_symdb) {
        ImGui::TextDisabled("(%d %s)", s_state.symdb_hits_in_range,
                            _("syms in range"));
    } else {
        ImGui::TextDisabled("(%s)", _("off"));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Browse...###dasm_sym_browse")))
    {
        g_gui->showSymbolsWindow = true;
    }

    ImGui::SameLine();
    if (ImGui::Checkbox(_L("use CDL/mhmap###dasm_use_cdl"),
                        &s_state.use_cdl))
    {
        s_state.dirty = true;
    }
    /*
     * F8: status pro CDL/mhmap je v status baru dole (vpravo, za
     * dialect). Tlačítko Heatmap... je na 1. řádku zarovnané vpravo
     * (= rychlý přístup nezávislý na stavu checkboxu).
     */
}


/**
 * @brief Context menu pro jeden řádek listingu.
 *
 * Položky:
 *   1. Set as PC                 - dbg_ui_set_reg(Z80_REG_PC, addr)
 *   2. Set Breakpoint            - dbg_ui_bp_add(addr)
 *   3. Copy address              - clipboard "XXXX" (4-hex)
 *   4. Copy mnemonic             - clipboard pre-formátovaný text
 *   5. Focus to target (cond.)   - pokud r.target_addr != 0xFFFF a leží
 *                                  v rozumném rozmezí: nastav range tak,
 *                                  aby okolo target bylo cca 256 B,
 *                                  označ dirty (Disassemble triggered
 *                                  user-side button click; auto-refresh
 *                                  by mohl být sporný UX).
 *
 * Identifikátor popupu je @c ###row_ctx_<addr> aby každý řádek měl
 * stabilní ID (= ImGui pop-up stack se nezamotá při scrollu).
 *
 * @param r Aktuální řádek (kopie z s_state.rows[i]).
 */
static void render_row_context_menu(const DasmRow &r)
{
    char popup_id[32];
    snprintf(popup_id, sizeof popup_id, "###row_ctx_%04X", r.addr);
    if (ImGui::BeginPopupContextItem(popup_id)) {
        if (ImGui::MenuItem(_L("Set as PC###ctx_setpc"))) {
            dbg_ui_set_reg((uint8_t)Z80_REG_PC, r.addr);
        }
        if (ImGui::MenuItem(_L("Set Breakpoint###ctx_setbp"))) {
            dbg_ui_bp_add(r.addr, NULL);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(_L("Copy address###ctx_copyaddr"))) {
            char buf[8];
            snprintf(buf, sizeof buf, "%04X", r.addr);
            ImGui::SetClipboardText(buf);
        }
        if (ImGui::MenuItem(_L("Copy mnemonic###ctx_copymnem"))) {
            ImGui::SetClipboardText(r.mnemonic);
        }
        /* Focus to target - jen pokud má instrukce statický cíl. Sentinel
         * 0xFFFF z z80_dasm_target_addr() = "no target". */
        if (r.target_addr != (uint16_t)0xFFFFu) {
            ImGui::Separator();
            char label[64];
            snprintf(label, sizeof label, "%s %04X###ctx_focus",
                     _("Focus to target"), r.target_addr);
            if (ImGui::MenuItem(label)) {
                uint32_t lo = (uint32_t)r.target_addr;
                /* Centrované okolí cca 256 B; clampu na 0x0000-0xFFFF. */
                uint32_t hi = lo + 0xFFu;
                if (hi > 0xFFFFu) hi = 0xFFFFu;
                s_state.range_from = (uint16_t)lo;
                s_state.range_to   = (uint16_t)hi;
                s_state.dirty = true;
                s_state.symdb_hits_cached_version = (unsigned)~0u;
                dasm_window_run();
            }
        }
        ImGui::EndPopup();
    }
}


/**
 * @brief Vykreslí tabulku listingu (Addr | Bytes | Label | Mnemonic).
 *
 * Scroll-area s frozen header rowem. Sloupec Bytes vypisuje
 * surové bajty hex stylem "XX XX XX". Sloupec Label se ve F2 plní
 * auto-labely (S/L/D/W) přes s_state.label_at_addr lookup.
 *
 * F7: každý řádek dostane neviditelný Selectable s SpanAllColumns -
 * cílí jen na context menu (pravý klik). Levý klik nemá akci ve V1
 * (= focus / step-to-here přijde případně později).
 *
 * Velikost tabulky je vyplněna do okna s výjimkou poslední
 * řádky (status bar).
 */
static void render_listing_table(void)
{
    /* Rezerva pro action buttons + status bar dole = 2 frame heights. */
    const float reserve_h = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    const ImGuiTableFlags flags =
          ImGuiTableFlags_RowBg
        | ImGuiTableFlags_BordersInnerV
        | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_Resizable;

    if (ImGui::BeginTable("##dasm_listing", 4, flags,
                          ImVec2(0, -reserve_h)))
    {
        ImGui::TableSetupColumn(_("Addr"),
            ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn(_("Bytes"),
            ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn(_("Label"),
            ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn(_("Mnemonic"),
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)s_state.rows.size());
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart;
                 i < clipper.DisplayEnd; i++)
            {
                const DasmRow &r = s_state.rows[(size_t)i];
                ImGui::TableNextRow();

                /* Sloupec 0: adresa + Selectable přes celý řádek pro
                 * context menu (SpanAllColumns + AllowOverlap aby
                 * jednotlivé buňky šly hover/select). */
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(r.addr);
                char addr_label[16];
                snprintf(addr_label, sizeof addr_label, "%04X", r.addr);
                ImGui::Selectable(addr_label, false,
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap);
                /* Context menu - sdílený popup ID per addr. */
                render_row_context_menu(r);

                /* Sloupec 1: surové bajty. */
                ImGui::TableSetColumnIndex(1);
                char bytes_buf[16];
                int bp = 0;
                for (int b = 0; b < r.num_bytes && bp < 15; b++)
                {
                    bp += std::snprintf(bytes_buf + bp,
                                        sizeof(bytes_buf) - bp,
                                        (b == 0) ? "%02X" : " %02X",
                                        r.bytes[b]);
                }
                bytes_buf[bp] = '\0';
                ImGui::TextUnformatted(bytes_buf);

                /* Sloupec 2: Label - F2 plnění */
                ImGui::TableSetColumnIndex(2);
                auto it = s_state.label_at_addr.find(r.addr);
                if (it != s_state.label_at_addr.end()) {
                    const dasm_label_t &lbl =
                        s_state.labels[(size_t)it->second];
                    ImVec4 c = label_color_for(lbl);
                    ImGui::TextColored(c, "%s:", lbl.name);
                }

                /* Sloupec 3: mnemonic + operandy (s případnou
                 * symbolovou substitucí). */
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(r.mnemonic);

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}


/**
 * @brief Zajistí, že @p path končí požadovanou příponou.
 *
 * Pokud aktuální @p path neobsahuje tečku v poslední komponentě, nebo
 * končí jinou příponou než @p ext, doplní/nahradí na @p ext.
 *
 * Bezpečnostní limit: zápis se neprovede, pokud by celková délka
 * přesáhla @p path_sz - 1 (= zachová null terminator).
 *
 * @param[in,out] path    Cesta k souboru (in-place modifikace).
 * @param         path_sz Velikost bufferu @p path.
 * @param         ext     Cílová přípona včetně tečky (".asm" / ".s").
 */
static void ensure_extension(char *path, size_t path_sz, const char *ext)
{
    if (path == NULL || ext == NULL || path_sz < 2) return;
    size_t plen = strlen(path);
    if (plen == 0) {
        /* Empty path - jen vlož extension. */
        size_t elen = strlen(ext);
        if (elen + 1 < path_sz) {
            memcpy(path, ext, elen + 1);
        }
        return;
    }

    /* Najdi pozici poslední tečky za posledním separátorem. */
    size_t dot_pos = (size_t)-1;
    for (size_t i = plen; i-- > 0; ) {
        char c = path[i];
        if (c == '/' || c == '\\') break;
        if (c == '.') { dot_pos = i; break; }
    }

    if (dot_pos != (size_t)-1) {
        /* Už existuje přípona - porovnej s ext (case-sensitive). */
        if (strcmp(path + dot_pos, ext) == 0) return;
        /* Jiná přípona - nahradíme od dot_pos. */
        size_t elen = strlen(ext);
        if (dot_pos + elen + 1 <= path_sz) {
            memcpy(path + dot_pos, ext, elen + 1);
        }
        return;
    }

    /* Žádná přípona - připoj. */
    size_t elen = strlen(ext);
    if (plen + elen + 1 <= path_sz) {
        memcpy(path + plen, ext, elen + 1);
    }
}


/**
 * @brief Default přípona pro dialekt.
 *
 * pasmo / sjasmplus → ".asm" (de-facto standard)
 * sdcc-asz80        → ".s"   (sdcc toolchain konvence)
 *
 * @param d Dialect.
 * @return Pointer na statický literál.
 */
static const char *default_extension_for(dasm_dialect_t d)
{
    return (d == DASM_DIALECT_SDCC_ASZ80) ? ".s" : ".asm";
}


/**
 * @brief Vrátí ImGuiFileDialog filter string pro dialekt.
 *
 * Filter syntaxe IGFD: ".ext1,.ext2,.*" (čárkou oddělené, ".*" = vše).
 *
 * @param d Dialect.
 * @return Statický string s filtrem.
 */
static const char *igfd_filter_for_dialect(dasm_dialect_t d)
{
    return (d == DASM_DIALECT_SDCC_ASZ80) ? ".s,.txt,.*" : ".asm,.txt,.*";
}


/**
 * @brief Implementace Copy-to-clipboard.
 *
 * Refactor F7: dříve stub. Nyní volá @ref dasm_export_to_string
 * s aktuálními export options, výsledek předává
 * @c ImGui::SetClipboardText. Error mapping přes
 * @ref map_export_result_to_message.
 *
 * @post Při OK je clipboard aktualizovaný. Při error zůstává starý
 *       obsah clipboardu.
 */
static void dasm_window_copy_clipboard(void)
{
    dasm_export_opts_t opts = build_export_opts();
    char *out = NULL;
    int rc = dasm_export_to_string(
        s_state.range_from, s_state.range_to,
        dasm_read_byte_cb, NULL,
        s_state.labels.data(), (int)s_state.labels.size(),
        &opts, &out);
    if (rc == DASM_EXPORT_OK && out != NULL) {
        ImGui::SetClipboardText(out);
        free(out);
    }
    map_export_result_to_message(rc, NULL);
}


/**
 * @brief IGFD Browse... handler pro Save dialog.
 *
 * Otevře IGFD pod ID @c "DasmSavePicker" s filtrem dle dialektu.
 * Po @c IsOk() přepíše @c s_state.save_path. Layout file pickeru je
 * shodný s @c memext_save_dialog (modal, devices, hidden files off,
 * confirm overwrite).
 */
static void open_save_browser(void)
{
    IGFD::FileDialogConfig cfg;
    cfg.path = ".";
    cfg.countSelectionMax = 1;
    cfg.flags = ImGuiFileDialogFlags_Modal
              | ImGuiFileDialogFlags_DontShowHiddenFiles
              | ImGuiFileDialogFlags_ShowDevicesButton
              | ImGuiFileDialogFlags_ConfirmOverwrite;
    cfg.fileName = (s_state.save_dialect == DASM_DIALECT_SDCC_ASZ80)
                   ? "disasm.s" : "disasm.asm";
    ImGuiFileDialog::Instance()->OpenDialog(
        "DasmSavePicker",
        _("Save Disassembly As..."),
        igfd_filter_for_dialect(s_state.save_dialect),
        cfg);
    s_state.save_browse_open = true;
}


/**
 * @brief Vykreslí (pokud otevřený) IGFD picker pro Save dialog.
 *
 * Při OK uloží vybranou cestu do @c s_state.save_path. Cancel: nic.
 * V obou případech zavře dialog a vynuluje @c save_browse_open.
 */
static void render_save_browser(void)
{
    if (!s_state.save_browse_open) return;
    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
    if (ImGuiFileDialog::Instance()->Display("DasmSavePicker")) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string path =
                ImGuiFileDialog::Instance()->GetFilePathName();
            strncpy(s_state.save_path, path.c_str(),
                    sizeof s_state.save_path - 1);
            s_state.save_path[sizeof s_state.save_path - 1] = '\0';
        }
        ImGuiFileDialog::Instance()->Close();
        s_state.save_browse_open = false;
    }
}


/**
 * @brief Modální Save dialog.
 *
 * Render volat z dasm_window_render uvnitř Begin()/End() okna (ImGui
 * popup ID hierarchy: child popup okna). Otevírání: nastavit
 * @c s_state.save_request_open = true v handleru tlačítka; render
 * funkce zavolá OpenPopup ve stejném scope a otevře BeginPopupModal.
 *
 * Layout (vertikální):
 *   - "Target assembler:" + 3x RadioButton (pasmo / sjasmplus / sdcc)
 *   - separator
 *   - "Options:" + 3x Checkbox (ORG / bytes / uppercase)
 *   - separator
 *   - "Path:" + InputText + "Browse..." (F7: IGFD picker)
 *   - separator
 *   - [Save] [Cancel]
 *
 * F7: Esc v popupu = Cancel; Browse... button volá IGFD picker.
 *
 * Při Save: ensure_extension dle dialektu, volání
 * @c dasm_export_write s aktuálními labely. Hlášky přes
 * @ref map_export_result_to_message.
 */
static void render_save_dialog(void)
{
    /* OpenPopup musí být ve stejném ID scope jako BeginPopupModal
     * - tj. ne v handleru tlačítka v jiné funkci. Tick request
     * vyřešen flagem s_state.save_request_open. */
    if (s_state.save_request_open) {
        ImGui::OpenPopup("##dasm_save_popup");
        s_state.save_request_open = false;
    }

    if (!ImGui::BeginPopupModal("##dasm_save_popup", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::TextUnformatted(_("Save Disassembly"));
    ImGui::Separator();

    /* --- Target assembler --- */
    ImGui::TextUnformatted(_("Target assembler:"));
    int dialect_int = (int)s_state.save_dialect;
    ImGui::RadioButton(_L("pasmo###dasm_save_d_pasmo"),
                       &dialect_int, (int)DASM_DIALECT_PASMO);
    ImGui::SameLine();
    ImGui::RadioButton(_L("sjasmplus###dasm_save_d_sjasmplus"),
                       &dialect_int, (int)DASM_DIALECT_SJASMPLUS);
    ImGui::SameLine();
    ImGui::RadioButton(_L("sdcc-asz80###dasm_save_d_sdcc"),
                       &dialect_int, (int)DASM_DIALECT_SDCC_ASZ80);
    s_state.save_dialect = (dasm_dialect_t)dialect_int;

    ImGui::Separator();

    /* --- Options --- */
    ImGui::TextUnformatted(_("Options:"));
    ImGui::Checkbox(_L("include ORG directive###dasm_save_opt_org"),
                    &s_state.save_include_org);
    ImGui::Checkbox(_L("include bytes as comments###dasm_save_opt_bytes"),
                    &s_state.save_include_bytes);
    ImGui::Checkbox(_L("uppercase mnemonics###dasm_save_opt_upper"),
                    &s_state.save_uppercase);

    ImGui::Separator();

    /* --- Path --- */
    ImGui::TextUnformatted(_("Path:"));
    ImGui::PushItemWidth(400.0f);
    ImGui::InputText("###dasm_save_path", s_state.save_path,
                     sizeof s_state.save_path);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button(_L("Browse...###dasm_save_browse"))) {
        open_save_browser();
    }

    ImGui::Separator();

    /* --- Save / Cancel --- */
    if (ImGui::Button(_L("Save###dasm_save_confirm"))) {
        ensure_extension(s_state.save_path, sizeof s_state.save_path,
                         default_extension_for(s_state.save_dialect));

        dasm_export_opts_t opts = build_export_opts();
        int rc = dasm_export_write(
            s_state.save_path,
            s_state.range_from, s_state.range_to,
            dasm_read_byte_cb, NULL,
            s_state.labels.data(),
            (int)s_state.labels.size(),
            &opts);

        map_export_result_to_message(rc, s_state.save_path);
        if (rc == DASM_EXPORT_OK) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(_L("Cancel###dasm_save_cancel"))) {
        ImGui::CloseCurrentPopup();
    }

    /* F7: Esc = Cancel uvnitř popupu. */
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}


/**
 * @brief Vykreslí dolní lištu s akčními tlačítky (Save / Copy / Refresh).
 *
 * Layout: [Save .asm/.s...] [Copy to clipboard] [Refresh]
 *
 * - Save otevírá modální Save dialog (popup je v render_save_dialog).
 * - Copy spustí @ref dasm_window_copy_clipboard.
 * - Refresh nastaví dirty a spustí dasm_window_run() (= explicit
 *   re-disassemble bez modifikace range/options).
 */
static void render_action_buttons(void)
{
    if (ImGui::Button(_L("Save .asm/.s...###dasm_save_btn"))) {
        s_state.save_request_open = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(_L("Copy to clipboard###dasm_copy_btn"))) {
        dasm_window_copy_clipboard();
    }
    ImGui::SameLine();
    if (ImGui::Button(_L("Refresh###dasm_refresh_btn"))) {
        dasm_window_run();
    }
}


/**
 * @brief Vykreslí status bar dole.
 *
 * F7 + F8 layout (jeden řádek se separátory "|"):
 *   "N instr | auto: A | sym: S | warn: W | range: $XXXX-$XXXX | dialect: name        [mhmap: <stav>]"
 *
 * - "warn: W" je červeně pokud W > 0 (jinak default barva).
 * - "sym: S" se vypisuje jen pokud use_symdb == true.
 * - "range" a "dialect" se vždy vypisují (= identifikace co se vidí).
 * - mhmap stav je vpravo zarovnaný a zobrazuje se jen pokud
 *   use_cdl == true (žlutě pokud mhmap_mode == OFF, jinak disabled).
 */
static void render_status_bar(void)
{
    int n_auto = 0, n_sym = 0, n_warn = 0;
    for (const auto &lbl : s_state.labels) {
        if (lbl.from_symdb) {
            n_sym++;
        } else if (lbl.type == DASM_LABEL_WARN) {
            n_warn++;
        } else {
            n_auto++;
        }
    }

    ImGui::Text("%zu %s", s_state.rows.size(), _("instr"));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(_("auto: %d"), n_auto);
    if (s_state.use_symdb) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text(_("sym: %d"), n_sym);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (n_warn > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                           _("warn: %d"), n_warn);
    } else {
        ImGui::Text(_("warn: %d"), n_warn);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(_("range: $%04X-$%04X"),
                s_state.range_from, s_state.range_to);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(_("dialect: %s"), dialect_name_str(s_state.save_dialect));

    /*
     * mhmap status vpravo zarovnaný (F8). Zobrazuje se jen pokud
     * use_cdl je ON. Žluté varování pro OFF mode, disabled pro
     * aktivní mode (informativní label).
     */
    if (s_state.use_cdl) {
        char mhmap_text[64];
        bool warn_color = (g_debugger.mhmap_mode == DEBUGGER_MHMAP_MODE_OFF);
        if (warn_color) {
            snprintf(mhmap_text, sizeof mhmap_text, "%s: %s",
                     _("mhmap"), _("OFF (no data)"));
        } else {
            snprintf(mhmap_text, sizeof mhmap_text, "%s: %s",
                     _("mhmap"), mhmap_mode_name(g_debugger.mhmap_mode));
        }
        float text_w = ImGui::CalcTextSize(mhmap_text).x;
        float right_x = ImGui::GetWindowContentRegionMax().x - text_w;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < right_x) {
            ImGui::SetCursorPosX(right_x);
        }
        if (warn_color) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                               mhmap_text);
        } else {
            ImGui::TextDisabled("%s", mhmap_text);
        }
    }
}

} /* namespace */


void dasm_window_render(void)
{
    if (!g_gui->showDisassemblerWindow) return;

    /*
     * Počáteční velikost okna při prvním otevření (FirstUseEver).
     * 1024 x 480 - šířka stačí na celou top-bar s Heatmap tlačítkem
     * vpravo bez zalomení a na statusbar s mhmap statusem za "dialect"
     * (změřeno ze screenshotu 2026-05-26). User si může zmenšit/zvětšit,
     * ImGui ini si zachová poslední velikost.
     */
    ImGui::SetNextWindowSize(ImVec2(1024, 480), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(_("Disassembler###dasm_window"),
                     &g_gui->showDisassemblerWindow,
                     ImGuiWindowFlags_NoCollapse))
    {
        /* F7 shortcuts - gated na "okno focused" aby globální Ctrl+S
         * mimo Disassembler nedělal nic. Volání před top_bar aby se
         * shortcut zachytil i když user nedrží myš nad listingem. */
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S,
                                ImGuiInputFlags_None)) {
                s_state.save_request_open = true;
            }
            if (ImGui::Shortcut(ImGuiKey_F5, ImGuiInputFlags_None)) {
                dasm_window_run();
            }
        }

        render_top_bar();
        ImGui::Separator();
        render_listing_table();
        render_action_buttons();
        render_status_bar();

        /* Save popup je ve stejném ID scope jako tlačítko které ho
         * otevírá (přes save_request_open flag). */
        render_save_dialog();

        /* IGFD picker (modal) - render na úrovni okna, ne v rámci
         * Save popup-u (IGFD si ManagedDisplay řídí sám). */
        render_save_browser();
    }
    ImGui::End();
}


/* =========================================================================
 *  F7: Persistence (mz<arch>emu.ini sekce [DASM_WINDOW])
 * ========================================================================= */

/**
 * @brief Naplní @c persist_* pole z aktuálního UI stavu (volat před save).
 *
 * cfgmain volá save callbacky po @c capture (= užívá @c persist_* hodnoty
 * jako "save_handler"). Tato funkce mostí typy: UI uchovává @c uint16_t /
 * @c bool / @c dasm_dialect_t, persist pole jsou @c unsigned aby pasovaly
 * na CFGENTYPE_UNSIGNED / CFGENTYPE_BOOL.
 *
 * @param[in,out] data Cast na @c DasmWindowState*.
 */
static void capture_persist(void)
{
    s_state.persist_range_from    = (unsigned)s_state.range_from;
    s_state.persist_range_to      = (unsigned)s_state.range_to;
    s_state.persist_use_symdb     = s_state.use_symdb ? 1u : 0u;
    s_state.persist_use_cdl       = s_state.use_cdl ? 1u : 0u;
    s_state.persist_dialect       = (unsigned)s_state.save_dialect;
    s_state.persist_include_org   = s_state.save_include_org ? 1u : 0u;
    s_state.persist_include_bytes = s_state.save_include_bytes ? 1u : 0u;
    s_state.persist_uppercase     = s_state.save_uppercase ? 1u : 0u;

    /* Save path: cfgelement_bind drží char** který v save fázi čte. Náš
     * pointer @c persist_save_path musí ukazovat na čerstvý strdup
     * aktuální cesty; cfgfile knihovna ho free/strdup automaticky.
     * Při idempotentním save by se to volalo opakovaně - cfgfile to
     * řeší v cfgelement_set_text_value (alloc/free). */
    if (s_state.persist_save_path) {
        free(s_state.persist_save_path);
    }
    s_state.persist_save_path = strdup(s_state.save_path);
}


/**
 * @brief Naplní UI stav z @c persist_* polí (volat po cfgmodule_propagate).
 *
 * Inverze @ref capture_persist - po načtení .ini se zkopírují hodnoty
 * zpět do UI typů. Defenzivní clamping (např. dialect na známé enum
 * hodnoty) zde aplikován.
 */
static void apply_persisted(void)
{
    s_state.range_from         = (uint16_t)(s_state.persist_range_from & 0xFFFFu);
    s_state.range_to           = (uint16_t)(s_state.persist_range_to & 0xFFFFu);
    s_state.use_symdb          = (s_state.persist_use_symdb != 0u);
    s_state.use_cdl            = (s_state.persist_use_cdl != 0u);
    if (s_state.persist_dialect <= (unsigned)DASM_DIALECT_SDCC_ASZ80) {
        s_state.save_dialect = (dasm_dialect_t)s_state.persist_dialect;
    } else {
        s_state.save_dialect = DASM_DIALECT_SJASMPLUS;
    }
    s_state.save_include_org   = (s_state.persist_include_org != 0u);
    s_state.save_include_bytes = (s_state.persist_include_bytes != 0u);
    s_state.save_uppercase     = (s_state.persist_uppercase != 0u);

    if (s_state.persist_save_path && s_state.persist_save_path[0]) {
        strncpy(s_state.save_path, s_state.persist_save_path,
                sizeof s_state.save_path - 1);
        s_state.save_path[sizeof s_state.save_path - 1] = '\0';
    }
    /* Range změna → invalidace cached sym_db count. */
    s_state.symdb_hits_cached_version = (unsigned)~0u;
    s_state.dirty = true;
}


/**
 * @brief cfgmain save callback - volá se před zápisem INI.
 *
 * @param m    Modulový pointer (nepoužitý - bindings jsou per-element).
 * @param data Nepoužito.
 */
static void cfg_save_cb(void *m, void *data)
{
    (void)m;
    (void)data;
    capture_persist();
}


extern "C" void dasm_window_register_persistence(void *cmod_void)
{
    if (!cmod_void) return;
    st_CFGMODULE *cmod = (st_CFGMODULE *)cmod_void;
    st_CFGELEMENT *elm;

    /* Range from/to - default 0x0000-0x00FF (= match UI startup). */
    elm = cfgmodule_register_new_element(cmod, (char *)"range_from",
                                          CFGENTYPE_UNSIGNED, 0u,
                                          0u, 0xFFFFu);
    cfgelement_set_handlers(elm,
                            (void *)&s_state.persist_range_from,
                            (void *)&s_state.persist_range_from);

    elm = cfgmodule_register_new_element(cmod, (char *)"range_to",
                                          CFGENTYPE_UNSIGNED, 0xFFu,
                                          0u, 0xFFFFu);
    cfgelement_set_handlers(elm,
                            (void *)&s_state.persist_range_to,
                            (void *)&s_state.persist_range_to);

    /* use_symdb / use_cdl bool. */
    elm = cfgmodule_register_new_element(cmod, (char *)"use_symdb",
                                          CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm,
                            (void *)&s_state.persist_use_symdb,
                            (void *)&s_state.persist_use_symdb);

    elm = cfgmodule_register_new_element(cmod, (char *)"use_cdl",
                                          CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm,
                            (void *)&s_state.persist_use_cdl,
                            (void *)&s_state.persist_use_cdl);

    /* Save dialect: 0=pasmo, 1=sjasmplus (default), 2=sdcc-asz80. */
    elm = cfgmodule_register_new_element(cmod, (char *)"save_dialect",
                                          CFGENTYPE_UNSIGNED,
                                          (unsigned)DASM_DIALECT_SJASMPLUS,
                                          0u, 2u);
    cfgelement_set_handlers(elm,
                            (void *)&s_state.persist_dialect,
                            (void *)&s_state.persist_dialect);

    elm = cfgmodule_register_new_element(cmod, (char *)"save_include_org",
                                          CFGENTYPE_BOOL, 1);
    cfgelement_set_handlers(elm,
                            (void *)&s_state.persist_include_org,
                            (void *)&s_state.persist_include_org);

    elm = cfgmodule_register_new_element(cmod, (char *)"save_include_bytes",
                                          CFGENTYPE_BOOL, 1);
    cfgelement_set_handlers(elm,
                            (void *)&s_state.persist_include_bytes,
                            (void *)&s_state.persist_include_bytes);

    elm = cfgmodule_register_new_element(cmod, (char *)"save_uppercase",
                                          CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm,
                            (void *)&s_state.persist_uppercase,
                            (void *)&s_state.persist_uppercase);

    /* save_path - TEXT, cfgelement_bind dělá strdup/free pres char*
     * pointer. Persist pointer musí existovat (NULL na vstupu by
     * crashlo knihovnu). */
    elm = cfgmodule_register_new_element(cmod, (char *)"save_path",
                                          CFGENTYPE_TEXT, (char *)"disasm.asm");
    cfgelement_bind(elm, (void *)&s_state.persist_save_path);

    /* Module-level save callback: cfgroot_save volá per-modul save_cb
     * před zápisem - tady kopírujeme aktuální UI stav do persist polí. */
    cfgmodule_set_save_cb(cmod, cfg_save_cb, NULL);
}


extern "C" void dasm_window_apply_persisted(void)
{
    apply_persisted();
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
