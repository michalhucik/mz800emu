/*
 * dbg_disassembled.cpp — Sekce Disassembled hlavního okna debuggeru
 *
 * PŘEHLED
 * =======
 * Tato sekce je nejdůležitější částí debuggeru. Zabírá celý levý sloupec
 * hlavního okna a zobrazuje disassemblovaný Z80 kód ve dvou tabulkách.
 *
 * INSTANCOVATELNÁ KOMPONENTA
 * ==========================
 * Veškerý per-instance stav (cache, dasm buffery, BPT cache, historie
 * cache) je zapouzdřen ve struktuře DisassembledView. Externí volající
 * pracují buď přes opaque handle (dbg_disasm_view_*), nebo přes tenké
 * shim funkce nad lazy-vytvořeným singletonem hlavní instance
 * (dbg_disassembled_render, dbg_dasm_is_addr_visible).
 *
 * Hlavní singleton "main" sdílí UI stav (focus_addr, selected_row,
 * history_split_ratio, visible_rows, selected_addr) s globální
 * g_dbg_ui - to zachovává zpětnou kompatibilitu s konzumenty zvenku
 * (debugger_window.cpp, dbg_focus_to.cpp). Sekundární instance (plán
 * č. 7) by měly mít vlastní lokální storage - připraveno polem
 * local_state, dnes nevyužito.
 *
 * LAYOUT
 * ======
 *
 *  ┌─── Disassembled ─────────────────────────┐
 *  │                                          │
 *  │  ┌─ Historie ───────────────────────┐ ┌─┐│
 *  │  │    E6C1  78        ld a,b     ┌─┐│ │ ││
 *  │  │    E6C2  FE 0D     cp 0Dh     │ ││ │ ││
 *  │  │    ...                        │ ││ │ ││
 *  │  │    E6D4  C3 CA 0F  jp 0FCAh   │ ││ │ ││
 *  │  │                               └─┘│ │ ││
 *  │  ├──────────────────────────────────┤ │ ││
 *  │  │         ═══ splitter ═══         │ │ ││
 *  │  ├──────────────────────────────────┤ │█││
 *  │  │  → E6D7  78        ld a,b     ┌─┐│ │ ││
 *  │  │ ●  E6D8  FE 61     cp 61h     │ ││ │ ││
 *  │  │    E6DA  FE 0D     cp 0Dh     │ ││ │ ││
 *  │  │    ...                        │ ││ │ ││
 *  │  │                               └─┘│ │ ││
 *  │  └──────────────────────────────────┘ └─┘│
 *  └──────────────────────────────────────────┘
 *
 * HORNÍ TABULKA — HISTORIE
 * ========================
 * Zobrazuje ring buffer posledních vykonaných instrukcí z g_debugger_history.
 * Ring buffer má kapacitu DEBUGGER_HISTORY_LENGTH (32) záznamů.
 * Každý záznam obsahuje adresu instrukce a bytekódy (max 4 bajty).
 *
 * Chování:
 * - Řádky jdou od nejstaršího (nahoře) k nejnovějšímu (dole)
 * - Po refreshi se scrollbar automaticky nastaví na konec (nejnovější instrukce)
 * - Tabulka je pouze pro čtení — žádná interakce kromě scrollování
 * - Každý řádek zobrazuje: ADRESA | BYTEKÓDY | MNEMONIC
 * - Disassembly se provádí přes z80_dasm() + z80_dasm_to_str_sym() s
 *   callback debugger_dasm_history_read_cb() a sdíleným symtab cache
 *
 * DOLNÍ TABULKA — DISASSEMBLY
 * ============================
 * Zobrazuje disassemblovanou paměť od adresy focusu (g_dbg_ui.focus_addr).
 * Počet zobrazených řádků se dynamicky počítá podle výšky okna a fontu.
 *
 * Chování při refreshi (přechod z animačního do editačního režimu):
 * 1. Disassembling prvních 50 instrukcí od regPC
 * 2. Focus se nastaví na regPC, selected_row na 0
 * 3. Slider se synchronizuje s focusem
 *
 * Interakce v editačním režimu:
 * - Jednořádkový selektor — vždy 1 řádek je "aktivní" (selectovaný)
 * - Adresa aktivního řádku = focus_addr
 * - Klik na řádek → změní selekci a focus
 *
 * Sloupce BPT a PC v dolní tabulce (sloupec ICONS = levý + pravý slot):
 *
 * Levý slot — ikona breakpointu (dle en_DBG_BPT_ROW_STATE):
 * - ● červený plný kruh   = aktivní BPT na počáteční adrese řádku
 * - ○ bílý prázdný kruh   = deaktivovaný BPT na počáteční adrese řádku
 * - ● žlutý plný kruh    = aktivní BPT uvnitř instrukce (ne na počátku)
 * - ○ žlutý prázdný kruh = deaktivovaný BPT uvnitř instrukce
 * - (prázdný)              = žádný breakpoint
 * Priorita: BPT na start adrese > první BPT uvnitř instrukce (dle adresy).
 *
 * Pravý slot — PC indikátor:
 * - ► zelený trojúhelník + zelený text = addr == regPC
 * - → žlutá šipka + žlutý text        = regPC uvnitř instrukce
 *
 * Navigace klávesnicí a kolečkem myši (UP/DOWN a kolečko):
 *
 * Dvou-fázové chování:
 * 1. Primárně posunují selekci (selected_row) v rámci viditelných řádků.
 * 2. Teprve až je selekce na prvním (nahoru) nebo posledním (dolů) řádku,
 *    změní se focus_addr — tj. scrolluje se obsah tabulky.
 *
 * Scrollování dolů (selected_row == poslední):
 *   focus_addr += row_lengths[0] — první řádek odscrolluje nahoru
 *   z viditelné oblasti. Posun je přesný — známe délku instrukce.
 *
 * Scrollování nahoru (selected_row == 0):
 *   focus_addr -= 1 — nový řádek se objeví nahoře. Posun je jen o 1 bajt,
 *   protože Z80 má instrukce variabilní délky (1–4 B) a zpětný disassembly
 *   je nejednoznačný. Pro ladění je to dostatečné.
 *
 * Kolečko reaguje na hover (nemusí být focus okna).
 *
 * Další klávesy:
 * - PgUp: posun o celou stránku nahoru (50 řádků × ~2 bajty)
 * - PgDown: posun o celou stránku dolů (50 instrukcí dopředu)
 * - Ctrl+PgUp: skok na začátek paměti (0x0000)
 * - Ctrl+PgDown: skok na konec paměti (blízko 0xFFFF)
 *
 * Context menu (pravé tlačítko):
 * - Set/Remove Breakpoint: vytvoří/smaže BPT na adrese řádku
 * - Enable/Disable Breakpoint: přepne enabled (jen pokud BPT existuje)
 * - Set as PC: nastaví regPC na adresu aktivního řádku
 * - Focus to <target>: focus na cíl branch instrukce (jen pokud má fixed
 *   target - CALL/JP/JR/DJNZ/RST)
 * - Focus To...: otevře dialog pro zadání adresy focusu (s historií)
 * - Focus to PC: vrátí focus na aktuální regPC
 * - Focus to register: submenu s 16-bit registry/páry (AF, BC, DE, HL,
 *   AF', BC', DE', HL', IX, IY, SP); klik = focus na hodnotu jako adresu
 * - Edit row: otevře Inline Assembler
 *
 * Double click / Enter:
 * - Otevře Inline Assembler na adrese aktivního řádku (zatím neimplementováno)
 *
 * ŠOUPÁTKO (SLIDER)
 * =================
 * Vertikální slider v pravé části sekce. Rozsah 0x0000–0xFFFF.
 * Přímo řídí adresu focusu — při změně slideru se mění obsah dolní tabulky.
 * Řádek s focusem je vždy na první pozici tabulky.
 *
 * Slider je "invertovaný" — nahoře je 0x0000, dole 0xFFFF (jako adresní prostor).
 *
 * SPLITTER
 * ========
 * Horizontální posuvník mezi horní a dolní tabulkou. Uživatel jím nastavuje
 * poměr výšek: kolik místa zabírá historie vs. disassembly.
 * Poměr se ukládá v g_dbg_ui.history_split_ratio (výchozí 0.35).
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

#include "libs/imgui/imgui.h"
#include "i18n.h"
#include "emulator/emulator.h"
#include "debugger/debugger.h"
#include "debugger/breakpoints.h"
#include "debugger/symbols/sym_db.h"
#include "ui-imgui/debugger/symbols/symdb_bridge.h"
#include "debugger/bookmarks/bookmarks.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"
#include "libs/dasm-z80/z80_dasm.h"
#include "libs/z80meta/z80_meta.h"

#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

#include "dbg_disassembled.h"
#include "dbg_extra_disasm.h"
#include "../debugger_window.h"   /* V9.4: debugger_window_request_focus */
#include "dbg_inline_asm.h"
#include "dbg_focus_to.h"
#include "../breakpoints/bpt_state.h"        /* V1.7: pending filter + last triggered */
#include "../breakpoints/bpt_edit_panel.h"    /* V1.7: open edit panel z popup menu */
#include "../debugger_state.h"
#include "../dbgapi_helpers.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Maximální délka Z80 instrukce v bajtech */
static const int Z80_MAX_INSTRUCTION_LENGTH = 4;

/* Maximální délka mnemoniky (včetně nulového terminátoru) */
static const int Z80_MAX_MNEMONIC_LENGTH = 64;

#define DISSAMBLE_BUFFER_SIZE (DBG_DASM_MAX_VISIBLE_ROWS * Z80_MAX_INSTRUCTION_LENGTH)

/**
 * @brief Buffer paměti připravený pro disassembly nebo cache jejího obsahu
 *        z předchozího framu.
 *
 * Drží počáteční adresu segmentu (start_addr) a binární obsah načtený
 * přes debugger_dasm_read_cb(). Slouží pro detekci self-modifying kódu
 * a invalidaci dasm cache (memcmp dvou bufferů).
 *
 * Ownership: vlastníkem je instance DisassembledView, která drží dvojici
 * dasm_buf[0] / dasm_buf[1] a střídá je ukazateli current_dasm_buf /
 * last_dasm_buf při každém disassembly cyklu.
 */
typedef struct st_DASM_BUFFER
{
    uint16_t start_addr;                  /* Počáteční adresa segmentu paměti pro disassembly */
    uint8_t bytes[DISSAMBLE_BUFFER_SIZE]; /* Buffer pro načtení paměti k disassemblování */
} st_DASM_BUFFER;

/**
 * @brief Struktura jednoho řádku disassembly (cache item).
 *
 * Při vykreslování se naplní pole těchto struktur — buď z historie
 * (pro horní tabulku), nebo z paměti (pro dolní tabulku).
 *
 * Pole bytes je pevně 4 bajty (max délka Z80 instrukce); pokud je
 * num_bytes < 4, zbývající pozice nejsou definované a nesmí se z nich
 * nic vykreslovat.
 */
typedef struct DasmRow
{
    uint16_t addr;     /* Adresa instrukce */
    uint8_t bytes[4];  /* Bytekódy instrukce (max 4) */
    int num_bytes;     /* Počet bajtů instrukce */
    char mnemonic[64]; /* Textová reprezentace (Z80 mnemonic, případně se symboly v operandech) */
    z80_dasm_inst_t inst; /* Strukturované metadata instrukce (T-states, flow, op1/op2, regs_*).
                             Vyplněno při disassemblaci v disassemble_at() / read_debugger_history().
                             Slouží pro tooltip (plán č. 3), branch arrows (plán č. 8) a
                             right-click menu (plán č. 5) - dispenzují re-disassemblaci řádku. */
} DasmRow;

/**
 * @brief Stav řádku z hlediska breakpointu pro vizualizaci ikony BPT.
 *
 * Cache se invaliduje při změně g_breakpoints.version nebo při refreshi
 * (nové řádky = nové adresy k prověření).
 */
typedef enum
{
    DBG_BPT_NONE,            /* Žádný breakpoint */
    DBG_BPT_START_ENABLED,   /* Aktivní BPT na počáteční adrese řádku */
    DBG_BPT_START_DISABLED,  /* Deaktivovaný BPT na počáteční adrese řádku */
    DBG_BPT_INNER_ENABLED,   /* Aktivní BPT uvnitř instrukce (ne na počátku) */
    DBG_BPT_INNER_DISABLED,  /* Deaktivovaný BPT uvnitř instrukce */
} en_DBG_BPT_ROW_STATE;

/**
 * @brief Per-instance stav sekce Disassembled.
 *
 * Zapouzdřuje veškerý stav, který byl dříve file-static. Hlavní singleton
 * instance ("main") sdílí UI stav (focus_addr, selected_row, ...) s
 * globální g_dbg_ui přes pointery shared_focus_addr atd. - to zachovává
 * zpětnou kompatibilitu s konzumenty zvenku, kteří dnes čtou g_dbg_ui
 * přímo.
 *
 * Sekundární instance (plán č. 7) by měly přepnout pointery na
 * local_state - tím získají nezávislý UI stav. Zatím v tomto refaktoru
 * není exposed (nikdo sekundární instanci nevytvoří přes public API).
 *
 * Ownership: ručně alokovaná struktura, vlastníkem je volající
 * dbg_disasm_view_create(). Singleton "main" se vytvoří lazy v
 * dbg_disassembled_render() a žije do ukončení procesu.
 *
 * Invarianty:
 * - shared_focus_addr, shared_selected_row, shared_selected_addr,
 *   shared_visible_rows, shared_history_split_ratio nesmí být NULL
 *   po dokončení create().
 * - current_dasm_buf a last_dasm_buf vždy ukazují na různé prvky
 *   dasm_buf[2] (po každém disassembly cyklu se prohazují).
 */
struct DisassembledView
{
    /* --- Identita instance --- */
    const char *window_id;     /**< stabilní ID ("main", "2", ...) - vlastněno volajícím */
    bool enable_history;       /**< true = renderovat horní tabulku + splitter */

    /* --- Per-instance dynamic flags (uživatelsky přepínatelné, persistované) --- */
    bool follow_pc;            /**< true = auto-focus na PC při běhu emulace.
                                    Per-instance, default ON pro "main", OFF pro
                                    sekundární. Uživatel přepíná v hlavičce view
                                    a hodnota se persistuje přes cfgmain. */
    bool show_tstates;         /**< true = renderovat 5. sloupec T-states v dolní
                                    disasm tabulce. Per-instance, default OFF. */

    /* --- Sdílený UI stav (pointery) ---
     * Pro hlavní instanci ("main") míří na pole g_dbg_ui, čímž se zachovává
     * zpětná kompatibilita s konzumenty čtoucími g_dbg_ui přímo
     * (debugger_window.cpp, dbg_focus_to.cpp atd.). Pro sekundární
     * instance budou mířit na local_state. */
    uint16_t *shared_focus_addr;
    int *shared_selected_row;
    uint16_t *shared_selected_addr;
    int *shared_visible_rows;
    float *shared_history_split_ratio;

    /* --- Lokální storage pro sekundární instance ---
     * Pro hlavní instanci se nepoužívá. Pro sekundární instance jím
     * naplní pointery shared_*. */
    struct
    {
        uint16_t focus_addr;
        int selected_row;
        uint16_t selected_addr;
        int visible_rows;
        float history_split_ratio;
    } local_state;

    /* --- Buffery pro disassembly + detekci self-modifying kódu ---
     * Buffer paměti kterou chceme disassemblovat a buffer pro poslední
     * disassemblovanou paměť. Pointery se prohazují po každém disassembly. */
    st_DASM_BUFFER dasm_buf[2];
    st_DASM_BUFFER *current_dasm_buf;  /**< buffer pro aktuální čtení */
    st_DASM_BUFFER *last_dasm_buf;     /**< cache obsahu z předchozího framu */
    int last_disassembled_rows;        /**< počet řádků z posledního disassembly (detekce změny velikosti okna) */

    /* --- Cache disassemblovaných řádků dolní tabulky --- */
    DasmRow cached_rows[DBG_DASM_MAX_VISIBLE_ROWS];
    int cached_row_lengths[DBG_DASM_MAX_VISIBLE_ROWS];

    /* --- Cache historie instrukcí (horní tabulka) ---
     * hist_cache_valid zajišťuje zpracování při prvním volání (kdy by
     * memcmp dvou nulových struktur vrátil 0 = "beze změny"). */
    st_DEBUGGER_HISTORY last_history;             /**< poslední cachovaný stav historie */
    DasmRow hist_cached_rows[DEBUGGER_HISTORY_LENGTH];
    int hist_cached_count;                        /**< počet řádků v cache */
    bool hist_cache_valid;                        /**< false = cache ještě nebyla naplněna */

    /* --- Cache stavu breakpointů pro řádky dolní tabulky ---
     * Cache se invaliduje při změně g_breakpoints.version nebo při refreshi
     * (nové řádky = nové adresy k prověření). */
    en_DBG_BPT_ROW_STATE bpt_row_state[DBG_DASM_MAX_VISIBLE_ROWS];
    unsigned cached_bpt_version;

    /* --- Stav rendreru --- */
    bool initial_ratio_set;   /**< split_ratio byl při prvním otevření vypočítán */

    /* --- Per-instance hlavička disasm view (text entry) ---
     * Buffer pro InputText v hlavičce. Užitečné jen do dokončení edit cyklu
     * (Enter = parse + jump + clear). Pole nesmí být lokální static, jinak
     * by se více instancí navzájem přepisovalo. */
    char header_input_buf[64];
    bool header_input_error;  /**< true = poslední Enter selhal (parse/lookup), zobrazit chybu */

    /* --- Tooltip snapshot ---
     * Při běhu emulátoru se paměť mění (system area, video RAM, IO regions);
     * cache cached_rows[] se rebuilduje každých DBG_REFRESH_INTERVAL_MS, takže
     * tooltip ukazující &rows[i] by každých 100 ms blikal s novými daty.
     * Snapshot kopie zachytí řádek při prvním zobrazení tooltipu a drží ho
     * stabilní dokud myš nezmění hovered řádek. Reset přes tooltip_snapshot_row
     * = -1. */
    int tooltip_snapshot_row;       /**< index řádku v rows[] který je zachycen, nebo -1 */
    DasmRow tooltip_snapshot;       /**< zachycená kopie pro stabilní tooltip render */

    /* --- Per-instance pixel state pro branch arrows ---
     *
     * Při renderu dolní disasm tabulky se zachycují pixel pozice řádků
     * sloupce ICONS, které potom slouží render_branch_arrows() volaného
     * po EndTable() ke kreslení šipek. Per-instance storage je nutné, aby
     * sekundární okna (plán č. 7) nepřepisovala stav hlavnímu oknu - jejich
     * render se sice provádí sekvenčně v rámci jednoho framu, ale arrows
     * pro instanci A se kreslí po EndTable instance A a v té chvíli musí
     * obsahovat pozice z instance A, ne ze sousední instance B vykreslené
     * předtím.
     */
    float arrow_row_screen_y[DBG_DASM_MAX_VISIBLE_ROWS];  /**< Y středu řádku (px) */
    float arrow_row_height;                               /**< Výška řádku (px), naplní 1. řádek */
    float arrow_icons_x;                                  /**< Levá hrana sloupce ICONS (px) */
    float arrow_table_min_y;                              /**< Horní okraj 1. řádku (px) */
    float arrow_table_max_y;                              /**< Spodní okraj posledního řádku (px) */

    /* --- Slider drag state pro Follow PC bypass ---
     * Pokud uživatel drží LMB na slideru a follow_pc=ON s běžícím emu,
     * follow_pc update by okamžitě přepsal focus_addr na PC = uživatel
     * by nemohl nikam "nakouknout". Bypass: po dobu držení slideru
     * vynecháme follow_pc update. Po release se vrací normální chování.
     * Hodnota se nastavuje per-frame po VSliderInt přes IsItemActive(). */
    bool slider_held;
};


/* ===========================================================================
 * SINGLETON HLAVNÍ INSTANCE
 * ===========================================================================
 *
 * Hlavní instance "main" se vytvoří lazy při prvním volání shim funkce
 * dbg_disassembled_render() a žije do ukončení procesu. Není explicitně
 * destruována - OS uvolní paměť při exit.
 *
 * Sekundární instance (plán č. 7) by si vytvořily vlastní handle přes
 * dbg_disasm_view_create() a musely by samy volat dbg_disasm_view_destroy().
 */
static DisassembledView *s_main_view = NULL;

/* Persistované per-instance dynamic flagy hlavní instance.
 * Klíče v cfgmain modulu DEBUGGER:
 *   - disasm_main_follow_pc (default 1, hlavní okno follow PC nesmí UI vypnout)
 *   - disasm_main_show_tstates (default 0)
 *
 * Storage je global static - cfgmain bind targets. Při lazy-create main
 * view v dbg_disassembled_render() se aplikují přes setters; v každém framu
 * se naopak aktuální hodnoty z view čtou zpět pro shutdown save.
 */
static unsigned s_main_persisted_follow_pc = 1;
static unsigned s_main_persisted_show_tstates = 0;
static bool s_main_persisted_applied = false;


/* ===========================================================================
 * POMOCNÉ FUNKCE - state-less helpery (instance-independent)
 * =========================================================================== */


/*
 * read_mapped_memory_segment — přečte blok paměti emulátoru do připraveného bufferu
 *
 * Přečte `size` bajtů z adresního prostoru Z80 počínaje adresou `addr`.
 * Paměť se čte přes debugger_dasm_read_cb() (s nastaveným memop_call flagem,
 * takže čtení nemá vedlejší efekty na stav emulátoru).
 *
 * Adresní prostor Z80 je 16bitový (0x0000–0xFFFF), při přetečení
 * se adresy automaticky zalamují (wrap-around).
 *
 * Parametry:
 *   buf  — ukazatel na buffer, kam se mají bajty uložit (musí být alokován volajícím)
 *   addr — počáteční adresa v adresním prostoru Z80
 *   size — počet bajtů k přečtení
 */
static void read_mapped_memory_segment(uint8_t *buf, uint16_t addr, int size)
{
    if (size <= 0 || !buf)
        return;

    for (int i = 0; i < size; i++)
    {
        buf[i] = debugger_dasm_read_cb((uint16_t)(addr + i), NULL);
    };
}


/*
 * Šířky sloupců tabulek.
 *
 * Obě tabulky (historie i disassembly) mají 4 sloupce — všechny s pevnou šířkou:
 *   ICONS (pevná) │  ADDR (pevná)  │  BYTES (pevná) │  MNEMONIC (pevná)
 *
 * Dolní tabulka (disassembly):
 *   ICONS (pevná) │  ADDR (pevná)  │  BYTES (pevná) │  MNEMONIC (pevná)
 *   ●►/→/prázdné  │  "E6CA"        │  "CD DA E6   " │  "call #e6da"
 *
 * Sloupec ICONS (COL_ICONS_WIDTH, 20px) obsahuje obě ikony (BPT + PC)
 * kreslené přes ImDrawList s překryvem — breakpoint (červený kruh) se
 * kreslí vlevo, PC indikátor (trojúhelník/šipka) vpravo.
 *
 * Textové sloupce ADDR, BYTES, MNEM mají šířku vypočítanou za běhu
 * podle velikosti aktuálně zvoleného fontu:
 *   char_w = šířka jednoho znaku (ImGui::CalcTextSize("0").x)
 *   ADDR:  (5 + 0) × char_w   — 4 hex + ':' + 0 separátor
 *   BYTES: (12 + 0) × char_w  — 12 znaků max + 0 separátor
 *   MNEM:  (15 + 0) × char_w  — 15 znaků + 0 separátor
 *
 */
