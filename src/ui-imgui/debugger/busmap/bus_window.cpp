/**
 * @file   bus_window.cpp
 * @brief  Okno debuggeru Bus Map (kontrakt v bus_window.h).
 *
 * Rozložení (reference v2 návrhu 3, emu-experiments/mz700-devices/gui/bus-map):
 *
 *   [Ports] [MMIO]   Color by [Owners v] (?)   [x] Track activity
 *   +-- pruh stavu nad mřížkou -------------+||+-- Owners -----------------------------+
 *   |     0  1  2 ... F                     ||||[CH] CMT hack   01h-02h  none  present |
 *   | 00 [. ][CH][CH] ...                   ||||[K8] K&P 80 ... 70h-73h  tri.. [Connect]|
 *   | ...                                   ||||  ... (ScrollY, clipper, zebra)        |
 *   | F0 [G ][G ] ...                       |||+-- [K8] Port 70h  K&P 80 Column Card --+
 *   +---------------------------------------+||| IN/s 0  OUT/s 0  ghost 0  conflicts 0 |
 *                                           ||| Last IN -   Last OUT -                 |
 *                                           ||| Peek: - (automatic while paused)       |
 *                                           ||| Flags: potential conflict              |
 *                                           ||| [Show in I/O Ports] [Reset activity]   |
 *                                           ||| [Break on IN]       [Break on OUT]     |
 *   MZ-700 | 3 devices, 0 connected (?)
 *
 * Rozměry z GetFontSize() / GetFrameHeight(): buňka = nejširší zkratka
 * + vzduch (nejméně 2 px), řádek seznamu = výška rámečku. Pravá část má
 * minimum z nejdelších textů sloupců a tlačítek (stejné pro obě záložky,
 * aby přepnutí Ports / MMIO nic neposunulo); pod součtem minim dostane
 * tělo okna vodorovný posuvník a nic se nepřeskládává. Okno samo nikdy
 * neroluje (NoScrollbar), rolují jen tělo a seznam. Detail má pevný počet
 * řádků ve všech stavech, aby seznam nad ním neposkakoval. Stav nikdy jen
 * barvou: zkratka v buňce, šrafování nepřítomných, odznaky (tmavý kruh
 * s bílým glyfem, sedí na rohu buňky z větší části uvnitř, kreslí se až
 * po všech buňkách) pro kolizi, nedoložený budič, rozdělení směru a ghost
 * přístup; význam slovy v tooltipu buňky a v detailu.
 *
 * @par Licence: GPLv3
 */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "bus_window.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "ui-imgui/debugger/debugger_state.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/debugger/ioview/io_window.h"
#include "ui-imgui/debugger/breakpoints/bpt_edit_panel.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

extern "C" {
#include "emulator.h"
#include "device_request.h"
#include "debugger/bus_map.h"
#include "debugger/io_activity.h"
#include "debugger/dbgapi_cmdrq.h"
#include "hw-generic/devbus/devbus_request.h"
}