/*
 * Šířky pro ICONS sloupec.
 *
 * Gutter má dvě části:
 *   - Levá (ARROWS_WIDTH px): prostor pro vizualizaci skoků (branch arrows
 *     kreslené po EndTable přes ImDrawList). Pokud je toggle "Show branch
 *     arrows" off, prostor se nevynechává a sloupec smrskne na zbývající
 *     šířku BPT/PC ikon.
 *   - Pravá (zbytek): BPT ikona (centrum kolem +6 px od leve strany pravé
 *     části) + PC indikátor (centrum kolem +14 px).
 *
 * COL_ICONS_BASE_WIDTH = 20 px = původní šířka sloupce v dobách kdy obsahoval
 * jen BPT/PC ikony. Při zapnutých arrows se šířka rozšíří o ARROWS_WIDTH px.
 */
static const float COL_ICONS_BASE_WIDTH = 20.0f;
static const float COL_ICONS_ARROWS_WIDTH = 20.0f;

/**
 * @brief Aktuální šířka ICONS sloupce v px.
 *
 * @return COL_ICONS_BASE_WIDTH + COL_ICONS_ARROWS_WIDTH pokud je zapnutá
 *         vizualizace skoků (g_debugger.disasm_show_branch_arrows != 0),
 *         jinak COL_ICONS_BASE_WIDTH.
 */
static inline float icons_col_width(void)
{
    return (g_debugger.disasm_show_branch_arrows != 0)
               ? (COL_ICONS_BASE_WIDTH + COL_ICONS_ARROWS_WIDTH)
               : COL_ICONS_BASE_WIDTH;
}

/**
 * @brief Offset levé hrany pravé části ICONS sloupce (= prostor pro BPT/PC ikony).
 *
 * Pokud je vizualizace skoků zapnutá, BPT/PC ikony se kreslí posunuté doprava
 * o COL_ICONS_ARROWS_WIDTH. Vlevo zůstává prostor pro arrow shaft.
 */
static inline float icons_bpt_pc_xoffset(void)
{
    return (g_debugger.disasm_show_branch_arrows != 0)
               ? COL_ICONS_ARROWS_WIDTH
               : 0.0f;
}

/* Zachováno pro zpětnou kompatibilitu se starými místy v souboru,
 * která ještě používaly COL_ICONS_WIDTH jako konstantu. Po refaktoru
 * arrows-aware šířky se odkazujte na icons_col_width(). */
#define COL_ICONS_WIDTH (icons_col_width())

/*
 * Počty znaků (obsah + separátor) pro textové sloupce dolní tabulky.
 * Používají se jak pro výpočet šířky sloupců, tak pro výpočet
 * celkové šířky sekce (shrink-to-content).
 */
#define DASM_ADDR_CHARS (5 + 0)   /* 4 hex + ':' + 0 separátor */
#define DASM_BYTES_CHARS (12 + 0) /* 12 znaků + 0 separátor */
#define DASM_MNEM_CHARS (15 + 0)  /* 15 znaků + 0 separátor */
#define DASM_TSTATES_CHARS (5 + 0) /* "21/16" + 0 separátor (max je 23 pro EX (SP),HL apod.;
                                      formát "TT/TT" = 5 znaků; pro non-branch jen "TT" = 2 znaky) */

/* Minimální výška pro jednu tabulku (historie nebo disassembly) */
static const float MIN_TABLE_HEIGHT = 60.0f;

/* Výška splitteru (posuvníku mezi tabulkami) */
static const float SPLITTER_HEIGHT = 6.0f;

/* Šířka vertikálního šoupátka (slideru) pro nastavení adresy focusu */
static const float SLIDER_WIDTH = 20.0f;

/* =========================================================================
 * MOST sym_db ↔ z80_symtab
 * =========================================================================
 *
 * Bridge cache (s_dasm_symtab + version + sync) byla extrahována ve fázi
 * F3 (disassembler-window-v1) do samostatného modulu
 * @c ui-imgui/debugger/symbols/symdb_bridge.{c,h}. Důvodem byl druhý
 * konzument - samostatné Disassembler V1 okno - které potřebovalo
 * stejnou cestu, ale bez závislosti na této sekci.
 *
 * Lokální symboly @c sync_dasm_symtab() a @c get_dasm_symtab() jsou
 * zachovány jako tenký proxy nad bridge API, aby všechna existující
 * call sites v této sekci zůstaly beze změny (= refactor bez
 * sémantické změny).
 *
 * Threading: jen UI vlákno (disasm render). Žádný přístup z EMU vlákna.
 */

/**
 * @brief Proxy: deleguje na @ref symdb_bridge_get_symtab.
 *
 * Zachovaný název pro kompatibilitu s call sites v této sekci.
 * Bridge sám interně synchronizuje s sym_db verzí, takže explicit
 * "sync" už není potřeba - funkce zde slouží jen jako čitelnostní
 * lokátor v existujícím kódu.
 */
static void sync_dasm_symtab(void)
{
    (void)symdb_bridge_get_symtab();
}


/**
 * @brief Proxy: vrací cached symtab z bridge modulu (může být NULL).
 *
 * @return symtab handle nebo NULL pokud bridge alokace selhala.
 */
static const z80_symtab_t *get_dasm_symtab(void)
{
    return symdb_bridge_get_symtab();
}


/*
 * make_default_dasm_format — vrací default format kompatibilní se starým
 * z80ex_dasm() výstupem (lowercase mnemoniky, hex jako #XXXX, JR/JP s
 * absolutní adresou).
 */
static z80_dasm_format_t make_default_dasm_format(void)
{
    z80_dasm_format_t fmt;
    z80_dasm_format_default(&fmt);
    fmt.uppercase = 0; /* zachovat lowercase styl staré tabulky */
    return fmt;
}


/*
 * Buffer-aware read callback pro disasm scan.
 *
 * Při periodickém cache rebuild se nejprve do bufferu načte snapshot
 * paměti pod focus_addr (read_mapped_memory_segment). Disasm scan pak
 * dekóduje instrukce z TĚCHTO snapshot bajtů (= konzistentní s tím, co
 * se v tabulce zobrazí), nikoliv ze živé paměti, která se může mezi
 * tickem cache rebuild a zobrazením tabulky změnit (banking switch
 * z emu vlákna). Mimo rozsah snapshotu fallback na živé čtení paměti
 * (= edge case kdy poslední instrukce přesahuje konec bufferu).
 */
typedef struct
{
    const uint8_t *buf;     /**< snapshot bajtů */
    uint16_t buf_start;     /**< adresa, na které začíná buf[0] */
    int buf_size;           /**< počet bajtů v buf */
} DasmBufferCtx;

static uint8_t dasm_buffer_read_cb(uint16_t addr, void *user_data)
{
    const DasmBufferCtx *ctx = (const DasmBufferCtx *)user_data;
    /* Wrap-around 16-bit: buf_start může být blízko 0xFFFF a buf přesáhne
     * do 0x0000. Spočítáme offset s přetečením. */
    uint16_t offset = (uint16_t)(addr - ctx->buf_start);
    if (offset < (uint16_t)ctx->buf_size)
        return ctx->buf[offset];
    /* Mimo snapshot - fallback na živou paměť. Statisticky vzácné
     * (vyskytne se jen pokud poslední řádek tabulky disassembluje
     * instrukci, která svojí délkou přesáhne konec snapshotu). */
    return debugger_dasm_read_cb(addr, NULL);
}


/*
 * disassemble_at — disassembluje jednu instrukci na zadané adrese.
 *
 * Pokud je předán buffer kontext (ctx != NULL), čte se z bufferu (= disasm
 * konzistentní se snapshot paměti zachyceným v cache rebuild). Jinak fallback
 * na živé čtení paměti přes debugger_dasm_read_cb (no-context cesta používaná
 * stateless API jako dbg_dasm_get_line).
 *
 * Mnemonika se formátuje přes z80_dasm_to_str_sym() s aktuálně synchronizovaným
 * symtab, takže branch / direct-mem operandy obsahují symbolické názvy
 * (pokud jsou v sym_db). Pro adresy bez symbolu výstup zůstává hex (#XXXX).
 *
 * Parametry:
 *   addr     — adresa první instrukce
 *   out_row  — výstupní struktura DasmRow (vyplní addr, bytes, num_bytes,
 *              mnemonic a inst)
 *   ctx      — buffer kontext (NULL = fallback na živou paměť)
 *
 * Návratová hodnota: délka instrukce v bajtech (1–4)
 */
static int disassemble_at(uint16_t addr, DasmRow *out_row, const DasmBufferCtx *ctx)
{
    out_row->addr = addr;

    /* Strukturovaná disassemblace - vyplní out_row->inst kompletně.
     * Buffer-aware cesta zaručí konzistenci se snapshot zobrazeným
     * v tabulce (eliminuje race UI vs emu vlákno během dasm scanu). */
    int len;
    if (ctx)
        len = z80_dasm(&out_row->inst, dasm_buffer_read_cb, (void *)ctx, addr);
    else
        len = z80_dasm(&out_row->inst, debugger_dasm_read_cb, NULL, addr);

    /* Formátování mnemoniky se symbol substitucí (no-op pokud symtab NULL
     * nebo žádný symbol pro target). */
    z80_dasm_format_t fmt = make_default_dasm_format();
    z80_dasm_to_str_sym(out_row->mnemonic, sizeof(out_row->mnemonic),
                        &out_row->inst, &fmt, get_dasm_symtab());

    out_row->num_bytes = (len > 4) ? 4 : len;
    for (int i = 0; i < out_row->num_bytes; i++)
    {
        out_row->bytes[i] = out_row->inst.bytes[i];
    };

    return len;
}

/*
 * format_bytes_fixed — formátuje bytekódy instrukce s pevnou šířkou
 *
 * Výstup má VŽDY konstantní šířku 11 znaků ("XX XX XX XX"),
 * aby sloupce v tabulce byly zarovnané. Nepoužité pozice se vyplní mezerami.
 *
 * Příklady:
 *   1 bajt:  "CD         "
 *   2 bajty: "ED B2      "
 *   3 bajty: "CD DA E6   "
 *   4 bajty: "DD CB 10 06"
 *
 * Parametry:
 *   bytes    — pole bajtů instrukce
 *   max_bytes — maximální počet bajtů k zobrazení (typicky 4)
 *   buf      — výstupní buffer (min 12 bajtů)
 *   buf_size — velikost bufferu
 */
static void format_bytes_fixed(const uint8_t *bytes, int max_bytes, char *buf, int buf_size)
{
    int pos = 0;
    /* num_bytes je autoritativní z disasm (z80_dasm_inst_t::length nebo
     * historie ring buffer). Žádná heuristika "trailing-zeros = end" -
     * platná instrukce může mít legitimní 0x00 bajty (např. LD BC,#0000
     * = 01 00 00). */
    int actual_bytes = max_bytes;
    if (actual_bytes < 0) actual_bytes = 0;
    if (actual_bytes > 4) actual_bytes = 4;

    /* Formátování bajtů */
    for (int i = 0; i < actual_bytes && pos < buf_size - 3; i++)
    {
        if (i > 0)
            buf[pos++] = ' ';
        pos += snprintf(buf + pos, buf_size - pos, "%02X", bytes[i]);
    };

    /* Doplnění mezerami na pevnou šířku 11 znaků ("XX XX XX XX") */
    while (pos < 11 && pos < buf_size - 1)
    {
        buf[pos++] = ' ';
    };
    buf[pos] = '\0';
}

/*
 * format_row_bytes_fixed — wrapper pro DasmRow strukturu
 *
 * Volá format_bytes_fixed() s daty z DasmRow.
 */
static void format_row_bytes_fixed(const DasmRow *row, char *buf, int buf_size)
{
    format_bytes_fixed(row->bytes, row->num_bytes, buf, buf_size);
}


/**
 * @brief Pocet bajtu, ktere dany operand typ konzumuje za opcode.
 *
 * Slouzi pro per-byte coloring v BYTES sloupci - rozdeleni na opcode bajty
 * (= delkove ridici bajty instrukce vc. prefixu) a operand bajty (= immediate
 * hodnoty, displacementy).
 *
 * Pro typy ktere jsou kodovane primo v opcode (REG8, REG16, MEM_REG16,
 * CONDITION, BIT_INDEX, RST_VEC) vraci 0 - operandova hodnota neobsazuje
 * separatni byte.
 *
 * @param t typ operandu (z80_operand_type_t)
 * @return pocet operand bajtu (0, 1 nebo 2)
 */
static int operand_byte_count(z80_operand_type_t t)
{
    switch (t)
    {
    case Z80_OP_IMM8:
    case Z80_OP_MEM_IMM8:
    case Z80_OP_REL8:
    case Z80_OP_MEM_IX_D:
    case Z80_OP_MEM_IY_D:
        return 1;
    case Z80_OP_IMM16:
    case Z80_OP_MEM_IMM16:
        return 2;
    default:
        return 0;
    };
}


/**
 * @brief Vykresli BYTES sloupec s 2-tone barvenim (opcode vs operand).
 *
 * Opcode bajty (= prvni cast instrukce vc. pripadnych prefixu CB/DD/ED/FD)
 * jsou vykresleny default barvou textu. Operand bajty (= zaverna immediate
 * hodnota / displacement / target adresa) jsou vykresleny cyan.
 *
 * Edge case DDCB/FDCB (ddcb d op): displacement v bytes[2] uprostred
 * sekvence - pro jednoduchost kreslime celou DDCB/FDCB instrukci default
 * barvou (= 4 bytes, vsechno opcode/prefix). Tato kategorizace je
 * priblizna ale staci pro vetsinu beznych instrukci.
 *
 * Vystup ma pevnou sirku 11 znaku ("XX XX XX XX") - chybejici bajty
 * jsou nahrazeny prazdnymi mezerami pres SameLine + Text dummy.
 *
 * @param row radek disasmu (zejmena bytes, num_bytes, inst.op1/op2.type)
 */
static void render_bytes_column(const DasmRow *row)
{
    int n = row->num_bytes;
    if (n < 0) n = 0;
    if (n > 4) n = 4;

    /* Detekce DDCB/FDCB sekvence - displacement uprostred, pro jednoduchost
     * vsechno default barvou. */
    bool is_ddcb = (n == 4 &&
                    (row->bytes[0] == 0xDD || row->bytes[0] == 0xFD) &&
                    row->bytes[1] == 0xCB);

    int operand_bytes = 0;
    if (!is_ddcb)
    {
        operand_bytes = operand_byte_count(row->inst.op1.type)
                      + operand_byte_count(row->inst.op2.type);
        if (operand_bytes > n) operand_bytes = n;
    };
    int opcode_bytes = n - operand_bytes;

    const ImVec4 col_operand = ImVec4(0.4f, 0.9f, 1.0f, 1.0f);  /* svetly cyan */
    const ImVec4 col_default = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    /* ImGui defaultne dela mezery mezi SameLine() volanimi (ItemSpacing.x).
     * Pro byte string chceme jen jednu mezeru mezi bajty. Manualne prepneme. */
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec2 saved_spacing = style.ItemSpacing;
    style.ItemSpacing.x = 0.0f;

    int char_count = 0;
    for (int i = 0; i < n; i++)
    {
        if (i > 0)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted(" ");
            ImGui::SameLine();
            char_count += 1;
        };
        bool is_operand = (i >= opcode_bytes);
        ImVec4 c = is_operand ? col_operand : col_default;
        ImGui::TextColored(c, "%02X", row->bytes[i]);
        char_count += 2;
    };

    /* Doplneni do pevne sirky 11 znaku ("XX XX XX XX") - pripojime mezery. */
    while (char_count < 11)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(" ");
        char_count++;
    };

    style.ItemSpacing = saved_spacing;
}


/**
 * @brief Kategorie Z80 instrukce pro barevne kodovani v MNEM sloupci.
 *
 * Pouziva se pro lookup barvy podle prvni mnemoniky (= prvni slovo
 * formatovane mnemoniky pred prvni mezerou). Mnemonika se v disasm radku
 * vykresluje barevne podle kategorie, zbytek (operandy) zustava default.
 */
typedef enum
{
    MNEM_CAT_DEFAULT = 0, /**< neutralni - LD, EX, EXX, ostatni nezarazene */
    MNEM_CAT_FLOW,        /**< rizeni toku: jp, jr, call, ret, rst, djnz, halt */
    MNEM_CAT_STACK,       /**< zasobnik: push, pop */
    MNEM_CAT_BLOCK,       /**< blokove instrukce: ldi, ldd, cpi, cpd, ... */
    MNEM_CAT_IO,          /**< I/O: in, out, ini, ind, outi, ... */
    MNEM_CAT_ARITH,       /**< aritmetika a logika: add, sub, cp, and, or, ... */
    MNEM_CAT_BIT,         /**< bitove operace a rotace: bit, set, res, rlc, ... */
    MNEM_CAT_CTRL,        /**< CPU control: nop, di, ei, im */
} en_MNEM_CATEGORY;


/**
 * @brief Lookup kategorie podle prvniho slova mnemoniky.
 *
 * Vstup je pointer na prvni znak mnemoniky (= z DasmRow.mnemonic). Funkce
 * porovnava case-insensitive prefix do prvni mezery / konce stringu.
 * Slozitost je linearni v poctu kategorickych pravidel (~50), call site
 * je hot path (= per radek per frame), ale pocet srovnani je maly.
 *
 * @param mnem null-terminovany retezec, prvni slovo je mnemonika
 * @return en_MNEM_CATEGORY hodnota
 */
static en_MNEM_CATEGORY mnemonic_category(const char *mnem)
{
    if (!mnem || !mnem[0]) return MNEM_CAT_DEFAULT;

    /* Skopirujeme prvni slovo do mensiho bufferu (max 8 znaku) a normalizujeme
     * na lowercase pro stable porovnani. */
    char w[8];
    int wlen = 0;
    for (int i = 0; i < 7 && mnem[i] && mnem[i] != ' ' && mnem[i] != '\t'; i++)
    {
        w[wlen++] = (char) tolower((unsigned char) mnem[i]);
    };
    w[wlen] = '\0';

    /* Definice kategorii. Vyhledavame podle strcmp - pro 50 mnemonik
     * a per-radek volani je linearni search OK. */
    static const struct { const char *m; en_MNEM_CATEGORY c; } table[] = {
        /* Flow */
        {"jp",   MNEM_CAT_FLOW},  {"jr",   MNEM_CAT_FLOW},
        {"call", MNEM_CAT_FLOW},  {"ret",  MNEM_CAT_FLOW},
        {"reti", MNEM_CAT_FLOW},  {"retn", MNEM_CAT_FLOW},
        {"rst",  MNEM_CAT_FLOW},  {"djnz", MNEM_CAT_FLOW},
        {"halt", MNEM_CAT_FLOW},
        /* Stack */
        {"push", MNEM_CAT_STACK}, {"pop",  MNEM_CAT_STACK},
        /* Block */
        {"ldi",  MNEM_CAT_BLOCK}, {"ldd",  MNEM_CAT_BLOCK},
        {"ldir", MNEM_CAT_BLOCK}, {"lddr", MNEM_CAT_BLOCK},
        {"cpi",  MNEM_CAT_BLOCK}, {"cpd",  MNEM_CAT_BLOCK},
        {"cpir", MNEM_CAT_BLOCK}, {"cpdr", MNEM_CAT_BLOCK},
        /* I/O */
        {"in",   MNEM_CAT_IO},    {"out",  MNEM_CAT_IO},
        {"ini",  MNEM_CAT_IO},    {"ind",  MNEM_CAT_IO},
        {"inir", MNEM_CAT_IO},    {"indr", MNEM_CAT_IO},
        {"outi", MNEM_CAT_IO},    {"outd", MNEM_CAT_IO},
        {"otir", MNEM_CAT_IO},    {"otdr", MNEM_CAT_IO},
        /* Arith / logic */
        {"add",  MNEM_CAT_ARITH}, {"adc",  MNEM_CAT_ARITH},
        {"sub",  MNEM_CAT_ARITH}, {"sbc",  MNEM_CAT_ARITH},
        {"inc",  MNEM_CAT_ARITH}, {"dec",  MNEM_CAT_ARITH},
        {"neg",  MNEM_CAT_ARITH}, {"cp",   MNEM_CAT_ARITH},
        {"daa",  MNEM_CAT_ARITH}, {"and",  MNEM_CAT_ARITH},
        {"or",   MNEM_CAT_ARITH}, {"xor",  MNEM_CAT_ARITH},
        {"cpl",  MNEM_CAT_ARITH}, {"scf",  MNEM_CAT_ARITH},
        {"ccf",  MNEM_CAT_ARITH},
        /* Bit / shift / rotate */
        {"bit",  MNEM_CAT_BIT},   {"set",  MNEM_CAT_BIT},
        {"res",  MNEM_CAT_BIT},   {"rlc",  MNEM_CAT_BIT},
        {"rl",   MNEM_CAT_BIT},   {"rrc",  MNEM_CAT_BIT},
        {"rr",   MNEM_CAT_BIT},   {"sla",  MNEM_CAT_BIT},
        {"sra",  MNEM_CAT_BIT},   {"sll",  MNEM_CAT_BIT},
        {"srl",  MNEM_CAT_BIT},   {"rld",  MNEM_CAT_BIT},
        {"rrd",  MNEM_CAT_BIT},   {"rlca", MNEM_CAT_BIT},
        {"rla",  MNEM_CAT_BIT},   {"rrca", MNEM_CAT_BIT},
        {"rra",  MNEM_CAT_BIT},
        /* CPU control */
        {"nop",  MNEM_CAT_CTRL},  {"di",   MNEM_CAT_CTRL},
        {"ei",   MNEM_CAT_CTRL},  {"im",   MNEM_CAT_CTRL},
    };
    static const size_t table_size = sizeof(table) / sizeof(table[0]);

    for (size_t k = 0; k < table_size; k++)
    {
        if (strcmp(w, table[k].m) == 0)
            return table[k].c;
    };
    return MNEM_CAT_DEFAULT;
}


/**
 * @brief Vrati barvu pro danou kategorii mnemoniky.
 */
static ImVec4 mnemonic_category_color(en_MNEM_CATEGORY cat)
{
    switch (cat)
    {
    case MNEM_CAT_FLOW:  return ImVec4(1.00f, 0.55f, 0.20f, 1.0f); /* oranzova */
    case MNEM_CAT_STACK: return ImVec4(0.55f, 0.75f, 1.00f, 1.0f); /* svetle modra */
    case MNEM_CAT_BLOCK: return ImVec4(1.00f, 0.55f, 0.85f, 1.0f); /* ruzova */
    case MNEM_CAT_IO:    return ImVec4(0.95f, 0.40f, 0.95f, 1.0f); /* magenta */
    case MNEM_CAT_ARITH: return ImVec4(1.00f, 0.95f, 0.55f, 1.0f); /* zluta */
    case MNEM_CAT_BIT:   return ImVec4(0.55f, 0.95f, 0.95f, 1.0f); /* cyan */
    case MNEM_CAT_CTRL:  return ImVec4(0.65f, 0.65f, 0.65f, 1.0f); /* svetla seda */
    case MNEM_CAT_DEFAULT:
    default:             return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    };
}