namespace {

/** @brief Režim obarvení mřížky (hodnoty = klíč INI color_mode, neměnit). */
enum en_BUS_COLOR_MODE { BUS_COLOR_OWNERS = 0, BUS_COLOR_ACTIVITY = 1, BUS_COLOR_DRIVE = 2 };

/** @brief Záložka okna (hodnoty = klíč INI active_tab). */
enum en_BUS_TAB { BUS_TAB_PORTS = 0, BUS_TAB_MMIO = 1 };

/** @brief Nejvyšší počet řádků seznamu vlastníků (id x záložka; MZ-800 má 19 id na portech). */
constexpr unsigned MAX_ROWS = 64;
/** @brief Nejvyšší počet rozsahů jednoho řádku (MZ-800 GDG má 5, RAM disk 4). */
constexpr unsigned MAX_RANGES = 8;
/** @brief Počet rozsahů vypsaných ve sloupci Ports; další jen jako "+N" a v tooltipu (K16). */
constexpr unsigned SHOWN_RANGES = 3;

/**
 * @brief Jeden řádek seznamu vlastníků: jedno id v aktivní mřížce se všemi jeho rozsahy.
 * @invariant id nenulové; range_count <= MAX_RANGES; rozsahy vzestupně a disjunktní.
 */
struct OwnerRow {
    const char *id;             /**< Stabilní id vlastníka. */
    uint8_t kind;               /**< en_BUS_OWNER_KIND. */
    int8_t slot;                /**< Slot devbus, -1 u statických. */
    char abbrev[4];             /**< Zkratka do mini buňky. */
    char name[64];              /**< Zobrazené jméno (první název bez závorky). */
    const char *full_name;      /**< První úplný název (tooltip). */
    bool connected;             /**< Přítomný / připojený. */
    uint8_t dirs;               /**< Sjednocení směrů. */
    en_DEVBUS_DRIVE drive;      /**< Nejsilnější budič čtení (UNKNOWN má přednost). */
    uint8_t drive_mask;         /**< Maska u PARTIAL. */
    unsigned first_addr;        /**< První adresa (řazení). */
    unsigned range_count;       /**< Počet rozsahů. */
    uint16_t rf[MAX_RANGES];    /**< Začátky rozsahů. */
    uint16_t rl[MAX_RANGES];    /**< Konce rozsahů. */
    const char *src[MAX_RANGES];/**< Zdroj deklarace budiče rozsahu (NULL = bez poznámky). */
    const char *mirror;         /**< Poznámka o zrcadlech (první nalezená). */
    uint32_t hps_max;           /**< Nejvyšší aktivita buňky řádku (režim Activity). */
    unsigned cells;             /**< Počet buněk. */
    bool overflow;              /**< Víc rozsahů než MAX_RANGES (tooltip to řekne). */
};

/** @brief Rozměry odvozené z písma (přepočet každý snímek). */
struct Metrics {
    float fs = 14.0f;      /**< GetFontSize(). */
    float line = 14.0f;    /**< GetTextLineHeight(). */
    float frame = 22.0f;   /**< GetFrameHeight(). */
    float margin = 2.0f;   /**< Vzduch kolem zkratky v buňce (>= 2 px). */
    float gap = 3.0f;      /**< Mezera mezi buňkami. */
    float cell_min = 24.0f;/**< Minimální buňka: nejširší zkratka + 2 x margin, nejméně řádek textu. */
    float badge_r = 5.0f;  /**< Poloměr odznaku (0,2 em, nejméně 5 px). */
    float badge_off = 2.0f;/**< Posun středu odznaku z rohu buňky dovnitř (r / 3); odznak tak nesahá na zkratku a jen zlehka přes mezeru. */
    float mini = 22.0f;    /**< Mini buňka v seznamu (= výška řádku). */
    float split = 6.0f;    /**< Šířka tažného oddělovače. */
    /** @brief Šířka (= výška) mřížky včetně záhlaví při buňce @p c. */
    float grid_w(float c) const { return 17.0f * c + 15.0f * gap; }
};

/** @brief Stav okna (jen UI vlákno). */
struct BusUiState {
    int color_mode = BUS_COLOR_OWNERS;   /**< Persist: color_mode. */
    int active_tab = BUS_TAB_PORTS;      /**< Persist: active_tab. */
    int list_width = 0;                  /**< Persist: list_width (0 = automaticky). */
    int cfg_tracking = 1;                /**< Persist: tracking_active (preference při otevření). */
    bool tracking = true;                /**< Aktuální přání sledovat (checkbox). */
    bool was_open = false;               /**< Okno bylo otevřené v minulém snímku. */
    bool want_tab[2] = { false, false }; /**< Jednorázové přepnutí záložky (persistence, skok). */
    unsigned sel_addr[2] = { 0x70, BUS_MAP_MMIO_FIRST }; /**< Vybraná adresa per záložka. */
    const char *sel_owner_id = NULL;     /**< Vybraný vlastník (řádek), NULL = primární vlastník buňky. */
    uint8_t sel_owner_kind = 0;          /**< Druh vybraného vlastníka. */
    bool want_scroll_row = false;        /**< Příští kreslení seznamu odroluje k vybranému řádku. */
    const char *hover_row_id = NULL;     /**< Řádek pod myší (seznam se kreslí po mřížce, mřížka čte minulý snímek). */
    uint8_t hover_row_kind = 0;          /**< Druh vlastníka řádku pod myší. */
    const char *hover_row_id_now = NULL; /**< Řádek pod myší v tomto snímku. */
    uint8_t hover_row_kind_now = 0;      /**< Jeho druh. */
    int hover_cell = -1;                 /**< Index buňky pod myší v tomto snímku, -1 = žádná. */
    bool peek_valid = false;             /**< Peek platí pro peek_addr. */
    unsigned peek_addr = 0;              /**< Adresa, pro kterou peek platí. */
    bool peek_mmio = false;              /**< Peek platí pro MMIO buňku. */
    uint8_t peek_value = 0;              /**< Hodnota peeku. */
    uint8_t peek_driven = 0;             /**< Maska buzených bitů peeku. */
    int peek_source = 0;                 /**< DBGAPI_BUS_PEEK_SRC_*. */
    uint32_t peek_frame = 0;             /**< Snímek obrazu, ve kterém peek vznikl. */
    bool peek_paused_done = false;       /**< V pauze už peek pro tuto adresu proběhl. */
    unsigned ctx_addr = 0;               /**< Adresa pro kontextové menu buňky. */
    bool ctx_mmio = false;               /**< Kontextové menu patří MMIO buňce. */
    st_BUS_MAP_SNAPSHOT snap;            /**< Snímek tohoto kreslení (cca 140 kB, proto statický). */
    OwnerRow rows[MAX_ROWS];             /**< Řádky seznamu aktivní záložky. */
    unsigned row_count = 0;              /**< Počet řádků. */
    bool rows_devbus = false;            /**< V seznamu je aspoň jedno zařízení devbus (sloupec Action). */
    int sel_row = -1;                    /**< Index vybraného řádku, -1 = žádný. */
    float row_y[MAX_ROWS];               /**< Střed řádku y z posledního kreslení (testy). */
    bool row_drawn[MAX_ROWS];            /**< Řádek se kreslil (clipper). */
    float row_x = 0.0f;                  /**< Střed sloupce Owner x. */
    Metrics m;                           /**< Rozměry tohoto snímku. */
    bool grid_drawn = false;             /**< Mřížka se v posledním snímku kreslila (geometrie níže platí). */
    bool grid_mmio = false;              /**< Kreslená mřížka byla MMIO. */
    ImVec2 grid_origin;                  /**< Levý horní roh mřížky (vč. záhlaví). */
    float cell = 24.0f;                  /**< Buňka posledního kreslení. */
    float natural_w = 0.0f;              /**< Přirozená šířka obsahu (testy, SPEC). */
    float list_w = 0.0f;                 /**< Šířka pravé části posledního kreslení. */
    float list_min_w = 0.0f;             /**< Minimum pravé části z textů. */
    float detail_h = 0.0f;               /**< Výška detailu změřená v minulém snímku (kontrola proti detail_height(), testy). */
    ImVec2 split_center;                 /**< Střed oddělovače mřížka / seznam z posledního kreslení (testy). */
    int hover_cells = 0;                 /**< Buňky zvýrazněné hoverem řádku (testy). */
    int visible_rows = 0;                /**< Řádky vykreslené clipperem (testy). */
    char activity_line[96] = "";         /**< Řádek aktivity detailu (testy). */
    char peek_line[96] = "";             /**< Řádek peeku detailu (testy). */
    char flags_line[300] = "";           /**< Řádek příznaků detailu "Flags: ..." (testy). */
    char status_line[96] = "";           /**< Stavový řádek (testy). */
};

BusUiState g_bus_ui;

/* Tokeny barev (SPEC, oddíl Společné požadavky; K12 ztmavené partial a unknown,
 * K13 světlé šrafování a rámeček). Stav nikdy jen barvou, viz odznaky. */
constexpr ImU32 COL_UNMAPPED   = IM_COL32(0x30, 0x30, 0x34, 255);
constexpr ImU32 COL_OWNED_IDLE = IM_COL32(0x40, 0x40, 0x46, 255);
constexpr ImU32 COL_STATIC     = IM_COL32(0x3A, 0x5C, 0x8C, 255);
constexpr ImU32 COL_STATIC_OFF = IM_COL32(0x2C, 0x3C, 0x54, 255);
constexpr ImU32 COL_DEVBUS     = IM_COL32(0x34, 0x80, 0x54, 255);
constexpr ImU32 COL_DEVBUS_OFF = IM_COL32(0x28, 0x4C, 0x38, 255);
constexpr ImU32 COL_DRV_NONE   = IM_COL32(0x5A, 0x5A, 0x60, 255);
constexpr ImU32 COL_DRV_TRI    = IM_COL32(0x3A, 0x6E, 0xAA, 255);
constexpr ImU32 COL_DRV_PART   = IM_COL32(0x1F, 0x70, 0x70, 255);
constexpr ImU32 COL_DRV_OC     = IM_COL32(0x82, 0x50, 0xA0, 255);
constexpr ImU32 COL_DRV_UNK    = IM_COL32(0x8C, 0x5A, 0x1E, 255);
constexpr ImU32 COL_ACT_WARM   = IM_COL32(0x3C, 0x78, 0x3C, 255);
constexpr ImU32 COL_ACT_HOT    = IM_COL32(0xAA, 0x3C, 0x32, 255);
constexpr ImU32 COL_TEXT       = IM_COL32(0xFF, 0xFF, 0xFF, 255);
constexpr ImU32 COL_TEXT_DIM   = IM_COL32(0x80, 0x80, 0x80, 255);
constexpr ImU32 COL_BADGE      = IM_COL32(0x0F, 0x0F, 0x0F, 255);
constexpr ImU32 COL_WARN       = IM_COL32(0xFF, 0xC8, 0x3C, 255);
constexpr ImU32 COL_BAD        = IM_COL32(0xFF, 0x50, 0x50, 255);
constexpr ImU32 COL_SELECT     = IM_COL32(0xFF, 0xFF, 0xFF, 255);
constexpr ImU32 COL_HOVER      = IM_COL32(0xFF, 0xFF, 0xFF, 150);
constexpr ImU32 COL_ROW_HILITE = IM_COL32(0xFF, 0xC8, 0x3C, 230);
constexpr ImU32 COL_HATCH      = IM_COL32(0xFF, 0xFF, 0xFF, 176);
constexpr ImU32 COL_BADGE_RING = IM_COL32(0xD0, 0xD0, 0xD0, 255);
constexpr ImU32 COL_BORDER     = IM_COL32(0x60, 0x6A, 0x78, 255);
constexpr ImU32 COL_DIM_OVER   = IM_COL32(0, 0, 0, 90);
constexpr ImU32 COL_STRIP      = IM_COL32(0x2F, 0x5A, 0x86, 255);
const ImVec4   COL4_ROW_SEL    = ImVec4(0x34 / 255.0f, 0x68 / 255.0f, 0xAD / 255.0f, 1.0f);
const ImVec4   COL4_ROW_SEL_HI = ImVec4(0x40 / 255.0f, 0x78 / 255.0f, 0xC0 / 255.0f, 1.0f);
/* Tlačítko Connect / Disconnect ve vybraném řádku: neprůhledné a tmavší než
 * řádek, bílý text >= 4,5:1 ve všech stavech (R09; výchozí Button s alfou 0,4
 * dával nad COL4_ROW_SEL 3A7ACC = 4,34:1). Hranici vůči řádku nese bílý rámeček. */
const ImVec4   COL4_SEL_BTN        = ImVec4(0x24 / 255.0f, 0x52 / 255.0f, 0x8C / 255.0f, 1.0f); /* 7,89:1 */
const ImVec4   COL4_SEL_BTN_HOVER  = ImVec4(0x2C / 255.0f, 0x62 / 255.0f, 0xA8 / 255.0f, 1.0f); /* 6,14:1 */
const ImVec4   COL4_SEL_BTN_ACTIVE = ImVec4(0x1E / 255.0f, 0x46 / 255.0f, 0x78 / 255.0f, 1.0f); /* 9,53:1 */


/** @brief Slovo pro výstupní stupeň (sloupec Drive, detail). */
const char *drive_word(en_DEVBUS_DRIVE d)
{
    switch (d) {
    case DEVBUS_DRIVE_NONE: return _("none");
    case DEVBUS_DRIVE_TRISTATE: return _("tri-state");
    case DEVBUS_DRIVE_PARTIAL: return _("partial");
    case DEVBUS_DRIVE_OPEN_COLLECTOR: return _("open collector");
    case DEVBUS_DRIVE_UNKNOWN:
    default: return _("not documented");
    }
}

/** @brief Slovo pro politiku kolize. */
const char *policy_word(en_DEVBUS_CONFLICT p)
{
    switch (p) {
    case DEVBUS_CONFLICT_AND: return _("and");
    case DEVBUS_CONFLICT_FIRST: return _("first");
    case DEVBUS_CONFLICT_REFUSE:
    default: return _("refuse");
    }
}

/** @brief Slovo pro směr. */
const char *dir_word(uint8_t dirs)
{
    return dirs == BUS_DIR_RW ? "IN/OUT" : (dirs & BUS_DIR_R) ? "IN" : "OUT";
}

/** @brief Slovo pro stav vlastníka (sloupec State). */
const char *state_word(uint8_t kind, bool connected)
{
    if (kind == BUS_OWNER_DEVBUS) return connected ? _("connected") : _("not connected");
    return connected ? _("present") : _("not present");
}

/** @brief Nejsilnější budič čtení vlastníků buňky (UNKNOWN má přednost). */
en_DEVBUS_DRIVE cell_drive(const st_BUS_OWNER *owners, unsigned n)
{
    en_DEVBUS_DRIVE best = DEVBUS_DRIVE_NONE;
    bool any = false;
    for (unsigned i = 0; i < n; i++) {
        if (!(owners[i].dirs & BUS_DIR_R)) continue;
        any = true;
        en_DEVBUS_DRIVE d = owners[i].drive;
        if (d == DEVBUS_DRIVE_UNKNOWN) return DEVBUS_DRIVE_UNKNOWN;
        if (d > best) best = d;
    }
    return any ? best : DEVBUS_DRIVE_NONE;
}

/** @brief Je mezi vlastníky aspoň jeden přítomný? */
bool any_present(const st_BUS_OWNER *owners, unsigned n)
{
    for (unsigned i = 0; i < n; i++) if (owners[i].connected) return true;
    return false;
}

/** @brief První přítomný vlastník (pro zkratku), jinak první registrovaný. */
const st_BUS_OWNER *primary_owner(const st_BUS_OWNER *owners, unsigned n)
{
    for (unsigned i = 0; i < n; i++) if (owners[i].connected) return &owners[i];
    return n ? &owners[0] : NULL;
}

/** @brief Barva podle druhu a přítomnosti (režim Owners). */
ImU32 owner_color(uint8_t kind, bool connected)
{
    if (kind == BUS_OWNER_DEVBUS) return connected ? COL_DEVBUS : COL_DEVBUS_OFF;
    return connected ? COL_STATIC : COL_STATIC_OFF;
}

/** @brief Barva podle výstupního stupně (režim Drive). */
ImU32 drive_color(en_DEVBUS_DRIVE d)
{
    switch (d) {
    case DEVBUS_DRIVE_TRISTATE: return COL_DRV_TRI;
    case DEVBUS_DRIVE_PARTIAL: return COL_DRV_PART;
    case DEVBUS_DRIVE_OPEN_COLLECTOR: return COL_DRV_OC;
    case DEVBUS_DRIVE_UNKNOWN: return COL_DRV_UNK;
    default: return COL_DRV_NONE;
    }
}

/** @brief Barva podle aktivity (režim Activity; prahy sdílené s oknem I/O Ports). */
ImU32 heat_color(uint32_t hits_per_sec, bool owned)
{
    int warm = 1, hot = 10000;
    io_window_heat_thresholds(&warm, &hot);
    if (hits_per_sec >= (uint32_t) hot) return COL_ACT_HOT;
    if (hits_per_sec >= (uint32_t) warm) return COL_ACT_WARM;
    return owned ? COL_OWNED_IDLE : COL_UNMAPPED;
}

/** @brief Barva pozadí buňky podle režimu. */
ImU32 cell_color(const st_BUS_OWNER *owners, unsigned n, uint32_t hits_per_sec)
{
    const st_BUS_OWNER *o = primary_owner(owners, n);
    switch (g_bus_ui.color_mode) {
    case BUS_COLOR_ACTIVITY: return heat_color(hits_per_sec, n != 0);
    case BUS_COLOR_DRIVE: return o ? drive_color(cell_drive(owners, n)) : COL_UNMAPPED;
    case BUS_COLOR_OWNERS:
    default: return o ? owner_color(o->kind, o->connected != 0) : COL_UNMAPPED;
    }
}

/** @brief Barva mini buňky řádku seznamu podle režimu (legenda). */
ImU32 row_color(const OwnerRow &r)
{
    switch (g_bus_ui.color_mode) {
    case BUS_COLOR_ACTIVITY: return heat_color(r.hps_max, true);
    case BUS_COLOR_DRIVE: return drive_color((r.dirs & BUS_DIR_R) ? r.drive : DEVBUS_DRIVE_NONE);
    case BUS_COLOR_OWNERS:
    default: return owner_color(r.kind, r.connected);
    }
}

/** @brief Zobrazené jméno: název bez závorky za mezerou ("FDC 1 (WD279x ...)" -> "FDC 1"). */
void display_name(const char *full, char *out, size_t n)
{
    snprintf(out, n, "%s", full ? full : "");
    char *p = strstr(out, " (");
    if (p && p != out) *p = '\0';
}

/** @brief Index řádku podle id a druhu, -1 = není. */
int find_row(const char *id, uint8_t kind)
{
    for (unsigned i = 0; i < g_bus_ui.row_count; i++) {
        const OwnerRow &r = g_bus_ui.rows[i];
        if (r.kind == kind && (r.id == id || strcmp(r.id, id) == 0)) return (int) i;
    }
    return -1;
}

/**
 * @brief Postaví řádky seznamu vlastníků pro mřížku @p mmio ze snímku.
 *
 * Jeden řádek na id; rozsahy se slučují z po sobě jdoucích adres, pořadí
 * řádků = první adresa (vzniká průchodem adres vzestupně).
 */
void build_rows(bool mmio)
{
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;
    g_bus_ui.row_count = 0;
    /* Sloupec Action podle registrovaných zařízení, ne podle řádků aktivní záložky (R05: stejná šířka obou záložek). */
    g_bus_ui.rows_devbus = s->devbus_initialized && s->devbus_registered > 0;
    for (unsigned idx = 0; idx < 256; idx++) {
        const st_BUS_OWNER *owners;
        unsigned n;
        uint32_t hps;
        unsigned addr;
        if (mmio) {
            owners = s->mmio[idx].owners; n = s->mmio[idx].owner_count;
            hps = s->mmio[idx].hits_out_per_sec; addr = BUS_MAP_MMIO_FIRST + idx;
        } else {
            owners = s->ports[idx].owners; n = s->ports[idx].owner_count;
            hps = s->ports[idx].hits_in_per_sec + s->ports[idx].hits_out_per_sec; addr = idx;
        }
        for (unsigned i = 0; i < n; i++) {
            const st_BUS_OWNER *o = &owners[i];
            int ri = find_row(o->id, o->kind);
            if (ri < 0) {
                if (g_bus_ui.row_count >= MAX_ROWS) continue;
                ri = (int) g_bus_ui.row_count++;
                OwnerRow &r = g_bus_ui.rows[ri];
                memset(&r, 0, sizeof(r));
                r.id = o->id; r.kind = o->kind; r.slot = o->devbus_slot;
                snprintf(r.abbrev, sizeof(r.abbrev), "%s", o->abbrev);
                r.full_name = o->name;
                display_name(o->name, r.name, sizeof(r.name));
                r.drive = DEVBUS_DRIVE_NONE;
                r.first_addr = addr;
            }
            OwnerRow &r = g_bus_ui.rows[ri];
            bool same_cell = r.range_count && r.rl[r.range_count - 1] == addr && r.cells;
            if (!same_cell) {
                if (r.range_count && r.rl[r.range_count - 1] + 1u == addr) {
                    r.rl[r.range_count - 1] = (uint16_t) addr;
                } else if (r.range_count < MAX_RANGES) {
                    r.rf[r.range_count] = (uint16_t) addr;
                    r.rl[r.range_count] = (uint16_t) addr;
                    r.src[r.range_count] = o->source_note;
                    r.range_count++;
                } else {
                    r.overflow = true;
                }
                r.cells++;
            }
            r.dirs |= o->dirs;
            r.connected = r.connected || o->connected;
            if (o->dirs & BUS_DIR_R) {
                if (o->drive == DEVBUS_DRIVE_UNKNOWN) r.drive = DEVBUS_DRIVE_UNKNOWN;
                else if (r.drive != DEVBUS_DRIVE_UNKNOWN && o->drive > r.drive) { r.drive = o->drive; r.drive_mask = o->drive_mask; }
            }
            if (!r.mirror && o->mirror_note) r.mirror = o->mirror_note;
            if (hps > r.hps_max) r.hps_max = hps;
        }
    }
}

/**
 * @brief Text rozsahů řádku ("70h-73h, 78h-7Fh +2").
 * @param max_shown Nejvyšší počet vypsaných rozsahů (0 = všechny).
 */
void ranges_text(const OwnerRow &r, bool mmio, unsigned max_shown, char *buf, size_t n)
{
    size_t used = 0;
    buf[0] = '\0';
    unsigned shown = (max_shown && r.range_count > max_shown) ? max_shown : r.range_count;
    for (unsigned i = 0; i < shown && used < n; i++) {
        int w;
        if (r.rf[i] == r.rl[i]) w = snprintf(buf + used, n - used, mmio ? "%s%04Xh" : "%s%02Xh", i ? ", " : "", r.rf[i]);
        else w = snprintf(buf + used, n - used, mmio ? "%s%04Xh-%04Xh" : "%s%02Xh-%02Xh", i ? ", " : "", r.rf[i], r.rl[i]);
        if (w < 0) break;
        used += (size_t) w;
    }
    if (shown < r.range_count && used < n) snprintf(buf + used, n - used, " +%u", r.range_count - shown);
    else if (r.overflow && used < n) snprintf(buf + used, n - used, " +");
}

/** @brief Šrafování buňky (nepřítomný vlastník): světlé diagonály (K13). */
void draw_hatch(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    float w = b.x - a.x, h = b.y - a.y;
    float step = 6.0f;
    for (float d = -h; d < w; d += step) {
        ImVec2 p0(a.x + d, b.y), p1(a.x + d + h, a.y);
        if (p0.x < a.x) { p0.y = b.y - (a.x - p0.x); p0.x = a.x; }
        if (p1.x > b.x) { p1.y = a.y + (p1.x - b.x); p1.x = b.x; }
        dl->AddLine(p0, p1, COL_HATCH, 1.0f);
    }
}

/**
 * @brief Odznak: tmavý kruh s bílým glyfem a 1 px prstencem (K06; prstenec >= 3:1 vůči každé výplni buňky).
 * @param glyph   Text glyfu, nebo NULL = tečka (ghost).
 * @param outline Barva prstence, 0 = neutrální světlý.
 */
void draw_badge(ImDrawList *dl, ImVec2 c, float r, const char *glyph, ImU32 outline)
{
    dl->AddCircleFilled(c, r, COL_BADGE);
    dl->AddCircle(c, r, outline ? outline : COL_BADGE_RING, 0, 1.0f);
    if (!glyph) {
        dl->AddCircleFilled(c, r * 0.4f, COL_TEXT);
        return;
    }
    ImFont *f = ImGui::GetFont();
    float gs = fmaxf(8.0f, r * 2.2f); /* glyf co největší, "?" se do kruhu vejde (výška verzálky ~0,7 em) */
    ImVec2 ts = f->CalcTextSizeA(gs, FLT_MAX, 0.0f, glyph);
    dl->AddText(f, gs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), COL_TEXT, glyph);
}

/** @brief Odznak odložený na druhý průchod kreslení mřížky (až po všech buňkách, aby ho soused nepřekreslil). */
struct PendingBadge {
    ImVec2 c;           /**< Střed. */
    const char *glyph;  /**< Glyf, NULL = tečka. */
    ImU32 outline;      /**< Barva prstence, 0 = neutrální. */
};

/** @brief Nejvyšší počet odznaků jedné mřížky (4 rohy x 256 buněk). */
constexpr unsigned MAX_BADGES = 4 * 256;

/**
 * @brief Jeden řádek textu bez zalamování, oříznutý na šířku zbývajícího místa (pevná výška detailu).
 *
 * Položka má vždy šířku zbývajícího místa a výšku řádku, takže obsah
 * dítěte nepřeteče ani u dlouhého textu.
 * @param text Text.
 * @param col  Barva.
 * @return true = text se celý nevešel (volající může dát úplné znění do tooltipu).
 */
bool clipped_line(const char *text, ImU32 col)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetTextLineHeight();
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddText(p, col, text);
    ImGui::PopClipRect();
    ImGui::Dummy(ImVec2(w, h));
    return ts.x > w;
}

/** @brief Mini buňka se zkratkou (legenda v seznamu, titulek detailu). */
void draw_mini(ImDrawList *dl, ImVec2 p, float size, ImU32 col, const char *abbrev, bool hatched)
{
    ImVec2 q(p.x + size, p.y + size);
    dl->AddRectFilled(p, q, col, 3.0f);
    if (hatched) draw_hatch(dl, p, q);
    dl->AddRect(p, q, COL_BORDER, 3.0f, 0, 1.0f);
    ImVec2 ts = ImGui::CalcTextSize(abbrev);
    dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), COL_TEXT, abbrev);
}

/** @brief Nastaví výběr adresy aktivní záložky (zámek výběru není, V2 reference ho nemá). */
void select_cell(unsigned addr, bool mmio)
{
    int t = mmio ? BUS_TAB_MMIO : BUS_TAB_PORTS;
    if (g_bus_ui.sel_addr[t] != addr) {
        g_bus_ui.sel_addr[t] = addr;
        g_bus_ui.peek_paused_done = false;
        g_bus_ui.want_scroll_row = true;
    }
}

/** @brief Nastaví vybraného vlastníka (řádek). */
void set_sel_owner(const char *id, uint8_t kind)
{
    g_bus_ui.sel_owner_id = id;
    g_bus_ui.sel_owner_kind = kind;
}

/** @brief Vlastníci vybrané adresy aktivní záložky. */
void selected_owners(const st_BUS_OWNER **owners, unsigned *n, uint8_t *flags)
{
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;
    bool mmio = g_bus_ui.active_tab == BUS_TAB_MMIO;
    unsigned addr = g_bus_ui.sel_addr[g_bus_ui.active_tab];
    if (mmio) {
        unsigned idx = addr - BUS_MAP_MMIO_FIRST;
        if (idx > 255) idx = 0;
        *owners = s->mmio[idx].owners; *n = s->mmio[idx].owner_count; *flags = s->mmio[idx].flags;
    } else {
        *owners = s->ports[addr & 0xFF].owners; *n = s->ports[addr & 0xFF].owner_count; *flags = s->ports[addr & 0xFF].flags;
    }
}

/** @brief Zařadí peek vybrané adresy do emu vlákna (sync přes dbgapi s krátkým timeoutem). */
void do_peek(void)
{
    uint8_t v = 0, d = 0;
    int src = 0;
    bool mmio = g_bus_ui.active_tab == BUS_TAB_MMIO;
    unsigned addr = g_bus_ui.sel_addr[g_bus_ui.active_tab];
    if (dbg_ui_bus_peek((uint16_t) addr, mmio, &v, &d, &src)) {
        g_bus_ui.peek_valid = true;
        g_bus_ui.peek_addr = addr;
        g_bus_ui.peek_mmio = mmio;
        g_bus_ui.peek_value = v;
        g_bus_ui.peek_driven = d;
        g_bus_ui.peek_source = src;
        g_bus_ui.peek_frame = g_bus_ui.snap.frame;
    }
}

/** @brief Kontextové menu buňky (pravé tlačítko). */
void cell_context_menu(unsigned addr, bool mmio)
{
    if (!mmio) {
        if (ImGui::MenuItem(_L("Show in I/O Ports##bus_ctx_io"))) io_window_request_show_port((uint16_t) addr);
        if (ImGui::MenuItem(_L("Break on IN##bus_ctx_bpr"))) bpt_edit_panel_open_new_iorq((uint16_t) addr, false);
        if (ImGui::MenuItem(_L("Break on OUT##bus_ctx_bpw"))) bpt_edit_panel_open_new_iorq((uint16_t) addr, true);
        if (ImGui::MenuItem(_L("Reset activity of this port##bus_ctx_rst"))) dbg_ui_io_activity_reset_port((uint16_t) addr, true);
    } else {
        if (ImGui::MenuItem(_L("Show in I/O Ports##bus_ctx_io_m"))) io_window_request_show_port((uint16_t) addr);
        if (ImGui::MenuItem(_L("Reset activity of this address##bus_ctx_rst_m"))) dbg_ui_io_activity_reset_port((uint16_t) addr, false);
    }
    if (ImGui::MenuItem(_L("Peek now (no side effects)##bus_ctx_peek"))) {
        select_cell(addr, mmio);
        do_peek();
    }
}

/**
 * @brief Příznaky buňky slovy (detail, tooltip).
 *
 * "no present owner" jen u adresy, která vlastníky má, ale žádný není
 * přítomný (BUS_PORT_UNMAPPED je nastavený i u adresy bez vlastníka;
 * tam to říká titulek "no owner" a řádek o nebuzené sběrnici, R08).
 * @param flags  Příznaky BUS_PORT_* adresy.
 * @param owners Vlastníci adresy (n položek, smí být NULL při n == 0).
 * @param n      Počet vlastníků.
 * @param buf    Výstup, slova oddělená čárkou; prázdný řetězec = žádný příznak.
 * @param size   Velikost bufferu (> 0).
 */
void flags_words(uint8_t flags, const st_BUS_OWNER *owners, unsigned n, char *buf, size_t size)
{
    buf[0] = '\0';
    auto add = [&](const char *w) {
        size_t used = strlen(buf);
        snprintf(buf + used, size - used, "%s%s", used ? ", " : "", w);
    };
    if ((flags & BUS_PORT_UNMAPPED) && n > 0) add(_("no present owner"));
    if (flags & BUS_PORT_DIR_SPLIT) add(_("different owners for IN and OUT"));
    if (flags & BUS_PORT_POTENTIAL_CONFLICT) add(_("potential conflict"));
    if (flags & BUS_PORT_CONFLICT_SEEN) add(_("conflict seen at run time"));
    if (flags & BUS_PORT_GHOST_SEEN) add(_("ghost access seen"));
    if (cell_drive(owners, n) == DEVBUS_DRIVE_UNKNOWN) add(_("drive not documented"));
}