/**
 * @brief Vykresli MNEM sloupec s kategorickym barvenim mnemoniky.
 *
 * Mnemonika (= prvni slovo do prvni mezery) je vybarvena podle kategorie
 * (viz mnemonic_category()). Operandy (zbytek za prvni mezerou) jsou
 * vykresleny default barvou. Render je dvouczasti pres SameLine.
 *
 * Pokud mnemonika nema operandy (= cely string je jedno slovo, napr. "nop",
 * "halt", "di"), vykresli se jen barvene slovo.
 *
 * @param mnem null-terminovana mnemonika z DasmRow (vc. operandu)
 */
static void render_mnem_column(const char *mnem)
{
    if (!mnem || !mnem[0])
    {
        ImGui::TextUnformatted("");
        return;
    };

    /* Najdeme delku prvniho slova (= mnemoniky bez operandu). */
    int word_len = 0;
    while (mnem[word_len] && mnem[word_len] != ' ' && mnem[word_len] != '\t')
        word_len++;

    en_MNEM_CATEGORY cat = mnemonic_category(mnem);
    ImVec4 col_mnem = mnemonic_category_color(cat);
    const ImVec4 col_default = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 col_value   = ImVec4(0.4f, 0.9f, 1.0f, 1.0f); /* svetly cyan, stejne jako operand bajty */

    /* Mnemonika - kategoricky barvena. */
    if (cat == MNEM_CAT_DEFAULT)
        ImGui::TextUnformatted(mnem, mnem + word_len);
    else
        ImGui::TextColored(col_mnem, "%.*s", word_len, mnem);

    if (!mnem[word_len])
        return;

    /* Operandy: tokenizovany render. Hex literaly (#1234, $FF, 0xFF, FFh) a
     * neregisterove identifikatory (= symboly z sym_db) jsou cyan; registry,
     * podminky, zavorky a separatory zustavaji default. Tokenizace je
     * deterministicka per sekvence znaku a-z, A-Z, 0-9, '_', '#', '$' - vse
     * ostatni (`,`, `(`, `)`, mezera, `+`, `-`) jsou separatory. */

    /* Seznamy registru a podminek - rozpoznani tokenu = default barva. */
    static const char *const regs[] = {
        "a", "b", "c", "d", "e", "h", "l", "f", "i", "r",
        "bc", "de", "hl", "af", "sp", "ix", "iy",
        "ixh", "ixl", "iyh", "iyl", "af'", "bc'", "de'", "hl'",
        NULL
    };
    static const char *const conds[] = {
        "nz", "z", "nc", "c", "po", "pe", "p", "m", NULL
    };

    /* Helper lambdas (C++): test zda je c separator / token-char. */
    auto is_sep = [](char c) -> bool {
        return (c == ',' || c == '(' || c == ')' || c == ' ' || c == '\t'
                || c == '+' || c == '-');
    };

    /* ItemSpacing.x = 0 aby SameLine() nepridaval extra mezery. */
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec2 saved_spacing = style.ItemSpacing;
    style.ItemSpacing.x = 0.0f;

    int i = word_len;
    while (mnem[i])
    {
        if (is_sep(mnem[i]))
        {
            /* Separator - default barva, jeden znak najednou. */
            ImGui::SameLine(0.0f, 0.0f);
            char buf[2] = { mnem[i], '\0' };
            ImGui::TextUnformatted(buf);
            i++;
            continue;
        };

        /* Token - akumulace do prvniho separatoru nebo konce stringu. */
        int start = i;
        while (mnem[i] && !is_sep(mnem[i])) i++;
        int len = i - start;

        /* Klasifikace tokenu. Pokud zacina #/$ nebo prefix 0x = hex literal.
         * Pokud konci 'h' nebo 'H' a uvnitr jsou hex znaky = hex literal.
         * Jinak case-insensitive porovnani s regs/conds = default.
         * Vse ostatni = symbol/value = cyan. */
        bool is_value = false;
        if (mnem[start] == '#' || mnem[start] == '$')
        {
            is_value = true;
        }
        else if (len >= 2 && mnem[start] == '0'
                 && (mnem[start+1] == 'x' || mnem[start+1] == 'X'))
        {
            is_value = true;
        }
        else if (len >= 2 && (mnem[start+len-1] == 'h' || mnem[start+len-1] == 'H'))
        {
            /* Test zda predchozi znaky jsou hex digits. */
            bool all_hex = true;
            for (int k = start; k < start + len - 1; k++)
            {
                char ch = mnem[k];
                if (!((ch >= '0' && ch <= '9') ||
                      (ch >= 'a' && ch <= 'f') ||
                      (ch >= 'A' && ch <= 'F')))
                {
                    all_hex = false;
                    break;
                };
            };
            if (all_hex) is_value = true;
        }
        else
        {
            /* Pure decimal? */
            bool all_digit = (len > 0);
            for (int k = start; k < i && all_digit; k++)
            {
                if (!(mnem[k] >= '0' && mnem[k] <= '9'))
                    all_digit = false;
            };
            if (all_digit)
            {
                is_value = true;
            }
            else
            {
                /* Identifikator - test proti registrum a podminkam.
                 * Pokud neni v seznamu = symbol = value barva. */
                char tok_lower[16];
                int tlen = (len < 15) ? len : 15;
                for (int k = 0; k < tlen; k++)
                    tok_lower[k] = (char) tolower((unsigned char) mnem[start + k]);
                tok_lower[tlen] = '\0';

                bool found = false;
                for (int k = 0; regs[k] && !found; k++)
                    if (strcmp(tok_lower, regs[k]) == 0) found = true;
                for (int k = 0; conds[k] && !found; k++)
                    if (strcmp(tok_lower, conds[k]) == 0) found = true;

                /* Specialni: shadow registry s apostrofem (af', bc', ...)
                 * mohou skoncit pred apostrofem pokud apostrof byl v separator.
                 * Neni v naszem seznamu separatoru, takze cely token vc.
                 * apostrofu se zachyti. Vyse uz mame v regs[]. */

                if (!found)
                    is_value = true;
            };
        };

        ImVec4 c = is_value ? col_value : col_default;
        ImGui::SameLine(0.0f, 0.0f);
        if (is_value)
            ImGui::TextColored(c, "%.*s", len, mnem + start);
        else
            ImGui::TextUnformatted(mnem + start, mnem + start + len);
    };

    style.ItemSpacing = saved_spacing;
}


/* ===========================================================================
 * INSTANCE-SPECIFIC HELPERY
 * =========================================================================== */


/**
 * @brief Přepočítá breakpoint cache pro všechny viditelné řádky dolní tabulky.
 *
 * Pro každý řádek:
 * 1. Zkontroluje adresu počátku řádku (breakpoints_find_by_addr)
 * 2. Pokud na počátku BPT není, prohledá bajty uvnitř instrukce (addr+1..addr+len-1)
 * 3. U nalezených BPT rozhodne stav: effectively enabled / disabled
 *
 * Priorita:
 * - BPT na start adrese má vždy přednost před BPT uvnitř instrukce
 * - Pokud je uvnitř instrukce více BPT, použije se první (dle adresy)
 *
 * @param self        instance (cache cíl)
 * @param rows        pole disassemblovaných řádků
 * @param row_lengths délky instrukcí (paralelní pole k rows)
 * @param num_rows    počet řádků k prověření
 *
 * Side effects: aktualizuje self->bpt_row_state[] a self->cached_bpt_version.
 */
static void dbg_bpt_cache_rebuild(DisassembledView *self,
                                   DasmRow *rows, int *row_lengths, int num_rows)
{
    for (int i = 0; i < num_rows; i++)
    {
        self->bpt_row_state[i] = DBG_BPT_NONE;

        /* 1. Zkontrolovat počáteční adresu řádku */
        st_BPT *bpt = breakpoints_find_by_addr(rows[i].addr);
        if (bpt)
        {
            self->bpt_row_state[i] = breakpoints_is_effectively_enabled(bpt->id)
                                         ? DBG_BPT_START_ENABLED
                                         : DBG_BPT_START_DISABLED;
            continue;
        };

        /* 2. Prohledat bajty uvnitř instrukce (addr+1 .. addr+len-1) */
        for (int j = 1; j < row_lengths[i]; j++)
        {
            uint16_t inner_addr = (uint16_t)(rows[i].addr + j);
            st_BPT *inner_bpt = breakpoints_find_by_addr(inner_addr);
            if (inner_bpt)
            {
                self->bpt_row_state[i] = breakpoints_is_effectively_enabled(inner_bpt->id)
                                             ? DBG_BPT_INNER_ENABLED
                                             : DBG_BPT_INNER_DISABLED;
                break; /* první nalezený má prioritu */
            };
        };
    };

    self->cached_bpt_version = g_breakpoints.version;
}


/**
 * @brief Načte a disassembluje historii instrukcí s cache.
 *
 * Porovná aktuální stav g_debugger_history s posledním cachovaným stavem
 * (self->last_history). Pokud se nezměnil, přeskočí disassembly a ponechá
 * data v self->hist_cached_rows / self->hist_cached_count.
 *
 * Pokud se historie změnila, projde ring buffer od nejstaršího záznamu
 * k nejnovějšímu a disassembluje všechny přes z80ex_dasm()
 * s callback debugger_dasm_history_read_cb().
 *
 * @param self    instance (cache cíl)
 * @param dbghist aktuální stav historie (kopie g_debugger_history)
 *
 * Side effects: aktualizuje self->hist_cached_rows[], self->hist_cached_count,
 * self->last_history, self->hist_cache_valid.
 */
static void read_debugger_history(DisassembledView *self, st_DEBUGGER_HISTORY dbghist)
{
    /*
     * Porovnáme s posledním cachovaným stavem.
     * Pokud cache ještě nebyla naplněna (hist_cache_valid == false),
     * vždy provedeme disassembly — i když je historie nulová (zobrazí se NOP).
     */
    if (self->hist_cache_valid &&
        memcmp(&dbghist, &self->last_history, sizeof(st_DEBUGGER_HISTORY)) == 0)
        return; /* Beze změny — cache je platná */

    /* Historie se změnila (nebo první volání) — provedeme disassembly */
    self->hist_cached_count = 0;
    unsigned pos = dbghist.position;

    /*
     * Pořadí v tabulce: nejstarší nahoře, nejnovější (= naposledy vykonaná
     * instrukce) DOLE. Auto-scroll v render_history_table() drží spodek
     * tabulky viditelný, takže uživatel musí na posledním řádku vidět
     * právě dokončenou instrukci.
     *
     * dbghist.position po zápisu M1 ukazuje na slot právě vykonané (nejnovější)
     * instrukce. V ring bufferu je tedy "nejstarší" slot na (pos + 1) (= ten,
     * který bude přepsán dalším M1) a "nejnovější" na (pos + LENGTH) = pos.
     *
     * Iterujeme i = 0..LENGTH-1 a vyplňujeme od nejstaršího k nejnovějšímu:
     *   i=0           → idx = (pos + 1) & POSMASK  (nejstarší)
     *   i=LENGTH-1    → idx = (pos + LENGTH) & POSMASK = pos  (nejnovější)
     *
     * Předchozí varianta používala (pos + i), čímž horní řádek byl
     * nejnovější a spodní řádek byl pos - 1 (= druhá nejnovější), což
     * vypadalo jako "o 1 instrukci pozadu" - bug oznámený 2026-05-11.
     */
    for (int i = 0; i < DEBUGGER_HISTORY_LENGTH; i++)
    {
        unsigned idx = debugger_history_position(pos + 1 + i);
        st_DEBUGGER_HISTORY_ROW *hrow = &dbghist.row[idx];

        DasmRow *out = &self->hist_cached_rows[self->hist_cached_count];
        out->addr = hrow->addr;

        /* Disassemblujeme instrukci z uložených bajtů v historii.
         * read_cb dostane hist_pos jako user_data (= index řádku v ring
         * bufferu) a interně se kombinací s bytovým offsetem dostane na
         * správný uložený byte. */
        uint8_t hist_pos = idx;
        int hlen = z80_dasm(&out->inst, debugger_dasm_history_read_cb,
                            &hist_pos, hrow->addr);

        /* Formát s aktuálně synchronizovaným symtab (bridge na sym_db) */
        z80_dasm_format_t fmt = make_default_dasm_format();
        z80_dasm_to_str_sym(out->mnemonic, sizeof(out->mnemonic),
                            &out->inst, &fmt, get_dasm_symtab());

        /* Délka je autoritativně z disassembly (= z80_dasm návratová hodnota,
         * shodná s inst.length, invariant 1-4), NIKDY heuristikou
         * "poslední nenulový bajt = konec". Ring buffer historie totiž při
         * M1 čistí jen byte[0]; byte[1..3] krátké instrukce nepřepíše a drží
         * fosilie po delší instrukci, která slot obývala dříve. Heuristika
         * by je vykreslila jako součást instrukce (a opačně by usekla
         * legitimní 0x00 operandy, např. LD BC,#0000 = 01 00 00).
         * Bajty kopírujeme z historie (= autoritativní pro reálně přečtené,
         * instr.bytes by se mohl rozejít při přepisu paměti mezi exec a
         * render), ale jen num_bytes z nich. */
        out->num_bytes = (hlen > DEBUGGER_MAX_INSTR_BYTES)
                             ? DEBUGGER_MAX_INSTR_BYTES
                             : hlen;
        for (int b = 0; b < out->num_bytes; b++)
        {
            out->bytes[b] = hrow->byte[b];
        };

        self->hist_cached_count++;
    };

    /* Uložíme aktuální stav jako referenci pro příští porovnání */
    memcpy(&self->last_history, &dbghist, sizeof(st_DEBUGGER_HISTORY));
    self->hist_cache_valid = true;
}


/* =========================================================================
 * HORNÍ TABULKA — HISTORIE
 * =========================================================================
 *
 * Zobrazuje cachované řádky historie (self->hist_cached_rows).
 * Data se připravují v read_debugger_history() před voláním této funkce.
 *
 * Formát tabulky je shodný s dolní tabulkou (4 sloupce):
 *   ICONS (prázdný) │  ADDR  │  BYTES  │  MNEMONIC
 * Sloupec ICONS je přítomen kvůli vizuálnímu zarovnání s dolní tabulkou,
 * ale v historii se ikony nevykreslují (žádné breakpointy, žádný PC indikátor).
 *
 * Po vykreslení se scrollbar automaticky nastaví na konec (nejnovější instrukce).
 */
static void render_history_table(DisassembledView *self, float height)
{
    ImGui::BeginChild("##dbg_history", ImVec2(0, height), ImGuiChildFlags_Borders);

    /* Zmenšení fontu — viz DBG_CONTENT_FONT_SIZE_OFFSET v debugger_state.h */
    {
        float base = ImGui::GetFontSize();
        float scale = (base + DBG_CONTENT_FONT_SIZE_OFFSET) / base;
        ImGui::SetWindowFontScale(scale > 0.5f ? scale : 0.5f);
    };

    /*
     * Načtení historie s cache — aktualizuje self->hist_cached_rows pokud je potřeba.
     * Čtení a porovnání historie se provádí jen když refresh kontroler rozhodl,
     * že je čas zkontrolovat data (should_refresh).
     */
    if (g_dbg_ui.refresh.should_refresh)
    {
        read_debugger_history(self, g_debugger_history);
    };

    /*
     * Výpočet šířek textových sloupců podle velikosti aktuálního fontu.
     * Shodné s dolní tabulkou — zajišťuje vizuální zarovnání.
     */
    float char_w = ImGui::CalcTextSize("0").x;
    float col_addr_w = char_w * DASM_ADDR_CHARS;
    float col_bytes_w = char_w * DASM_BYTES_CHARS;
    float col_mnem_w = char_w * DASM_MNEM_CHARS;
    float col_tstates_w = char_w * DASM_TSTATES_CHARS;

    /* Per-instance flag (nahradil globální g_debugger.disasm_show_tstates_col) */
    bool show_tstates = self->show_tstates;
    int n_columns = show_tstates ? 5 : 4;

    /*
     * ImGui tabulka se 4 nebo 5 sloupci — shodná struktura s dolní tabulkou.
     * Sloupec ICONS je přítomen pro zarovnání, ale zůstává prázdný.
     * Sloupec TSTATES (volitelný 5. sloupec) je u historických instrukcí
     * informativní - pro vykonané kondicionální větvení zobrazí stejnou
     * notaci taken/not jako dolní tabulka, kontextová informace o tom,
     * která větev se ve skutečnosti vykonala, není zachycena.
     */
    ImGuiTableFlags table_flags = ImGuiTableFlags_NoBordersInBody |
                                  ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_NoHostExtendX |
                                  ImGuiTableFlags_RowBg;

    if (ImGui::BeginTable("##hist_table", n_columns, table_flags))
    {
        ImGui::TableSetupColumn("Icons", ImGuiTableColumnFlags_WidthFixed, COL_ICONS_WIDTH);
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, col_addr_w);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, col_bytes_w);
        ImGui::TableSetupColumn("Mnem", ImGuiTableColumnFlags_WidthFixed, col_mnem_w);
        if (show_tstates)
            ImGui::TableSetupColumn("TStates", ImGuiTableColumnFlags_WidthFixed, col_tstates_w);

        for (int i = 0; i < self->hist_cached_count; i++)
        {
            char bytes_str[16];
            format_row_bytes_fixed(&self->hist_cached_rows[i], bytes_str, sizeof(bytes_str));

            ImGui::TableNextRow();

            /* Sloupec 1 (ICONS): prázdný — historie nemá ikony */
            ImGui::TableNextColumn();

            /*
             * Sloupec 2 (ADDR): adresa instrukce.
             * Stejné chování jako dolní tabulka - symbol DB lookup, zobrazí
             * jméno místo hex pokud existuje, tooltip s adresou + commentem.
             */
            ImGui::TableNextColumn();
            const st_SYMBOL *sym = sym_db_lookup_by_addr(self->hist_cached_rows[i].addr, 0);
            if (sym && sym->name)
            {
                ImGui::TextUnformatted(sym->name);
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("0x%04X", (unsigned) self->hist_cached_rows[i].addr);
                    if (sym->comment && sym->comment[0])
                    {
                        ImGui::Separator();
                        ImGui::TextUnformatted(sym->comment);
                    };
                    ImGui::EndTooltip();
                };
            }
            else
            {
                ImGui::Text("%04X:", self->hist_cached_rows[i].addr);
            };

            /* Sloupec 3 (BYTES): bytekódy instrukce */
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bytes_str);

            /* Sloupec 4 (MNEM): Z80 mnemonic */
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(self->hist_cached_rows[i].mnemonic);

            /* Sloupec 5 (TSTATES, volitelný): počet T-stavů. Pro non-branch
             * jediné číslo, pro podmíněné větvení "taken/not" notace. */
            if (show_tstates)
            {
                ImGui::TableNextColumn();
                const z80_dasm_inst_t *hi = &self->hist_cached_rows[i].inst;
                if (hi->t_states2 != 0)
                {
                    ImGui::Text("%u/%u",
                                (unsigned) hi->t_states2,
                                (unsigned) hi->t_states);
                }
                else
                {
                    ImGui::Text("%u", (unsigned) hi->t_states);
                };
            };
        };

        ImGui::EndTable();
    };

    /*
     * Auto-scroll na konec — aby byl vidět nejnovější záznam.
     * Scrollujeme jen pokud je uživatel na konci (neposunul ručně).
     */
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
    {
        ImGui::SetScrollHereY(1.0f);
    };

    ImGui::EndChild();

    /* Tooltip pro horní tabulku — popis obsahu */
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    {
        ImGui::SetTooltip("%s", _("History of already executed instructions"));
    };
}

/* =========================================================================
 * SPLITTER
 * =========================================================================
 *
 * Horizontální posuvník mezi horní a dolní tabulkou.
 * Uživatel ho táhne myší a mění poměr výšek tabulek.
 *
 * Implementace: neviditelné tlačítko s kurzorem pro resize.
 * Při tažení se přepočítává *split_ratio (= ukazatel na shared_history_split_ratio
 * instance).
 */
static bool render_splitter(float available_width, float *split_ratio, float total_height)
{
    bool changed = false;

    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Separator));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorActive));

    ImGui::Button("##dbg_splitter", ImVec2(available_width, SPLITTER_HEIGHT));

    if (ImGui::IsItemActive())
    {
        float delta = ImGui::GetIO().MouseDelta.y;
        if (delta != 0.0f)
        {
            *split_ratio += delta / total_height;
            if (*split_ratio < 0.1f)
                *split_ratio = 0.1f;
            if (*split_ratio > 0.9f)
                *split_ratio = 0.9f;
            changed = true;
        };
    };

    /* Kurzor pro vertikální resize */
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    };

    ImGui::PopStyleColor(3);

    return changed;
}

/* =========================================================================
 * SJEDNOCENÝ PER-ROW TOOLTIP - HELPER FUNKCE
 * =========================================================================
 *
 * Tooltip se skládá ze čtyř sekcí oddělených ImGui::Separator():
 *   1. Instrukční info  (header + popis + T-states + 8 flagů)
 *   2. Symbol info pro adresu řádku  (jen pokud existuje symbol)
 *   3. Symbol info pro operandy  (jen pokud target/MEM má symbol)
 *   4. Edit hints  (návod k interakci)
 *
 * Sekce 2 a 3 se vynechávají, pokud nejsou dostupná data. Tooltip volá
 * pouze čtení sym_db / z80_meta - žádný side effect na emulátor.
 */

/**
 * @brief Vykreslí instrukční sekci tooltipu (header, popis, T-states, flagy).
 *
 * Header obsahuje plnou disassemblaci řádku ve formátu
 * "AAAA: BB BB BB  mnemonika". Popis pochází z `meta->description`,
 * pokud `meta` není NULL. T-states se zobrazí jako jedna hodnota nebo
 * "N/M (taken/not taken)" pro podmíněná větvení.
 *
 * Pro flagy se preferuje detailní notace z `meta->flags[]` (osm znaků
 * S/Z/F5/H/F3/PV/N/C). Pokud `meta == NULL`, použije se fallback ze
 * surové bitové masky `inst->flags_affected` - každý dotčený flag se
 * zobrazí jako '*', netknutý jako '-'.
 *
 * @param row  řádek disassembly (adresa, bajty, mnemonika), nesmí být NULL
 * @param inst strukturovaná data instrukce (T-states, flags_affected),
 *             nesmí být NULL
 * @param meta metadata instrukce z `z80_meta_lookup()`, smí být NULL
 *             (= neznámá třída, použije se fallback)
 *
 * @pre Volat pouze uvnitř ImGui::BeginTooltip()/EndTooltip() bloku.
 */
static void render_instruction_tooltip(const DasmRow *row,
                                       const z80_dasm_inst_t *inst,
                                       const z80_meta_t *meta)
{
    /* Header: addr + bajty + mnemonika (tučně přes bold font není v tomto
       buildu jednoduše dostupné, použijeme alespoň barevně zvýrazněný řádek). */
    char bytes_str[16];
    format_row_bytes_fixed(row, bytes_str, sizeof(bytes_str));
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f),
                       "%04X: %s %s",
                       (unsigned) row->addr, bytes_str, row->mnemonic);

    /* Popis třídy instrukce (jen pokud máme meta záznam). */
    if (meta && meta->description && meta->description[0])
    {
        ImGui::TextUnformatted(meta->description);
    };

    /* T-states: jedna hodnota nebo two-branch zápis. */
    if (inst->t_states2 != 0)
    {
        ImGui::Text("%s %u/%u (%s)",
                    _("T-states:"),
                    (unsigned) inst->t_states2,
                    (unsigned) inst->t_states,
                    _("taken/not taken"));
    }
    else
    {
        ImGui::Text("%s %u", _("T-states:"), (unsigned) inst->t_states);
    };

    /*
     * Flagy: jeden řádek labelů S Z F5 H F3 P/V N C, ovlivněné flagy
     * vykreslené zelenou barvou, neovlivněné výchozí barvou.
     *
     * Detail efektu (set/reset/result/parity/...) byl odstraněn - místo
     * druhého řádku s konkrétními kódy se spoléháme na popis instrukce
     * z meta->description, který typicky efekty zmiňuje.
     */
    ImGui::TextUnformatted(_("Flags:"));
    ImGui::SameLine();

    static const char *flag_labels[Z80_META_FLAG_COUNT] = {
        "S", "Z", "F5", "H", "F3", "P/V", "N", "C"
    };

    /* Spočítáme bool affected[i] - meta preferujeme, fallback z bitmask. */
    bool affected[Z80_META_FLAG_COUNT];
    if (meta != NULL)
    {
        for (int i = 0; i < Z80_META_FLAG_COUNT; i++)
        {
            affected[i] = (meta->flags[i] != Z80_FE_UNAFFECTED);
        };
    }
    else
    {
        uint8_t fa = inst->flags_affected;
        affected[Z80_META_F_S]  = (fa & Z80_FLAG_S)  != 0;
        affected[Z80_META_F_Z]  = (fa & Z80_FLAG_Z)  != 0;
        affected[Z80_META_F_F5] = (fa & Z80_FLAG_5)  != 0;
        affected[Z80_META_F_H]  = (fa & Z80_FLAG_H)  != 0;
        affected[Z80_META_F_F3] = (fa & Z80_FLAG_3)  != 0;
        affected[Z80_META_F_PV] = (fa & Z80_FLAG_PV) != 0;
        affected[Z80_META_F_N]  = (fa & Z80_FLAG_N)  != 0;
        affected[Z80_META_F_C]  = (fa & Z80_FLAG_C)  != 0;
    };

    /* Barvy - default text vs zelená pro ovlivněné. */
    const ImVec4 col_default = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 col_affected = ImVec4(0.5f, 0.9f, 0.5f, 1.0f);

    ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerV
                           | ImGuiTableFlags_NoHostExtendX
                           | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##flags_tbl", Z80_META_FLAG_COUNT, tflags))
    {
        for (int i = 0; i < Z80_META_FLAG_COUNT; i++)
        {
            ImGui::TableSetupColumn(flag_labels[i],
                                    ImGuiTableColumnFlags_WidthFixed);
        };

        /* Jednořádková forma: obarvíme přímo data row místo headers,
         * jinak ImGui rendruje header style pozadí které nelze barvít
         * per-cell přes TextColored. Header line se vynechá. */
        ImGui::TableNextRow();
        for (int i = 0; i < Z80_META_FLAG_COUNT; i++)
        {
            ImGui::TableSetColumnIndex(i);
            ImGui::TextColored(affected[i] ? col_affected : col_default,
                               "%s", flag_labels[i]);
        };
        ImGui::EndTable();
    };
}

/**
 * @brief Vykreslí sekci tooltipu se symbol info pro adresu řádku.
 *
 * Zobrazí jméno symbolu, hex adresu a (pokud jsou) komentář a modul.
 *
 * @param sym záznam ze sym_db, nesmí být NULL
 *
 * @pre Volat pouze uvnitř ImGui::BeginTooltip()/EndTooltip() bloku.
 */
static void render_symbol_tooltip(const st_SYMBOL *sym)
{
    if (sym->name)
    {
        ImGui::Text("%s %s", _("Symbol:"), sym->name);
    };
    ImGui::Text("%s 0x%04X", _("Address:"), (unsigned)(sym->addr & 0xFFFF));
    if (sym->comment && sym->comment[0])
    {
        ImGui::Text("%s %s", _("Comment:"), sym->comment);
    };
    if (sym->module && sym->module[0])
    {
        ImGui::Text("%s %s", _("Module:"), sym->module);
    };
}

/**
 * @brief Vykreslí sekci tooltipu se symboly pro operandy instrukce.
 *
 * Tato sekce ukazuje dva nepovinné řádky:
 * - Target: cílová adresa skoku/volání pro flow instrukce
 *   (`z80_dasm_target_addr()` jiná než `(uint16_t)-1`).
 * - Mem: přímá paměťová adresa pro operandy typu Z80_OP_MEM_IMM16
 *   (např. `LD A,(nn)`, `LD (nn),HL`).
 *
 * Pokud na cílové adrese existuje symbol v sym_db, doplní se " -> NAME".
 * Funkce sama nezobrazí žádný řádek, pokud instrukce nemá target ani
 * MEM_IMM16 operand.
 *
 * @param inst    strukturovaná data instrukce, nesmí být NULL
 * @param bank_id bank scope pro sym_db_lookup_by_addr() (0 = CPU view)
 * @return true pokud byl vykreslen alespoň jeden řádek (užitečné pro
 *         rozhodnutí, zda volající má vykreslit Separator)
 *
 * @pre Volat pouze uvnitř ImGui::BeginTooltip()/EndTooltip() bloku.
 */
static bool render_operand_symbols(const z80_dasm_inst_t *inst, uint8_t bank_id)
{
    bool any = false;

    /* Target adresa flow instrukcí (JP/JR/CALL/DJNZ se statickým cílem). */
    uint16_t target = z80_dasm_target_addr(inst);
    if (target != (uint16_t)-1)
    {
        const st_SYMBOL *tsym = sym_db_lookup_by_addr(target, bank_id);
        if (tsym && tsym->name)
        {
            ImGui::Text("%s 0x%04X -> %s", _("Target:"), (unsigned) target, tsym->name);
        }
        else
        {
            ImGui::Text("%s 0x%04X", _("Target:"), (unsigned) target);
        };
        any = true;
    };

    /* MEM_IMM16: přímý paměťový operand, zkontrolujeme oba sloty. */
    const z80_operand_t *ops[2] = { &inst->op1, &inst->op2 };
    for (int i = 0; i < 2; i++)
    {
        if (ops[i]->type == Z80_OP_MEM_IMM16)
        {
            uint16_t addr = ops[i]->val.imm16;
            const st_SYMBOL *msym = sym_db_lookup_by_addr(addr, bank_id);
            if (msym && msym->name)
            {
                ImGui::Text("%s (0x%04X) -> %s",
                            _("Mem:"), (unsigned) addr, msym->name);
            }
            else
            {
                ImGui::Text("%s (0x%04X)", _("Mem:"), (unsigned) addr);
            };
            any = true;
            /* Pro stejnou MEM adresu v op1 a op2 (vzácně) se vypíše dvakrát -
               akceptovatelné, žádná deduplikace. */
        };
    };

    return any;
}

/**
 * @brief Sestaví label pro položku context menu "Focus to <target>".
 *
 * Formátuje label ve tvaru "Focus to 0xXXXX" nebo (pokud existuje symbol
 * pro adresu) "Focus to 0xXXXX (NAME)". Volající poskytuje vlastní buffer,
 * aby nedocházelo ke kolizím při více volání v rámci jednoho framu (např.
 * tisk více položek menu nad sebou).
 *
 * @param buf      cílový buffer, nesmí být NULL
 * @param buf_size velikost cílového bufferu (doporučeno >= 64 B)
 * @param target   cílová adresa skoku/volání
 * @param bank_id  bank scope pro sym_db_lookup_by_addr() (0 = CPU view)
 *
 * @pre buf != NULL && buf_size > 0
 * @post buf obsahuje nul-terminovaný řetězec
 */
static void format_focus_target_label(char *buf, size_t buf_size,
                                      uint16_t target, uint8_t bank_id)
{
    const st_SYMBOL *sym = sym_db_lookup_by_addr(target, bank_id);
    if (sym && sym->name)
    {
        snprintf(buf, buf_size, "%s 0x%04X (%s)",
                 _("Focus to"), (unsigned) target, sym->name);
    }
    else
    {
        snprintf(buf, buf_size, "%s 0x%04X",
                 _("Focus to"), (unsigned) target);
    };
}

/**
 * @brief Sestaví label pro položku submenu "Focus to register".
 *
 * Formátuje label ve tvaru "REG = 0xXXXX" nebo (pokud existuje symbol pro
 * hodnotu interpretovanou jako adresa) "REG = 0xXXXX (NAME)". Volající
 * poskytuje vlastní buffer pro lokální životnost (label se v ImGui menu
 * při MenuItem() volání kopíruje, ale aliasing dvou volání se stejným
 * static bufferem by byl zranitelný).
 *
 * @param buf      cílový buffer, nesmí být NULL
 * @param buf_size velikost cílového bufferu (doporučeno >= 64 B)
 * @param reg      jméno registru (literál, např. "BC", "AF'")
 * @param value    aktuální hodnota registru (16-bit snapshot)
 * @param bank_id  bank scope pro sym_db_lookup_by_addr() (0 = CPU view)
 *
 * @pre buf != NULL && buf_size > 0 && reg != NULL
 * @post buf obsahuje nul-terminovaný řetězec
 */
static void format_focus_reg_label(char *buf, size_t buf_size,
                                   const char *reg, uint16_t value,
                                   uint8_t bank_id)
{
    /*
     * Levostranné padding registru na 3 znaky - sjednotí vizuální
     * pozici "=" napříč 2-znakovými (BC, SP, HL ...) i 3-znakovými
     * (AF', BC', ...) labely. ImGui MenuItem používá proporcionální
     * font, takže výsledek není 100% sloupcový, ale rozhodně lepší
     * než variabilní šířka register prefixu.
     */
    const st_SYMBOL *sym = sym_db_lookup_by_addr(value, bank_id);
    if (sym && sym->name)
    {
        snprintf(buf, buf_size, "%-3s = 0x%04X (%s)",
                 reg, (unsigned) value, sym->name);
    }
    else
    {
        snprintf(buf, buf_size, "%-3s = 0x%04X", reg, (unsigned) value);
    };
}

/**
 * @brief Helper - jednořádkový summary BP do tooltipu.
 *
 * Volá se uvnitř ImGui::BeginTooltip() bloku, vykreslí 1-4 řádků popisu
 * jednoho BP. Pro typy které find_all_by_addr vrací (PC_EXEC, MEM_R/W,
 * GLOBAL) zobrazí relevantní detaily (adresní range, match mode,
 * condition, hit count).
 *
 * Formát:
 *   [#id] TYPE  enabled/disabled
 *     addr: 0xXXXX  /  range: 0xXXXX..0xYYYY  /  mask: 0xXXXX
 *     cond: ...     (jen pokud non-empty)
 *     hits: N       (jen pokud > 0)
 *
 * @param bp_id  identifikátor BP (musí být validní, jinak no-op)
 */
static void render_bpt_summary_in_tooltip(int bp_id)
{
    st_BPT *bp = breakpoints_find_by_id(bp_id);
    if (!bp) return;

    /* Řádek 1: [#id] TYP enabled/disabled */
    ImGui::Text("[#%d] %s  %s",
                bp->id,
                bpt_type_to_string(bp->type),
                bp->enabled ? _("enabled") : _("disabled"));

    /* User label (= bpt->name) pokud non-empty - na samostatném řádku
     * (= odlišit od auto-generated). */
    if (bp->name && bp->name[0] && !bp->auto_name)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(%s)", bp->name);
    };

    /* Řádek 2: adresní info per typ + match mode. */
    switch (bp->type)
    {
    case BPT_TYPE_PC_EXEC:
    case BPT_TYPE_MEM_R:
    case BPT_TYPE_MEM_W:
        switch (bp->addr_match_mode)
        {
        case BP_MATCH_SINGLE:
            ImGui::Text("  %s: 0x%04X", _("addr"), bp->addr);
            break;
        case BP_MATCH_RANGE:
            ImGui::Text("  %s: 0x%04X..0x%04X", _("range"), bp->addr, bp->addr_end);
            break;
        case BP_MATCH_MASK:
            ImGui::Text("  %s: (x & 0x%04X) == (0x%04X & 0x%04X)",
                        _("mask"), bp->addr_mask, bp->addr, bp->addr_mask);
            break;
        default:
            break;
        };
        break;
    case BPT_TYPE_GLOBAL:
        /* Bez adresy - GLOBAL test čistě na condition. */
        ImGui::TextDisabled("  %s", _("(no address - condition-based)"));
        break;
    default:
        /* Ostatní typy by find_all_by_addr neměl vracet pro hover na
         * disasm řádek, ale pro robustnost zobrazíme aspoň raw addr. */
        ImGui::Text("  %s: 0x%04X", _("addr"), bp->addr);
        break;
    };

    /* Condition (pokud non-empty). */
    if (bp->expr && bp->expr[0])
    {
        ImGui::Text("  %s: %s", _("cond"), bp->expr);
    };

    /* Hit count (pokud > 0). */
    if (bp->hits > 0)
    {
        ImGui::Text("  %s: %llu", _("hits"), (unsigned long long)bp->hits);
    };
}


/**
 * @brief Sjednocený per-row tooltip pro dolní disassembly tabulku.
 *
 * Vyvolává se pro hovered řádek (po Selectable() s ImGuiHoveredFlags_DelayNormal).
 * Sestaví tooltip obsahující instrukční info, symbol info pro adresu řádku,
 * symboly pro operandy, BP detail pokud jsou na adrese, a edit hints.
 * Sekce, ke kterým chybí data, se vynechají.
 *
 * @param row řádek disassembly, nesmí být NULL
 *
 * @pre Volat pouze uvnitř render-loopu ImGui jako reakce na hover.
 */
static void render_row_tooltip(const DasmRow *row)
{
    if (!ImGui::BeginTooltip())
        return;

    /* 1. Instrukční info - z80_meta může být NULL pro neznámé třídy. */
    const z80_meta_t *meta = z80_meta_lookup(&row->inst);
    render_instruction_tooltip(row, &row->inst, meta);

    /* 2. Symbol info pro adresu řádku. */
    const st_SYMBOL *sym = sym_db_lookup_by_addr(row->addr, 0);
    if (sym)
    {
        ImGui::Separator();
        render_symbol_tooltip(sym);
    };

    /*
     * 3. Symbol info pro operandy (target/MEM).
     * Nejprve se zeptáme, zda existují - pokud ano, separator + render.
     * Vyhneme se tak prázdné sekci (visual noise).
     */
    {
        uint16_t target = z80_dasm_target_addr(&row->inst);
        bool has_target = (target != (uint16_t)-1);
        bool has_mem = (row->inst.op1.type == Z80_OP_MEM_IMM16
                       || row->inst.op2.type == Z80_OP_MEM_IMM16);
        if (has_target || has_mem)
        {
            ImGui::Separator();
            (void)render_operand_symbols(&row->inst, 0);
        };
    };

    /*
     * 4. Breakpoints na řádku - PC_EXEC + MEM_R/W (vč. range/mask) +
     * GLOBAL. Range BP které pokrývají adresu (= addr leží uvnitř
     * bpt->addr..addr_end) se taky zahrnou. Více BP oddělené Separator.
     */
    {
        GArray *bpt_ids = g_array_new(FALSE, FALSE, sizeof(int));
        int n = breakpoints_find_all_by_addr(row->addr, bpt_ids);
        if (n > 0)
        {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.7f, 1.0f), "%s:", _("Breakpoints"));
            for (int k = 0; k < n; k++)
            {
                if (k > 0)
                    ImGui::Separator();
                int bp_id = g_array_index(bpt_ids, int, k);
                render_bpt_summary_in_tooltip(bp_id);
            };
        };
        g_array_free(bpt_ids, TRUE);
    };

    /* 5. Edit hints - šedý dim text. */
    ImGui::Separator();
    ImGui::TextDisabled("%s", _("Double-click a row or start typing to open the inline assembler."));
    ImGui::TextDisabled("%s", _("Right-click for context menu."));

    ImGui::EndTooltip();
}

/* =========================================================================
 * BRANCH ARROWS - sdílená pixel pozice řádků dolní tabulky
 * =========================================================================
 *
 * Statická pole se naplňují při render dolní tabulky a používají se
 * po EndTable v render_branch_arrows() ke kreslení šipek nad tabulkou.
 *
 * Multi-instance (plán č. 7): pixel state se drží per-instance v
 * DisassembledView::arrow_*. Hlavní singleton i sekundární okna mají
 * vlastní storage - během renderu instance A se v poli arrow_row_screen_y
 * naplní její pozice, render_branch_arrows() je čte z téže instance.
 * Pořadí volání mezi instancemi je nezávislé.
 */


/* =========================================================================
 * BRANCH ARROWS - vizualizace skoků
 * =========================================================================
 *
 * Kreslení šipek z zdrojové branch instrukce na cíl. Volá se po EndTable
 * dolní tabulky, kdy už jsou známy pixel pozice všech řádků (zachycené
 * při render do self->arrow_row_screen_y[]).
 *
 * Pravidla:
 * - Šipka jen pro instrukce s fixed targetem: JP, JR, CALL, RST, DJNZ
 *   (= flow ∈ {JUMP, JUMP_COND, CALL, CALL_COND, RST}).
 * - JP (HL/IX/IY) (JUMP_INDIRECT), RET, RET cc, RETI/RETN: žádná šipka
 *   (target nelze staticky určit).
 * - Pokud cíl leží mezi visible řádky: plná šipka spojuje zdroj a cíl.
 * - Pokud cíl je mimo okno (před prvním nebo za posledním řádkem):
 *   krátký stub se šipkou ven (^ nahoru / v dolů ASCII glyphy).
 *
 * Barvy:
 * - CALL/CALL_COND: světle modrá (volání = side path).
 * - JP/JR/DJNZ/RST: šedá (běžný kontrolní tok).
 * - Hover (myš nad zdrojovým nebo cílovým řádkem): žlutá místo
 *   default barvy, plus mírně tlustší shaft.
 *
 * Layout:
 * - Shaft sloupec se kreslí v levé části ICONS gutteru (mezi
 *   icons_x a icons_x + COL_ICONS_ARROWS_WIDTH).
 * - Více šipek v jednom okně se posouvá vlevo o ~3 px, max 4 sloupce
 *   (5. a další = trim, nezobrazí se = nepřeplácaný gutter).
 */

/**
 * @brief Směr (a typ) šipky pro vizualizaci skoku.
 *
 * Pro UP/DOWN je cíl viditelný v tabulce - kreslí se plná šipka mezi
 * zdrojem a cílem. Pro OFF_UP/OFF_DOWN je cíl mimo okno - jen krátký
 * stub se špičkou ven. SELF je tight loop (target == zdroj, např.
 * JR -2 / JR $).
 */
typedef enum {
    ARROW_DIR_UP,        /**< row_to < row_from (= vizuálně nahoru). */
    ARROW_DIR_DOWN,      /**< row_to > row_from. */
    ARROW_DIR_OFF_UP,    /**< Cíl před prvním řádkem (mimo viditelné okno). */
    ARROW_DIR_OFF_DOWN,  /**< Cíl za posledním řádkem (mimo viditelné okno). */
    ARROW_DIR_SELF,      /**< row_to == row_from (tight loop). */
} ArrowDir;

/**
 * @brief Pomocné info pro plánovanou šipku (= jeden řádek se zdrojem skoku).
 *
 * Naplňuje se při sběru ve fázi pre-render, pak se podle row_to / dir
 * přiřadí horizontální offset (shaft column) a vykreslí se.
 */
typedef struct {
    int row_from;          /**< Index řádku zdroje (0..visible-1). */
    int row_to;            /**< Index řádku cíle, -1 = mimo okno. */
    ArrowDir dir;          /**< Směr / typ šipky. */
    bool is_call;          /**< true = CALL/CALL_COND (modrá), false = JP/JR/DJNZ/RST (šedá). */
    int shaft_col;         /**< Přiřazený offset shaft sloupce (0..3). Vyplněno při assignmentu. */
} BranchArrowInfo;

/**
 * @brief Vykreslí všechny branch arrows nad dolní tabulkou.
 *
 * Volá se po dokončení table renderu, kdy self->arrow_row_screen_y[]
 * obsahuje pixel pozice řádků právě dokončeného renderu této instance.
 * Iteruje rows[0..visible-1], pro každou branch instrukci spočítá target
 * row a vykreslí shaft + arrow head.
 *
 * Hover detekce: aktuální mouse pozice se porovná s pixel rectem
 * řádků [row_from] a [row_to]. Pokud myš v některém z nich je, šipka
 * se kreslí ve zvýrazněné barvě (žlutá).
 *
 * @param self         instance (per-instance pixel pozice)
 * @param rows         pole disassemblovaných řádků
 * @param visible_rows počet platných řádků v rows[]
 *
 * @pre self->arrow_row_screen_y[0..visible_rows-1], self->arrow_icons_x,
 *      self->arrow_row_height jsou naplněny aktuálními pixel pozicemi
 *      z právě dokončeného renderu.
 */