/** @brief Tooltip buňky: strukturovaně, zalomené na 35 em (K09). */
void cell_tooltip(unsigned addr, bool mmio, const st_BUS_OWNER *owners, unsigned n, uint8_t flags, unsigned idx)
{
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;
    if (!ImGui::BeginItemTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
    const st_BUS_OWNER *p = primary_owner(owners, n);
    if (p) ImGui::Text(mmio ? "%04Xh  %s" : "%02Xh  %s", addr, p->name);
    else ImGui::Text(mmio ? "%04Xh  %s" : "%02Xh  %s", addr, _("no owner"));
    for (unsigned i = 0; i < n; i++) {
        const st_BUS_OWNER *o = &owners[i];
        if (i) ImGui::Separator();
        if (n > 1 || o != p) ImGui::TextUnformatted(o->name);
        ImGui::Text("%s, %s%s, %s", dir_word(o->dirs),
                    o->kind == BUS_OWNER_DEVBUS ? _("device") : _("board"),
                    o->connected ? "" : (o->kind == BUS_OWNER_DEVBUS ? _(", not connected") : _(", not present")),
                    drive_word(o->drive));
        if (o->drive == DEVBUS_DRIVE_PARTIAL) ImGui::Text(_("Driven bits: %02Xh"), o->drive_mask);
        if ((o->dirs & BUS_DIR_R) && o->drive == DEVBUS_DRIVE_NONE) ImGui::TextUnformatted(_("Decodes IN but does not drive the bus (reads count as ghost accesses)"));
        if (mmio) {
            ImGui::Text(_("Range %04Xh-%04Xh, decodes when: %s%s"), o->range_first, o->range_last,
                        (o->when_mask & DEVBUS_MMIO_MAPPED) ? _("upper area mapped") : "",
                        (o->when_mask & DEVBUS_MMIO_DRAM) ? ((o->when_mask & DEVBUS_MMIO_MAPPED) ? _(", DRAM at 0E000h") : _("DRAM at 0E000h")) : "");
        } else {
            ImGui::Text(_("Range %02Xh-%02Xh"), o->range_first, o->range_last);
        }
        if (o->source_note) ImGui::TextDisabled("%s %s", _("Source:"), o->source_note);
        ImGui::TextDisabled("%s %s", _("Mirrors:"), o->mirror_note ? o->mirror_note : _("not verified"));
    }
    if (!n) ImGui::Text(_("A read returns the undriven bus: %s %02Xh"),
                        s->bus_idle == DEVBUS_IDLE_CONST ? _("constant") : _("bus latch"), s->idle_byte);
    char fl[256];
    flags_words(flags, owners, n, fl, sizeof(fl));
    if (fl[0]) ImGui::Text("%s %s", _("Marks:"), fl);
    if (!s->tracking_active) {
        ImGui::TextDisabled("%s", _("Activity tracking is off"));
    } else if (!mmio) {
        const st_BUS_PORT *bp = &s->ports[idx];
        if (bp->last_read_valid) ImGui::Text(_("Last IN %02Xh (%u frames ago)"), bp->last_read_value, bp->last_read_age);
        if (bp->last_write_valid) ImGui::Text(_("Last OUT %02Xh (%u frames ago)"), bp->last_write_value, bp->last_write_age);
        ImGui::Text(_("IN/s %u, OUT/s %u"), bp->hits_in_per_sec, bp->hits_out_per_sec);
    } else {
        const st_BUS_MMIO_CELL *mc = &s->mmio[idx];
        if (mc->last_write_valid) ImGui::Text(_("Last write %02Xh (%u frames ago)"), mc->last_write_value, mc->last_write_age);
        ImGui::Text(_("Writes/s %u"), mc->hits_out_per_sec);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/**
 * @brief Pruh stavu nad mřížkou (K18): krátký text, zvýrazněný při nestandardním stavu.
 * @param width Šířka pruhu (= šířka mřížky).
 */
void render_strip(bool mmio, float width)
{
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;
    const Metrics &m = g_bus_ui.m;
    const char *text;
    const char *tip;
    bool hi = false;
    if (mmio && !s->mmio_mapped_now) {
        hi = true;
        text = _("DRAM at 0E000h now: dimmed cells do not decode");
        tip = _("The upper area 0E000h+ is not mapped at the moment (OUT (0E1h)); only owners decoding in the DRAM state react. Left half of a cell = upper area mapped, right half = DRAM at 0E000h.");
    } else if (!s->tracking_active) {
        hi = true;
        text = _("Activity tracking is off: enable Track activity");
        tip = _("Without tracking the grid shows owners, marks and conflict counters of the bus core; IN/OUT rates, last values and ghost accesses are not collected.");
    } else if (mmio) {
        text = _("Left half: upper area mapped, right half: DRAM");
        tip = _("Addresses 0E000h-0E0FFh (row + column = low byte). Each cell has two halves: left = owner when the upper area is mapped, right = owner with DRAM at 0E000h; grey half = nobody decodes in that state.");
    } else {
        text = _("Ports 00h-0FFh (low address byte)");
        tip = _("Row + column = low byte of the port address; the emulator decodes the low byte only. Left click selects, right click opens actions, arrows move the selection.");
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 q(p.x + width, p.y + m.frame);
    if (hi) dl->AddRectFilled(p, q, COL_STRIP, 3.0f);
    ImGui::InvisibleButton("##bus_strip", ImVec2(width, m.frame));
    ImGui::PushClipRect(p, q, true);
    dl->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + (m.frame - m.line) * 0.5f), hi ? COL_TEXT : COL_TEXT_DIM, text);
    ImGui::PopClipRect();
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/**
 * @brief Mřížka 16 x 16 (porty nebo MMIO okno) s buňkou @p cell.
 */
void render_grid(bool mmio, float cell)
{
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;
    const Metrics &m = g_bus_ui.m;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    const float head = cell;
    const float pitch = cell + m.gap;
    char lbl[8];
    g_bus_ui.grid_drawn = true;
    g_bus_ui.grid_mmio = mmio;
    g_bus_ui.grid_origin = origin;
    g_bus_ui.cell = cell;
    g_bus_ui.hover_cell = -1;
    g_bus_ui.hover_cells = 0;
    unsigned sel = g_bus_ui.sel_addr[mmio ? BUS_TAB_MMIO : BUS_TAB_PORTS];
    const bool nav_visible = ImGui::GetIO().NavVisible;
    static PendingBadge badges[MAX_BADGES];
    unsigned badge_count = 0;
    auto badge = [&](ImVec2 c, const char *glyph, ImU32 outline) {
        if (badge_count < MAX_BADGES) badges[badge_count++] = { c, glyph, outline };
    };

    /* Záhlaví sloupců "0".."F" vystředěné nad sloupcem (V2). */
    for (int c = 0; c < 16; c++) {
        snprintf(lbl, sizeof(lbl), "%X", c);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        ImVec2 p(origin.x + head + (float) c * pitch + (cell - ts.x) * 0.5f, origin.y + (cell - ts.y) * 0.5f);
        dl->AddText(p, COL_TEXT_DIM, lbl);
    }
    for (int r = 0; r < 16; r++) {
        snprintf(lbl, sizeof(lbl), "%X0", r);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(origin.x + (cell - ts.x) * 0.5f, origin.y + head + (float) r * pitch + (cell - ts.y) * 0.5f), COL_TEXT_DIM, lbl);
        for (int c = 0; c < 16; c++) {
            unsigned idx = (unsigned) (r * 16 + c);
            unsigned addr = mmio ? BUS_MAP_MMIO_FIRST + idx : idx;
            const st_BUS_OWNER *owners;
            unsigned n;
            uint8_t flags;
            uint32_t hps;
            if (mmio) {
                owners = s->mmio[idx].owners; n = s->mmio[idx].owner_count;
                flags = s->mmio[idx].flags; hps = s->mmio[idx].hits_out_per_sec;
            } else {
                owners = s->ports[idx].owners; n = s->ports[idx].owner_count;
                flags = s->ports[idx].flags;
                hps = s->ports[idx].hits_in_per_sec + s->ports[idx].hits_out_per_sec;
            }
            ImVec2 a(origin.x + head + (float) c * pitch, origin.y + head + (float) r * pitch);
            ImVec2 b(a.x + cell, a.y + cell);
            ImGui::SetCursorScreenPos(a);
            char id[24];
            snprintf(id, sizeof(id), "##bus_cell_%c%03X", mmio ? 'm' : 'p', idx);
            ImGui::InvisibleButton(id, ImVec2(cell, cell), ImGuiButtonFlags_EnableNav);
            bool hovered = ImGui::IsItemHovered();
            const st_BUS_OWNER *o = primary_owner(owners, n);
            if (hovered) g_bus_ui.hover_cell = (int) idx;
            /* Klik myší, nebo aktivace klávesnicí (Space / Enter) vybere buňku a její primární vlastníka. */
            if (ImGui::IsItemActivated()) {
                select_cell(addr, mmio);
                set_sel_owner(o ? o->id : NULL, o ? o->kind : 0);
            }
            /* Šipky: ImGui přesune zaměření na sousední buňku; výběr ho následuje (K08). */
            if (nav_visible && ImGui::IsItemFocused() && sel != addr) {
                select_cell(addr, mmio);
                set_sel_owner(o ? o->id : NULL, o ? o->kind : 0);
                sel = addr;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                g_bus_ui.ctx_addr = addr;
                g_bus_ui.ctx_mmio = mmio;
                ImGui::OpenPopup("##bus_cell_ctx");
            }

            /* Pozadí: MMIO buňka se dělí na půlky MAPPED | DRAM podle when_mask
             * (šedá půlka = v tom stavu nikdo nedekóduje). */
            ImU32 col = cell_color(owners, n, hps);
            if (mmio && n) {
                uint8_t wm = s->mmio[idx].when_mask;
                ImU32 cl = (wm & DEVBUS_MMIO_MAPPED) ? col : COL_UNMAPPED;
                ImU32 cr = (wm & DEVBUS_MMIO_DRAM) ? col : COL_UNMAPPED;
                float mid = a.x + cell * 0.5f;
                dl->AddRectFilled(a, ImVec2(mid, b.y), cl, 3.0f, ImDrawFlags_RoundCornersLeft);
                dl->AddRectFilled(ImVec2(mid, a.y), b, cr, 3.0f, ImDrawFlags_RoundCornersRight);
            } else {
                dl->AddRectFilled(a, b, col, 3.0f);
            }
            if (mmio && !s->mmio_mapped_now && n) dl->AddRectFilled(a, b, COL_DIM_OVER, 3.0f);
            if (n && !any_present(owners, n)) draw_hatch(dl, a, b);
            dl->AddRect(a, b, COL_BORDER, 3.0f, 0, 1.0f);

            /* Zkratka; na šrafované buňce s tmavým stínem, aby ji světlé čáry nerozbily. */
            const char *txt = o ? o->abbrev : ".";
            ImVec2 ts = ImGui::CalcTextSize(txt);
            ImVec2 tp(a.x + (cell - ts.x) * 0.5f, a.y + (cell - ts.y) * 0.5f);
            if (n && !any_present(owners, n)) {
                dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), COL_BADGE, txt);
                dl->AddText(ImVec2(tp.x - 1.0f, tp.y + 1.0f), COL_BADGE, txt);
            }
            dl->AddText(tp, o ? COL_TEXT : COL_TEXT_DIM, txt);

            /* Odznaky: "!" kolize (vpravo nahoře), "?" nedoložený budič (vlevo nahoře),
             * "~" rozdělení podle směru (vlevo dole), tečka ghost (vpravo dole).
             * Střed sedí badge_off od rohu buňky, takže odznak zasahuje jen do
             * rohu (mimo zkratku) a zlehka přes mezeru; kreslí se až po všech
             * buňkách (R02). */
            const float bo = m.badge_off;
            if (flags & (BUS_PORT_POTENTIAL_CONFLICT | BUS_PORT_CONFLICT_SEEN)) {
                badge(ImVec2(b.x - bo, a.y + bo), "!", (flags & BUS_PORT_CONFLICT_SEEN) ? COL_BAD : COL_WARN);
            }
            if (cell_drive(owners, n) == DEVBUS_DRIVE_UNKNOWN) badge(ImVec2(a.x + bo, a.y + bo), "?", 0);
            if (flags & BUS_PORT_DIR_SPLIT) badge(ImVec2(a.x + bo, b.y - bo), "~", 0);
            if (flags & BUS_PORT_GHOST_SEEN) badge(ImVec2(b.x - bo, b.y - bo), NULL, COL_WARN);

            /* Zvýraznění: řádek seznamu pod myší (minulý snímek), hover buňky, výběr. */
            if (g_bus_ui.hover_row_id && o && o->kind == g_bus_ui.hover_row_kind
                && (o->id == g_bus_ui.hover_row_id || strcmp(o->id, g_bus_ui.hover_row_id) == 0)) {
                dl->AddRect(a, b, COL_ROW_HILITE, 3.0f, 0, 2.0f);
                g_bus_ui.hover_cells++;
            }
            if (sel == addr) dl->AddRect(a, b, COL_SELECT, 3.0f, 0, 2.0f);
            else if (hovered) dl->AddRect(a, b, COL_HOVER, 3.0f, 0, 1.0f);

            if (hovered) cell_tooltip(addr, mmio, owners, n, flags, idx);
        }
    }
    /* Druhý průchod: odznaky nad všemi buňkami (přesahují do mezery, soused je nesmí překreslit). */
    for (unsigned i = 0; i < badge_count; i++) draw_badge(dl, badges[i].c, m.badge_r, badges[i].glyph, badges[i].outline);
    /* Kontextové menu (jeden popup pro celou mřížku, adresa v g_bus_ui.ctx_*). */
    if (ImGui::BeginPopup("##bus_cell_ctx")) {
        ImGui::Text(g_bus_ui.ctx_mmio ? "%04Xh" : "%02Xh", g_bus_ui.ctx_addr);
        ImGui::Separator();
        cell_context_menu(g_bus_ui.ctx_addr, g_bus_ui.ctx_mmio);
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + m.grid_w(cell)));
    ImGui::Dummy(ImVec2(m.grid_w(cell), 0.0f));
}