static void render_branch_arrows(const DisassembledView *self,
                                 const DasmRow *rows, int visible_rows)
{
    if (visible_rows <= 0)
        return;
    if (g_debugger.disasm_show_branch_arrows == 0)
        return;

    /* Sběr branch instrukcí - max 1 šipka per zdrojový řádek. */
    BranchArrowInfo arrows[DBG_DASM_MAX_VISIBLE_ROWS];
    int n_arrows = 0;

    /* Hranice viditelného adresního okna pro klasifikaci off-window cílů.
     * Poslední řádek může mít vícebajtovou instrukci, takže visible end
     * = addr posledního řádku + jeho délka. */
    uint16_t addr_first = rows[0].addr;
    uint16_t addr_last_end = (uint16_t)(rows[visible_rows - 1].addr
                                        + rows[visible_rows - 1].inst.length);

    for (int i = 0; i < visible_rows; i++)
    {
        const z80_dasm_inst_t *inst = &rows[i].inst;
        z80_flow_type_t fl = inst->flow;
        bool is_branch = (fl == Z80_FLOW_JUMP || fl == Z80_FLOW_JUMP_COND
                          || fl == Z80_FLOW_CALL || fl == Z80_FLOW_CALL_COND
                          || fl == Z80_FLOW_RST);
        if (!is_branch)
            continue;

        uint16_t target = z80_dasm_target_addr(inst);
        if (target == (uint16_t)-1)
            continue;

        /* Najít cílový řádek mezi visible. Match na exact addr - pokud
         * target padne doprostřed instrukce, považujeme za off-window
         * (nezobrazené v tabulce ve své pozici by se nešipkovalo
         * smysluplně - target by ukazoval mezi řádky). */
        int row_to = -1;
        for (int j = 0; j < visible_rows; j++)
        {
            if (rows[j].addr == target)
            {
                row_to = j;
                break;
            };
        };

        BranchArrowInfo a;
        a.row_from = i;
        a.row_to = row_to;
        a.is_call = (fl == Z80_FLOW_CALL || fl == Z80_FLOW_CALL_COND);
        a.shaft_col = 0;

        if (row_to >= 0)
        {
            if (row_to == i)
                a.dir = ARROW_DIR_SELF;
            else if (row_to < i)
                a.dir = ARROW_DIR_UP;
            else
                a.dir = ARROW_DIR_DOWN;
        }
        else
        {
            /* Off-window klasifikace podle relativní pozice cíle vůči
             * viditelnému adresnímu rozsahu. Wrap-around (target za 0xFFFF)
             * řešíme jako stejnou stranu jako addr_first vs addr_last_end -
             * pokud target < first nebo target >= last_end (s mod 16-bit
             * sémantikou), použijeme bližší stranu. */
            uint16_t dist_up = (uint16_t)(addr_first - target);     /* kolik bytes 'pod' first */
            uint16_t dist_down = (uint16_t)(target - addr_last_end);/* kolik bytes 'za' last_end */
            a.dir = (dist_up <= dist_down) ? ARROW_DIR_OFF_UP : ARROW_DIR_OFF_DOWN;
        };

        arrows[n_arrows++] = a;
        if (n_arrows >= DBG_DASM_MAX_VISIBLE_ROWS)
            break;
    };

    if (n_arrows == 0)
        return;

    /*
     * Přiřazení shaft sloupců - greedy: pro každou šipku najít nejmenší
     * sloupec 0..3 ve kterém její Y rozsah nezasahuje s žádnou už
     * přiřazenou šipkou ve stejném sloupci. Off-window stub šipky
     * se přiřazují stejně (zabírají jen řádek zdroje).
     */
    const int MAX_SHAFT_COLS = 4;
    /* Per sloupec si pamatujeme bitmapu Y-obsazení (po řádcích).
     * Délka = visible_rows. */
    bool occupied[MAX_SHAFT_COLS][DBG_DASM_MAX_VISIBLE_ROWS];
    memset(occupied, 0, sizeof(occupied));

    int n_drawn = 0;
    for (int k = 0; k < n_arrows; k++)
    {
        BranchArrowInfo *a = &arrows[k];
        int y_lo, y_hi; /* inkluzivní range řádků */
        if (a->dir == ARROW_DIR_OFF_UP || a->dir == ARROW_DIR_OFF_DOWN
            || a->dir == ARROW_DIR_SELF)
        {
            y_lo = a->row_from;
            y_hi = a->row_from;
        }
        else if (a->dir == ARROW_DIR_UP)
        {
            y_lo = a->row_to;
            y_hi = a->row_from;
        }
        else /* DOWN */
        {
            y_lo = a->row_from;
            y_hi = a->row_to;
        };

        int chosen = -1;
        for (int c = 0; c < MAX_SHAFT_COLS; c++)
        {
            bool conflict = false;
            for (int y = y_lo; y <= y_hi; y++)
            {
                if (occupied[c][y]) { conflict = true; break; };
            };
            if (!conflict)
            {
                chosen = c;
                for (int y = y_lo; y <= y_hi; y++)
                    occupied[c][y] = true;
                break;
            };
        };

        if (chosen < 0)
        {
            /* Žádný sloupec volný - šipka se nezobrazí (limit 4 paralelní). */
            a->shaft_col = -1;
            continue;
        };
        a->shaft_col = chosen;
        n_drawn++;
    };

    if (n_drawn == 0)
        return;

    /*
     * Hover detekce. Mouse pos a pixel rect každého řádku. Hover na řádku
     * znamená: myš je v Y rozsahu řádku a v X rozsahu ICONS sloupce nebo
     * v rozsahu zbytku tabulky. Pro jednoduchost akceptujeme cokoliv mezi
     * arrow_icons_x a 'okno doprava' (ImGui::GetWindowPos().x + width).
     *
     * Per-okno gate: pokud kurzor není v aktuálním okně (= myš je nad
     * sousedním disasm oknem), hover detekci přeskočíme. Bez tohoto gate
     * by pohyb myši nad oknem #2 falešně zvýrazňoval šipky v okně #3 na
     * stejné Y souřadnici (mp.y je globální screen pozice).
     */
    int hovered_row = -1;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
    {
        ImVec2 mp = ImGui::GetIO().MousePos;
        for (int i = 0; i < visible_rows; i++)
        {
            float y0 = self->arrow_row_screen_y[i] - self->arrow_row_height * 0.5f;
            float y1 = self->arrow_row_screen_y[i] + self->arrow_row_height * 0.5f;
            if (mp.y >= y0 && mp.y < y1)
            {
                hovered_row = i;
                break;
            };
        };
    };

    /*
     * Vlastní kreslení.
     *
     * Shaft sloupec je v levé části ICONS gutteru. Sloupec 0 = nejvíc
     * vpravo (= těsně před BPT/PC slotem), sloupec 3 = nejvíc vlevo.
     * Krok mezi sloupci ~3 px (3 * 4 = 12 < ARROWS_WIDTH=16).
     */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 col_call    = IM_COL32(100, 150, 255, 220);
    const ImU32 col_jump    = IM_COL32(160, 160, 160, 200);
    const ImU32 col_hovered = IM_COL32(255, 220, 0, 255);

    /* Reservujeme 2 px paddingu vlevo a před BPT/PC slotem. */
    float shaft_right = self->arrow_icons_x + COL_ICONS_ARROWS_WIDTH - 2.0f;
    float shaft_step = 4.0f;
    float arrow_head_h = self->arrow_row_height * 0.25f;
    if (arrow_head_h < 3.0f) arrow_head_h = 3.0f;
    float arrow_head_w = 4.0f;

    for (int k = 0; k < n_arrows; k++)
    {
        const BranchArrowInfo *a = &arrows[k];
        if (a->shaft_col < 0)
            continue;

        bool hovered = (hovered_row == a->row_from
                        || (a->row_to >= 0 && hovered_row == a->row_to));
        ImU32 color = hovered ? col_hovered
                              : (a->is_call ? col_call : col_jump);
        float thickness = hovered ? 2.0f : 1.5f;

        float x_shaft = shaft_right - a->shaft_col * shaft_step;
        float y_from = self->arrow_row_screen_y[a->row_from];

        if (a->dir == ARROW_DIR_OFF_UP)
        {
            /* Stub: krátký svislý čáry směrem nahoru s ^ glyfem. */
            float y_top = self->arrow_table_min_y + 1.0f;
            dl->AddLine(ImVec2(x_shaft, y_from), ImVec2(x_shaft, y_top),
                        color, thickness);
            /* Šipková špička nahoru (^) */
            dl->AddTriangleFilled(
                ImVec2(x_shaft, y_top - arrow_head_w * 0.7f),
                ImVec2(x_shaft - arrow_head_w * 0.5f, y_top + arrow_head_h * 0.3f),
                ImVec2(x_shaft + arrow_head_w * 0.5f, y_top + arrow_head_h * 0.3f),
                color);
            /* Krátká vodorovná čárka ze shaftu k řádku zdroje (= "tail"). */
            dl->AddLine(ImVec2(x_shaft, y_from),
                        ImVec2(shaft_right + 1.0f, y_from),
                        color, thickness);
        }
        else if (a->dir == ARROW_DIR_OFF_DOWN)
        {
            float y_bot = self->arrow_table_max_y - 1.0f;
            dl->AddLine(ImVec2(x_shaft, y_from), ImVec2(x_shaft, y_bot),
                        color, thickness);
            /* Šipková špička dolů (v) */
            dl->AddTriangleFilled(
                ImVec2(x_shaft, y_bot + arrow_head_w * 0.7f),
                ImVec2(x_shaft - arrow_head_w * 0.5f, y_bot - arrow_head_h * 0.3f),
                ImVec2(x_shaft + arrow_head_w * 0.5f, y_bot - arrow_head_h * 0.3f),
                color);
            dl->AddLine(ImVec2(x_shaft, y_from),
                        ImVec2(shaft_right + 1.0f, y_from),
                        color, thickness);
        }
        else if (a->dir == ARROW_DIR_SELF)
        {
            /* Tight loop (JR -2 / JR $) - mini smyčka vedle řádku. */
            float y_mid = y_from;
            dl->AddLine(ImVec2(shaft_right + 1.0f, y_mid - 2.0f),
                        ImVec2(x_shaft, y_mid - 2.0f), color, thickness);
            dl->AddLine(ImVec2(x_shaft, y_mid - 2.0f),
                        ImVec2(x_shaft, y_mid + 2.0f), color, thickness);
            dl->AddLine(ImVec2(x_shaft, y_mid + 2.0f),
                        ImVec2(shaft_right + 1.0f, y_mid + 2.0f), color, thickness);
            /* Špička zpět doprava na zdrojovém řádku. */
            dl->AddTriangleFilled(
                ImVec2(shaft_right + 1.0f + arrow_head_w, y_mid),
                ImVec2(shaft_right + 1.0f, y_mid - arrow_head_h * 0.5f),
                ImVec2(shaft_right + 1.0f, y_mid + arrow_head_h * 0.5f),
                color);
        }
        else
        {
            /* Plná šipka mezi from a to. */
            float y_to = self->arrow_row_screen_y[a->row_to];
            /* Vodorovná tail u zdroje (z řádku do shaftu). */
            dl->AddLine(ImVec2(shaft_right + 1.0f, y_from),
                        ImVec2(x_shaft, y_from), color, thickness);
            /* Svislý shaft. */
            dl->AddLine(ImVec2(x_shaft, y_from), ImVec2(x_shaft, y_to),
                        color, thickness);
            /* Vodorovná head u cíle (ze shaftu zpět doprava na cílový
             * řádek), zakončená trojúhelníkovou špičkou doprava. */
            dl->AddLine(ImVec2(x_shaft, y_to),
                        ImVec2(shaft_right - 1.0f, y_to), color, thickness);
            dl->AddTriangleFilled(
                ImVec2(shaft_right + arrow_head_w - 1.0f, y_to),
                ImVec2(shaft_right - 1.0f, y_to - arrow_head_h * 0.5f),
                ImVec2(shaft_right - 1.0f, y_to + arrow_head_h * 0.5f),
                color);
        };
    };
}


/* =========================================================================
 * HLAVIČKA SEKCE - text entry + flag checkboxy
 * =========================================================================
 *
 * Hlavička je tenký řádek na vrcholu sekce, který slouží jako rychlý
 * navigační vstup a kontrolér flagů zobrazení. Layout (zleva doprava):
 *
 *   [Address or symbol _______]  [x] Follow PC  [x] T-states
 *
 * Text entry: uživatel zapíše hex adresu (libovolný běžný formát) nebo
 * jméno symbolu. Enter aplikuje (parse + jump na adresu nebo lookup
 * symbol v sym_db). Při neúspěšném parse se nastaví header_input_error,
 * pole se neclearuje, uživatel může opravit.
 *
 * Akceptované formáty hex (podle existujícího IASM parseru):
 *   - "0x1234", "1234h", "01234h", "#1234", nebo holé "1234"
 *   - case-insensitive
 * Symbol lookup je case-sensitive (= jak je v sym_db_lookup_by_name).
 *
 * Pro hlavní instanci je checkbox "Follow PC" vykreslen jako BeginDisabled
 * (přístupný jen vizuálně, hodnota zůstává true).
 */

/**
 * @brief Pokus o parse hex adresy z uživatelského stringu.
 *
 * Akceptuje běžné formáty: 0x1234, 1234h, 01234h, #1234, 1234.
 * Vrací true a vyplní out_addr při úspěchu, jinak false (out_addr
 * nemodifikováno).
 *
 * Whitespace na začátku/konci je akceptován, jinak žádné mezery uvnitř.
 *
 * @param s         vstupní řetězec
 * @param out_addr  pointer na uint16_t pro výstupní adresu (16-bit)
 * @return          true při úspěchu
 */
static bool parse_hex_addr(const char *s, uint16_t *out_addr)
{
    if (!s || !out_addr)
        return false;

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t')
        s++;

    if (*s == '\0')
        return false;

    /* Konec stringu (před trailing whitespace) */
    const char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        end--;

    /* Detekce prefixů 0x / # */
    const char *p = s;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    else if (p[0] == '#')
        p += 1;

    /* Detekce sufixu 'h' / 'H' */
    const char *p_end = end;
    if (p_end > p && (p_end[-1] == 'h' || p_end[-1] == 'H'))
        p_end--;

    if (p == p_end)
        return false; /* žádné hex digits */

    /* Validace + parse */
    unsigned long val = 0;
    for (const char *q = p; q < p_end; q++)
    {
        char c = *q;
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = (unsigned)(c - 'A' + 10);
        else
            return false;
        val = (val << 4) | digit;
        if (val > 0xFFFFu)
            return false; /* overflow */
    };

    *out_addr = (uint16_t)val;
    return true;
}


/**
 * @brief Aplikuje uživatelský vstup z text entry: parse adresu nebo
 *        lookup symbolu, při úspěchu nastaví focus_addr instance.
 *
 * Při úspěchu vyčistí header_input_buf a header_input_error.
 * Při neúspěchu nastaví header_input_error = true; buffer ponechá
 * (uživatel může opravit a zkusit znovu).
 */
static void apply_header_input(DisassembledView *self)
{
    if (!self)
        return;

    /* Prázdný vstup - nedělej nic, ale vyčisti případný error stav */
    const char *s = self->header_input_buf;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0')
    {
        self->header_input_error = false;
        return;
    };

    /* Pokus 1: hex parse */
    uint16_t addr;
    if (parse_hex_addr(self->header_input_buf, &addr))
    {
        /* User Enter v header text entry = explicit user akce.
         * Použijeme focus_to (= auto-disable Follow PC pokud emu běží). */
        dbg_disasm_view_focus_to(self, addr);
        self->header_input_buf[0] = '\0';
        self->header_input_error = false;
        return;
    };

    /* Pokus 2: symbol lookup (case-sensitive, viz sym_db API).
     * Trim whitespace pro symbol jméno. */
    char trimmed[64];
    {
        const char *src = self->header_input_buf;
        while (*src == ' ' || *src == '\t')
            src++;
        size_t n = strlen(src);
        while (n > 0 && (src[n-1] == ' ' || src[n-1] == '\t'))
            n--;
        if (n >= sizeof(trimmed))
            n = sizeof(trimmed) - 1;
        memcpy(trimmed, src, n);
        trimmed[n] = '\0';
    };

    const st_SYMBOL *sym = sym_db_lookup_by_name(trimmed);
    if (sym)
    {
        /* Symbol lookup z user input = explicit user akce. */
        dbg_disasm_view_focus_to(self, (uint16_t)(sym->addr & 0xFFFFu));
        self->header_input_buf[0] = '\0';
        self->header_input_error = false;
        return;
    };

    /* Ani hex, ani symbol - error stav */
    self->header_input_error = true;
}


/**
 * @brief Vykreslí hlavičku sekce (text entry + checkboxy).
 *
 * Layout je SameLine arrangement: text entry užší (~16 znaků), pak
 * dva checkboxy. Pro hlavní instanci ("main") je "Follow PC" disabled.
 */
static void render_disasm_header(DisassembledView *self)
{
    /* Width text entry: ~16 znaků hex/symbol jména */
    float char_w_normal = ImGui::CalcTextSize("0").x;
    float input_w = char_w_normal * 16.0f;

    /* Pokud poslední vstup selhal, červené pozadí input boxu */
    bool error = self->header_input_error;
    if (error)
    {
        ImGui::PushStyleColor(ImGuiCol_FrameBg,
            ImVec4(0.5f, 0.15f, 0.15f, 1.0f));
    };

    ImGui::SetNextItemWidth(input_w);
    bool entered = ImGui::InputTextWithHint(
        "##disasm_header_input",
        _("Address or symbol"),
        self->header_input_buf,
        sizeof(self->header_input_buf),
        ImGuiInputTextFlags_EnterReturnsTrue);

    if (error)
        ImGui::PopStyleColor();

    if (entered)
        apply_header_input(self);

    /* Checkbox: Follow PC. Default ON pro hlavní instanci (z create()),
     * uživatel může přepínat ve všech instancích. Persist přes config. */
    ImGui::SameLine();
    bool follow_pc = self->follow_pc;
    if (ImGui::Checkbox(_L("Follow PC"), &follow_pc))
        dbg_disasm_view_set_follow_pc(self, follow_pc);

    /* Checkbox: T-states */
    ImGui::SameLine();
    bool show_tstates = self->show_tstates;
    if (ImGui::Checkbox(_L("T-states"), &show_tstates))
        dbg_disasm_view_set_show_tstates(self, show_tstates);
}


/* =========================================================================
 * DOLNÍ TABULKA — DISASSEMBLY
 * =========================================================================
 *
 * Disassemblovaná paměť od adresy focusu. Hlavní pracovní oblast debuggeru.
 *
 * Chování:
 * - Zobrazuje *self->shared_visible_rows instrukcí od *self->shared_focus_addr
 * - Řádek na pozici *self->shared_selected_row je zvýrazněný (selekce)
 * - Klik na řádek mění selekci a focus
 * - Navigace klávesnicí mění focus
 * - Context menu na pravé tlačítko
 * - Double click / Enter → Inline Assembler (placeholder)
 *
 * V animačním režimu je celá tabulka disabled (pouze zobrazuje stav).
 */
static void render_disassembly_table(DisassembledView *self, float height, bool is_paused)
{
    ImGui::BeginChild("##dbg_disasm", ImVec2(0, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollWithMouse);

    /* Zmenšení fontu — viz DBG_CONTENT_FONT_SIZE_OFFSET v debugger_state.h */
    {
        float base = ImGui::GetFontSize();
        float scale = (base + DBG_CONTENT_FONT_SIZE_OFFSET) / base;
        ImGui::SetWindowFontScale(scale > 0.5f ? scale : 0.5f);
    };

    /*
     * Dynamický výpočet počtu viditelných řádků podle výšky tabulky a fontu.
     * Výška řádku = výška textu + 2× CellPadding.y (ImGui tabulkový řádek).
     * Při změně počtu řádků vyžádáme refresh disassembly.
     */
    {
        float row_h = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
        float avail_h = ImGui::GetContentRegionAvail().y;
        int new_visible = (int)(avail_h / row_h);
        if (new_visible < 1) new_visible = 1;
        if (new_visible > DBG_DASM_MAX_VISIBLE_ROWS) new_visible = DBG_DASM_MAX_VISIBLE_ROWS;

        if (new_visible != *self->shared_visible_rows)
        {
            *self->shared_visible_rows = new_visible;
            /* Oříznutí selekce pokud přesahuje nový rozsah */
            if (*self->shared_selected_row >= new_visible)
                *self->shared_selected_row = new_visible - 1;
            dbg_refresh_request();
        };
    };

    /*
     * V animačním režimu: focus sleduje aktuální PC.
     * Dolní tabulka i šoupátko se tak aktualizují v reálném čase.
     *
     * Gate je čistě per-instance flag follow_pc (runtime, uživatelsky
     * přepínatelný v hlavičce view) a běh emulace (= !is_paused). Platí
     * pro všechny instance stejně - hlavní i sekundární okna mohou
     * následovat PC.
     *
     * Slider drag bypass: pokud uživatel drží LMB na slideru, vynecháme
     * update aby mohl "nakouknout" jinam. Po release se follow_pc opět
     * převezme řízení (a focus_addr se vrátí na aktuální PC).
     */
    if (self->follow_pc && !is_paused && !self->slider_held)
    {
        uint16_t pc = g_mzarch_main.cpu->pc;
        if (pc != *self->shared_focus_addr)
        {
            *self->shared_focus_addr = pc;
            *self->shared_selected_row = 0;
        };
    };

    /*
     * Načtení aktuální paměti do current bufferu a porovnání s posledním stavem.
     *
     * Čtení paměti a memcmp se provádí jen pokud refresh kontroler rozhodl,
     * že je čas zkontrolovat data (should_refresh). Pokud ne, přeskočíme
     * a použijeme cachované řádky z minulého refreshe.
     *
     * Pokud se adresa focusu a obsah paměti nezměnily, přeskočíme i disassembly.
     */
    if (g_dbg_ui.refresh.should_refresh)
    {
        self->current_dasm_buf->start_addr = *self->shared_focus_addr;
        read_mapped_memory_segment(self->current_dasm_buf->bytes, *self->shared_focus_addr, DISSAMBLE_BUFFER_SIZE);

        bool need_disassembly = (self->current_dasm_buf->start_addr != self->last_dasm_buf->start_addr) ||
                                (memcmp(self->current_dasm_buf->bytes, self->last_dasm_buf->bytes, DISSAMBLE_BUFFER_SIZE) != 0) ||
                                (*self->shared_visible_rows != self->last_disassembled_rows);

        if (need_disassembly)
        {
            /* Obsah se změnil — provedeme disassembly a aktualizujeme cache.
             *
             * Disasm scan čte z buffer snapshotu (DasmBufferCtx), ne ze
             * živé paměti. Eliminuje race UI vs emu vlákno: kdyby emu
             * mezi buffer snapshot a disasm scan přepnul mapping (banking
             * switch), disasm by viděl jiné bajty než snapshot a tabulka
             * by ukazovala nekonzistentní mnemoniku/délku vůči bytes
             * sloupci. Buffer-aware read tu konzistenci zajistí. */
            DasmBufferCtx ctx = {
                self->current_dasm_buf->bytes,
                self->current_dasm_buf->start_addr,
                DISSAMBLE_BUFFER_SIZE
            };
            uint16_t addr = *self->shared_focus_addr;
            for (int i = 0; i < *self->shared_visible_rows; i++)
            {
                self->cached_row_lengths[i] = disassemble_at(addr, &self->cached_rows[i], &ctx);
                addr = (uint16_t)(addr + self->cached_row_lengths[i]);
            };

            /* Zapamatujeme si počet řádků pro detekci změny velikosti okna */
            self->last_disassembled_rows = *self->shared_visible_rows;

            /* Prohodíme buffery — current se stane last pro příští frame */
            st_DASM_BUFFER *tmp = self->current_dasm_buf;
            self->current_dasm_buf = self->last_dasm_buf;
            self->last_dasm_buf = tmp;
        };
    };

    /* Používáme cachované řádky (buď čerstvě disassemblované, nebo z minulého framu) */
    DasmRow *rows = self->cached_rows;
    int *row_lengths = self->cached_row_lengths;

    /* Cache adresy selectovaného řádku do shared_selected_addr pro externí
     * konzumenty (Run To Cursor, ev. další iconbar akce). Bez tohoto cache
     * by museli duplikovat disassembly logiku z focus_addr + selected_row
     * krát variabilní délka instrukce. Updatuje se každý frame, takže
     * odráží aktuální stav po klikání i scrollování v disasm tabulce. */
    if (*self->shared_selected_row >= 0 && *self->shared_selected_row < *self->shared_visible_rows)
    {
        *self->shared_selected_addr = rows[*self->shared_selected_row].addr;
    };

    /* Přepočítat BPT cache pokud se změnila verze breakpointů nebo při refreshi */
    if (self->cached_bpt_version != g_breakpoints.version || g_dbg_ui.refresh.should_refresh)
    {
        dbg_bpt_cache_rebuild(self, rows, row_lengths, *self->shared_visible_rows);
    };

    /*
     * Aktuální PC pro zvýraznění - čte se z CPU každý frame pro plynulou
     * animaci. Globální gate animated_updates byl odstraněn; cached_pc
     * field na DisassembledView byl odstraněn také (zmrazení PC při
     * Disabled již nemá smysl).
     */
    uint16_t current_pc = g_mzarch_main.cpu->pc;

    /*
     * Výpočet šířek textových sloupců podle velikosti aktuálního fontu.
     *
     * Šířka = (počet znaků obsahu + separátor) × šířka jednoho znaku.
     * Ikony BPT a PC jsou kreslené — konstantní šířka v pixelech.
     */
    float char_w = ImGui::CalcTextSize("0").x;
    float col_addr_w = char_w * DASM_ADDR_CHARS;
    float col_bytes_w = char_w * DASM_BYTES_CHARS;
    float col_mnem_w = char_w * DASM_MNEM_CHARS;
    float col_tstates_w = char_w * DASM_TSTATES_CHARS;

    /* Per-instance flag (nahradil globální g_debugger.disasm_show_tstates_col) */
    bool show_tstates = self->show_tstates;
    int n_columns = show_tstates ? 5 : 4;

    /*
     * ImGui tabulka se 4 nebo 5 sloupci — všechny s pevnou šířkou.
     *
     * Sloupce:
     *   ICONS   — ikony BPT + PC (kreslené přes ImDrawList, 20px, s překryvem)
     *   ADDR    — adresa instrukce (4 hex znaky + ':')
     *   BYTES   — bytekódy instrukce (12 znaků)
     *   MNEM    — Z80 mnemonic (15 znaků)
     *   TSTATES — počet T-stavů (volitelný 5. sloupec, default ON,
     *             toggle v menu Debugger Settings -> Disassembled)
     *
     * RowBg — střídavé pozadí řádků pro lepší čitelnost.
     */
    ImGuiTableFlags table_flags = ImGuiTableFlags_NoBordersInBody |
                                  ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_NoHostExtendX |
                                  ImGuiTableFlags_RowBg;

    /*
     * Pixel-pos zachycení pro post-render branch arrows.
     *
     * Ukládáme střední Y pozici každého řádku ICONS sloupce + společnou X
     * pozici levé hrany sloupce (= posun pro arrow shaft). Po EndTable
     * pak iterujeme branch instrukce a kreslíme šipky přes ImDrawList,
     * což je bezpečnější než inline během renderu (target řádek nemusí
     * existovat v okamžiku zdrojového řádku - 2-pass je zbytečně složitý).
     *
     * Pole se naplňuje vždy, šipky se kreslí jen pokud je zapnutý toggle
     * disasm_show_branch_arrows. Hover detekce funguje i bez kreslení.
     */
    bool show_arrows = (g_debugger.disasm_show_branch_arrows != 0);
    (void) show_arrows; /* gating se děje uvnitř render_branch_arrows() přes
                         * g_debugger.disasm_show_branch_arrows; sběr pozic
                         * běží vždy (cheap), aby fungoval hover detect. */

    if (ImGui::BeginTable("##dasm_table", n_columns, table_flags))
    {
        ImGui::TableSetupColumn("Icons", ImGuiTableColumnFlags_WidthFixed, COL_ICONS_WIDTH);
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, col_addr_w);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, col_bytes_w);
        ImGui::TableSetupColumn("Mnem", ImGuiTableColumnFlags_WidthFixed, col_mnem_w);
        if (show_tstates)
            ImGui::TableSetupColumn("TStates", ImGuiTableColumnFlags_WidthFixed, col_tstates_w);

        self->arrow_table_min_y = 0.0f;
        self->arrow_table_max_y = 0.0f;

        for (int i = 0; i < *self->shared_visible_rows; i++)
        {
            char bytes_str[16];
            format_row_bytes_fixed(&rows[i], bytes_str, sizeof(bytes_str));

            /*
             * Zvýraznění řádků:
             * - selected_row: řádek na kterém je focus (modrá selekce)
             * - PC přesná shoda: zelený text + zelený trojúhelník ► ve sloupci ICONS
             * - PC uvnitř instrukce: žlutý text + žlutá šipka → ve sloupci ICONS
             * - Breakpoint ikona ve sloupci ICONS (viz en_DBG_BPT_ROW_STATE)
             */
            bool is_selected = (i == *self->shared_selected_row);
            bool is_pc_exact = (rows[i].addr == current_pc);
            bool is_pc_within = (!is_pc_exact &&
                                 current_pc > rows[i].addr &&
                                 current_pc < rows[i].addr + row_lengths[i]);
            bool is_pc_row = (is_pc_exact || is_pc_within);
            en_DBG_BPT_ROW_STATE bpt_state = self->bpt_row_state[i];

            ImGui::TableNextRow();

            /*
             * PC zvyrazneni - aplikuje se POUZE na ADDR sloupec (= adresa
             * radku nebo symbol). BYTES, MNEM a TSTATES sloupce zustavaji
             * default barvou. Drive bylo zvyrazneni na cely radek pres
             * PushStyleColor + Pop, ale to bylo prilis silne a komplikovalo
             * to push/pop wrap kolem context popup menu. Misto toho lokalne
             * obarvíme jen ADDR text (TextColored) v prislusnem sloupci.
             */

            /*
             * Sloupec 1 (ICONS): Ikony BPT + PC + Selectable.
             *
             * Jeden sloupec (COL_ICONS_WIDTH = 20px) obsahuje obě ikony:
             * - Breakpoint (červený kruh) — kreslí se vlevo (centrum ~6px)
             * - PC indikátor (trojúhelník/šipka) — kreslí se vpravo (centrum ~14px)
             * Pokud jsou obě přítomny, částečně se překrývají.
             *
             * Selectable přes celou šířku řádku (SpanAllColumns) — umístěn
             * v prvním sloupci, ale vizuálně zabírá celý řádek.
             * AllowDoubleClick povoluje detekci double clicku.
             */
            ImGui::TableNextColumn();
            ImVec2 icons_cell_pos = ImGui::GetCursorScreenPos();

            /* Zachycení pozic řádků pro post-render branch arrows.
             * Y = střed řádku, X (společné pro všechny) = levá hrana ICONS
             * sloupce. row_height se nastaví podle prvního řádku (všechny
             * řádky tabulky mají stejnou výšku). */
            {
                float row_h_local = ImGui::GetTextLineHeight()
                                    + ImGui::GetStyle().CellPadding.y * 2.0f;
                self->arrow_row_screen_y[i] = icons_cell_pos.y + row_h_local * 0.5f;
                if (i == 0)
                {
                    self->arrow_row_height = row_h_local;
                    self->arrow_icons_x = icons_cell_pos.x;
                    self->arrow_table_min_y = icons_cell_pos.y;
                };
                self->arrow_table_max_y = icons_cell_pos.y + row_h_local;
            };

            char sel_id[32];
            snprintf(sel_id, sizeof(sel_id), "##dasm_%d", i);
            if (ImGui::Selectable(sel_id, is_selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick))
            {
                /*
                 * Klik na řádek disassembly - selekce + případný double-click
                 * pro Inline Assembler.
                 *
                 * Historicky tu byla automatická pauza emu přes
                 * dbg_autopause_on_interaction() (= klik za běhu emu pozastavil
                 * emu a otevřel info modal). Ta byla potřeba před implementací
                 * dbgapi, kdy nebylo bezpečné modifikovat debugger state za
                 * běhu. S dbgapi je interakce za běhu OK, takže klik se
                 * zpracuje v obou režimech bez auto-pauzy.
                 *
                 * Auto-disable Follow PC: pokud je ON a emu běží, klik na
                 * řádek znamená že uživatel chce manuálně řídit focus -
                 * vypneme follow_pc aby se hned nepřepsalo focus_addr.
                 */
                if (self->follow_pc && !is_paused)
                    dbg_disasm_view_set_follow_pc(self, false);
                *self->shared_selected_row = i;

                /*
                 * Double click → Inline Assembler. Otevírá se i za běhu
                 * emu - dialog je modal a má vlastní cestu pro Apply, která
                 * dokončuje úpravu přes dbgapi.
                 */
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    dbg_iasm_open(rows[i].addr, rows[i].mnemonic, 0, IASM_OPEN_DOUBLECLICK);
                };
            };

            /*
             * Sjednocený per-row tooltip - po ~500 ms hover.
             *
             * Hover detekce se ptá na "last item" = Selectable() nahoře, který
             * má SpanAllColumns, takže pokrývá celou šířku řádku. Tooltip
             * obsahuje instrukční info (popis + T-states + flagy), symbol info
             * pro adresu řádku, symboly v operandech a edit hints.
             *
             * Voláme před IsItemClicked(Right) / BeginPopupContextItem() -
             * IsItemHovered i tyto dotazy si pamatují stejný "last item",
             * takže pořadí dotazů nezáleží, ale tooltip render musí být
             * mimo right-click větev.
             *
             * Per-okno gate (IsWindowHovered): zabrání tooltip leaku mezi
             * souběžně otevřenými disasm instancemi. ImGui sice obsahuje
             * window check uvnitř IsItemHovered, ale hover delay timer
             * (g.HoverItemDelayTimer) je sdílený mezi položkami napříč
             * okny - pokud user navede myš na řádek v sekundárním okně a
             * primary má follow_pc/animaci, primary v některých framech
             * svým iterováním rows[i] přechodně způsobí, že IsItemHovered
             * vrátí true pro primary's row na stejné Y (race v aktualizaci
             * HoveredWindow vs. HoveredRect při následném redraw obou oken).
             * Pre-gate na "myš je v tomto okně" je deterministické řešení
             * nezávislé na pořadí update() v ImGui.
             */
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                                       | ImGuiHoveredFlags_AllowWhenBlockedByPopup)
                && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            {
                /*
                 * Snapshot strategie - drží data tooltipu stabilní po
                 * dobu hover na konkrétní index řádku.
                 *
                 * Důvod: za běhu emu se cache cached_rows[] přepisuje
                 * každých DBG_REFRESH_INTERVAL_MS, navíc dochází k race
                 * mezi UI vláknem (debug read) a emu vláknem (banking
                 * switch). Důsledek:
                 *   - bytes[1..n-1] mohou být přečtené z různých bank
                 *     mappingů ve dvou framech
                 *   - inst.length může flickovat (1 vs 3 vs ...)
                 *   - layout následujících řádků se posune (rows[i+1].addr
                 *     se změní), takže "stejný i" může v různých framech
                 *     reprezentovat různou adresu
                 *
                 * Snapshot se zachytí při prvním hover na index i a drží
                 * dokud user nezmění hovered index. Nezruší se při
                 * address-shift v rows[i] - tooltip drží originální adresu
                 * a inst, i když se rows[i] po cache rebuild posune.
                 *
                 * Invalidace na opuštění řádku se ne-řeší v else větvi -
                 * IsItemHovered s DelayNormal stejně tooltip skryje a
                 * příští hover (i na stejný i) přichází po znovu-aktivaci
                 * delay timeru, což sám ImGui detekuje jako "fresh hover".
                 * Reset děláme jen při změně indexu.
                 */
                if (self->tooltip_snapshot_row != i)
                {
                    self->tooltip_snapshot = rows[i];
                    self->tooltip_snapshot_row = i;
                };
                render_row_tooltip(&self->tooltip_snapshot);
            };
            /*
             * Pozn.: snapshot se NEinvaliduje když IsWindowHovered/IsItemHovered
             * vrátí false. ImGui může intermittently shodit hover stav (např.
             * modal popup, briefly out-of-window kurzor, race v aktualizaci
             * HoveredWindow), což by způsobilo invalidaci a při dalším true
             * by se snapshot zachytil z aktuálních (změněných) rows[i] dat
             * = flicker. Snapshot zůstává platný dokud user nezmění hovered
             * řádek (= jiný i). Když user změní index, předchozí snapshot
             * se přepíše. Pokud user opustí okno a vrátí se na stejný řádek,
             * uvidí původní snapshot - drobné nezvyklé chování (data nemusí
             * reflektovat aktuální stav paměti), ale pre-cycle stability
             * tooltipu má vyšší prioritu pro debugger UX.
             */

            /*
             * Pravý klik — selekce řádku (stejně jako levý klik).
             * Musí být před BeginPopupContextItem(), aby se řádek selektoval
             * ještě před otevřením context menu.
             */
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                /* Pravý klik: selekce řádku v obou režimech (auto-pauza
                 * odstraněna, viz komentář u levého kliku výše).
                 * Auto-disable Follow PC stejně jako u LMB - viz výše. */
                if (self->follow_pc && !is_paused)
                    dbg_disasm_view_set_follow_pc(self, false);
                *self->shared_selected_row = i;
            };

            /*
             * Context menu (pravé tlačítko myši) — dostupné pouze v editačním režimu.
             *
             * Položky:
             * 1. Set/Remove Breakpoint — vytvoří/smaže BPT na adrese řádku
             * 2. Enable/Disable Breakpoint — přepne enabled (jen pokud BPT existuje)
             * 3. Set as PC — nastaví regPC na adresu řádku
             * 4. Focus to <target> — focus na cíl branch instrukce (jen pokud
             *    má fixed target, viz z80_dasm_target_addr())
             * 5. Focus To... — otevře dialog pro zadání adresy focusu
             * 6. Focus to PC — vrátí focus na aktuální regPC
             * 7. Focus to register ▶ — submenu s 16-bit registry/páry
             *    (AF, BC, DE, HL, AF', BC', DE', HL', IX, IY, SP); klik =
             *    focus dolní tabulky na hodnotu registru jako adresu
             * 8. Edit row — otevře Inline Assembler
             */
            if (ImGui::BeginPopupContextItem())
            {
                /* V1.7: Najdi všechny BP relevantní pro tuto adresu
                 * (PC_EXEC, MEM_R/W vč. range/mask, GLOBAL). Větvíme dle
                 * počtu - 0 / 1 / >1 - jednotlivé scénáře viz níže.
                 * GLOBAL BP jsou zde technicky vždy "match" - filtrujeme
                 * z menu klasické toggle akce, ať uživatele nemate. */
                GArray *bp_ids_all = g_array_new(FALSE, FALSE, sizeof(int));
                breakpoints_find_all_by_addr(rows[i].addr, bp_ids_all);

                /* Pro menu chceme jen BP které mají adresu (= PC_EXEC,
                 * MEM_R/W). GLOBAL přeskakujeme - menu na disasm řádku
                 * je addr-centric. */
                GArray *bp_ids_addr = g_array_new(FALSE, FALSE, sizeof(int));
                for (guint k = 0; k < bp_ids_all->len; k++)
                {
                    int bp_id = g_array_index(bp_ids_all, int, k);
                    st_BPT *bp = breakpoints_find_by_id(bp_id);
                    if (!bp) continue;
                    if (bp->type == BPT_TYPE_PC_EXEC ||
                        bp->type == BPT_TYPE_MEM_R ||
                        bp->type == BPT_TYPE_MEM_W)
                    {
                        g_array_append_val(bp_ids_addr, bp_id);
                    };
                };
                g_array_free(bp_ids_all, TRUE);

                int n_addr = (int)bp_ids_addr->len;

                if (n_addr == 0)
                {
                    /* === 0 BP - klasický Set EXEC BP === */
                    if (ImGui::MenuItem(_L("Set Breakpoint")))
                    {
                        dbg_ui_bp_add(rows[i].addr, NULL);
                        dbg_refresh_request();
                    };
                }
                else if (n_addr == 1)
                {
                    /* === 1 BP - dnešní chování (toggle, enable/disable) === */
                    int bp_id = g_array_index(bp_ids_addr, int, 0);
                    st_BPT *bp = breakpoints_find_by_id(bp_id);
                    bool is_eff_enabled = bp ? breakpoints_is_effectively_enabled(bp_id) : false;

                    if (ImGui::MenuItem(_L("Remove Breakpoint")))
                    {
                        dbg_ui_bp_remove(bp_id);
                        dbg_refresh_request();
                    };
                    if (bp)
                    {
                        if (ImGui::MenuItem(_L("Disable Breakpoint"), NULL, false, is_eff_enabled))
                        {
                            dbg_ui_bp_set_enabled(bp_id, false);
                            dbg_refresh_request();
                        };
                        if (ImGui::MenuItem(_L("Enable Breakpoint"), NULL, false, !is_eff_enabled))
                        {
                            dbg_ui_bp_set_enabled(bp_id, true);
                            dbg_refresh_request();
                        };
                        if (ImGui::MenuItem(_L("Edit Breakpoint...")))
                        {
                            bpt_edit_panel_open(BPT_ITEM_EVENT, bp_id);
                            g_gui->showBreakpointsWindow = true;
                        };
                    };
                }
                else
                {
                    /* === >1 BP na adrese ===
                     * Pro každý BP submenu se Quick action (enable / disable
                     * / remove / edit). Plus "Show all in BP List" jako
                     * shortcut na addr-filter v BP window (= aktivace
                     * g_bpt_ui.pending_filter_*). */
                    char hdr[64];
                    snprintf(hdr, sizeof(hdr),
                             _("Show %d Breakpoints at 0x%04X in BP List"),
                             n_addr, rows[i].addr);
                    if (ImGui::MenuItem(hdr))
                    {
                        g_bpt_ui.pending_filter_addr = rows[i].addr;
                        g_bpt_ui.pending_filter_active = true;
                        g_gui->showBreakpointsWindow = true;
                    };

                    if (ImGui::BeginMenu(_L("Quick action on...")))
                    {
                        for (int k = 0; k < n_addr; k++)
                        {
                            int bp_id = g_array_index(bp_ids_addr, int, k);
                            st_BPT *bp = breakpoints_find_by_id(bp_id);
                            if (!bp) continue;
                            char sub_label[96];
                            snprintf(sub_label, sizeof(sub_label),
                                     "#%d %s%s%s",
                                     bp_id,
                                     bpt_type_to_string(bp->type),
                                     (bp->name && bp->name[0] && !bp->auto_name) ? " - " : "",
                                     (bp->name && bp->name[0] && !bp->auto_name) ? bp->name : "");
                            if (ImGui::BeginMenu(sub_label))
                            {
                                bool is_eff = breakpoints_is_effectively_enabled(bp_id);
                                if (ImGui::MenuItem(_L("Disable"), NULL, false, is_eff))
                                {
                                    dbg_ui_bp_set_enabled(bp_id, false);
                                    dbg_refresh_request();
                                };
                                if (ImGui::MenuItem(_L("Enable"), NULL, false, !is_eff))
                                {
                                    dbg_ui_bp_set_enabled(bp_id, true);
                                    dbg_refresh_request();
                                };
                                ImGui::Separator();
                                if (ImGui::MenuItem(_L("Edit...")))
                                {
                                    bpt_edit_panel_open(BPT_ITEM_EVENT, bp_id);
                                    g_gui->showBreakpointsWindow = true;
                                };
                                if (ImGui::MenuItem(_L("Remove")))
                                {
                                    dbg_ui_bp_remove(bp_id);
                                    dbg_refresh_request();
                                };
                                ImGui::EndMenu();
                            };
                        };
                        ImGui::EndMenu();
                    };

                    ImGui::Separator();

                    /* Convenience - hromadné akce na všech BP na adrese. */
                    if (ImGui::MenuItem(_L("Disable all at this address")))
                    {
                        for (int k = 0; k < n_addr; k++)
                        {
                            int bp_id = g_array_index(bp_ids_addr, int, k);
                            dbg_ui_bp_set_enabled(bp_id, false);
                        };
                        dbg_refresh_request();
                    };
                    if (ImGui::MenuItem(_L("Enable all at this address")))
                    {
                        for (int k = 0; k < n_addr; k++)
                        {
                            int bp_id = g_array_index(bp_ids_addr, int, k);
                            dbg_ui_bp_set_enabled(bp_id, true);
                        };
                        dbg_refresh_request();
                    };
                    if (ImGui::MenuItem(_L("Add another Execution Breakpoint")))
                    {
                        dbg_ui_bp_add(rows[i].addr, NULL);
                        dbg_refresh_request();
                    };
                };

                g_array_free(bp_ids_addr, TRUE);

                ImGui::Separator();

                if (ImGui::MenuItem(_L("Set as PC")))
                {
                    dbg_ui_set_reg((uint8_t)Z80_REG_PC, rows[i].addr);
                    *self->shared_focus_addr = rows[i].addr;
                    *self->shared_selected_row = 0;
                    dbg_refresh_request();
                };

                ImGui::Separator();

                /*
                 * Focus to <target> - jen pro branch instrukce s fixed
                 * targetem (CALL/JP/JR/DJNZ/RST). z80_dasm_target_addr()
                 * vrátí (uint16_t)-1 pro JP (HL/IX/IY), RET, non-branch a
                 * v takovém případě položku nezobrazujeme. Label obsahuje
                 * cílovou adresu a (pokud existuje) symbol jméno.
                 */
                {
                    uint16_t target = z80_dasm_target_addr(&rows[i].inst);
                    if (target != (uint16_t)-1)
                    {
                        char target_label[64];
                        format_focus_target_label(target_label,
                                                  sizeof(target_label),
                                                  target, 0);
                        if (ImGui::MenuItem(target_label))
                        {
                            dbg_disasm_view_focus_to(self, target);
                        };
                    };
                }

                if (ImGui::MenuItem(_L("Focus To...")))
                {
                    /*
                     * Předáváme self - dialog si pointer uloží a při Apply
                     * zapíše focus_addr do TÉTO instance (= toho okna,
                     * odkud uživatel context menu otevřel), ne do hlavního
                     * view. Pro main view to chování zůstává shodné, protože
                     * shared_focus_addr ukazuje na g_dbg_ui.focus_addr.
                     */
                    dbg_focus_to_open(self);
                };

                if (ImGui::MenuItem(_L("Focus to PC")))
                {
                    dbg_disasm_view_focus_to(self, g_mzarch_main.cpu->pc);
                };

                /*
                 * Focus to register submenu - 11 položek (AF, BC, DE, HL
                 * primární; AF', BC', DE', HL' shadow; IX, IY, SP). Klik
                 * = focus dolní tabulky na hodnotu registru jako adresu.
                 *
                 * Hodnoty se snapshotují při otevření menu (= ne live update)
                 * - menu se renderuje v UI vlákně, takže CPU registry mohou
                 * být v okamžiku otevření měněny emulátorovým vláknem.
                 * Per-call lokální buffer brání aliasingu, hodnota se
                 * zachytí v okamžiku otevření submenu.
                 */
                if (ImGui::BeginMenu(_L("Focus to register")))
                {
                    z80_t *cpu = g_mzarch_main.cpu;
                    char reg_label[64];

                    /* Primární páry */
                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "AF", cpu->af.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->af.w);
                    };

                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "BC", cpu->bc.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->bc.w);
                    };

                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "DE", cpu->de.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->de.w);
                    };

                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "HL", cpu->hl.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->hl.w);
                    };

                    ImGui::Separator();

                    /* Shadow páry (af2/bc2/de2/hl2 = AF'/BC'/DE'/HL') */
                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "AF'", cpu->af2.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->af2.w);
                    };

                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "BC'", cpu->bc2.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->bc2.w);
                    };

                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "DE'", cpu->de2.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->de2.w);
                    };

                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "HL'", cpu->hl2.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->hl2.w);
                    };

                    ImGui::Separator();

                    /* Index registry + SP */
                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "IX", cpu->ix.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->ix.w);
                    };

                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "IY", cpu->iy.w, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->iy.w);
                    };

                    format_focus_reg_label(reg_label, sizeof(reg_label),
                                           "SP", cpu->sp, 0);
                    if (ImGui::MenuItem(reg_label))
                    {
                        dbg_disasm_view_focus_to(self, cpu->sp);
                    };

                    ImGui::EndMenu();
                };

                /*
                 * Show in ▶ submenu - 5 položek pro otevření / focus na
                 * konkrétní disasm instanci. Aktuální instance (= ze které
                 * je menu vyvoláno) je vykreslena jako disabled.
                 *
                 * Mapování:
                 *   - "Disassembly #1" = main instance (slot 0)
                 *   - "Disassembly #N" pro N=2..5 = sekundární okno
                 *     (slot 1..4)
                 *
                 * Vlastní logika ensure-open + focus + auto-disable
                 * follow_pc je ve sjednoceném helperu
                 * dbg_disasm_show_in_slot.
                 */
                if (ImGui::BeginMenu(_L("Show in")))
                {
                    uint16_t target_addr = rows[i].addr;
                    bool is_self_main = (self->window_id != NULL
                        && strcmp(self->window_id, "main") == 0);

                    /* #1 - main (slot 0) */
                    ImGui::BeginDisabled(is_self_main);
                    if (ImGui::MenuItem(_L("Disassembly #1")))
                    {
                        dbg_disasm_show_in_slot(0, target_addr);
                    };
                    ImGui::EndDisabled();

                    /* #2 .. #5 - extra okna (slot 1..4) */
                    static const char *const slot_labels[DBG_EXTRA_DISASM_COUNT] = {
                        N_("Disassembly #2"),
                        N_("Disassembly #3"),
                        N_("Disassembly #4"),
                        N_("Disassembly #5"),
                    };
                    for (int k = 0; k < DBG_EXTRA_DISASM_COUNT; k++)
                    {
                        char self_id[2] = { (char)('2' + k), '\0' };
                        bool is_self_slot = (self->window_id != NULL
                            && strcmp(self->window_id, self_id) == 0);
                        ImGui::BeginDisabled(is_self_slot);
                        if (ImGui::MenuItem(_(slot_labels[k])))
                        {
                            dbg_disasm_show_in_slot(1 + k, target_addr);
                        };
                        ImGui::EndDisabled();
                    };

                    ImGui::EndMenu();
                };

                /*
                 * Add to bookmarks - rychlé přidání aktuálního řádku do
                 * Bookmarks. Pokud existuje symbol pro adresu, použije
                 * jeho jméno (= robustní vůči pozdějším změnám layoutu);
                 * jinak hex literál. Comment je prázdný - user může
                 * doplnit v Bookmarks okně. Po přidání se Bookmarks okno
                 * otevře (= visual feedback uživateli).
                 */
                if (ImGui::MenuItem(_L("Add to bookmarks")))
                {
                    const st_SYMBOL *sym = sym_db_lookup_by_addr(rows[i].addr, 0);
                    char input_buf[64];
                    if (sym && sym->name && sym->name[0])
                    {
                        snprintf(input_buf, sizeof(input_buf), "%s", sym->name);
                    }
                    else
                    {
                        snprintf(input_buf, sizeof(input_buf), "#%04X", rows[i].addr);
                    };
                    bookmarks_add(input_buf, "");
                    g_gui->showBookmarksWindow = true;
                };

                ImGui::Separator();

                if (ImGui::MenuItem(_L("Edit row")))
                {
                    dbg_iasm_open(rows[i].addr, rows[i].mnemonic, 0, IASM_OPEN_DOUBLECLICK);
                };

                ImGui::EndPopup();
            };

            /*
             * Ikony v jednom sloupci — breakpoint vlevo, PC vpravo.
             * Kreslíme přes ImDrawList na pozici buňky sloupce ICONS.
             *
             * Levý slot (breakpoint):
             *   ● červený plný   = aktivní BPT na počáteční adrese
             *   ○ bílý prázdný   = deaktivovaný BPT na počáteční adrese
             *   ● žlutý plný    = aktivní BPT uvnitř instrukce
             *   ○ žlutý prázdný = deaktivovaný BPT uvnitř instrukce
             *
             * Pravý slot (PC indikátor):
             *   ► zelený trojúhelník = regPC == addr
             *   → žlutá šipka       = regPC uvnitř instrukce
             */
            {
                float row_h = ImGui::GetTextLineHeight();
                ImDrawList *dl = ImGui::GetWindowDrawList();

                /* Pravá část ICONS sloupce začíná za prostorem pro arrow shaft.
                 * Pokud jsou branch arrows vypnuté, offset = 0 = původní layout. */
                float bpt_pc_x0 = icons_cell_pos.x + icons_bpt_pc_xoffset();

                /* Breakpoint ikona — vlevo v BPT/PC slotu (centrum ~6px) */
                if (bpt_state != DBG_BPT_NONE)
                {
                    float radius = row_h * 0.3f;
                    ImVec2 center(bpt_pc_x0 + 6.0f,
                                  icons_cell_pos.y + row_h * 0.5f);

                    switch (bpt_state)
                    {
                    case DBG_BPT_START_ENABLED:
                        /* Červený plný kruh — aktivní BPT na počáteční adrese */
                        dl->AddCircleFilled(center, radius, IM_COL32(255, 48, 48, 255));
                        break;
                    case DBG_BPT_START_DISABLED:
                        /* Bílý prázdný kruh — deaktivovaný BPT na počáteční adrese */
                        dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 255), 0, 1.5f);
                        break;
                    case DBG_BPT_INNER_ENABLED:
                        /* Žlutý plný kruh — aktivní BPT uvnitř instrukce */
                        dl->AddCircleFilled(center, radius, IM_COL32(255, 255, 0, 255));
                        break;
                    case DBG_BPT_INNER_DISABLED:
                        /* Žlutý prázdný kruh — deaktivovaný BPT uvnitř instrukce */
                        dl->AddCircle(center, radius, IM_COL32(255, 255, 0, 255), 0, 1.5f);
                        break;
                    default:
                        break;
                    };
                };

                /* PC indikátor — vpravo v BPT/PC slotu (centrum ~14px) */
                if (is_pc_exact)
                {
                    /* Zelený trojúhelník ► — regPC ukazuje přesně na začátek instrukce */
                    float arrow_h = row_h * 0.35f;
                    float arrow_w = row_h * 0.4f;
                    float cx = bpt_pc_x0 + 14.0f;
                    float cy = icons_cell_pos.y + row_h * 0.5f;
                    ImVec2 p1(cx - arrow_w * 0.5f, cy - arrow_h);
                    ImVec2 p2(cx + arrow_w * 0.5f, cy);
                    ImVec2 p3(cx - arrow_w * 0.5f, cy + arrow_h);
                    dl->AddTriangleFilled(p1, p2, p3, IM_COL32(0, 255, 0, 255));
                }
                else if (is_pc_within)
                {
                    /* Žlutá šipka → — regPC ukazuje dovnitř instrukce */
                    float arrow_h = row_h * 0.25f;
                    float line_start = bpt_pc_x0 + 3.0f;
                    float line_end = bpt_pc_x0 + COL_ICONS_BASE_WIDTH - 3.0f;
                    float cy = icons_cell_pos.y + row_h * 0.5f;
                    ImU32 yellow = IM_COL32(255, 255, 0, 255);
                    dl->AddLine(ImVec2(line_start, cy), ImVec2(line_end - 3, cy), yellow, 2.0f);
                    dl->AddTriangleFilled(
                        ImVec2(line_end - 5, cy - arrow_h),
                        ImVec2(line_end, cy),
                        ImVec2(line_end - 5, cy + arrow_h),
                        yellow);
                };
            };

            /*
             * Sloupec 2 (ADDR): Adresa instrukce.
             * Pokud existuje symbol pro tuto adresu (D.8 Symbol DB), zobrazí se
             * jméno symbolu místo hex. Detailní info (adresa, comment, modul)
             * se zobrazí v sjednoceném per-row tooltipu (viz render_row_tooltip()).
             *
             * PC zvyrazneni: lokalni TextColored pro is_pc_exact (zelena) /
             * is_pc_within (zluta). BYTES, MNEM, TSTATES sloupce zustavaji
             * default barvou.
             */
            ImGui::TableNextColumn();
            const st_SYMBOL *sym = sym_db_lookup_by_addr(rows[i].addr, 0);
            ImVec4 addr_color;
            bool addr_colored = false;
            if (is_pc_exact)
            {
                addr_color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                addr_colored = true;
            }
            else if (is_pc_within)
            {
                addr_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
                addr_colored = true;
            };
            if (sym && sym->name)
            {
                if (addr_colored)
                    ImGui::TextColored(addr_color, "%s", sym->name);
                else
                    ImGui::TextUnformatted(sym->name);
            }
            else
            {
                if (addr_colored)
                    ImGui::TextColored(addr_color, "%04X:", rows[i].addr);
                else
                    ImGui::Text("%04X:", rows[i].addr);
            };

            /* Sloupec 3 (BYTES): Bytekódy instrukce - 2-tone barveni
             * (opcode default, operand cyan). bytes_str se nepouzije, render
             * je per-byte pres render_bytes_column(). */
            ImGui::TableNextColumn();
            render_bytes_column(&rows[i]);

            /* Sloupec 4 (MNEM): Z80 mnemonic - kategoricke barveni mnemoniky
             * (= prvni slovo) podle typu instrukce (flow, arith, I/O, ...).
             * Operandy zustavaji default barvou. */
            ImGui::TableNextColumn();
            render_mnem_column(rows[i].mnemonic);

            /* Sloupec 5 (TSTATES, volitelný): počet T-stavů.
             * Pro non-branch (t_states2 == 0): jediné číslo.
             * Pro podmíněné větvení (t_states2 != 0): notace "taken/not"
             * = "t_states2/t_states" (taken je vyšší hodnota).
             *
             * Příklady (ověřeno proti z80_dasm_tables.c):
             *   NOP            -> "4"
             *   CALL nn        -> "17"
             *   CALL Z,nn      -> "17/10"
             *   JR e           -> "12"
             *   JR cc,e        -> "12/7"
             *   DJNZ e         -> "13/8"
             *   LDIR           -> "21/16"
             *   ADD A,r        -> "4"
             *   IN A,(n)       -> "11"
             *   EX (SP),HL     -> "19"
             */
            if (show_tstates)
            {
                ImGui::TableNextColumn();
                const z80_dasm_inst_t *ri = &rows[i].inst;
                if (ri->t_states2 != 0)
                {
                    ImGui::Text("%u/%u",
                                (unsigned) ri->t_states2,
                                (unsigned) ri->t_states);
                }
                else
                {
                    ImGui::Text("%u", (unsigned) ri->t_states);
                };
            };
        };

        ImGui::EndTable();

        /* Po EndTable: vizualizace skoků (branch arrows) v ICONS gutteru.
         * Volá se nezávisle na is_paused - šipky jsou užitečné i během
         * animation režimu, kdy uživatel sleduje běh emulace. */
        render_branch_arrows(self, rows, *self->shared_visible_rows);
    };

    /*
     * Navigace klávesnicí — vyžaduje focus okna disasm. Funguje v obou
     * režimech (animace i pauza); historicky byla gated na is_paused
     * z důvodu před-dbgapi bezpečnosti, dnes je modifikace focus_addr
     * za běhu OK.
     *
     * UP/DOWN mají dvou-fázové chování:
     * 1. Primárně posunují selekci (selected_row) v rámci viditelných řádků.
     * 2. Teprve až je selekce na okraji (řádek 0 nebo poslední), změní
     *    se focus_addr — scrolluje se obsah tabulky:
     *    - Dolů: focus_addr += row_lengths[0] (délka první instrukce,
     *      která odscrolluje z viditelné oblasti).
     *    - Nahoru: focus_addr -= 1 (posun o 1 bajt, protože zpětný
     *      disassembly Z80 je nejednoznačný — variabilní délka 1–4 B).
     */
    if (ImGui::IsWindowFocused())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
        {
            /* DOWN fáze 1: posun selekce dolů v rámci viditelných řádků */
            if (*self->shared_selected_row < *self->shared_visible_rows - 1)
            {
                (*self->shared_selected_row)++;
            }
            else
            {
                /*
                 * DOWN fáze 2: selekce je na posledním řádku — scrollování obsahu.
                 * focus_addr += délka první instrukce, která odscrolluje nahoru
                 * z viditelné oblasti. Nová instrukce se objeví dole.
                 */
                *self->shared_focus_addr = (uint16_t)(*self->shared_focus_addr + row_lengths[0]);
                dbg_refresh_request();
            };
        };

        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
        {
            /* UP fáze 1: posun selekce nahoru v rámci viditelných řádků */
            if (*self->shared_selected_row > 0)
            {
                (*self->shared_selected_row)--;
            }
            else
            {
                /*
                 * UP fáze 2: selekce je na prvním řádku — scrollování obsahu.
                 * focus_addr -= 1 bajt. Posun je jen o 1, protože zpětný
                 * disassembly Z80 je nejednoznačný (variabilní délka 1–4 B).
                 * Nová instrukce se objeví nahoře.
                 */
                *self->shared_focus_addr = (uint16_t)(*self->shared_focus_addr - 1);
                dbg_refresh_request();
            };
        };

        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
        {
            if (ImGui::GetIO().KeyCtrl)
            {
                /* Ctrl+PgDown: skok blízko konce paměti */
                *self->shared_focus_addr = 0xFFF0;
                *self->shared_selected_row = 0;
            }
            else
            {
                /*
                 * PgDown: posun o celou stránku dolů.
                 * Nový focus = adresa posledního řádku aktuální stránky.
                 */
                *self->shared_focus_addr = rows[*self->shared_visible_rows - 1].addr;
                *self->shared_selected_row = 0;
            };
            dbg_refresh_request();
        };

        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
        {
            if (ImGui::GetIO().KeyCtrl)
            {
                /* Ctrl+PgUp: skok na začátek paměti */
                *self->shared_focus_addr = 0x0000;
                *self->shared_selected_row = 0;
            }
            else
            {
                /*
                 * PgUp: posun o celou stránku nahoru.
                 * Odhad: průměrná délka Z80 instrukce je ~2 bajty,
                 * takže se posuneme o přibližně 20*2 = 40 bajtů zpět.
                 */
                int offset = *self->shared_visible_rows * 2;
                *self->shared_focus_addr = (uint16_t)(*self->shared_focus_addr - offset);
                *self->shared_selected_row = 0;
            };
            dbg_refresh_request();
        };

        /*
         * Enter → Inline Assembler (placeholder).
         * V budoucnu se otevře modální dialog pro editaci instrukce
         * na adrese aktivního řádku.
         */
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false))
        {
            int sel = *self->shared_selected_row;
            if (sel >= 0 && sel < *self->shared_visible_rows)
                dbg_iasm_open(rows[sel].addr, rows[sel].mnemonic, 0, IASM_OPEN_DOUBLECLICK);
        };

        /*
         * Detekce psaní — otevření inline assembleru prvním znakem instrukce.
         * Validní první znaky Z80 instrukcí: A, B, C, D, E, H, I, J, L, N, O, P, R, S, X
         * (odpovídají prvním písmenům všech Z80 mnemoniků).
         */
        if (ImGui::IsWindowFocused()
            && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeySuper)
        {
            static const ImGuiKey valid_keys[] = {
                ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E,
                ImGuiKey_H, ImGuiKey_I, ImGuiKey_J, ImGuiKey_L, ImGuiKey_N,
                ImGuiKey_O, ImGuiKey_P, ImGuiKey_R, ImGuiKey_S, ImGuiKey_X
            };
            static const char valid_chars[] = "ABCDEHIJLNOPRSX";

            for (int k = 0; k < IM_ARRAYSIZE(valid_keys); k++)
            {
                if (ImGui::IsKeyPressed(valid_keys[k], false))
                {
                    int sel = *self->shared_selected_row;
                    if (sel >= 0 && sel < *self->shared_visible_rows)
                        dbg_iasm_open(rows[sel].addr, NULL, valid_chars[k], IASM_OPEN_TYPING);
                    break;
                };
            };
        };
    };

    /*
     * Kolečko myši — totožné dvou-fázové chování jako UP/DOWN šipky.
     *
     * Fáze 1: Primárně posunuje selekci (selected_row) v rámci viditelných řádků.
     * Fáze 2: Teprve při dosažení okraje scrolluje obsah změnou focus_addr:
     *   - Nahoru: focus_addr -= 1 (zpětný disassembly nejednoznačný, posun o 1 B)
     *   - Dolů: focus_addr += row_lengths[0] (délka první instrukce, která odscrolluje)
     *
     * Reaguje na hover (nemusí být focus okna) — přirozenější pro myš.
     * NoScrollWithMouse flag na childu zabraňuje ImGui v konzumaci wheel eventů.
     */
    if (ImGui::IsWindowHovered())
    {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.0f)
        {
            /* Kolečko nahoru — fáze 1: posun selekce */
            if (*self->shared_selected_row > 0)
            {
                (*self->shared_selected_row)--;
            }
            else
            {
                /* Fáze 2: scrollování obsahu — focus_addr -= 1 */
                *self->shared_focus_addr = (uint16_t)(*self->shared_focus_addr - 1);
                dbg_refresh_request();
            };
        }
        else if (wheel < 0.0f)
        {
            /* Kolečko dolů — fáze 1: posun selekce */
            if (*self->shared_selected_row < *self->shared_visible_rows - 1)
            {
                (*self->shared_selected_row)++;
            }
            else
            {
                /* Fáze 2: scrollování obsahu — focus_addr += délka první instrukce */
                *self->shared_focus_addr = (uint16_t)(*self->shared_focus_addr + row_lengths[0]);
                dbg_refresh_request();
            };
        };
    };

    ImGui::EndChild();

    /*
     * Per-table tooltip s edit hints byl nahrazen sjednoceným per-row
     * tooltipem (viz render_row_tooltip()) - hover na konkrétním řádku
     * zobrazí instrukční info, symbol info i edit hints najednou.
     */
}