/** @brief Tooltip řádku seznamu: úplný název, směr, všechny rozsahy, zdroje, zrcadla, údaje devbus. */
void row_tooltip(const OwnerRow &r, bool mmio)
{
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;
    if (!ImGui::BeginItemTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
    ImGui::TextUnformatted(r.full_name ? r.full_name : r.id);
    char rg[256];
    ranges_text(r, mmio, 0, rg, sizeof(rg));
    ImGui::Text("%s  %s", dir_word(r.dirs), rg);
    if (r.drive == DEVBUS_DRIVE_PARTIAL) ImGui::Text(_("Drive: %s, driven bits %02Xh"), drive_word(r.drive), r.drive_mask);
    else ImGui::Text(_("Drive: %s"), drive_word((r.dirs & BUS_DIR_R) ? r.drive : DEVBUS_DRIVE_NONE));
    for (unsigned i = 0; i < r.range_count; i++) {
        if (r.src[i]) ImGui::TextDisabled("%s %s", _("Source:"), r.src[i]);
    }
    ImGui::TextDisabled("%s %s", _("Mirrors:"), r.mirror ? r.mirror : _("not verified"));
    if (r.kind == BUS_OWNER_DEVBUS) {
        for (unsigned i = 0; i < s->device_count; i++) {
            const st_BUS_DEVICE *d = &s->devices[i];
            if (d->kind != BUS_OWNER_DEVBUS || strcmp(d->id, r.id) != 0) continue;
            ImGui::Text(_("Ports %u, MMIO addresses %u, memory regions %u, outputs %u%s%s"), d->port_count, d->mmio_count,
                        d->region_count, d->vout_count, d->has_glyph ? _(", glyph source") : "", d->has_row ? _(", render row") : "");
            break;
        }
        ImGui::TextDisabled("%s", r.connected ? _("Enter or the button disconnects the device") : _("Enter or the button connects the device"));
    } else {
        ImGui::TextDisabled("%s", _("Board chips are attached in the HW & Devices menu"));
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/**
 * @brief Šířka sloupce Ports: z delší referenční hodnoty obou záložek (R05: přepnutí záložky nic neposune).
 */
float ports_column_width(void)
{
    float cp = ImGui::GetStyle().CellPadding.x * 2.0f;
    return fmaxf(ImGui::CalcTextSize("E000h-E003h, E008h +2").x, ImGui::CalcTextSize("CCh-CEh, CFh, F0h +2").x) + cp;
}

/**
 * @brief Seznam vlastníků: ImGui tabulka se zebra pruhy, ScrollY, clipper, řazení podle prvního portu.
 * @param height Výška tabulky.
 */
void render_owner_list(bool mmio, float height)
{
    const Metrics &m = g_bus_ui.m;
    const ImGuiStyle &st = ImGui::GetStyle();
    const float row_h = m.mini;
    const float cp = st.CellPadding.x * 2.0f;
    float col_mini = m.mini + cp;
    float col_ports = ports_column_width();
    float col_drive = 0.0f, col_state = 0.0f;
    const en_DEVBUS_DRIVE drives[] = { DEVBUS_DRIVE_NONE, DEVBUS_DRIVE_TRISTATE, DEVBUS_DRIVE_PARTIAL, DEVBUS_DRIVE_OPEN_COLLECTOR, DEVBUS_DRIVE_UNKNOWN };
    for (en_DEVBUS_DRIVE d : drives) col_drive = fmaxf(col_drive, ImGui::CalcTextSize(drive_word(d)).x);
    col_drive = fmaxf(col_drive, ImGui::CalcTextSize(_("Drive")).x) + cp;
    for (int k = 0; k < 4; k++) col_state = fmaxf(col_state, ImGui::CalcTextSize(state_word(k & 1 ? BUS_OWNER_DEVBUS : BUS_OWNER_STATIC, (k & 2) != 0)).x);
    col_state = fmaxf(col_state, ImGui::CalcTextSize(_("State")).x) + cp;
    float col_action = fmaxf(ImGui::CalcTextSize(_("Connect")).x, ImGui::CalcTextSize(_("Disconnect")).x) + st.FramePadding.x * 2.0f + cp;
    int ncol = g_bus_ui.rows_devbus ? 6 : 5;
    /* Sloupec Owner pevný: zbytek šířky po odečtení posuvníku (vždy, i když tabulka zrovna neroluje)
     * a mezer mezi sloupci (bez vnitřních rámečků = 2 x CellPadding.x na mezeru); roztažný sloupec
     * by se s objevením posuvníku zúžil a posunul sloupce vpravo (R05). */
    float fixed_sum = col_mini + col_ports + col_drive + col_state + (ncol == 6 ? col_action : 0.0f);
    float col_owner = ImGui::GetContentRegionAvail().x - st.ScrollbarSize - cp * (float) (ncol - 1) - fixed_sum;
    if (col_owner < m.fs * 2.0f) col_owner = m.fs * 2.0f;
    char tid[64];
    snprintf(tid, sizeof(tid), "##bus_owners_%c_%d", mmio ? 'm' : 'p', (int) (col_ports + col_drive + col_state + col_action + m.mini));
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings;
    g_bus_ui.visible_rows = 0;
    g_bus_ui.hover_row_id_now = NULL;
    for (unsigned i = 0; i < MAX_ROWS; i++) g_bus_ui.row_drawn[i] = false;
    if (!ImGui::BeginTable(tid, ncol, tf, ImVec2(0.0f, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("##mini", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, col_mini);
    ImGui::TableSetupColumn(_("Owner"), ImGuiTableColumnFlags_WidthFixed, col_owner);
    ImGui::TableSetupColumn(_("Ports"), ImGuiTableColumnFlags_WidthFixed, col_ports);
    ImGui::TableSetupColumn(_("Drive"), ImGuiTableColumnFlags_WidthFixed, col_drive);
    ImGui::TableSetupColumn(_("State"), ImGuiTableColumnFlags_WidthFixed, col_state);
    if (ncol == 6) ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, col_action);
    ImGui::TableHeadersRow();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin((int) g_bus_ui.row_count, row_h + st.CellPadding.y * 2.0f);
    if (g_bus_ui.sel_row >= 0) clipper.IncludeItemByIndex(g_bus_ui.sel_row);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            const OwnerRow &r = g_bus_ui.rows[i];
            bool selected = (i == g_bus_ui.sel_row);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Header, COL4_ROW_SEL);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, COL4_ROW_SEL_HI);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, COL4_ROW_SEL_HI);
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, row_h))) {
                select_cell(r.first_addr, mmio);
                set_sel_owner(r.id, r.kind);
            }
            ImGui::PopStyleColor(3);
            bool row_hovered = ImGui::IsItemHovered();
            if (row_hovered) { g_bus_ui.hover_row_id_now = r.id; g_bus_ui.hover_row_kind_now = r.kind; }
            if (r.kind == BUS_OWNER_DEVBUS && ImGui::IsItemFocused()
                && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
                devbus_request_connect(r.id, !r.connected);
            }
            row_tooltip(r, mmio);
            if (selected && g_bus_ui.want_scroll_row) {
                ImGui::SetScrollHereY(0.5f);
                g_bus_ui.want_scroll_row = false;
            }
            /* Mini buňka = legenda: barva sleduje režim obarvení, zkratka jako v mřížce. */
            draw_mini(dl, ImVec2(p.x, p.y), m.mini, row_color(r), r.abbrev, !r.connected);

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImVec2 np = ImGui::GetCursorScreenPos();
            g_bus_ui.row_x = np.x + ImGui::GetContentRegionAvail().x * 0.5f;
            g_bus_ui.row_y[i] = p.y + row_h * 0.5f;
            g_bus_ui.row_drawn[i] = true;
            ImGui::TextUnformatted(r.name);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            char rg[64];
            ranges_text(r, mmio, SHOWN_RANGES, rg, sizeof(rg));
            ImGui::TextUnformatted(rg);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(drive_word((r.dirs & BUS_DIR_R) ? r.drive : DEVBUS_DRIVE_NONE));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(state_word(r.kind, r.connected));
            if (ncol == 6) {
                ImGui::TableNextColumn();
                if (r.kind == BUS_OWNER_DEVBUS) {
                    if (selected) {
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.7f));
                        ImGui::PushStyleColor(ImGuiCol_Button, COL4_SEL_BTN);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL4_SEL_BTN_HOVER);
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL4_SEL_BTN_ACTIVE);
                    }
                    if (ImGui::Button(r.connected ? _L("Disconnect##bus_row_act") : _L("Connect##bus_row_act"), ImVec2(-FLT_MIN, 0.0f))) {
                        devbus_request_connect(r.id, !r.connected);
                    }
                    if (selected) { ImGui::PopStyleColor(4); ImGui::PopStyleVar(); }
                }
            }
            g_bus_ui.visible_rows++;
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

/**
 * @brief Detail vybrané adresy pod seznamem (nízký panel, K07, K10, K17, K21).
 *
 * Pevný počet řádků ve všech stavech (R04): oddělovač, titulek, řádek
 * o vlastníkovi (směr a budič, další vlastníci; bez vlastníka co vrátí
 * čtení), dva řádky aktivity (bez sledování pomlčky a "Activity tracking
 * is off" místo posledních hodnot), peek, příznaky, 2 x 2 tlačítka.
 * Dlouhé řádky se neořezávají zalomením, ale oříznutím s úplným zněním
 * v tooltipu. Skutečnou výšku si změří pro kontrolu detail_height().
 */