/* =========================================================================
 * HLAVNÍ RENDER FUNKCE INSTANCE
 * =========================================================================
 *
 * Sestaví celou sekci Disassembled instance:
 * 1. Obalový child s rámečkem a fixní šířkou (shrink-to-content, nikdy neexpanduje)
 * 2. Nadpis sekce — SeparatorText("Disassembled") zarovnaný na střed
 * 3. Horní tabulka (historie) — výška = (section_height - nadpis) * split_ratio
 *    (vykreslí se pouze pokud self->enable_history)
 * 4. Splitter (posuvník mezi tabulkami) - pouze pokud enable_history
 * 5. Dolní tabulka (disassembly) — výška = zbytek
 *
 * Obě tabulky používají zmenšený font (DBG_CONTENT_FONT_SIZE_OFFSET)
 * definovaný v debugger_state.h — jednotné nastavení pro celou středovou část.
 *
 * Šířka sekce se počítá z šířek sloupců dolní tabulky (se zmenšeným fontem)
 * + padding. Výška expanduje podle parametru section_height.
 */
void dbg_disasm_view_render(DisassembledView *self, float section_height)
{
    /*
     * PushID(window_id) - prefix všech vnitřních ImGui IDs unikátním ID
     * instance. Zaručí, že ID vnitřních widgetů ("##dbg_section",
     * "##dbg_disasm", "##dasm_table", "##dasm_<i>" atd.) nebudou kolidovat
     * mezi hlavní instancí a sekundárními okny renderovanými ve stejném
     * framu. Pro singleton "main" je prefix neutrální (žádný viditelný
     * efekt). PopID() musí být párový před returnem.
     */
    ImGui::PushID(self->window_id ? self->window_id : "main");

    bool is_paused = EMULATOR_TEST_PAUSED;

    /* Synchronizace symtab cache - volá se 1× per render frame, no-op pokud
     * sym_db verze odpovídá. Bridge sloupec je sdílený pro obě tabulky
     * i pro stateless dbg_dasm_get_line(). */
    sync_dasm_symtab();

    /*
     * Výpočet celkové šířky sekce (shrink-to-content).
     *
     * Šířka = součet sloupců dolní tabulky + padding.
     * Dolní tabulka má zmenšený font (DBG_CONTENT_FONT_SIZE_OFFSET), proto
     * počítáme char_w se zmenšením. Horní tabulka (historie) používá
     * normální font, ale má stretch sloupec — přizpůsobí se.
     */
    float base_font_size = ImGui::GetFontSize();
    float dasm_font_scale = (base_font_size + DBG_CONTENT_FONT_SIZE_OFFSET) / base_font_size;
    if (dasm_font_scale < 0.5f)
        dasm_font_scale = 0.5f;
    float char_w = ImGui::CalcTextSize("0").x * dasm_font_scale;
    int dasm_n_cols = self->show_tstates ? 5 : 4;
    float text_chars = (float)(DASM_ADDR_CHARS + DASM_BYTES_CHARS + DASM_MNEM_CHARS);
    if (self->show_tstates)
        text_chars += (float) DASM_TSTATES_CHARS;
    float table_content_w = COL_ICONS_WIDTH + char_w * text_chars;
    /* Přičteme tabulkový padding: dasm_n_cols sloupců × 2 × CellPadding.x */
    table_content_w += (float) dasm_n_cols * 2.0f * ImGui::GetStyle().CellPadding.x;
    /* Přičteme padding child okna (tabulky jsou uvnitř childu s borders) */
    table_content_w += ImGui::GetStyle().WindowPadding.x * 2;
    /* Šířka sekce = tabulky + mezera + slider */
    float section_width = table_content_w + ImGui::GetStyle().ItemSpacing.x + SLIDER_WIDTH;

    /*
     * Obalový child — fixní šířka, sekce nikdy neexpanduje horizontálně.
     * Výška se přebírá z parametru section_height.
     * ImGuiChildFlags_Borders kreslí rámeček kolem celé sekce
     * (odpovídá GTK Frame ze starého debuggeru).
     */
    ImGui::BeginChild("##dbg_section", ImVec2(section_width, section_height),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    /*
     * Hlavička sekce - rich layout:
     *   [text entry: Address or symbol] [Follow PC] [T-states]
     *
     * Text entry: Enter = parse + jump na adresu / symbol. Pro hlavní
     * instanci je "Follow PC" disabled (= UI nedovolí vypnout, hodnota
     * vždy true). "T-states" přepíná per-instance show_tstates.
     *
     * Implementace v render_disasm_header() níže (free function).
     */
    float header_y_before = ImGui::GetCursorPosY();
    render_disasm_header(self);
    float header_y_after = ImGui::GetCursorPosY();
    float title_height = header_y_after - header_y_before + ImGui::GetStyle().ItemSpacing.y;

    /*
     * Výpočet výšek tabulek na základě split_ratio.
     * Odečteme výšku nadpisu, splitteru a drobné mezery.
     */
    float remaining_height = section_height - title_height;
    float usable_height = remaining_height - SPLITTER_HEIGHT - ImGui::GetStyle().ItemSpacing.y * 2;

    /*
     * Při prvním otevření vypočítáme split_ratio tak, aby v horní tabulce
     * bylo vidět přesně 3 řádky. Používáme zmenšený font (stejný jako tabulka).
     * Příznak je per-instance (initial_ratio_set), takže se přepočet provede
     * jednou per instance.
     */
    if (self->enable_history && !self->initial_ratio_set && usable_height > 0.0f)
    {
        float row_h = ImGui::GetTextLineHeight() * dasm_font_scale
                      + ImGui::GetStyle().CellPadding.y * 2.0f;
        /* 3 řádky + padding child okna (borders) */
        float desired_h = row_h * 3.0f + ImGui::GetStyle().WindowPadding.y * 2.0f;
        float ratio = desired_h / usable_height;
        if (ratio < 0.05f) ratio = 0.05f;
        if (ratio > 0.8f) ratio = 0.8f;
        *self->shared_history_split_ratio = ratio;
        self->initial_ratio_set = true;
    };

    float history_height = usable_height * (*self->shared_history_split_ratio);
    float disasm_height = usable_height * (1.0f - *self->shared_history_split_ratio);

    /* Zajistíme minimální výšku pro obě tabulky */
    if (history_height < MIN_TABLE_HEIGHT)
        history_height = MIN_TABLE_HEIGHT;
    if (disasm_height < MIN_TABLE_HEIGHT)
        disasm_height = MIN_TABLE_HEIGHT;

    /*
     * Layout pod nadpisem: vlevo tabulky+splitter, vpravo vertikální slider.
     *
     * Tabulky (historie + splitter + disassembly) zabírají levou část,
     * slider (šoupátko) je napravo a sahá přes celou výšku obou tabulek.
     */

    /* Šířka tabulek = dostupná šířka minus slider a mezera */
    float tables_width = ImGui::GetContentRegionAvail().x - SLIDER_WIDTH - ImGui::GetStyle().ItemSpacing.x;

    /* Levá část — tabulky a splitter v child okně (kvůli horizontálnímu layoutu) */
    ImGui::BeginChild("##dbg_tables", ImVec2(tables_width, remaining_height),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (self->enable_history)
    {
        /*
         * Horní tabulka — historie vykonaných instrukcí.
         * V animačním režimu se aktualizuje s každým framem.
         * V editačním režimu je zamrzlá na posledním stavu.
         */
        render_history_table(self, history_height);

        /* Splitter — horizontální posuvník mezi tabulkami */
        float splitter_width = ImGui::GetContentRegionAvail().x;
        render_splitter(splitter_width, self->shared_history_split_ratio, usable_height);
    }
    else
    {
        /* Bez historie - dolní tabulka zabírá celou plochu (history_height = 0) */
        disasm_height = remaining_height;
    };

    /*
     * Dolní tabulka — disassemblovaná paměť.
     * V animačním režimu: zobrazuje disassembly od aktuálního PC (read-only).
     * V editačním režimu: plná interakce (selekce, klávesnice, context menu).
     */
    render_disassembly_table(self, disasm_height, is_paused);

    ImGui::EndChild(); /* ##dbg_tables */

    /*
     * Pravá část — vertikální šoupátko (slider) pro nastavení adresy focusu.
     *
     * Rozsah: 0x0000–0xFFFF. Nahoře je 0x0000, dole 0xFFFF.
     * ImGui VSliderInt má hodnotu nahoře = max, dole = min,
     * proto invertujeme: slider_val = 0xFFFF - focus_addr.
     *
     * Při změně slideru se nastaví focus_addr a selected_row = 0.
     */
    ImGui::SameLine();

    int slider_val = 0xFFFF - (int)(*self->shared_focus_addr);
    int slider_min = 0;
    int slider_max = 0xFFFF;

    /* Prázdný formátovací řetězec — nechceme zobrazovat číslo na slideru */
    if (ImGui::VSliderInt("##dbg_addr_slider", ImVec2(SLIDER_WIDTH, remaining_height),
                           &slider_val, slider_min, slider_max, ""))
    {
        uint16_t new_addr = (uint16_t)(0xFFFF - slider_val);
        if (new_addr != *self->shared_focus_addr)
        {
            *self->shared_focus_addr = new_addr;
            *self->shared_selected_row = 0;
            dbg_refresh_request();
        };
    };

    /* Detekce drag stavu - po VSliderInt() je IsItemActive() true pokud
     * uživatel drží LMB na slideru. Per-frame update flagu pro Follow PC
     * bypass v dalším framu. */
    self->slider_held = ImGui::IsItemActive();

    /*
     * Navigace šoupátkem — kolečko myši, klávesy.
     *
     * Kolečko: posun o 1 bajt (hover stačí).
     * UP/DOWN: posun o 1 bajt (vyžaduje focus — klik na slider).
     * PgUp/PgDown: posun o ~stránku (visible_rows * 2 bajtů).
     * Ctrl+PgUp/PgDown: skok na začátek/konec paměti.
     * Home/End: skok na 0x0000 / 0xFFFF.
     */
    {
        bool changed = false;
        int addr = (int)(*self->shared_focus_addr);

        /* Kolečko myši — reaguje na hover */
        if (ImGui::IsItemHovered())
        {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel > 0.0f)
            {
                addr -= 1;
                changed = true;
            }
            else if (wheel < 0.0f)
            {
                addr += 1;
                changed = true;
            };
        };

        /* Klávesy — reagují na focus (po kliknutí na slider) */
        if (ImGui::IsItemFocused())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
            {
                addr -= 1;
                changed = true;
            };
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
            {
                addr += 1;
                changed = true;
            };
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
            {
                if (ImGui::GetIO().KeyCtrl)
                    addr = 0x0000;
                else
                    addr -= *self->shared_visible_rows * 2;
                changed = true;
            };
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
            {
                if (ImGui::GetIO().KeyCtrl)
                    addr = 0xFFFF;
                else
                    addr += *self->shared_visible_rows * 2;
                changed = true;
            };
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
            {
                addr = 0x0000;
                changed = true;
            };
            if (ImGui::IsKeyPressed(ImGuiKey_End, false))
            {
                addr = 0xFFFF;
                changed = true;
            };
        };

        if (changed)
        {
            /* Oříznutí do rozsahu 0x0000–0xFFFF */
            if (addr < 0) addr = 0;
            if (addr > 0xFFFF) addr = 0xFFFF;

            *self->shared_focus_addr = (uint16_t)addr;
            *self->shared_selected_row = 0;
            dbg_refresh_request();
        };
    };

    ImGui::EndChild(); /* ##dbg_section */

    ImGui::PopID(); /* párový k PushID(window_id) na začátku */
}