void render_detail(bool mmio)
{
    const float y0 = ImGui::GetCursorPosY();
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;
    const Metrics &m = g_bus_ui.m;
    const ImGuiStyle &st = ImGui::GetStyle();
    unsigned addr = g_bus_ui.sel_addr[g_bus_ui.active_tab];
    const st_BUS_OWNER *owners;
    unsigned n;
    uint8_t flags;
    selected_owners(&owners, &n, &flags);
    const st_BUS_PORT *bp = mmio ? NULL : &s->ports[addr & 0xFF];
    const st_BUS_MMIO_CELL *mc = mmio ? &s->mmio[(addr - BUS_MAP_MMIO_FIRST) & 0xFF] : NULL;

    /* Vlastník titulku: vybraný řádek, pokud adresu vlastní, jinak primární. */
    const st_BUS_OWNER *o = primary_owner(owners, n);
    if (g_bus_ui.sel_owner_id) {
        for (unsigned i = 0; i < n; i++) {
            if (owners[i].kind == g_bus_ui.sel_owner_kind
                && (owners[i].id == g_bus_ui.sel_owner_id || strcmp(owners[i].id, g_bus_ui.sel_owner_id) == 0)) { o = &owners[i]; break; }
        }
    }

    ImGui::Separator();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (o) draw_mini(dl, p, m.mini, g_bus_ui.color_mode == BUS_COLOR_ACTIVITY ? heat_color(bp ? bp->hits_in_per_sec + bp->hits_out_per_sec : mc->hits_out_per_sec, true)
                                               : g_bus_ui.color_mode == BUS_COLOR_DRIVE ? drive_color(cell_drive(owners, n)) : owner_color(o->kind, o->connected != 0),
                     o->abbrev, !any_present(owners, n));
    else draw_mini(dl, p, m.mini, COL_UNMAPPED, ".", false);
    ImGui::Dummy(ImVec2(m.mini, m.mini));
    ImGui::SameLine(0.0f, st.ItemSpacing.x);
    ImGui::AlignTextToFramePadding();
    char title[160];
    if (o) snprintf(title, sizeof(title), mmio ? "%s %04Xh  %s" : "%s %02Xh  %s", mmio ? _("Address") : _("Port"), addr, o->name);
    else snprintf(title, sizeof(title), mmio ? "%s %04Xh - %s" : "%s %02Xh - %s", mmio ? _("Address") : _("Port"), addr, _("no owner"));
    ImGui::TextUnformatted(title);
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        for (unsigned i = 0; i < n; i++) {
            ImGui::Text("%s  %s (%s%s)", dir_word(owners[i].dirs), owners[i].name,
                        owners[i].kind == BUS_OWNER_DEVBUS ? _("device") : _("board"),
                        owners[i].connected ? "" : (owners[i].kind == BUS_OWNER_DEVBUS ? _(", not connected") : _(", not present")));
        }
        if (!n) ImGui::Text(_("A read returns the undriven bus: %s %02Xh"), s->bus_idle == DEVBUS_IDLE_CONST ? _("constant") : _("bus latch"), s->idle_byte);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    /* Řádek o vlastníkovi (vždy jeden): směr a budič vybraného vlastníka, další vlastníci
     * téže adresy (dva statické téhož směru, rozdělení podle směru); bez vlastníka co
     * čtení vrátí (K21), přímo v detailu, ne jen v tooltipu. */
    char info[320];
    if (!n) {
        snprintf(info, sizeof(info), _("A read returns the undriven bus: %s %02Xh"),
                 s->bus_idle == DEVBUS_IDLE_CONST ? _("constant") : _("bus latch"), s->idle_byte);
    } else {
        size_t used = (size_t) snprintf(info, sizeof(info), "%s, %s", dir_word(o->dirs), drive_word(o->drive));
        if (o->drive == DEVBUS_DRIVE_PARTIAL && used < sizeof(info)) {
            char bits[48];
            snprintf(bits, sizeof(bits), _("Driven bits: %02Xh"), o->drive_mask);
            used += (size_t) snprintf(info + used, sizeof(info) - used, " (%s)", bits);
        }
        if (n > 1 && used < sizeof(info)) used += (size_t) snprintf(info + used, sizeof(info) - used, "; %s", _("Also:"));
        for (unsigned i = 0; i < n && used < sizeof(info); i++) {
            if (&owners[i] == o) continue;
            used += (size_t) snprintf(info + used, sizeof(info) - used, " %s %s;", dir_word(owners[i].dirs), owners[i].name);
        }
        if (n > 1 && used && used < sizeof(info) && info[used - 1] == ';') info[used - 1] = '\0';
    }
    if (clipped_line(info, COL_TEXT)) ImGui::SetItemTooltip("%s", info);

    ImFont *mono = myimgui_get_monospace_font();
    if (mono) ImGui::PushFont(mono, 0.0f);
    char act[96], last[96];
    if (!s->tracking_active) {
        if (!mmio) snprintf(act, sizeof(act), "%-5s %6s  %-5s %6s  %-5s %6s  %-9s %6s", _("IN/s"), "-", _("OUT/s"), "-", _("ghost"), "-", _("conflicts"), "-");
        else snprintf(act, sizeof(act), "%-8s %6s", _("writes/s"), "-");
        last[0] = '\0'; /* místo posledních hodnot řádek "Activity tracking is off" (stejná výška) */
    } else if (!mmio) {
        snprintf(act, sizeof(act), "%-5s %6u  %-5s %6u  %-5s %6llu  %-9s %6llu", _("IN/s"), bp->hits_in_per_sec, _("OUT/s"), bp->hits_out_per_sec,
                 _("ghost"), (unsigned long long) bp->ghost_hits, _("conflicts"), (unsigned long long) bp->conflict_hits);
        char li[40], lo[40];
        if (bp->last_read_valid) snprintf(li, sizeof(li), "%02Xh (%u)", bp->last_read_value, bp->last_read_age); else snprintf(li, sizeof(li), "-");
        if (bp->last_write_valid) snprintf(lo, sizeof(lo), "%02Xh (%u)", bp->last_write_value, bp->last_write_age); else snprintf(lo, sizeof(lo), "-");
        snprintf(last, sizeof(last), "%s %-12s %s %-12s", _("Last IN"), li, _("Last OUT"), lo);
    } else {
        snprintf(act, sizeof(act), "%-8s %6u", _("writes/s"), mc->hits_out_per_sec);
        if (mc->last_write_valid) snprintf(last, sizeof(last), "%s %02Xh (%u)", _("Last write"), mc->last_write_value, mc->last_write_age);
        else snprintf(last, sizeof(last), "%s -", _("Last write"));
    }
    ImGui::TextUnformatted(act);
    if (s->tracking_active) {
        ImGui::TextUnformatted(last);
        if (mono) ImGui::PopFont();
        ImGui::SetItemTooltip("%s", _("Last value and its age in frames; rates are per second of emulated time"));
    } else {
        if (mono) ImGui::PopFont();
        ImGui::TextDisabled("%s", _("Activity tracking is off"));
    }
    snprintf(g_bus_ui.activity_line, sizeof(g_bus_ui.activity_line), "%s", act);

    /* Peek: v pauze automaticky (jednou pro výběr a pak po 25 snímcích), za běhu na tlačítko. */
    bool paused = EMULATOR_TEST_PAUSED;
    if (paused && (!g_bus_ui.peek_paused_done || (s->frame - g_bus_ui.peek_frame) > 25u
                   || g_bus_ui.peek_addr != addr || g_bus_ui.peek_mmio != mmio)) {
        do_peek();
        g_bus_ui.peek_paused_done = true;
    }
    bool pk_ok = g_bus_ui.peek_valid && g_bus_ui.peek_addr == addr && g_bus_ui.peek_mmio == mmio;
    char pk[96];
    if (pk_ok) {
        const char *src = g_bus_ui.peek_source == DBGAPI_BUS_PEEK_SRC_DEVBUS ? _("device")
                        : g_bus_ui.peek_source == DBGAPI_BUS_PEEK_SRC_BOARD ? _("board") : _("undriven bus");
        if (g_bus_ui.peek_driven == 0xFF) snprintf(pk, sizeof(pk), _("Peek: %02Xh, %s"), g_bus_ui.peek_value, src);
        else snprintf(pk, sizeof(pk), _("Peek: %02Xh, driven bits %02Xh, %s"), g_bus_ui.peek_value, g_bus_ui.peek_driven, src);
    } else {
        snprintf(pk, sizeof(pk), "%s", _("Peek: -"));
    }
    if (paused) {
        size_t used = strlen(pk);
        snprintf(pk + used, sizeof(pk) - used, " %s", _("(automatic while paused)"));
    }
    snprintf(g_bus_ui.peek_line, sizeof(g_bus_ui.peek_line), "%s", pk);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(pk);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", _("The value the CPU would read now, without side effects (device peek handlers, board register mirrors, undriven bus model)"));
    if (!paused) {
        ImGui::SameLine(0.0f, st.ItemSpacing.x * 2.0f);
        if (ImGui::Button(_L("Peek now##bus_det_peek"))) do_peek();
    }

    char fl[256], fl_line[300];
    flags_words(flags, owners, n, fl, sizeof(fl));
    snprintf(fl_line, sizeof(fl_line), "%s %s", _("Flags:"), fl[0] ? fl : "-");
    snprintf(g_bus_ui.flags_line, sizeof(g_bus_ui.flags_line), "%s", fl_line);
    bool fl_cut = clipped_line(fl_line, COL_TEXT);
    bool fl_conflict = (flags & (BUS_PORT_POTENTIAL_CONFLICT | BUS_PORT_CONFLICT_SEEN)) != 0;
    if ((fl_cut || fl_conflict) && ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        if (fl_cut) ImGui::TextUnformatted(fl_line);
        if (fl_conflict) ImGui::TextUnformatted(_("Two drivers on the same bits: the result on real hardware is not modelled; the conflict policy decides the emulated value."));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    /* 2 x 2 tlačítka stejné šířky (K07). */
    float bw = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
    float bmin = 0.0f;
    const char *labels[] = { _("Show in I/O Ports"), _("Reset activity"), _("Break on IN"), _("Break on OUT") };
    for (const char *l : labels) bmin = fmaxf(bmin, ImGui::CalcTextSize(l).x + st.FramePadding.x * 2.0f);
    if (bw < bmin) bw = bmin;
    if (ImGui::Button(_L("Show in I/O Ports##bus_det_io"), ImVec2(bw, 0.0f))) io_window_request_show_port((uint16_t) addr);
    ImGui::SameLine();
    if (ImGui::Button(_L("Reset activity##bus_det_rst"), ImVec2(bw, 0.0f))) dbg_ui_io_activity_reset_port((uint16_t) addr, !mmio);
    ImGui::BeginDisabled(mmio);
    if (ImGui::Button(_L("Break on IN##bus_det_bpr"), ImVec2(bw, 0.0f))) bpt_edit_panel_open_new_iorq((uint16_t) addr, false);
    if (mmio) ImGui::SetItemTooltip("%s", _("IORQ breakpoints apply to ports only"));
    ImGui::SameLine();
    if (ImGui::Button(_L("Break on OUT##bus_det_bpw"), ImVec2(bw, 0.0f))) bpt_edit_panel_open_new_iorq((uint16_t) addr, true);
    if (mmio) ImGui::SetItemTooltip("%s", _("IORQ breakpoints apply to ports only"));
    ImGui::EndDisabled();
    g_bus_ui.detail_h = ImGui::GetCursorPosY() - y0 - st.ItemSpacing.y;
}

/**
 * @brief Výška detailu, stejná ve všech stavech (R04): oddělovač (1 px čára v layoutu bez výšky, jen
 *        mezera), titulek, 4 řádky textu, řádek peeku a 2 řady tlačítek ve výšce rámečku, 8 mezer;
 *        bez mezery za poslední řadou. Test hlídá shodu se změřenou výškou.
 */
float detail_height(void)
{
    const Metrics &m = g_bus_ui.m;
    const ImGuiStyle &st = ImGui::GetStyle();
    return m.frame * 4.0f + m.line * 4.0f + st.ItemSpacing.y * 8.0f;
}

/**
 * @brief Minimum pravé části z textů sloupců a tlačítek detailu (K01).
 *
 * Nezávisí na záložce (R05): názvy vlastníků se berou z obou mřížek
 * (každý název jednou, podle ukazatele), sloupec Ports z delší referenční
 * hodnoty, sloupec Action podle registrovaných zařízení.
 */
float list_min_width(void)
{
    const Metrics &m = g_bus_ui.m;
    const ImGuiStyle &st = ImGui::GetStyle();
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;
    float cp = st.CellPadding.x * 2.0f;
    float w = m.mini + cp;
    float owner_w = ImGui::CalcTextSize("K&P 80 Column Card").x;
    const char *seen[2 * MAX_ROWS];
    unsigned seen_n = 0;
    auto measure = [&](const st_BUS_OWNER *owners, unsigned n) {
        for (unsigned k = 0; k < n; k++) {
            const char *name = owners[k].name;
            unsigned j = 0;
            while (j < seen_n && seen[j] != name) j++;
            if (j < seen_n) continue;
            if (seen_n < 2 * MAX_ROWS) seen[seen_n++] = name;
            char disp[64];
            display_name(name, disp, sizeof(disp));
            owner_w = fmaxf(owner_w, ImGui::CalcTextSize(disp).x);
        }
    };
    for (unsigned i = 0; i < 256; i++) {
        measure(s->ports[i].owners, s->ports[i].owner_count);
        measure(s->mmio[i].owners, s->mmio[i].owner_count);
    }
    w += owner_w + cp + m.fs * 0.5f;
    w += ports_column_width();
    float dmax = ImGui::CalcTextSize(_("Drive")).x, smax = ImGui::CalcTextSize(_("State")).x;
    const en_DEVBUS_DRIVE drives[] = { DEVBUS_DRIVE_NONE, DEVBUS_DRIVE_TRISTATE, DEVBUS_DRIVE_PARTIAL, DEVBUS_DRIVE_OPEN_COLLECTOR, DEVBUS_DRIVE_UNKNOWN };
    for (en_DEVBUS_DRIVE d : drives) dmax = fmaxf(dmax, ImGui::CalcTextSize(drive_word(d)).x);
    for (int k = 0; k < 4; k++) smax = fmaxf(smax, ImGui::CalcTextSize(state_word(k & 1 ? BUS_OWNER_DEVBUS : BUS_OWNER_STATIC, (k & 2) != 0)).x);
    w += dmax + cp + smax + cp;
    if (g_bus_ui.rows_devbus) w += fmaxf(ImGui::CalcTextSize(_("Connect")).x, ImGui::CalcTextSize(_("Disconnect")).x) + st.FramePadding.x * 2.0f + cp;
    w += st.ScrollbarSize + st.WindowPadding.x * 2.0f;
    float bmin = 0.0f;
    const char *labels[] = { _("Show in I/O Ports"), _("Reset activity"), _("Break on IN"), _("Break on OUT") };
    for (const char *l : labels) bmin = fmaxf(bmin, ImGui::CalcTextSize(l).x + st.FramePadding.x * 2.0f);
    float dw = bmin * 2.0f + st.ItemSpacing.x + st.WindowPadding.x * 2.0f;
    return ceilf(fmaxf(w, dw));
}

/** @brief Rozměry z písma a ze zkratek snímku. */
void compute_metrics(void)
{
    Metrics &m = g_bus_ui.m;
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;
    m.fs = ImGui::GetFontSize();
    m.line = ImGui::GetTextLineHeight();
    m.frame = ImGui::GetFrameHeight();
    m.margin = fmaxf(2.0f, roundf(m.fs * 0.1f));
    m.gap = fmaxf(2.0f, roundf(m.fs * 0.1f));
    float abbr_w = ImGui::CalcTextSize("F0").x;
    for (unsigned i = 0; i < 256; i++) {
        for (unsigned k = 0; k < s->ports[i].owner_count; k++) abbr_w = fmaxf(abbr_w, ImGui::CalcTextSize(s->ports[i].owners[k].abbrev).x);
        for (unsigned k = 0; k < s->mmio[i].owner_count; k++) abbr_w = fmaxf(abbr_w, ImGui::CalcTextSize(s->mmio[i].owners[k].abbrev).x);
    }
    m.cell_min = fmaxf(24.0f, ceilf(fmaxf(abbr_w, m.line) + 2.0f * m.margin)); /* 24 px = cíl myši */
    /* Odznak 0,2 em na rohu buňky (R02): zasahuje dovnitř badge_off + r + 0,5 px prstence, zkratka
     * s výškou řádku vycentrovaná má k okraji buňky víc než to (28 px: 8,5 px vs. 11 px). */
    m.badge_r = fmaxf(5.0f, roundf(m.fs * 0.2f));
    m.badge_off = fmaxf(1.0f, roundf(m.badge_r / 3.0f));
    m.mini = m.frame;
    m.split = fmaxf(6.0f, roundf(m.fs * 0.25f));
}

/** @brief Pomocník "(?)" s tooltipem (vzor HelpMarker z imgui_demo). */
void help_marker(const char *text)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/** @brief Přepínač záložky jako tlačítko ve stylu záložky. */
void tab_button(const char *label, int tab)
{
    bool active = g_bus_ui.active_tab == tab;
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(active ? ImGuiCol_TabSelected : ImGuiCol_Tab));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
    if (ImGui::Button(label) && !active) {
        g_bus_ui.active_tab = tab;
        g_bus_ui.sel_owner_id = NULL;
        g_bus_ui.want_scroll_row = true;
    }
    ImGui::PopStyleColor(3);
}

} /* anonymous namespace */