/* ===========================================================================
 * KONSTRUKCE / DESTRUKCE INSTANCE
 * =========================================================================== */


DisassembledView *dbg_disasm_view_create(bool enable_history,
                                          const char *window_id)
{
    DisassembledView *self = (DisassembledView *)calloc(1, sizeof(DisassembledView));
    if (!self)
        return NULL;

    self->window_id = window_id;
    self->enable_history = enable_history;

    /*
     * Per-instance dynamic flags - výchozí hodnoty:
     * - hlavní instance ("main"): follow_pc=true, show_tstates=false
     * - sekundární instance ("2"-"5"): follow_pc=false, show_tstates=false
     *
     * Po create() volající (debugger_window.cpp / dbg_extra_disasm.cpp)
     * případně přemaže hodnoty z persistovaného cfgmain stavu.
     */
    {
        bool is_main_for_flags = (window_id != NULL && strcmp(window_id, "main") == 0);
        self->follow_pc = is_main_for_flags ? true : false;
        self->show_tstates = false;
    };

    /* Inicializace dasm bufferů - dva, prohazované po každém disassembly */
    self->current_dasm_buf = &self->dasm_buf[0];
    self->last_dasm_buf = &self->dasm_buf[1];
    self->last_disassembled_rows = 0;

    /* Cache flags */
    self->hist_cached_count = 0;
    self->hist_cache_valid = false;
    self->cached_bpt_version = 0;
    self->initial_ratio_set = false;
    self->header_input_buf[0] = '\0';
    self->header_input_error = false;

    /* Tooltip snapshot - nezachycený při startu */
    self->tooltip_snapshot_row = -1;

    /* Slider drag state */
    self->slider_held = false;

    /* Pointery na sdílený UI stav.
     * Hlavní instance ("main") sdílí stav s g_dbg_ui - zachování
     * zpětné kompatibility s konzumenty čtoucími g_dbg_ui přímo
     * (debugger_window.cpp, dbg_focus_to.cpp, ...).
     *
     * Detekce hlavní instance podle window_id == "main". Sekundární
     * instance budou v plánu č. 7 přepnuty na local_state. */
    bool is_main = (window_id != NULL && strcmp(window_id, "main") == 0);
    if (is_main)
    {
        self->shared_focus_addr = &g_dbg_ui.focus_addr;
        self->shared_selected_row = &g_dbg_ui.selected_row;
        self->shared_selected_addr = &g_dbg_ui.selected_addr;
        self->shared_visible_rows = &g_dbg_ui.visible_rows;
        self->shared_history_split_ratio = &g_dbg_ui.history_split_ratio;
    }
    else
    {
        /* Sekundární instance: vlastní storage v local_state.
         * Inicializace výchozími hodnotami stejnými jako g_dbg_ui (viz
         * debugger_state.cpp / debugger_ui_state_init).
         *
         * TODO: pro plán č. 7 doladit hodnoty výchozího stavu sekundárních
         *       instancí (např. selected_row = 0, history_split_ratio = 0.35,
         *       visible_rows = počítáno dynamicky při render).
         */
        self->local_state.focus_addr = 0x0000;
        self->local_state.selected_row = 0;
        self->local_state.selected_addr = 0x0000;
        self->local_state.visible_rows = 1;
        self->local_state.history_split_ratio = 0.35f;
        self->shared_focus_addr = &self->local_state.focus_addr;
        self->shared_selected_row = &self->local_state.selected_row;
        self->shared_selected_addr = &self->local_state.selected_addr;
        self->shared_visible_rows = &self->local_state.visible_rows;
        self->shared_history_split_ratio = &self->local_state.history_split_ratio;
    };

    return self;
}


void dbg_disasm_view_destroy(DisassembledView *self)
{
    if (!self)
        return;
    /*
     * Pokud byl pro tuto instanci otevřený Focus To dialog (= dialog drží
     * pointer na nás), invalidujeme ho. Jinak by Apply zapsal na uvolněnou
     * paměť (use-after-free).
     */
    dbg_focus_to_invalidate_target_if_matches(self);
    free(self);
}


void dbg_disasm_view_set_focus_addr(DisassembledView *self, uint16_t addr)
{
    if (!self)
        return;
    if (*self->shared_focus_addr == addr)
        return;
    *self->shared_focus_addr = addr;
    *self->shared_selected_row = 0;
    dbg_refresh_request();
}


void dbg_disasm_view_focus_to(DisassembledView *self, uint16_t addr)
{
    if (!self)
        return;

    /* Auto-disable Follow PC pokud je zapnutý a emulátor běží.
     * Pokud je emu v pauze, Follow PC nemá okamžitý side effect, takže
     * ho ponecháváme (= uživatel ho explicitně OFF nechtěl). */
    bool is_paused = EMULATOR_TEST_PAUSED ? true : false;
    if (self->follow_pc && !is_paused)
    {
        dbg_disasm_view_set_follow_pc(self, false);
    };

    /* Záměrně NEvoláme dbg_disasm_view_set_focus_addr - ten zkratuje
     * pokud focus_addr == addr a neresetuje selected_row. Z user akce
     * chceme vždy reset výběru (= zvýraznit cílový řádek) i refresh. */
    *self->shared_focus_addr = addr;
    *self->shared_selected_row = 0;
    dbg_refresh_request();
}


void dbg_disasm_show_in_slot(int slot_idx, uint16_t addr)
{
    if (slot_idx == 0)
    {
        /* Main: ensure open + focus. Pokud hlavní debug okno není
         * viditelné, otevři přes core API. dbg_disasm_view_get_main()
         * může vrátit NULL pokud render hlavního okna ještě neproběhl
         * - v takovém případě view eagerly vytvoříme tady, aby focus_to
         * v aktuálním framu zafungoval. Bez tohoto se první "Focus to"
         * jen otevřelo okno bez auto-disable follow_pc + bez nastavení
         * focus_addr (Michal 2026-05-11). */
        if (!g_gui->showDebuggerWindow)
        {
            debugger_show_main_window_request();
        };
        DisassembledView *main = dbg_disasm_view_get_main();
        if (!main)
        {
            /* Lazy create main view + apply persisted flagy (= stejny
             * postup jako v dbg_disassembled_render). */
            s_main_view = dbg_disasm_view_create(true, "main");
            main = s_main_view;
            if (main && !s_main_persisted_applied)
            {
                dbg_disasm_view_set_follow_pc(main,
                                                s_main_persisted_follow_pc != 0);
                dbg_disasm_view_set_show_tstates(main,
                                                  s_main_persisted_show_tstates != 0);
                s_main_persisted_applied = true;
            };
        };
        if (main)
            dbg_disasm_view_focus_to(main, addr);

        /* V9.4: bring-to-front hlavního debug okna. Pokud bylo už
         * otevřené, ale překryté jiným oknem, jen focus_to by změnil
         * obsah ale OS-level z-order by zůstal. V9.3 dual-step pattern
         * (request flag + Platform_SetWindowFocus) v debugger_window
         * vyřeší multi-viewport raise. */
        debugger_window_request_focus();
    }
    else if (slot_idx >= 1 && slot_idx <= 4)
    {
        /* Sekundární Disassembly #2..#5. Existující helper interně řeší
         * ensure-open, persisted_focus pro lazy create i auto-disable
         * follow_pc na běžící instanci. */
        dbg_extra_disasm_show_window(slot_idx + 1, addr);

        /* V9.4: bring-to-front sekundárního okna. slot_idx 1..4 mapuje
         * na s_focus_pending index 0..3 (= Disassembly #2..#5). */
        dbg_extra_disasm_request_focus(slot_idx - 1);
    };
}


uint16_t dbg_disasm_view_get_focus_addr(const DisassembledView *self)
{
    if (!self)
        return 0;
    return *self->shared_focus_addr;
}


void dbg_disasm_view_set_follow_pc(DisassembledView *self, bool enable)
{
    if (!self)
        return;
    if (self->follow_pc == enable)
        return;
    self->follow_pc = enable;
    /* Při zapnutí auto-follow chceme okamžitý refresh tak, aby se
     * focus přepočetl na PC a tabulka rerenderovala. Při vypnutí stačí
     * zachovat aktuální focus_addr (= zmrazit pohled). */
    if (enable)
        dbg_refresh_request();
}


bool dbg_disasm_view_get_follow_pc(const DisassembledView *self)
{
    if (!self)
        return false;
    return self->follow_pc;
}


void dbg_disasm_view_set_show_tstates(DisassembledView *self, bool enable)
{
    if (!self)
        return;
    if (self->show_tstates == enable)
        return;
    self->show_tstates = enable;
    /* Změna počtu sloupců přepíše šířku sekce - vyžádáme refresh aby
     * se přepočítaly cachované rozměry a obsah řádků. */
    dbg_refresh_request();
}


bool dbg_disasm_view_get_show_tstates(const DisassembledView *self)
{
    if (!self)
        return false;
    return self->show_tstates;
}


bool dbg_disasm_view_is_addr_visible(const DisassembledView *self, uint16_t addr)
{
    if (!self)
        return false;

    /* Iteruje cachované řádky z posledního renderu. Adresa je viditelná
     * pokud leží v některém [row_start, row_start + row_length) - tj.
     * jak start adresa instrukce, tak mid-instruction pozice (pro
     * vícebajtové opcody, kde PC mid-instr může nastat při některých
     * IRQ scénářích). visible_rows je per-instance (sdílený s g_dbg_ui
     * pro hlavní instanci) dynamicky počítaný podle výšky tabulky a
     * aktuálního fontu. */
    int n = *self->shared_visible_rows;
    if (n > DBG_DASM_MAX_VISIBLE_ROWS)
        n = DBG_DASM_MAX_VISIBLE_ROWS;
    for (int i = 0; i < n; i++)
    {
        uint16_t row_start = self->cached_rows[i].addr;
        uint16_t row_end = (uint16_t)(row_start + self->cached_row_lengths[i]);
        if (addr >= row_start && addr < row_end)
            return true;
    };
    return false;
}


/* ===========================================================================
 * BACKWARD-COMPAT C-API SHIMS
 * ===========================================================================
 *
 * Shim funkce pro hlavní okno debuggeru - lazy-vytvoří singleton "main"
 * a deleguje na něj. Nepřesouvejte do hlavičky - hlavička exportuje jen
 * deklarace bez explicitní zmínky o singletonu.
 */


DisassembledView *dbg_disasm_view_get_main(void)
{
    return s_main_view;
}


float dbg_disasm_view_compute_default_width(bool include_table_extras)
{
    const ImGuiStyle &st = ImGui::GetStyle();

    /* Hlavicka disasm sekce - 3 prvky:
     * 1) InputTextWithHint("Address or symbol") - sirka odhadnuta z placeholder
     *    textu + frame padding + bezpecny rezerva pro user input.
     * 2) Checkbox "Follow PC" - frame_height + label + inner spacing.
     * 3) Checkbox "T-states" - dtto. */
    float input_text_w = ImGui::CalcTextSize("Address or symbol  ").x
                         + st.FramePadding.x * 2.0f
                         + 4.0f;  /* drobna rezerva */
    float follow_w = ImGui::GetFrameHeight()
                     + st.ItemInnerSpacing.x
                     + ImGui::CalcTextSize("Follow PC").x;
    float tstates_w = ImGui::GetFrameHeight()
                      + st.ItemInnerSpacing.x
                      + ImGui::CalcTextSize("T-states").x;

    float header_w = input_text_w
                     + st.ItemSpacing.x + follow_w
                     + st.ItemSpacing.x + tstates_w;

    /* Disasm tabulka - sloupce ICONS + ADDR + BYTES + MNEM (+ TSTATES) +
     * vertikalni slider. ICONS gutter pri zapnutych branch arrows je 40 px,
     * jinak 20 px (default ON). Char width pro mono pasaze fontu odhadneme
     * z "0" (= ~7 px pro default 13px font). */
    float char_w = ImGui::CalcTextSize("0").x;
    float icons_w = 40.0f;            /* COL_ICONS_BASE_WIDTH + ARROWS_WIDTH */
    float addr_w  = 5.0f * char_w;    /* "XXXX:" */
    float bytes_w = 12.0f * char_w;   /* "XX XX XX XX" */
    float mnem_w  = 15.0f * char_w;   /* "ld a,(ix+127)" */
    float tcol_w  = include_table_extras ? (5.0f * char_w) : 0.0f;
    float cell_padding_total = st.CellPadding.x * 2.0f
                                * (4.0f + (include_table_extras ? 1.0f : 0.0f));
    float slider_w = 22.0f;
    float table_w = icons_w + addr_w + bytes_w + mnem_w + tcol_w
                    + cell_padding_total + slider_w;

    float content_w = (header_w > table_w) ? header_w : table_w;

    /* Window padding (oba okraje) + scrollbar size + drobna rezerva. */
    float chrome_w = st.WindowPadding.x * 2.0f
                   + st.ScrollbarSize
                   + 4.0f;

    return content_w + chrome_w;
}


void dbg_disassembled_render(float section_height)
{
    if (!s_main_view)
    {
        s_main_view = dbg_disasm_view_create(true, "main");
        if (!s_main_view)
            return; /* alokace selhala - tichý no-op (UI vlákno nemá kam reportovat) */
    };

    /* Aplikace persistovaných flagů (jen jednou per existence singletonu).
     * Hodnoty s_main_persisted_* jsou naplněny cfgmain při startu z .ini;
     * tady se přenesou do view přes setters. */
    if (!s_main_persisted_applied)
    {
        dbg_disasm_view_set_follow_pc(s_main_view, s_main_persisted_follow_pc != 0);
        dbg_disasm_view_set_show_tstates(s_main_view, s_main_persisted_show_tstates != 0);
        s_main_persisted_applied = true;
    };

    dbg_disasm_view_render(s_main_view, section_height);

    /* Sync zpět do persist storage - aby shutdown uložil aktuální stav.
     * Cheap (2× bool read + assign). */
    s_main_persisted_follow_pc = dbg_disasm_view_get_follow_pc(s_main_view) ? 1 : 0;
    s_main_persisted_show_tstates = dbg_disasm_view_get_show_tstates(s_main_view) ? 1 : 0;
}


void dbg_disassembled_register_cfg(void *cmod_void)
{
    if (!cmod_void)
        return;
    st_CFGMODULE *cmod = (st_CFGMODULE *) cmod_void;

    /* follow_pc default 1 - hlavní okno auto-follow PC. UI checkbox bude
     * disabled v hlavičce hlavní view, takže reálně se hodnota nemění. */
    st_CFGELEMENT *elm = cfgmodule_register_new_element(
        cmod, (char *) "disasm_main_follow_pc", CFGENTYPE_BOOL, 1);
    cfgelement_set_handlers(elm,
                            (void *) &s_main_persisted_follow_pc,
                            (void *) &s_main_persisted_follow_pc);

    /* show_tstates default 0 - sloupec T-states vypnut by default. */
    elm = cfgmodule_register_new_element(
        cmod, (char *) "disasm_main_show_tstates", CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm,
                            (void *) &s_main_persisted_show_tstates,
                            (void *) &s_main_persisted_show_tstates);
}


/*
 * Per-frame init disasm UI - viz Doxygen v dbg_disassembled.h.
 *
 * Frame guard přes ImGui::GetFrameCount() zajistí, že refresh_tick
 * se v jednom framu provede přesně jednou. Init g_dbg_ui je sám o
 * sobě idempotentní (debugger_ui_state_init kontroluje initialized
 * flag), guard tu primárně chrání refresh_tick.
 */
void dbg_disassembled_frame_init(void)
{
    static int s_last_frame = -1;
    int now_frame = ImGui::GetFrameCount();
    if (now_frame == s_last_frame)
        return;
    s_last_frame = now_frame;

    if (!g_dbg_ui.initialized)
    {
        debugger_ui_state_init();
        debugger_ui_state_refresh_from_cpu();
    };
    dbg_refresh_tick();
}


bool dbg_dasm_is_addr_visible(uint16_t addr)
{
    if (!s_main_view)
        return false;
    return dbg_disasm_view_is_addr_visible(s_main_view, addr);
}


/* ===========================================================================
 * STATELESS API
 * ===========================================================================
 *
 * dbg_dasm_get_line() - využíván z dbg_focus_to.cpp / bpt_edit_panel.cpp
 * jako on-demand disasm bez sdílené cache. Není vázaný na žádnou instanci.
 */
bool dbg_dasm_get_line(uint16_t addr, DbgDasmLine *out)
{
    if (!out) return false;
    /* Stateless API se může volat mimo render kontext - sync přesto bezpečný
     * (no-op pokud je verze aktuální). */
    sync_dasm_symtab();
    DasmRow row;
    /* Stateless cesta - bez bufferu, fallback na živé čtení paměti.
     * Volající (Code Preview v breakpoint editoru, focus_to dialog)
     * není v hot path, race s emu vláknem je minimálně viditelný. */
    int len = disassemble_at(addr, &row, NULL);
    out->addr = row.addr;
    out->length = len;
    out->length = (out->length < 1) ? 1 : (out->length > 4 ? 4 : out->length);
    out->bytes[0] = row.bytes[0];
    out->bytes[1] = row.bytes[1];
    out->bytes[2] = row.bytes[2];
    out->bytes[3] = row.bytes[3];
    snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", row.mnemonic);
    return true;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