extern "C" void bus_window_select(unsigned addr, bool mmio)
{
    select_cell(addr, mmio);
    g_bus_ui.sel_owner_id = NULL;
    g_bus_ui.want_tab[mmio ? BUS_TAB_MMIO : BUS_TAB_PORTS] = true;
}


extern "C" unsigned bus_window_selected(bool *mmio)
{
    if (mmio) *mmio = g_bus_ui.active_tab == BUS_TAB_MMIO;
    return g_bus_ui.sel_addr[g_bus_ui.active_tab];
}


extern "C" bool bus_window_cell_center(unsigned addr, float *x, float *y)
{
    if (!g_bus_ui.grid_drawn) return false;
    unsigned idx = g_bus_ui.grid_mmio ? addr - BUS_MAP_MMIO_FIRST : addr;
    if (idx > 255) return false;
    unsigned r = idx / 16, c = idx % 16;
    float pitch = g_bus_ui.cell + g_bus_ui.m.gap;
    if (x) *x = g_bus_ui.grid_origin.x + g_bus_ui.cell + (float) c * pitch + g_bus_ui.cell * 0.5f;
    if (y) *y = g_bus_ui.grid_origin.y + g_bus_ui.cell + (float) r * pitch + g_bus_ui.cell * 0.5f;
    return true;
}


extern "C" bool bus_window_row_center(int row, float *x, float *y)
{
    if (row < 0 || row >= (int) g_bus_ui.row_count || !g_bus_ui.row_drawn[row]) return false;
    if (x) *x = g_bus_ui.row_x;
    if (y) *y = g_bus_ui.row_y[row];
    return true;
}


extern "C" void bus_window_show_tab(int tab)
{
    g_bus_ui.want_tab[tab == BUS_TAB_MMIO ? BUS_TAB_MMIO : BUS_TAB_PORTS] = true;
}


extern "C" void bus_window_debug_get(st_BUS_WINDOW_DEBUG *out)
{
    memset(out, 0, sizeof(*out));
    out->cell = g_bus_ui.cell;
    out->cell_min = g_bus_ui.m.cell_min;
    out->natural_w = g_bus_ui.natural_w;
    out->list_w = g_bus_ui.list_w;
    out->list_min_w = g_bus_ui.list_min_w;
    out->row_h = g_bus_ui.m.mini;
    out->detail_h = g_bus_ui.detail_h;
    out->detail_fixed_h = detail_height();
    out->split_x = g_bus_ui.split_center.x;
    out->split_y = g_bus_ui.split_center.y;
    out->color_mode = g_bus_ui.color_mode;
    out->active_tab = g_bus_ui.active_tab;
    out->row_count = (int) g_bus_ui.row_count;
    out->selected_row = g_bus_ui.sel_row;
    out->hover_cells = g_bus_ui.hover_cells;
    out->visible_rows = g_bus_ui.visible_rows;
    snprintf(out->activity_line, sizeof(out->activity_line), "%s", g_bus_ui.activity_line);
    snprintf(out->peek_line, sizeof(out->peek_line), "%s", g_bus_ui.peek_line);
    snprintf(out->flags_line, sizeof(out->flags_line), "%s", g_bus_ui.flags_line);
    snprintf(out->status_line, sizeof(out->status_line), "%s", g_bus_ui.status_line);
}


extern "C" void bus_window_debug_set(int color_mode, int tracking)
{
    if (color_mode >= BUS_COLOR_OWNERS && color_mode <= BUS_COLOR_DRIVE) g_bus_ui.color_mode = color_mode;
    if (tracking == 0 || tracking == 1) {
        g_bus_ui.tracking = tracking != 0;
        g_bus_ui.cfg_tracking = tracking;
        if (io_activity_tracking_set_client(IO_TRACKING_CLIENT_BUS_MAP, g_bus_ui.tracking)) dbg_ui_debugger_state_recompute();
    }
}


extern "C" void bus_window_render(bool *p_open)
{
    if (!p_open || !*p_open) {
        if (g_bus_ui.was_open) {
            if (io_activity_tracking_set_client(IO_TRACKING_CLIENT_BUS_MAP, false)) dbg_ui_debugger_state_recompute();
            g_bus_ui.was_open = false;
        }
        return;
    }

    g_bus_ui.grid_drawn = false;
    /* Snímek pod zámkem stavu zařízení (jen čtení tabulek a deskriptorů). */
    device_state_lock();
    bus_map_snapshot(&g_bus_ui.snap, BUS_WANT_ALL);
    device_state_unlock();
    const st_BUS_MAP_SNAPSHOT *s = &g_bus_ui.snap;

    if (!g_bus_ui.was_open) {
        g_bus_ui.tracking = g_bus_ui.cfg_tracking != 0;
        if (io_activity_tracking_set_client(IO_TRACKING_CLIENT_BUS_MAP, g_bus_ui.tracking)) dbg_ui_debugger_state_recompute();
        if (g_bus_ui.active_tab != BUS_TAB_MMIO) g_bus_ui.active_tab = BUS_TAB_PORTS; /* 2 = zrušená záložka Devices */
        if (g_bus_ui.color_mode < BUS_COLOR_OWNERS || g_bus_ui.color_mode > BUS_COLOR_DRIVE) g_bus_ui.color_mode = BUS_COLOR_OWNERS;
    }
    for (int t = 0; t < 2; t++) {
        if (g_bus_ui.want_tab[t]) { g_bus_ui.active_tab = t; g_bus_ui.want_scroll_row = true; }
        g_bus_ui.want_tab[t] = false;
    }
    const bool mmio = g_bus_ui.active_tab == BUS_TAB_MMIO;

    compute_metrics();
    build_rows(mmio);
    Metrics &m = g_bus_ui.m;
    const ImGuiStyle &st = ImGui::GetStyle();

    /* Vybraný řádek: vybraný vlastník, pokud adresu vlastní, jinak primární vlastník buňky. */
    {
        const st_BUS_OWNER *owners; unsigned n; uint8_t flags;
        selected_owners(&owners, &n, &flags);
        const st_BUS_OWNER *pick = NULL;
        if (g_bus_ui.sel_owner_id) {
            for (unsigned i = 0; i < n; i++) {
                if (owners[i].kind == g_bus_ui.sel_owner_kind
                    && (owners[i].id == g_bus_ui.sel_owner_id || strcmp(owners[i].id, g_bus_ui.sel_owner_id) == 0)) { pick = &owners[i]; break; }
            }
        }
        if (!pick) pick = primary_owner(owners, n);
        g_bus_ui.sel_row = pick ? find_row(pick->id, pick->kind) : -1;
        if (pick) set_sel_owner(pick->id, pick->kind); else g_bus_ui.sel_owner_id = NULL;
    }

    /* Přirozená šířka obsahu: mřížka s minimální buňkou + oddělovač + minimum seznamu (stejné pro obě záložky).
     * Stavový řádek pod tělem = mezera, oddělovač 1 px, mezera, řádek ve výšce rámečku; tělo dostane zbytek,
     * takže obsah okna nikdy nepřeteče a okno samo nepotřebuje posuvník (R01). */
    float left_min_w = m.grid_w(m.cell_min) + st.WindowPadding.x * 2.0f;
    float list_min_w = list_min_width();
    float natural_w = left_min_w + m.split + list_min_w;
    float status_h = m.frame + st.ItemSpacing.y * 2.0f + 1.0f;
    float natural_h = m.frame * 3.0f + status_h + m.grid_w(m.cell_min) + st.ItemSpacing.y * 5.0f + st.WindowPadding.y * 4.0f;
    g_bus_ui.natural_w = natural_w;
    g_bus_ui.list_min_w = list_min_w;

    ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    ImVec2 first(fminf(natural_w + st.WindowPadding.x * 2.0f, work.x * 0.95f), fminf(natural_h, work.y * 0.95f));
    ImGui::SetNextWindowSize(first, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(m.fs * 18.0f, m.fs * 13.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!g_bus_ui.was_open) ImGui::SetNextWindowFocus();
    const char *title = _L("Bus Map###bus_map");
    auto_layout_first_use_portrait(title, first.x, first.y);
    if (!ImGui::Begin(title, p_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        g_bus_ui.was_open = true;
        return;
    }
    g_bus_ui.was_open = true;

    /* Toolbar: Ports | MMIO, Color by + (?), Track activity */
    tab_button(_L("Ports##bus_tab_ports"), BUS_TAB_PORTS);
    ImGui::SameLine(0.0f, 2.0f);
    tab_button(_L("MMIO##bus_tab_mmio"), BUS_TAB_MMIO);
    ImGui::SameLine(0.0f, st.ItemSpacing.x * 4.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(_("Color by"));
    ImGui::SameLine();
    {
        static const int mode_of_idx[3] = { BUS_COLOR_OWNERS, BUS_COLOR_DRIVE, BUS_COLOR_ACTIVITY };
        static const int idx_of_mode[3] = { 0, 2, 1 };
        const char *items[3] = { _("Owners"), _("Drive"), _("Activity") };
        int idx = idx_of_mode[g_bus_ui.color_mode];
        float cw = 0.0f;
        for (const char *it : items) cw = fmaxf(cw, ImGui::CalcTextSize(it).x);
        ImGui::SetNextItemWidth(cw + st.FramePadding.x * 2.0f + m.frame + st.ItemInnerSpacing.x);
        if (ImGui::Combo("##bus_color_by", &idx, items, 3)) g_bus_ui.color_mode = mode_of_idx[idx];
    }
    ImGui::SameLine();
    {
        int warm = 1, hot = 10000;
        io_window_heat_thresholds(&warm, &hot);
        char help[640];
        snprintf(help, sizeof(help), "%s\n%s\n%s", _("Owners: blue = board chip, green = expansion device; dark and hatched = not present or not connected; grey = no owner."),
                 _("Drive: how the owner drives the data bus when read: tri-state, partial (some bits), open collector, none (write only), not documented (no schematic in the knowledge base)."),
                 _("Activity: grey = no access, green = active, red = hot; the thresholds are shared with the I/O Ports window:"));
        size_t used = strlen(help);
        snprintf(help + used, sizeof(help) - used, " %s %d, %s %d.", _("active above"), warm, _("hot above"), hot);
        help_marker(help);
    }
    ImGui::SameLine(0.0f, st.ItemSpacing.x * 3.0f);
    if (ImGui::Checkbox(_L("Track activity##bus_track"), &g_bus_ui.tracking)) {
        g_bus_ui.cfg_tracking = g_bus_ui.tracking ? 1 : 0;
        if (io_activity_tracking_set_client(IO_TRACKING_CLIENT_BUS_MAP, g_bus_ui.tracking)) dbg_ui_debugger_state_recompute();
    }
    ImGui::SetItemTooltip("%s", _("Shared with the I/O Ports window: counts IN / OUT per port, ghost and conflict accesses (logging CPU callbacks)."));

    /* Tělo: pod přirozenou šířkou vodorovný posuvník, nic se nepřeskládává (K01). */
    float avail_w = ImGui::GetContentRegionAvail().x;
    float inner_w = fmaxf(avail_w, natural_w);
    ImGui::SetNextWindowContentSize(ImVec2(inner_w, 0.0f));
    if (ImGui::BeginChild("##bus_body", ImVec2(0.0f, -status_h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_HorizontalScrollbar)) {
        float body_h = ImGui::GetContentRegionAvail().y;
        float grid_avail_h = body_h - m.frame - st.ItemSpacing.y - st.WindowPadding.y * 2.0f;
        float cell_fit_h = floorf((grid_avail_h - 15.0f * m.gap) / 17.0f);
        float cell, left_w, list_w;
        if (g_bus_ui.list_width <= 0) {
            cell = fmaxf(m.cell_min, cell_fit_h);
            left_w = m.grid_w(cell) + st.WindowPadding.x * 2.0f;
            list_w = inner_w - left_w - m.split;
            if (list_w < list_min_w) {
                list_w = list_min_w;
                left_w = inner_w - list_w - m.split;
                float fit_w = floorf((left_w - st.WindowPadding.x * 2.0f - 15.0f * m.gap) / 17.0f);
                cell = fmaxf(m.cell_min, fminf(fit_w, fmaxf(m.cell_min, cell_fit_h)));
            }
        } else {
            list_w = fminf(fmaxf((float) g_bus_ui.list_width, list_min_w), inner_w - left_min_w - m.split);
            left_w = inner_w - list_w - m.split;
            float fit_w = floorf((left_w - st.WindowPadding.x * 2.0f - 15.0f * m.gap) / 17.0f);
            cell = fmaxf(m.cell_min, fminf(fit_w, cell_fit_h));
        }
        g_bus_ui.list_w = list_w;

        if (ImGui::BeginChild("##bus_left", ImVec2(left_w, body_h), ImGuiChildFlags_NavFlattened)) {
            render_strip(mmio, m.grid_w(cell));
            render_grid(mmio, cell);
        }
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("##bus_splitter", ImVec2(m.split, body_h));
        bool split_active = ImGui::IsItemActive();
        bool split_hot = ImGui::IsItemHovered() || split_active;
        if (split_active) {
            float dx = ImGui::GetIO().MouseDelta.x;
            if (dx != 0.0f) g_bus_ui.list_width = (int) fmaxf(list_min_w, list_w - dx);
        }
        if (split_hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) g_bus_ui.list_width = 0;
        {
            /* Viditelná čára oddělovače (R06): barva tématu s plnou alfou (Dark: 6E6E80 na 0E0E0E = 3,9:1),
             * 1 px uprostřed úchopu, při hoveru / tažení 2 px v barvě hoveru / tažení. */
            ImVec2 sa = ImGui::GetItemRectMin(), sb = ImGui::GetItemRectMax();
            g_bus_ui.split_center = ImVec2((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f);
            ImVec4 c4 = ImGui::GetStyleColorVec4(split_active ? ImGuiCol_SeparatorActive : split_hot ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator);
            c4.w = 1.0f;
            float x = floorf((sa.x + sb.x) * 0.5f);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(split_hot ? x - 1.0f : x, sa.y), ImVec2(x + 1.0f, sb.y), ImGui::GetColorU32(c4));
        }
        ImGui::SameLine(0.0f, 0.0f);
        /* Pravá část: seznam dostane vše nad detailem pevné výšky; bez posuvníku dítěte (obsah = přesně jeho výška). */
        if (ImGui::BeginChild("##bus_right", ImVec2(list_w, body_h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            float dh = detail_height();
            float lh = ImGui::GetContentRegionAvail().y - dh;
            if (lh < m.mini * 3.0f) lh = m.mini * 3.0f;
            render_owner_list(mmio, lh);
            render_detail(mmio);
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    g_bus_ui.hover_row_id = g_bus_ui.hover_row_id_now;
    g_bus_ui.hover_row_kind = g_bus_ui.hover_row_kind_now;

    /* Stavový řádek: platforma, zařízení devbus (jen když jsou), nestandardní model (K14, K19). */
    char status[96];
    int used = snprintf(status, sizeof(status), "MZ-%u", s->platform);
    if (s->devbus_initialized && s->devbus_registered) {
        used += snprintf(status + used, sizeof(status) - (size_t) used, " | %u %s, %u %s", s->devbus_registered, _("devices"), s->devbus_connected, _("connected"));
    }
    if (s->bus_idle == DEVBUS_IDLE_CONST && used < (int) sizeof(status)) used += snprintf(status + used, sizeof(status) - (size_t) used, " | %s %02Xh", _("undriven: const"), s->bus_idle_value);
    if (s->conflict_policy != DEVBUS_CONFLICT_REFUSE && used < (int) sizeof(status)) snprintf(status + used, sizeof(status) - (size_t) used, " | %s %s", _("policy:"), policy_word(s->conflict_policy));
    snprintf(g_bus_ui.status_line, sizeof(g_bus_ui.status_line), "%s", status);
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(status);
    ImGui::SameLine();
    {
        char tip[256];
        snprintf(tip, sizeof(tip), "%s %s\n%s %s (%02Xh %s)\n%s %s\n%s %u",
                 _("Activity tracking:"), s->tracking_active ? _("on") : _("off"),
                 _("Undriven bits:"), s->bus_idle == DEVBUS_IDLE_CONST ? _("constant") : _("bus latch"), s->idle_byte, _("now"),
                 _("Conflict policy:"), policy_word(s->conflict_policy),
                 _("Frame"), s->frame);
        help_marker(tip);
    }

    ImGui::End();
}


extern "C" void bus_window_register_persistence(void *cmod_void)
{
    if (!cmod_void) return;
    st_CFGMODULE *cmod = (st_CFGMODULE *) cmod_void;
    st_CFGELEMENT *elm;
    elm = cfgmodule_register_new_element(cmod, (char *) "color_mode", CFGENTYPE_UNSIGNED, 0, 0, 2);
    cfgelement_set_handlers(elm, (void *) &g_bus_ui.color_mode, (void *) &g_bus_ui.color_mode);
    elm = cfgmodule_register_new_element(cmod, (char *) "active_tab", CFGENTYPE_UNSIGNED, 0, 0, 2);
    cfgelement_set_handlers(elm, (void *) &g_bus_ui.active_tab, (void *) &g_bus_ui.active_tab);
    elm = cfgmodule_register_new_element(cmod, (char *) "list_width", CFGENTYPE_UNSIGNED, 0, 0, 4000);
    cfgelement_set_handlers(elm, (void *) &g_bus_ui.list_width, (void *) &g_bus_ui.list_width);
    elm = cfgmodule_register_new_element(cmod, (char *) "tracking_active", CFGENTYPE_BOOL, 1);
    cfgelement_set_handlers(elm, (void *) &g_bus_ui.cfg_tracking, (void *) &g_bus_ui.cfg_tracking);
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
