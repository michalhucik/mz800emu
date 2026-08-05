/**
 * @file membrowser_hexview.cpp
 * @brief Core hex table renderer - implementace.
 *
 * Layout per row (default 16 bytes per row, V0-polish-6):
 *   |  AAAA: | XX XX XX XX | XX XX XX XX | XX XX XX XX | XX XX XX XX  | ASCII... |
 *
 * Mezi posledním hex bytem a ASCII sloupcem je 2-char gutter (trailing
 * spaces v hex bufferu). Mezi 4-byte grupami je literal " | " separator
 * (space + pipe + space) - výrazně rozdělí grupy a ulehčí počítání offsetu.
 *
 * Read chunk: per row 32 byte buffer naplněný přes be->read_bytes
 * (z mezery 8 + 16 + 8 + ...). Pro V0 jednoduché chunky per render -
 * žádná page cache (V1+).
 *
 * Edit: click na HEX nibble nebo ASCII char -> aktivuje InputText
 * v cellu. Commit při Enter / Tab / focus loss. ASCII edit používá
 * membrowser_encoding_utf8_to_byte pro round-trip validation.
 *
 * KRITICKÉ: ŽÁDNÝ direct dbgapi_regions_* call v tomto souboru.
 * Vše přes be->read_bytes / be->write_bytes. Audit: grep "dbgapi"
 * v tomto souboru musí být prázdný.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_hexview.h"
#include "membrowser_encoding.h"
#include "membrowser_layers.h"

#include "libs/imgui/imgui.h"
/* V1-polish-4: ImGui::GetActiveID() v ASCII edit skip checku pro toolbar
 * InputText konkurenci je v internal API. */
#include "libs/imgui/imgui_internal.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"
/* V2: cross-window Show in Memory Map - C-API header je k dispozici
 * i z C++ TU bez extern "C" wrapperu (header sám má extern "C" guard). */
#include "ui-imgui/debugger/memmap/memmap_window.h"
/* V6: fill dialog request hook a undo/redo stats pro context menu. */
#include "membrowser_fill_dialog.h"
#include "membrowser_undo.h"
/* V6: annotations - dialog request + per-row tooltip + check existence. */
#include "membrowser_annot_dialog.h"
#include "membrowser_annotations.h"
/* V6: PCG editor (MZ-1500) - aktivace z context menu nad PCG regionem. */
#include "membrowser_pcg.h"
/* Edit semantics: per-byte recently-edited storage pro vizuální badge. */
#include "membrowser_edited.h"
/* V1.5+: Char Inserter - "Insert character..." item v context menu. */
#include "membrowser_char_inserter.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

/* ASCII edit dispatch potřebuje io.InputQueueCharacters, který SDL3 ImGui
 * backend plní jen pokud SDL_StartTextInput() je aktivní. Backend ji volá
 * jen když má aktivní InputText widget (= WantTextInput true). V hexview
 * žádný InputText není, takže queue je defaultně prázdný a ASCII typing
 * by neviděl žádné znaky. Voláme tedy SDL_StartTextInput() ručně per-frame
 * když je memory browser v ASCII edit modu. */
#include <SDL3/SDL_keyboard.h>

extern "C" {
#include "emulator/debugger/symbols/sym_db.h"
#include "emulator/debugger/freeze/freeze.h"
#include "emulator/debugger/bookmarks/bookmarks.h"
#include "emulator/debugger/dbgapi_regions.h"
/* V2: per-row origin label používá memmap_query() - no-side-effect read
 * banking stavu (g_memory.map + případně g_gdg.regDMD). UI vlákno OK. */
#include "emulator/hw-generic/memory/memory.h"
/* Bugfix-final Bug 2: origin label rozlišuje Memext bank pro RAM stránky.
 * memext.h dává g_memext.map[addr_point] = active raw bank index 0..0xFF
 * a MEMEXT_TEST_CONNECTED test. */
#include "emulator/hw-generic/memory/memext.h"
}


/**
 * @brief Vrátí anglický label pro en_MEMMAP_REGION_KIND.
 *
 * Statické UTF-8 ASCII řetězce - klíče pro i18n překlad přes _() v UI
 * vrstvě (volající wrapuje volání _( label ) až při Text render).
 *
 * @param k Druh regionu (banking-aware per 4 KB stránka).
 * @return Pointer na statický řetězec; nikdy NULL.
 */
static const char *memmap_kind_label ( en_MEMMAP_REGION_KIND k )
{
    switch ( k ) {
        case MEMMAP_KIND_RAM:           return "RAM";
        case MEMMAP_KIND_ROM_LOW:       return "ROM low";
        case MEMMAP_KIND_ROM_HIGH:      return "ROM high";
        case MEMMAP_KIND_CGROM:         return "CG-ROM";
        case MEMMAP_KIND_VRAM_I:        return "VRAM I";
        case MEMMAP_KIND_VRAM_II:       return "VRAM II";
        case MEMMAP_KIND_VRAM_TEXT:     return "VRAM text";
        case MEMMAP_KIND_CGRAM:         return "CG-RAM";
        case MEMMAP_KIND_PCG_1:         return "PCG 1";
        case MEMMAP_KIND_PCG_2:         return "PCG 2";
        case MEMMAP_KIND_PCG_3:         return "PCG 3";
        case MEMMAP_KIND_MAPPED_PORTS:  return "ports";
        case MEMMAP_KIND_PROHIBITED:    return "PROHIBITED";
        case MEMMAP_KIND_UNMAPPED:      return "unmapped";
        default:                        return "?";
    }
}


/* Spočítá počet hex digitů potřebných pro nejvyšší adresu (size-1).
 * 4 pro <= 64 KB, 6 pro <= 16 MB, 8 pro větší. */
static int hexview_addr_digits ( uint64_t total )
{
    if ( total <= 0x10000ULL ) return 4;
    if ( total <= 0x1000000ULL ) return 6;
    return 8;
}


/* Vrátí, zda je byte pro danou encoding "tisknutelný" - tj. dává smysl
 * ukázat ho v ASCII sloupci. Pro Raw / ASCII variant vrátí false pro
 * netisknutelné bajty (chceme tečku místo glyfu). Pro CG variant vždy
 * true - tam jsou všechny indexy v CG-ROM platné glyfy. UTF-8 / KOI8-CS
 * variant: lib resolve si poradí, vždy true.
 *
 * Pozn.: použitý hlavně pro "muted" barvu v hex sloupci - actual rendering
 * v ASCII column dělá membrowser_encoding_byte_to_utf8 a sám rozhodne. */
static bool hexview_is_printable_in_encoding ( uint8_t b, int enc )
{
    switch ( enc ) {
        case MB_CHARSET_RAW:
            return ( b >= 0x20 && b <= 0x7E );
        case MB_CHARSET_SHARPMZ_EU_CG1:
        case MB_CHARSET_SHARPMZ_EU_CG2:
        case MB_CHARSET_SHARPMZ_JP_CG1:
        case MB_CHARSET_SHARPMZ_JP_CG2:
            /* CG kódy: všechny indexy v CG-ROM jsou validní glyfy. */
            return true;
        case MB_CHARSET_SHARPMZ_EU_ASCII:
        case MB_CHARSET_SHARPMZ_JP_ASCII:
        case MB_CHARSET_SHARPMZ_EU_UTF8:
        case MB_CHARSET_SHARPMZ_JP_UTF8:
        case MB_CHARSET_KOI8CS:
        default:
            /* Sharp ASCII/UTF-8/KOI8: heuristika - 0x20-0x7E + horní polovina.
             * Skutečné rozhodnutí dělá encoding_byte_to_utf8 ('.' nebo glyf). */
            return ( b >= 0x20 && b <= 0x7E ) || ( b >= 0x80 );
    }
}


/* Žlutá barva pro adresový sloupec - per stará GTK reference. */
static const ImVec4 c_addr_color = ImVec4 ( 1.00f, 0.85f, 0.20f, 1.00f );

/* Žlutooranžová barva pro byte pod cursorem v BROWSE režimu (Edit OFF) -
 * výrazný kontrast proti tmavému pozadí (per stará GTK reference: žlutý
 * znak na modrém poli). */
static const ImVec4 c_cursor_color = ImVec4 ( 1.00f, 0.80f, 0.20f, 1.00f );

/* Sytě červená barva pro byte pod cursorem v EDIT režimu (V1-polish-2
 * Bug 3) - vizuálně odlišuje "writing mode" od pouhého browse. Použito
 * v hex sloupci na cur cell a v ASCII column pro cur cell pokud
 * edit_mode = ASCII. */
static const ImVec4 c_cursor_edit_color = ImVec4 ( 1.00f, 0.45f, 0.40f, 1.00f );

/* Pro caret v edit režimu - stejný odstín, plně sytý (ImU32 pro DrawList). */
static inline ImU32 hexview_caret_color_u32 ( void )
{
    return IM_COL32 ( 255, 90, 80, 255 );
}

/* V1-polish-2 Bug 6+7: deferred open requested z hit-testu (right-click)
 * pro popup ##mb_row_ctx. Před V1-polish-2 byl popup BeginPopupContextItem
 * uvnitř PushID(row_addr) scope, takže ID se měnil per řádek a opakovaný
 * RMB na ten samý byte (cursor se nezmění → render je beat-to-beat stejný
 * ale ID se neresetuje) z nějakého důvodu nezpůsobil reopen. Nový model:
 * RMB hit-test si zapamatuje "otevřít popup" + uloží target adresu; po
 * dokončení per-row PushID scope a před EndTable se OpenPopup zavolá
 * jednou na stabilní ID. BeginPopup je rovněž volán mimo PushID.
 *
 * Žije přes frame boundaries (set v hit-test frame N, čten ve frame N+0
 * po PopID). Vlákno: UI only.
 */
static bool s_ctx_open_requested = false;
static uint32_t s_ctx_target_addr = 0;

/* Layout konstanty pro click hit-test - per řádek dopočítané z monospace
 * font metrics, předané hexview_render_row z hlavní funkce, aby se
 * neopakovaný CalcTextSize() volání. */
typedef struct {
    float char_w;       /**< Šířka monospace znaku '0'. */
    float space_w;      /**< Šířka mezery (= char_w pro monospace). */
    float pair_w;       /**< Šířka "XX " = 3 znaky. */
    float group_sep_w;  /**< Šířka separátoru "| " mezi 4-byte grupami = 2 znaky. */
    float ascii_w;      /**< Šířka jednoho ASCII cellu (= char_w, monospace; CG glyfy ořezány). */
} st_HEX_LAYOUT_METRICS;


/* Spočítá X offset (od začátku hex cell oblasti) konkrétního bytu v řádku.
 * Vrací posun v px od start hex cellu, kde začíná byte[col]. */
static float hexview_byte_x_offset ( const st_HEX_LAYOUT_METRICS *m, int col )
{
    /* Každé 4 byty: 4 * pair_w + group_sep_w. Pak v aktuální grupě
     * je col_in_group * pair_w. */
    int groups = col / 4;
    int col_in_group = col % 4;
    return groups * ( 4.0f * m->pair_w + m->group_sep_w )
           + col_in_group * m->pair_w;
}


/* Hit-test hex cell oblasti: vrátí byte index 0..row_len-1 nebo -1 pokud
 * mouse_x mimo hex oblast (např. v group separator nebo za posledním
 * bytem v řádku). */
static int hexview_hit_test_hex ( const st_HEX_LAYOUT_METRICS *m,
                                  float rel_x, int row_len )
{
    if ( rel_x < 0.0f ) return -1;
    float pos = 0.0f;
    for ( int i = 0; i < row_len; i++ ) {
        float byte_start = pos;
        float byte_end = pos + m->pair_w;  /* "XX " včetně trailing mezery. */
        if ( rel_x >= byte_start && rel_x < byte_end ) {
            return i;
        }
        pos = byte_end;
        /* Po každé čtveřici extra group separator " | " width
         * (= group_sep_w navíc - mezera už je v pair_w trailing). */
        if ( ( i + 1 ) % 4 == 0 && i + 1 < row_len ) {
            pos += m->group_sep_w;
        }
    }
    return -1;
}


/* Hit-test ASCII cell oblasti: rel_x = offset od začátku ASCII column.
 * Monospace ASCII: každý cell má šířku ascii_w. Pro UTF-8 vícebajtové
 * glyfy (např. CG režimy s 3-byte PUA codepoint) máme ve fontu fixed
 * šířku, takže hit-test pos / ascii_w spolehlivě mapuje col index. */
static int hexview_hit_test_ascii ( const st_HEX_LAYOUT_METRICS *m,
                                    float rel_x, int row_len )
{
    if ( rel_x < 0.0f || m->ascii_w <= 0.0f ) return -1;
    int col = ( int ) ( rel_x / m->ascii_w );
    if ( col < 0 || col >= row_len ) return -1;
    return col;
}


/* V1: vykreslí background rectangles per byte v HEX sloupci.
 *
 * Volá se před hex Text render. Pre-calculate per byte color v bg_colors[]
 * a pozici cell přes monospace char width. AddRectFilled jde do
 * GetWindowDrawList() přímo - render order pak je bg pod textem protože
 * Text následuje. Pro byte s bg_colors[i] = 0 (= no highlight) skip.
 *
 * Cell layout v hex sloupci je "XX " (3 char) per byte, navíc po každé
 * 4-byte grupě extra "| " (2 char). Mezi byty 0-3 = posun 3, mezi 3 a 4 =
 * posun 5 (3 + 2 pro " | "), atd.
 */
static void hexview_paint_bg_rects ( const ImVec2 &origin, float char_w,
                                      float line_h, int row_len,
                                      const ImU32 *bg_colors )
{
    if ( !bg_colors ) return;
    ImDrawList *dl = ImGui::GetWindowDrawList ( );

    for ( int i = 0; i < row_len; i++ ) {
        if ( bg_colors[i] == 0 ) continue;

        /* Offset v char units = i*3 (3 char per byte: "XX ") + 2*(group_idx)
         * kde group_idx = i/4 (= počet separátorů "| " před tímto byte). */
        int char_off = i * 3 + 2 * ( i / 4 );
        float x0 = origin.x + ( float ) char_off * char_w;
        float x1 = x0 + 2.0f * char_w;  /* 2 hex digity, BEZ trailing space. */
        ImVec2 p0 ( x0, origin.y );
        ImVec2 p1 ( x1, origin.y + line_h );
        dl->AddRectFilled ( p0, p1, bg_colors[i] );
    }
}


/* Edit semantics: vykreslí žlutý underline pod 2-digit hex byte v HEX
 * sloupci pokud je daný byte v recently-edited seznamu pro instanci.
 *
 * Layout počítá stejně jako @ref hexview_paint_bg_rects (3 char per byte
 * + 2 char " | " mezi 4-byte grupami). Underline je 2 px silný a kreslí
 * se těsně pod baseline (= line_h - 1).
 *
 * @param origin       Levý okraj prvního hex bytu (= cursor screen pos po
 *                     PushID + before TextUnformatted).
 * @param char_w       Šířka monospace charu v px.
 * @param line_h       Výška řádky v px.
 * @param row_len      Počet bytů v řádku (1..64).
 * @param is_edited    Boolean per-byte pole; index 0..row_len-1.
 * @param color        ImU32 barva underline (typicky žlutá).
 */
static void hexview_paint_edited_marks ( const ImVec2 &origin, float char_w,
                                          float line_h, int row_len,
                                          const bool *is_edited, ImU32 color )
{
    if ( !is_edited ) return;
    ImDrawList *dl = ImGui::GetWindowDrawList ( );

    for ( int i = 0; i < row_len; i++ ) {
        if ( !is_edited[i] ) continue;
        int char_off = i * 3 + 2 * ( i / 4 );
        float x0 = origin.x + ( float ) char_off * char_w;
        float x1 = x0 + 2.0f * char_w;
        float y  = origin.y + line_h - 1.0f;
        dl->AddRectFilled ( ImVec2 ( x0, y ),
                            ImVec2 ( x1, y + 2.0f ),
                            color );
    }
}


/* Edit semantics: vykreslí žlutý underline pod ASCII cell pokud je daný
 * byte v recently-edited seznamu. Layout = 1 char per byte.
 */
static void hexview_paint_edited_marks_ascii ( const ImVec2 &origin, float char_w,
                                                float line_h, int row_len,
                                                const bool *is_edited, ImU32 color )
{
    if ( !is_edited ) return;
    ImDrawList *dl = ImGui::GetWindowDrawList ( );

    for ( int i = 0; i < row_len; i++ ) {
        if ( !is_edited[i] ) continue;
        float x0 = origin.x + ( float ) i * char_w;
        float x1 = x0 + char_w;
        float y  = origin.y + line_h - 1.0f;
        dl->AddRectFilled ( ImVec2 ( x0, y ),
                            ImVec2 ( x1, y + 2.0f ),
                            color );
    }
}


/* V1: vykreslí background rectangles per byte v ASCII sloupci.
 *
 * ASCII cell je 1 char wide per byte (decoded UTF-8 může být víc, ale
 * rectangle bereme jako 1 monospace cell - vizuálně dostatečné). Žádné
 * group separátory v ASCII column.
 */
static void hexview_paint_ascii_bg_rects ( const ImVec2 &origin, float char_w,
                                            float line_h, int row_len,
                                            const ImU32 *bg_colors )
{
    if ( !bg_colors ) return;
    ImDrawList *dl = ImGui::GetWindowDrawList ( );

    for ( int i = 0; i < row_len; i++ ) {
        if ( bg_colors[i] == 0 ) continue;
        float x0 = origin.x + ( float ) i * char_w;
        float x1 = x0 + char_w;
        ImVec2 p0 ( x0, origin.y );
        ImVec2 p1 ( x1, origin.y + line_h );
        dl->AddRectFilled ( p0, p1, bg_colors[i] );
    }
}


/* Vykreslí jednotlivý řádek hex dumpu (bez ScrollY virtualizace).
 * Volá se z ListClipperu pro každý visible row.
 *
 * V0-polish-2: HEX sloupec se vykresluje jako jeden monospace Text per
 * řádek (NE per-byte SameLine) - díky monospace fontu lícují sloupce
 * automaticky a šetříme draw calls. Cursor highlight řešený přes
 * 3-part split (pre / cursor / post) se SameLine(0,0) bez padding.
 * Click detection (leftovers F1): manuální hit-test z mouse X vůči
 * cell screen pos zachycené před TextUnformatted. Per HEX a ASCII zóna
 * (rozlišení via in_ascii flag), nastaví st->cursor_addr a edit_mode.
 *
 * Grupování po 4 bajtech: mezera po každých 4 bajtech extra jeden space
 * (= " " mezi byty + extra " " po každé čtveřici). Pro 32 B/row → 8 grup
 * po 4 (7 viditelných separátorů), 16 B/row → 4 grupy (3 separátory),
 * 8 B/row → 2 grupy (1 separátor). Per stará GTK reference.
 *
 * V1: pokud extras non-NULL a layers active, pre-calculate per-byte bg
 * colors + symbol overlay v ASCII column. Right-click na hex cell
 * otevírá context menu (bookmark/freeze). */


/* V6.1: helper pro per-byte annotation hover tooltip. Lookup annotation
 * pro (region_kind, sub_id, target_addr); pokud existuje, vyrenderuje
 * ImGui::BeginTooltip + color swatch + addr + text. Volá se z HEX i ASCII
 * hover hit-test bloků v hexview_render_row.
 *
 * Pre: voláno v aktivním ImGui scope (typicky uvnitř BeginChild hex view),
 *      extras != NULL (caller už guard).
 *
 * Note: color_rgba == 0 = "neutral" annotation bez color tagu. ColorButton
 *       pak vyrenderuje transparentní (= checkered) swatch - akceptovatelné,
 *       UX hint že annotation nemá explicit color. */
static void hexview_render_annotation_tooltip ( uint32_t target_addr,
                                                 const st_MEMBROWSER_HEXVIEW_EXTRAS *extras )
{
    if ( !extras ) return;
    const st_MB_ANNOTATION *a = membrowser_annotations_find (
        extras->region_kind, extras->sub_id, target_addr );
    if ( !a ) return;

    ImGui::BeginTooltip ( );
    ImVec4 col;
    col.x = ( ( a->color_rgba >> 24 ) & 0xFF ) / 255.0f;
    col.y = ( ( a->color_rgba >> 16 ) & 0xFF ) / 255.0f;
    col.z = ( ( a->color_rgba >>  8 ) & 0xFF ) / 255.0f;
    col.w = ( ( a->color_rgba >>  0 ) & 0xFF ) / 255.0f;
    ImGui::ColorButton ( "##mb_annot_hover_swatch", col,
                          ImGuiColorEditFlags_NoTooltip, ImVec2 ( 12, 12 ) );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "0x%X", ( unsigned ) target_addr );
    ImGui::Separator ( );
    ImGui::TextUnformatted ( a->text );
    ImGui::EndTooltip ( );
}


static void hexview_render_row ( st_MEMBROWSER_STATE *st,
                                 const st_HEX_VIEW_BACKEND *be,
                                 uint64_t row_addr,
                                 uint8_t *row_buf, int row_len,
                                 int addr_digits,
                                 const st_HEX_LAYOUT_METRICS *metrics,
                                 const st_MEMBROWSER_PCSP_INFO *pcsp,
                                 const st_MEMBROWSER_HEXVIEW_EXTRAS *extras )
{
    /* Detekce PC/SP v rámci řádku (V0-leftovers F3).
     * pc_in_row / sp_in_row: index v rámci řádku (0..row_len-1) nebo -1.
     * Jen pro Logical region (pcsp->pc_sp_valid). */
    int pc_in_row = -1, sp_in_row = -1;
    if ( pcsp && pcsp->pc_sp_valid ) {
        uint64_t end_addr = row_addr + ( uint64_t ) row_len;
        if ( pcsp->show_pc
                && ( uint64_t ) pcsp->pc >= row_addr
                && ( uint64_t ) pcsp->pc < end_addr ) {
            pc_in_row = ( int ) ( ( uint64_t ) pcsp->pc - row_addr );
        }
        if ( pcsp->show_sp
                && ( uint64_t ) pcsp->sp >= row_addr
                && ( uint64_t ) pcsp->sp < end_addr ) {
            sp_in_row = ( int ) ( ( uint64_t ) pcsp->sp - row_addr );
        }
    }

    ImGui::TableNextRow ( );

    /* Row background highlight pro PC / SP řádek - tmavě modrá pro PC,
     * tmavě zelená pro SP. Pokud oba na stejném řádku, PC vyhraje (modrá
     * = control flow je důležitější). */
    if ( pc_in_row >= 0 ) {
        ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg0,
                                  IM_COL32 ( 30, 60, 110, 255 ) );
    } else if ( sp_in_row >= 0 ) {
        ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg0,
                                  IM_COL32 ( 30, 90, 60, 255 ) );
    }

    /* Sloupec 0: adresa - žlutě (per stará GTK reference). Prefix s
     * PC/SP marker pro Logical region: " P AAAA: " / " S AAAA: " /
     * "PS AAAA:". Pro ne-Logical bez markeru jen "AAAA:". */
    ImGui::TableSetColumnIndex ( 0 );
    ImGui::PushStyleColor ( ImGuiCol_Text, c_addr_color );
    if ( pcsp && pcsp->pc_sp_valid && ( pcsp->show_pc || pcsp->show_sp ) ) {
        char marker[3];
        marker[0] = ( pc_in_row >= 0 ) ? 'P' : ' ';
        marker[1] = ( sp_in_row >= 0 ) ? 'S' : ' ';
        marker[2] = '\0';
        ImGui::Text ( "%s %0*X:", marker, addr_digits, ( unsigned ) row_addr );
    } else {
        ImGui::Text ( "%0*X:", addr_digits, ( unsigned ) row_addr );
    }
    ImGui::PopStyleColor ( );

    /* Sloupec 1: HEX bytes - jeden Text() na 3 části (před cursor,
     * cursor, za cursor) se SameLine(0,0) bez padding. */
    ImGui::TableSetColumnIndex ( 1 );

    /* Capture screen pos hex cell PŘED render - pro click hit-test.
     * GetCursorScreenPos vrací pozici kam se vykreslí další item -
     * to je přesně levý okraj hex pre/cur/post sekvence. */
    ImVec2 hex_screen_pos = ImGui::GetCursorScreenPos ( );

    ImGui::PushID ( ( int ) ( row_addr & 0x7FFFFFFF ) );

    /* V1: pre-calculate per-byte bg colors podle enabled layers.
     * bg_colors[i] = 0 → no highlight, jinak ImU32 RGBA. Skip pokud
     * extras = NULL nebo žádný color layer enabled (fast path). */
    ImU32 bg_colors[64];
    std::memset ( bg_colors, 0, sizeof ( bg_colors ) );
    bool have_bg = false;
    if ( extras && membrowser_layers_any_color_active ( st ) ) {
        for ( int i = 0; i < row_len; i++ ) {
            ImU32 c = membrowser_layers_compute_byte_bg ( st,
                          extras->region_id,
                          extras->region_kind, extras->sub_id,
                          ( uint32_t ) ( row_addr + ( uint64_t ) i ),
                          extras->logical_base );
            bg_colors[i] = c;
            if ( c != 0 ) have_bg = true;
        }
    }

    /* Pre-emit bg rectangles do drawList - musí být PŘED Text aby šly
     * pod text. Monospace char width = CalcTextSize("X").x. */
    if ( have_bg ) {
        ImVec2 origin = ImGui::GetCursorScreenPos ( );
        float char_w = ImGui::CalcTextSize ( "X" ).x;
        float line_h = ImGui::GetTextLineHeight ( );
        hexview_paint_bg_rects ( origin, char_w, line_h, row_len, bg_colors );
    }

    /* Edit semantics: per-byte recently-edited overlay (žlutý 2 px
     * underline pod hex bytem). Fast path: pokud instance má 0 záznamů,
     * is_marked vrátí false bez scan. */
    bool edited_marks[64];
    std::memset ( edited_marks, 0, sizeof ( edited_marks ) );
    bool have_edited_marks = false;
    if ( extras
         && membrowser_edited_count_for_instance ( st->instance_idx ) > 0 ) {
        for ( int i = 0; i < row_len; i++ ) {
            bool m = membrowser_edited_is_marked ( st->instance_idx,
                                                    extras->region_kind,
                                                    extras->sub_id,
                                                    ( uint32_t ) ( row_addr + ( uint64_t ) i ) );
            edited_marks[i] = m;
            if ( m ) have_edited_marks = true;
        }
    }
    if ( have_edited_marks ) {
        ImVec2 origin = ImGui::GetCursorScreenPos ( );
        float char_w = ImGui::CalcTextSize ( "X" ).x;
        float line_h = ImGui::GetTextLineHeight ( );
        /* Žlutá s plnou alfou - viditelná i bez bg, ale tenký underline
         * neruší čitelnost hex digit. */
        ImU32 col = IM_COL32 ( 255, 220, 70, 255 );
        hexview_paint_edited_marks ( origin, char_w, line_h, row_len,
                                      edited_marks, col );
    }

    int cursor_in_row = -1;
    if ( ( uint64_t ) st->cursor_addr >= row_addr
            && ( uint64_t ) st->cursor_addr < row_addr + ( uint64_t ) row_len ) {
        cursor_in_row = ( int ) ( ( uint64_t ) st->cursor_addr - row_addr );
    }

    /* Sestav 3 části hex stringu.
     * Formát per byte: "XX " (2 hex digity + 1 space).
     * Po každé čtveřici místo druhé mezery vlož "| " - tzn. mezi 4-byte
     * grupami vidíme literal " | " separator (space + pipe + space).
     * Pro 32 B/row: "XX XX XX XX | XX XX XX XX | XX XX XX XX | ... XX". */
    char hex_pre[256], hex_cur[8], hex_post[256];
    int p_pre = 0, p_post = 0;
    hex_pre[0] = hex_cur[0] = hex_post[0] = '\0';

    static const char hexdig[] = "0123456789ABCDEF";

    for ( int i = 0; i < row_len; i++ ) {
        uint8_t b = row_buf[i];
        char tmp[6];
        int tl = 0;
        tmp[tl++] = hexdig[ ( b >> 4 ) & 0x0F ];
        tmp[tl++] = hexdig[ b & 0x0F ];
        /* Separator po byte (kromě posledního v řádku). */
        if ( i + 1 < row_len ) {
            tmp[tl++] = ' ';
            /* Po každé čtveřici literal "| " místo druhé mezery - výsledný
             * separator mezi grupami je " | " (space + pipe + space).
             * 32 B/row = 8 grup po 4, 16 B/row = 4 grupy, 8 B/row = 2 grupy. */
            if ( ( ( i + 1 ) % 4 ) == 0 ) {
                tmp[tl++] = '|';
                tmp[tl++] = ' ';
            }
        }
        tmp[tl] = '\0';

        if ( i == cursor_in_row ) {
            /* Cursor část = jen 2 hex digity, BEZ trailing space
             * (oddělovač půjde do post části). */
            hex_cur[0] = tmp[0];
            hex_cur[1] = tmp[1];
            hex_cur[2] = '\0';
            /* Trailing space/group separator přidáme do post části. */
            if ( tl > 2 ) {
                for ( int j = 2; j < tl; j++ ) {
                    if ( p_post + 1 < ( int ) sizeof ( hex_post ) ) {
                        hex_post[p_post++] = tmp[j];
                    }
                }
            }
        } else if ( cursor_in_row < 0 || i < cursor_in_row ) {
            for ( int j = 0; j < tl; j++ ) {
                if ( p_pre + 1 < ( int ) sizeof ( hex_pre ) ) {
                    hex_pre[p_pre++] = tmp[j];
                }
            }
        } else {
            for ( int j = 0; j < tl; j++ ) {
                if ( p_post + 1 < ( int ) sizeof ( hex_post ) ) {
                    hex_post[p_post++] = tmp[j];
                }
            }
        }
    }
    hex_pre[p_pre] = '\0';
    hex_post[p_post] = '\0';

    /* Gutter mezi HEX a ASCII sloupcem - 2 trailing spaces v posledním
     * vykresleném segmentu (V0-polish-6: redukce ze 4 na 2 mezery -
     * separator " | " mezi 4-byte grupami je sám o sobě dost výrazný,
     * dvě mezery k ASCII stačí). Trailing spaces patří do "post" části
     * pokud existuje, jinak do "cur", jinak do "pre". */
    if ( p_post > 0 ) {
        if ( p_post + 2 < ( int ) sizeof ( hex_post ) ) {
            for ( int k = 0; k < 2; k++ ) hex_post[p_post++] = ' ';
            hex_post[p_post] = '\0';
        }
    } else if ( cursor_in_row >= 0 ) {
        /* Cursor je poslední cell - padding připoj přímo za 2 hex digity. */
        int cl = ( int ) std::strlen ( hex_cur );
        if ( cl + 2 < ( int ) sizeof ( hex_cur ) ) {
            for ( int k = 0; k < 2; k++ ) hex_cur[cl++] = ' ';
            hex_cur[cl] = '\0';
        }
    } else if ( p_pre > 0 ) {
        if ( p_pre + 2 < ( int ) sizeof ( hex_pre ) ) {
            for ( int k = 0; k < 2; k++ ) hex_pre[p_pre++] = ' ';
            hex_pre[p_pre] = '\0';
        }
    }

    /* Render pre / cursor / post.
     *
     * V1-polish-2 Bug 3+4: cur cell v HEX dostává jinou barvu podle režimu:
     *   - BROWSE (edit_enabled OFF) nebo edit_mode=ASCII → c_cursor_color
     *     (žlutá, klasická "browse highlight").
     *   - EDIT HEX (edit_enabled ON A edit_mode=HEX) → c_cursor_edit_color
     *     (červená, "writing mode").
     * Po renderu cur cell vykreslíme úzký caret (vertikální čára) pod
     * aktivní nibble (st->edit_nibble: 0=high, 1=low) - vizuálně ukazuje
     * která půlka bytu se bude editovat při dalším stisku 0-9/A-F. */
    bool hex_edit_cur = st->edit_enabled
                        && st->edit_mode == MEMBROWSER_EDIT_HEX
                        && cursor_in_row >= 0;
    /* Před renderem cur cell capture screen pos pro caret kreslení. */
    ImVec2 cur_cell_pos = { 0, 0 };
    if ( p_pre > 0 ) {
        ImGui::TextUnformatted ( hex_pre );
        if ( cursor_in_row >= 0 || p_post > 0 ) ImGui::SameLine ( 0, 0 );
    }
    if ( cursor_in_row >= 0 ) {
        cur_cell_pos = ImGui::GetCursorScreenPos ( );
        ImVec4 cur_col = hex_edit_cur ? c_cursor_edit_color : c_cursor_color;
        ImGui::PushStyleColor ( ImGuiCol_Text, cur_col );
        ImGui::TextUnformatted ( hex_cur );
        ImGui::PopStyleColor ( );
        if ( p_post > 0 ) ImGui::SameLine ( 0, 0 );
    }
    if ( p_post > 0 ) {
        ImGui::TextUnformatted ( hex_post );
    }
    if ( p_pre == 0 && cursor_in_row < 0 && p_post == 0 ) {
        ImGui::TextUnformatted ( "" );
    }

    /* HEX caret pod aktivní nibble v edit režimu. Šířka jednoho hex digitu
     * = metrics->char_w. Nibble 0 (high) sedí na cur_cell_pos.x..+char_w,
     * nibble 1 (low) na cur_cell_pos.x+char_w..+2*char_w. Caret kreslíme
     * jako 2 px silnou horizontální čáru těsně pod glyfem (= line_h-1). */
    if ( hex_edit_cur && metrics ) {
        ImDrawList *dl = ImGui::GetWindowDrawList ( );
        float ch = metrics->char_w;
        float nib_x = cur_cell_pos.x + ( st->edit_nibble == 1 ? ch : 0.0f );
        float line_h = ImGui::GetTextLineHeight ( );
        float y = cur_cell_pos.y + line_h - 1.0f;
        dl->AddRectFilled ( ImVec2 ( nib_x, y ),
                            ImVec2 ( nib_x + ch, y + 2.0f ),
                            hexview_caret_color_u32 ( ) );
    }

    /* Uzavři PushID scope HEX cellu (párovaný s PushID na začátku
     * HEX sloupce). Popup ##mb_row_ctx se NEvolá uvnitř PushID - po
     * Bug 6+7 fixu žije na top-level scope; viz dispatch v
     * membrowser_hexview_render_ex po EndTable. */
    ImGui::PopID ( );

    /* Click hit-test pro HEX zónu (leftovers F1 + V1-polish-2 Bug 6+7).
     * Vyhodnocuje se per-row v render-time - pokud byl klik LMB/RMB v tomto
     * frame A jeho Y padá do tohoto řádku A X do hex oblasti, posuneme
     * cursor. RMB navíc označí "open context menu" pro top-level dispatch.
     * Test Y rozsahu = mezi hex_screen_pos.y a (+ row height). */
    if ( metrics && metrics->pair_w > 0.0f ) {
        bool lmb = ImGui::IsMouseClicked ( ImGuiMouseButton_Left );
        bool rmb = ImGui::IsMouseClicked ( ImGuiMouseButton_Right );
        if ( lmb || rmb ) {
            ImVec2 mouse = ImGui::GetMousePos ( );
            float row_h = ImGui::GetTextLineHeightWithSpacing ( );
            if ( mouse.y >= hex_screen_pos.y && mouse.y < hex_screen_pos.y + row_h ) {
                float rel_x = mouse.x - hex_screen_pos.x;
                int col = hexview_hit_test_hex ( metrics, rel_x, row_len );
                if ( col >= 0 ) {
                    uint32_t target = ( uint32_t ) ( row_addr + ( uint64_t ) col );
                    st->cursor_addr = target;
                    st->edit_mode = MEMBROWSER_EDIT_HEX;
                    st->edit_nibble = 0;
                    if ( rmb && extras ) {
                        s_ctx_open_requested = true;
                        s_ctx_target_addr = target;
                    }
                }
            }
        }
    }

    /* V6.1: per-byte annotation hover tooltip v HEX zóně. Reuse hit-test
     * cesty (mouse Y v rangi + hexview_hit_test_hex). Bez click gate -
     * spustí se pokud myš nad cell + annotation pro byte existuje. */
    if ( extras && metrics && metrics->pair_w > 0.0f ) {
        ImVec2 mouse = ImGui::GetMousePos ( );
        float row_h = ImGui::GetTextLineHeightWithSpacing ( );
        if ( mouse.y >= hex_screen_pos.y && mouse.y < hex_screen_pos.y + row_h ) {
            float rel_x = mouse.x - hex_screen_pos.x;
            int col = hexview_hit_test_hex ( metrics, rel_x, row_len );
            if ( col >= 0 ) {
                uint32_t target = ( uint32_t ) ( row_addr + ( uint64_t ) col );
                hexview_render_annotation_tooltip ( target, extras );
            }
        }
    }

    ( void ) be;

    /* Sloupec 2: ASCII - tlumený sloupec (per stará reference). */
    if ( st->ascii_column_visible ) {
        ImGui::TableSetColumnIndex ( 2 );

        /* V1 symbol overlay - pokud Logical region A enabled, prepend
         * label se symbolem patřícím k row_addr (pokud existuje).
         * Render PŘED capture ascii_screen_pos, aby hit-test mířil na
         * skutečnou pozici prvního ASCII bajtu (= za labelem). */
        if ( extras && st->layer_symbols
             && extras->region_kind == REGION_KIND_LOGICAL
             && row_addr < 0x10000ull ) {
            const st_SYMBOL *sym = sym_db_lookup_by_addr (
                ( uint32_t ) row_addr, 0 );
            if ( sym && sym->name ) {
                ImGui::TextColored (
                    ImVec4 ( 0.95f, 0.85f, 0.35f, 1.00f ),
                    "%s ", sym->name );
                if ( ImGui::IsItemHovered ( ) ) {
                    const char *kind_str = "symbol";
                    switch ( sym->source ) {
                        case SYM_SOURCE_LBL: kind_str = "user label"; break;
                        case SYM_SOURCE_MAP: kind_str = "linker map"; break;
                        case SYM_SOURCE_NOI: kind_str = "NoICE"; break;
                        case SYM_SOURCE_SJASMPLUS: kind_str = "sjasmplus"; break;
                    }
                    if ( sym->comment ) {
                        ImGui::SetTooltip ( "%s (%s)\n%s",
                                            sym->name, kind_str, sym->comment );
                    } else {
                        ImGui::SetTooltip ( "%s (%s)",
                                            sym->name, kind_str );
                    }
                }
                ImGui::SameLine ( 0, 0 );
            }
        }

        /* Capture screen pos ASCII cell pro click hit-test (F1).
         * Po případném symbol overlay - ukazuje na první ASCII bajt. */
        ImVec2 ascii_screen_pos = ImGui::GetCursorScreenPos ( );

        /* V1: ASCII column bg colors (stejné per-byte mapping jako hex). */
        if ( have_bg ) {
            float char_w = ImGui::CalcTextSize ( "X" ).x;
            float line_h = ImGui::GetTextLineHeight ( );
            hexview_paint_ascii_bg_rects ( ascii_screen_pos, char_w, line_h, row_len, bg_colors );
        }

        /* Edit semantics: stejný recently-edited underline v ASCII column. */
        if ( have_edited_marks ) {
            float char_w = ImGui::CalcTextSize ( "X" ).x;
            float line_h = ImGui::GetTextLineHeight ( );
            ImU32 col = IM_COL32 ( 255, 220, 70, 255 );
            hexview_paint_edited_marks_ascii ( ascii_screen_pos, char_w, line_h,
                                                 row_len, edited_marks, col );
        }

        /* Sestav řetězec všech buněk najednou. Pro netisknutelné bajty
         * vždy tečku, pro tisknutelné konkrétní cell UTF-8. Cursor cell
         * vykreslíme zvlášť kvůli highlight barvě - rozdělíme řádek na
         * pre / cursor / post části. */
        int cursor_in_row = -1;
        if ( ( uint64_t ) st->cursor_addr >= row_addr
                && ( uint64_t ) st->cursor_addr < row_addr + ( uint64_t ) row_len ) {
            cursor_in_row = ( int ) ( ( uint64_t ) st->cursor_addr - row_addr );
        }

        /* Click hit-test pro ASCII zónu (V1-polish-2 Bug 5+6+7) - obdoba
         * HEX zóny ale s ascii_w cell width. LMB posune cursor + přepne
         * edit_mode na ASCII (auto-mode-switch dle clicked column). RMB
         * navíc spustí open context menu (top-level dispatch). */
        if ( metrics && metrics->ascii_w > 0.0f ) {
            bool lmb = ImGui::IsMouseClicked ( ImGuiMouseButton_Left );
            bool rmb = ImGui::IsMouseClicked ( ImGuiMouseButton_Right );
            if ( lmb || rmb ) {
                ImVec2 mouse = ImGui::GetMousePos ( );
                float row_h = ImGui::GetTextLineHeightWithSpacing ( );
                if ( mouse.y >= ascii_screen_pos.y && mouse.y < ascii_screen_pos.y + row_h ) {
                    float rel_x = mouse.x - ascii_screen_pos.x;
                    int col = hexview_hit_test_ascii ( metrics, rel_x, row_len );
                    if ( col >= 0 ) {
                        uint32_t target = ( uint32_t ) ( row_addr + ( uint64_t ) col );
                        st->cursor_addr = target;
                        st->edit_mode = MEMBROWSER_EDIT_ASCII;
                        st->edit_nibble = 0;
                        /* Recompute cursor_in_row pro správnou cursor cell highlight. */
                        cursor_in_row = col;
                        if ( rmb && extras ) {
                            s_ctx_open_requested = true;
                            s_ctx_target_addr = target;
                        }
                    }
                }
            }
        }

        /* V6.1: per-byte annotation hover tooltip v ASCII zóně. Reuse
         * hit-test (mouse Y + hexview_hit_test_ascii). */
        if ( extras && metrics && metrics->ascii_w > 0.0f ) {
            ImVec2 mouse = ImGui::GetMousePos ( );
            float row_h = ImGui::GetTextLineHeightWithSpacing ( );
            if ( mouse.y >= ascii_screen_pos.y && mouse.y < ascii_screen_pos.y + row_h ) {
                float rel_x = mouse.x - ascii_screen_pos.x;
                int col = hexview_hit_test_ascii ( metrics, rel_x, row_len );
                if ( col >= 0 ) {
                    uint32_t target = ( uint32_t ) ( row_addr + ( uint64_t ) col );
                    hexview_render_annotation_tooltip ( target, extras );
                }
            }
        }

        char buf_pre[256], buf_cur[8], buf_post[256];
        int p_pre = 0, p_post = 0;
        buf_pre[0] = buf_cur[0] = buf_post[0] = '\0';

        for ( int i = 0; i < row_len; i++ ) {
            const char *cell;
            char dot[2] = { '.', '\0' };
            if ( hexview_is_printable_in_encoding ( row_buf[i], st->current_encoding ) ) {
                cell = membrowser_encoding_byte_to_utf8 ( row_buf[i],
                                                           st->current_encoding );
            } else {
                cell = dot;
            }
            int cl = ( int ) std::strlen ( cell );
            if ( cl > 6 ) cl = 6;

            if ( i == cursor_in_row ) {
                int copy = cl;
                if ( copy >= ( int ) sizeof ( buf_cur ) ) copy = sizeof ( buf_cur ) - 1;
                std::memcpy ( buf_cur, cell, ( size_t ) copy );
                buf_cur[copy] = '\0';
            } else if ( cursor_in_row < 0 || i < cursor_in_row ) {
                if ( p_pre + cl + 1 < ( int ) sizeof ( buf_pre ) ) {
                    std::memcpy ( buf_pre + p_pre, cell, ( size_t ) cl );
                    p_pre += cl;
                }
            } else {
                if ( p_post + cl + 1 < ( int ) sizeof ( buf_post ) ) {
                    std::memcpy ( buf_post + p_post, cell, ( size_t ) cl );
                    p_post += cl;
                }
            }
        }
        buf_pre[p_pre] = '\0';
        buf_post[p_post] = '\0';

        /* ASCII sloupec render s mírně světlejší barvou (víc kontrast
         * než TextDisabled, ale tlumenější než hex sloupec). Cursor cell
         * ve žluté. V0-polish-2: dříve TextDisabled = příliš šedé, teď
         * světle modro-šedá (RGB 0.70/0.78/0.85). */
        ImVec4 muted = ImVec4 ( 0.70f, 0.78f, 0.85f, 1.00f );
        ImGui::PushStyleColor ( ImGuiCol_Text, muted );

        /* V1-polish-2 Bug 3+4: cur cell v ASCII dostává červenou pokud
         * jsme v EDIT ASCII režimu, jinak žlutou. Caret se kreslí pod
         * glyf na celou šířku ASCII cellu (jen 1 nibble v ASCII edit
         * režimu neexistuje, je tam jeden char). */
        bool ascii_edit_cur = st->edit_enabled
                              && st->edit_mode == MEMBROWSER_EDIT_ASCII
                              && cursor_in_row >= 0;
        ImVec2 ascii_cur_pos = { 0, 0 };

        if ( p_pre > 0 ) {
            ImGui::TextUnformatted ( buf_pre );
            if ( cursor_in_row >= 0 || p_post > 0 ) ImGui::SameLine ( 0, 0 );
        }
        if ( cursor_in_row >= 0 ) {
            ascii_cur_pos = ImGui::GetCursorScreenPos ( );
            ImGui::PopStyleColor ( );
            ImVec4 cur_col = ascii_edit_cur ? c_cursor_edit_color : c_cursor_color;
            ImGui::PushStyleColor ( ImGuiCol_Text, cur_col );
            ImGui::TextUnformatted ( buf_cur );
            ImGui::PopStyleColor ( );
            ImGui::PushStyleColor ( ImGuiCol_Text, muted );
            if ( p_post > 0 ) ImGui::SameLine ( 0, 0 );
        }
        if ( p_post > 0 ) {
            ImGui::TextUnformatted ( buf_post );
        }
        /* Pokud byl řádek úplně prázdný (nemělo by nastat) - aspoň
         * placeholder, jinak ImGui nedostane line. */
        if ( p_pre == 0 && cursor_in_row < 0 && p_post == 0 ) {
            ImGui::TextUnformatted ( "" );
        }

        ImGui::PopStyleColor ( );

        /* ASCII caret pod cur cell (V1-polish-2). Šířka = metrics->ascii_w
         * (= jeden monospace char). */
        if ( ascii_edit_cur && metrics ) {
            ImDrawList *dl = ImGui::GetWindowDrawList ( );
            float aw = metrics->ascii_w;
            float line_h = ImGui::GetTextLineHeight ( );
            float y = ascii_cur_pos.y + line_h - 1.0f;
            dl->AddRectFilled ( ImVec2 ( ascii_cur_pos.x, y ),
                                ImVec2 ( ascii_cur_pos.x + aw, y + 2.0f ),
                                hexview_caret_color_u32 ( ) );
        }

        /* V2: per-row physical origin label (na konci ASCII column).
         *
         * Pravidla zobrazení:
         *   - jen pokud st->show_origin_labels = true
         *   - jen pro REGION_KIND_LOGICAL (banking dává smysl jen pro Z80
         *     logickou adresu; fyzické regiony mají origin shodný se svým
         *     kindem = nic nového by neukázal)
         *   - jen pokud row_addr < 0x10000 (banking je per 4 KB Z80 stránka,
         *     mimo Z80 prostor memmap_query() nemá smysl)
         *
         * Hot path: per řádek 1× memmap_query (= O(1) lookup v g_memory.map +
         * případně g_gdg.regDMD). Viewport max ~32 řádků = 32 lookups per frame
         * (zanedbatelné).
         *
         * Label se kreslí přes SameLine + 2 mezery oddělovač + TextDisabled.
         * Pokud řádek (32 bajtů ASCII) zabere celou šířku okna, label se
         * vykreslí "za" okno - uživatel musí okno rozšířit. To je akceptovatelný
         * UX trade-off (alternativa: 4. column která rozbije existující layout). */
        if ( extras && st->show_origin_labels
                && extras->region_kind == REGION_KIND_LOGICAL
                && row_addr < 0x10000ull ) {
            uint8_t page = ( uint8_t ) ( row_addr >> 12 );
            en_MEMMAP_REGION_KIND mk = memmap_query ( page );
            ImGui::SameLine ( );
            /* Bugfix-final Bug 2: pokud kind == RAM a Memext je connected,
             * rozliš čistou User RAM od Memext aktivní banky. g_memext.map[page]
             * dává raw bank index 0..0xFF. Bez Memextu jde o čistou User RAM
             * (memram_read ukazuje do g_memory.RAM, ne do g_memext.RAM/FLASH).
             *
             * Detekce přes pointer comparison přes memext_get_ram_offset_from_pointer:
             * pokud memram_read[page] ukazuje do g_memext.RAM → memext bank.
             * Jinak (NULL fallback nebo nájde User RAM) → standardní "RAM". */
            if ( mk == MEMMAP_KIND_RAM && MEMEXT_TEST_CONNECTED ) {
                int32_t mxoff = memext_get_ram_offset_from_pointer (
                    g_memory.memram_read[page] );
                if ( mxoff >= 0 ) {
                    uint8_t bank = ( uint8_t ) g_memext.map[page];
                    ImGui::TextDisabled ( "  | %s 0x%02X",
                        _( "Memext bank" ), bank );
                } else {
                    ImGui::TextDisabled ( "  | %s", _( "RAM" ) );
                }
            } else {
                const char *raw = memmap_kind_label ( mk );
                ImGui::TextDisabled ( "  | %s", _( raw ) );
            }
        }
    }
}


/* Globální flag pro toast zprávu při round-trip selhání ASCII edit.
 * Číst přes membrowser_hexview_get_edit_error/clear API. Drží jen jeden
 * zprávový řádek, nahradí se další chybou. Vlákno: UI only. */
static char s_edit_error_msg[160] = { 0 };
static bool s_edit_error_active = false;

/* V1-polish-4: registrované ImGui IDs toolbar InputText prvků (Goto HEX,
 * Goto DEC, Search pattern). Pokud má jeden z nich ActiveID v daném frame,
 * ASCII edit dispatch v hex view se přeskočí - user píše do toolbar inputu,
 * který si konzumuje io.InputQueueCharacters sám.
 *
 * Cap pole je úmyslně malé (5 slotů) - tři aktuální InputTexty + 2 rezerva
 * pro budoucí rozšíření (např. inline edit ve filter sloupci).
 * Vlákno: UI only. Per-frame: registrace v render toolbar, konzumace v
 * hexview_handle_edit_input, reset volá render_ex před dispatch. */
#define HEXVIEW_TOOLBAR_INPUT_CAP 5
static unsigned int s_toolbar_input_ids[HEXVIEW_TOOLBAR_INPUT_CAP] = { 0 };
static int s_toolbar_input_count = 0;

/* Edit semantics: per-frame cached region kontext z render_ex extras.
 * Nastavuje se na začátku render_ex (pokud extras != NULL) a používá se
 * z hexview_handle_edit_input pro per-byte undo push + recently-edited
 * mark. Pokud extras NULL (= caller bez region info), s_current_extras_valid
 * zůstává false a undo/edited tracking se v dispatchi přeskočí. Reset na
 * false po každém dispatchi (per-frame validita). UI-thread only. */
static bool s_current_extras_valid = false;
static int  s_current_extras_kind = 0;
static int  s_current_extras_sub_id = 0;


extern "C" void membrowser_hexview_register_toolbar_input ( unsigned int id )
{
    if ( id == 0 ) return;
    if ( s_toolbar_input_count >= HEXVIEW_TOOLBAR_INPUT_CAP ) return;
    /* Duplicitu ignoruj - per-frame seznam, několikrát volaný stejný widget. */
    for ( int i = 0; i < s_toolbar_input_count; i++ ) {
        if ( s_toolbar_input_ids[i] == id ) return;
    }
    s_toolbar_input_ids[s_toolbar_input_count++] = id;
}


extern "C" void membrowser_hexview_reset_toolbar_inputs ( void )
{
    s_toolbar_input_count = 0;
}


/* Kontrola zda aktuální ActiveID patří některému registrovanému toolbar
 * InputText - volá se v ASCII edit dispatch před iterací InputQueueCharacters.
 * Vrací true pokud má některý toolbar InputText focus a hexview by měl
 * dispatch přeskočit. */
static bool hexview_toolbar_input_has_focus ( void )
{
    ImGuiID active = ImGui::GetActiveID ( );
    if ( active == 0 ) return false;
    for ( int i = 0; i < s_toolbar_input_count; i++ ) {
        if ( ( ImGuiID ) s_toolbar_input_ids[i] == active ) return true;
    }
    return false;
}


/* Vykóduje 32-bit unicode codepoint do UTF-8 sekvence (1-4 bytes).
 * Buffer @p out musí mít kapacitu min 5 bajtů (4 UTF-8 + NUL).
 * Vrací počet zapsaných UTF-8 bajtů (bez NUL), 0 při invalid codepoint.
 *
 * V1-polish-7: aktuálně nevolaný (ASCII edit dispatch disabled),
 * zachováván pro V1.5+ ASCII edit re-enable. */
[[maybe_unused]] static int hexview_codepoint_to_utf8 ( unsigned int cp, char out[5] )
{
    if ( cp < 0x80 ) {
        out[0] = ( char ) cp;
        out[1] = '\0';
        return 1;
    }
    if ( cp < 0x800 ) {
        out[0] = ( char ) ( 0xC0 | ( cp >> 6 ) );
        out[1] = ( char ) ( 0x80 | ( cp & 0x3F ) );
        out[2] = '\0';
        return 2;
    }
    if ( cp < 0x10000 ) {
        out[0] = ( char ) ( 0xE0 | ( cp >> 12 ) );
        out[1] = ( char ) ( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
        out[2] = ( char ) ( 0x80 | ( cp & 0x3F ) );
        out[3] = '\0';
        return 3;
    }
    if ( cp < 0x110000 ) {
        out[0] = ( char ) ( 0xF0 | ( cp >> 18 ) );
        out[1] = ( char ) ( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
        out[2] = ( char ) ( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
        out[3] = ( char ) ( 0x80 | ( cp & 0x3F ) );
        out[4] = '\0';
        return 4;
    }
    out[0] = '\0';
    return 0;
}


/* Globální hex view shortcuts (F2 / Esc / Tab) - volané z hlavního mb okna
 * scope, NE z hex view child. Detaily viz Doxygen header.
 *
 * V1-polish-2: extrahováno z hexview_handle_edit_input. Dříve F2 nefungovalo
 * protože dispatch byl uvnitř hex view child window scope (BeginChild při
 * Layers panel open) - IsWindowFocused vracelo false pro child pokud měl
 * focus parent. Také IsAnyItemActive() ignoroval F2 když uživatel měl
 * otevřený Goto/Search InputText. Function keys (F2) ale nemůžou kolizovat
 * s InputText queue (nejsou znak), proto bezpečně dispatchujeme i s
 * aktivním item.
 */
extern "C" void membrowser_hexview_handle_global_shortcuts ( st_MEMBROWSER_STATE *st,
                                                               bool writable )
{
    if ( !st ) return;
    /* IsWindowFocused na CURRENT window (= mb hlavní okno) včetně child
     * windows (hex view a layers panel jsou child). RootWindow flag NE -
     * jiná top-level okna (např. Disassembler) by jinak nezapnula F2 pro
     * Memory Browser. */
    if ( !ImGui::IsWindowFocused ( ImGuiFocusedFlags_ChildWindows ) ) return;

    ImGuiIO &io = ImGui::GetIO ( );
    /* Ignoruj modifikované varianty (Ctrl+F2 atd. nejsou tato akce). */
    if ( io.KeyCtrl || io.KeyAlt || io.KeySuper ) return;

    /* F2: toggle Edit. Pro RO region nemá smysl zapínat - ignoruj. */
    if ( ImGui::IsKeyPressed ( ImGuiKey_F2, false ) ) {
        if ( writable || st->edit_enabled ) {
            st->edit_enabled = !st->edit_enabled;
            st->edit_nibble = 0;
            s_edit_error_active = false;
        }
        return;
    }

    /* Esc: pokud edit ON, vypne. Jinak no-op (nepolykáme Esc pro jiné
     * shortcuty). */
    if ( ImGui::IsKeyPressed ( ImGuiKey_Escape, false ) ) {
        if ( st->edit_enabled ) {
            st->edit_enabled = false;
            st->edit_nibble = 0;
            s_edit_error_active = false;
        }
        return;
    }

    /* Tab: v edit režimu přepne HEX <-> ASCII. Mimo edit nepolykáme Tab
     * (mohl by mít jiný účel pro ImGui focus traversal). */
    if ( st->edit_enabled && ImGui::IsKeyPressed ( ImGuiKey_Tab, false )
            && !ImGui::IsAnyItemActive ( ) ) {
        st->edit_mode = ( st->edit_mode == MEMBROWSER_EDIT_HEX )
                          ? MEMBROWSER_EDIT_ASCII : MEMBROWSER_EDIT_HEX;
        st->edit_nibble = 0;
        return;
    }
}


/* Edit logic - hex/ASCII typing pokud edit_enabled.
 *
 * V1-polish-2: F2/Esc/Tab shortcuts vyseparované do
 * membrowser_hexview_handle_global_shortcuts (volané z top-level mb okna).
 * Tato funkce už řeší jen vlastní typing 0-9/A-F (HEX) nebo ASCII chars.
 *
 * Hex typing (0-9, A-F): nahrazuje high/low nibble, posun cursor po 2
 * nibblech.
 *
 * ASCII typing: jen raw printable 7-bit ASCII (0x20..0x7E). Pro speciální
 * znaky (KOI8-CS diakritika, MZ display codes, JP CG) má uživatel Char
 * Inserter okno (right-click → "Insert character..."). Záměrné zjednodušení
 * po V1-polish-7 zkušenosti: pure 7-bit ASCII má dostatečně úzký konflikt
 * surface s ImGui toolbar inputs - existující gating (Ctrl/Alt skip +
 * IsAnyItemActive skip + register_toolbar_input fallback) ho pokrývá.
 *
 * Write-through PŘÍMO přes be->write_bytes (jediný legal place v hexview).
 * Per stará reference + mzdisk pattern. */
static void hexview_handle_edit_input ( st_MEMBROWSER_STATE *st,
                                        const st_HEX_VIEW_BACKEND *be,
                                        uint64_t total )
{
    if ( !be || !be->write_bytes ) return;
    if ( total == 0 ) return;
    if ( !st->edit_enabled ) return;
    if ( !ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows ) ) return;
    if ( ImGui::IsAnyItemActive ( ) ) return;

    ImGuiIO &io = ImGui::GetIO ( );
    /* Ignoruj edit data klávesy pokud Ctrl/Alt/Super modifier - kolize
     * s globální shortcut (Ctrl+F search, Alt+E toggle, atd.). Shift OK
     * (kapitalizace ASCII chars přes InputQueueCharacters). */
    if ( io.KeyCtrl || io.KeyAlt || io.KeySuper ) return;

    uint64_t pos = ( uint64_t ) st->cursor_addr;
    if ( pos >= total ) return;

    /* Načti aktuální byte. */
    uint8_t cur_byte = 0;
    if ( be->read_bytes ( be->ctx, pos, &cur_byte, 1 ) != 1 ) return;

    if ( st->edit_mode == MEMBROWSER_EDIT_HEX ) {
        /* Mapovací tabulka kláves na hex nibble. */
        static const struct { ImGuiKey key; int nibble; } hex_keys[] = {
            { ImGuiKey_0, 0 }, { ImGuiKey_1, 1 }, { ImGuiKey_2, 2 },
            { ImGuiKey_3, 3 }, { ImGuiKey_4, 4 }, { ImGuiKey_5, 5 },
            { ImGuiKey_6, 6 }, { ImGuiKey_7, 7 }, { ImGuiKey_8, 8 },
            { ImGuiKey_9, 9 }, { ImGuiKey_A, 10 }, { ImGuiKey_B, 11 },
            { ImGuiKey_C, 12 }, { ImGuiKey_D, 13 }, { ImGuiKey_E, 14 },
            { ImGuiKey_F, 15 },
            { ImGuiKey_Keypad0, 0 }, { ImGuiKey_Keypad1, 1 },
            { ImGuiKey_Keypad2, 2 }, { ImGuiKey_Keypad3, 3 },
            { ImGuiKey_Keypad4, 4 }, { ImGuiKey_Keypad5, 5 },
            { ImGuiKey_Keypad6, 6 }, { ImGuiKey_Keypad7, 7 },
            { ImGuiKey_Keypad8, 8 }, { ImGuiKey_Keypad9, 9 },
        };

        for ( size_t k = 0; k < sizeof ( hex_keys ) / sizeof ( hex_keys[0] ); k++ ) {
            if ( !ImGui::IsKeyPressed ( hex_keys[k].key ) ) continue;
            int nibble = hex_keys[k].nibble;
            uint8_t new_byte;
            if ( st->edit_nibble == 0 ) {
                /* High nibble. */
                new_byte = ( uint8_t ) ( ( nibble << 4 ) | ( cur_byte & 0x0F ) );
            } else {
                /* Low nibble. */
                new_byte = ( uint8_t ) ( ( cur_byte & 0xF0 ) | nibble );
            }
            /* Per-byte undo push PŘED write - snapshot starého bytu (len=1).
             * Pokud se nibble nezměnil oproti původnímu bytu, undo push
             * vynecháme (= zbytečně by se plnil stack identickými snapshoty
             * při opakovaném mačkání stejné klávesy). Region kontext bere
             * z extras pokud jsou předané; pokud NULL (= V0 caller bez
             * region info), undo push se vynechá - žádné dispatch potom
             * Ctrl+Z nedohledá záznam pro správný region. */
            if ( new_byte != cur_byte ) {
                /* Write-through. */
                int wr = be->write_bytes ( be->ctx, pos, &new_byte, 1 );
                if ( wr != 1 ) {
                    std::snprintf ( s_edit_error_msg, sizeof ( s_edit_error_msg ),
                                    "Write failed at 0x%08X", ( unsigned ) pos );
                    s_edit_error_active = true;
                    return;
                }
                /* Po úspěšném write: undo push starého bytu + mark recently
                 * edited (s session-original tracking). extras dispatch je
                 * per-frame z render_ex; pokud byl předán, máme spolehlivý
                 * (kind, sub_id).
                 *
                 * V1.5+: mark s original=cur_byte (= hodnota PŘED tímto write);
                 * pokud entry už existuje z dřívějšího edit této addr v sesi,
                 * jeho original_byte se nepřepíše. Poté check_unmark s new_byte:
                 * pokud user upravil byte zpět na session-original (např. opakem
                 * předchozí akce), badge zmizí. */
                if ( s_current_extras_valid ) {
                    membrowser_undo_push ( s_current_extras_kind,
                                            s_current_extras_sub_id,
                                            pos, &cur_byte, 1 );
                    membrowser_edited_mark ( st->instance_idx,
                                              s_current_extras_kind,
                                              s_current_extras_sub_id,
                                              ( uint32_t ) pos, cur_byte );
                    membrowser_edited_check_unmark ( st->instance_idx,
                                                      s_current_extras_kind,
                                                      s_current_extras_sub_id,
                                                      ( uint32_t ) pos, new_byte );
                }
            }
            s_edit_error_active = false;
            /* Posun nibble / cursor. */
            if ( st->edit_nibble == 0 ) {
                st->edit_nibble = 1;
            } else {
                st->edit_nibble = 0;
                if ( pos + 1 < total ) st->cursor_addr = ( uint32_t ) ( pos + 1 );
            }
            return;  /* Jeden hit per frame. */
        }
    } else {
        /* Per-frame ručně aktivuj SDL text input pro keyboard-focused okno.
         * Bez toho SDL3 negeneruje TextInput eventy mimo aktivní InputText
         * a io.InputQueueCharacters zůstává prázdný (= ASCII typing by
         * neviděl žádné znaky). SDL_StartTextInput je idempotent + ImGui
         * SDL3 backend respektuje SDL_TextInputActive() check, takže náš
         * manual call se nepřebíjí.
         *
         * Důsledek: text input zůstává on i po opuštění ASCII modu, dokud
         * něco (ImGui InputText focus cyklus) nezavolá SDL_StopTextInput.
         * Na desktop platformách bez následku; na mobile/IME by se mohl
         * objevit on-screen keyboard, pro emulator scope ne relevant. */
        SDL_Window *kb_window = SDL_GetKeyboardFocus ( );
        if ( kb_window && !SDL_TextInputActive ( kb_window ) ) {
            SDL_StartTextInput ( kb_window );
        }

        /* ASCII edit dispatch - pouze raw 7-bit printable ASCII (0x20..0x7E).
         *
         * Záměrné omezení (po V1-polish-7 5 polish iteracích bez root-cause
         * řešení general-case race io.InputQueueCharacters s toolbar
         * InputTexty): nepokoušíme se ani diakritiku, ani layout-aware
         * extended characters. Uživatel pro speciální znaky (KOI8-CS
         * diakritika, MZ display codes, JP CG glyfy) otevře samostatné
         * okno Char Inserter (right-click → "Insert character...") a
         * vybere znak z tabbed palety per encoding.
         *
         * Race s toolbar je tu minimální: pure 7-bit ASCII zahrnuje jen
         * znaky které stávající gating spolehlivě filtruje (Ctrl/Alt skip
         * + IsAnyItemActive skip). Pokud má toolbar Goto/Search fokus,
         * jeho InputText konzumuje queue + náš dispatch má IsAnyItemActive
         * = true → early return na řádku 1103.
         *
         * Per char:
         *   - skip pokud < 0x20 nebo > 0x7E (= jen printable ASCII)
         *   - skip pokud register_toolbar_input má focus (polish-4 záchrana
         *     pro edge case kdy IsAnyItemActive ještě nezareagovalo)
         *   - round-trip přes membrowser_encoding_utf8_to_byte (Raw vždy
         *     prochází; pro JP/EU display nebo KOI8-CS některé chars
         *     nejsou reprezentovatelné = silent discard)
         *   - write_bytes + undo push + edited mark + cursor++ */
        if ( hexview_toolbar_input_has_focus ( ) ) return;

        for ( int ci = 0; ci < io.InputQueueCharacters.Size; ci++ ) {
            ImWchar wc = io.InputQueueCharacters[ci];
            /* Jen printable 7-bit ASCII. */
            if ( wc < 0x20 || wc > 0x7E ) continue;

            /* Single-byte UTF-8 (ASCII range je 1:1 s UTF-8). */
            char utf8[2] = { ( char ) wc, '\0' };

            uint8_t new_byte = 0;
            if ( !membrowser_encoding_utf8_to_byte ( utf8,
                                                      st->current_encoding,
                                                      &new_byte ) ) {
                /* Znak v aktivním encodingu neexistuje (např. CG-only
                 * tabulka). Silent discard - žádný toast. Uživatel může
                 * přepnout encoding nebo otevřít Char Inserter. */
                continue;
            }

            uint64_t cpos = ( uint64_t ) st->cursor_addr;
            if ( cpos >= total ) break;

            uint8_t old_byte = 0;
            if ( be->read_bytes ( be->ctx, cpos, &old_byte, 1 ) != 1 ) break;

            if ( new_byte != old_byte ) {
                int wr = be->write_bytes ( be->ctx, cpos, &new_byte, 1 );
                if ( wr != 1 ) {
                    std::snprintf ( s_edit_error_msg, sizeof ( s_edit_error_msg ),
                                    "Write failed at 0x%08X", ( unsigned ) cpos );
                    s_edit_error_active = true;
                    return;
                }
                if ( s_current_extras_valid ) {
                    membrowser_undo_push ( s_current_extras_kind,
                                            s_current_extras_sub_id,
                                            cpos, &old_byte, 1 );
                    /* V1.5+: mark s original=old_byte + check_unmark s new_byte
                     * (= případ kdy ASCII edit vrátí byte na session-original). */
                    membrowser_edited_mark ( st->instance_idx,
                                              s_current_extras_kind,
                                              s_current_extras_sub_id,
                                              ( uint32_t ) cpos, old_byte );
                    membrowser_edited_check_unmark ( st->instance_idx,
                                                      s_current_extras_kind,
                                                      s_current_extras_sub_id,
                                                      ( uint32_t ) cpos, new_byte );
                }
            }
            s_edit_error_active = false;

            /* Posun cursor na další byte. */
            if ( cpos + 1 < total ) {
                st->cursor_addr = ( uint32_t ) ( cpos + 1 );
            }
        }
        return;
    }
}


/* Public accessor pro edit error toast v okně (vykresluje se nad bottom bar). */
extern "C" const char *membrowser_hexview_get_edit_error ( void )
{
    return s_edit_error_active ? s_edit_error_msg : NULL;
}


extern "C" void membrowser_hexview_clear_edit_error ( void )
{
    s_edit_error_active = false;
    s_edit_error_msg[0] = '\0';
}


extern "C" void membrowser_hexview_render ( st_MEMBROWSER_STATE *st,
                                            const st_HEX_VIEW_BACKEND *be,
                                            float avail_h,
                                            const st_MEMBROWSER_PCSP_INFO *pcsp )
{
    membrowser_hexview_render_ex ( st, be, avail_h, pcsp, NULL );
}


extern "C" void membrowser_hexview_render_ex ( st_MEMBROWSER_STATE *st,
                                                const st_HEX_VIEW_BACKEND *be,
                                                float avail_h,
                                                const st_MEMBROWSER_PCSP_INFO *pcsp,
                                                const st_MEMBROWSER_HEXVIEW_EXTRAS *extras )
{
    if ( !st || !be || !be->read_bytes || !be->total_size ) {
        ImGui::TextDisabled ( "%s", _( "Backend not ready" ) );
        return;
    }

    uint64_t total = be->total_size ( be->ctx );
    if ( total == 0 ) {
        ImGui::TextDisabled ( "%s", _( "Region empty or disconnected" ) );
        return;
    }

    /* Edit semantics: zaznamenat region kontext z extras pro per-byte
     * undo push + recently-edited mark uvnitř dispatchu. Reset na konci. */
    if ( extras ) {
        s_current_extras_valid = true;
        s_current_extras_kind = extras->region_kind;
        s_current_extras_sub_id = extras->sub_id;
    } else {
        s_current_extras_valid = false;
    }

    /* Edit logic - dispatch před render aby cursor přesun se projevil
     * v aktuálním frame. HEX: IsKeyPressed dispatch. ASCII (V1-polish-7):
     * dispatch dočasně disabled, jen detekce typing -> toast hláška;
     * toolbar Goto/Search inputy mají přednost (early return). */
    hexview_handle_edit_input ( st, be, total );

    /* Per-frame extras kontext už není potřeba po dispatchi - reset. */
    s_current_extras_valid = false;

    /* V1-polish-4: konzumovat seznam registrovaných toolbar inputs - per
     * frame validity. Příští frame se znovu naplní v toolbar render. */
    membrowser_hexview_reset_toolbar_inputs ( );

    int bpr = st->bytes_per_row;
    if ( bpr != 8 && bpr != 16 && bpr != 32 ) bpr = 16;

    /* Clamp cursor do rozsahu. */
    if ( ( uint64_t ) st->cursor_addr >= total ) {
        st->cursor_addr = ( uint32_t ) ( total - 1 );
    }

    /* Spočti počet řádků. */
    int n_rows = ( int ) ( ( total + ( uint64_t ) bpr - 1 ) / ( uint64_t ) bpr );
    if ( n_rows < 1 ) n_rows = 1;

    /* Tabulka 2 nebo 3 sloupce (Addr + HEX + volitelně ASCII).
     *
     * Žádné inner borders ani row-bg - tabulka slouží jen jako sloupcové
     * zarovnání (addr / hex / ASCII), vizuálně chceme čistý "data dump"
     * feel jako stará GTK reference. ScrollY virtualizuje 16 MB Ramdisk
     * přes ImGuiListClipper. */
    int n_cols = st->ascii_column_visible ? 3 : 2;

    ImGuiTableFlags flags = ImGuiTableFlags_ScrollY
                            | ImGuiTableFlags_NoBordersInBody
                            | ImGuiTableFlags_SizingFixedFit;

    /* Min výška 8 řádků aby tabulka nebyla zmizelá. */
    float row_h = ImGui::GetTextLineHeightWithSpacing ( );
    float min_h = row_h * 8.0f;
    if ( avail_h < min_h ) avail_h = min_h;

    /* PushFont monospace pro celou tabulku - bajty pak mají fixed šířku
     * a sloupce hezky lícují (per stará GTK reference bitmap fontu).
     * Pokud monospace font není dostupný (TTF chybí), fallback na
     * default font - hex bude vypadat jako dříve. */
    ImFont *mono = myimgui_get_monospace_font ( );
    if ( mono ) ImGui::PushFont ( mono );

    /* Spočítej layout metrics pro hit-test (F1). Musí být po PushFont,
     * aby CalcTextSize vrátil monospace metriky. Šířka mezery = char_w
     * pro monospace font (všechny znaky stejně široké). */
    st_HEX_LAYOUT_METRICS metrics;
    metrics.char_w = ImGui::CalcTextSize ( "0" ).x;
    metrics.space_w = ImGui::CalcTextSize ( " " ).x;
    /* "XX " = 2 hex digity + 1 mezera. */
    metrics.pair_w = metrics.char_w * 2.0f + metrics.space_w;
    /* "| " = pipe + mezera (= 2 znaky, monospace všechny stejné). */
    metrics.group_sep_w = metrics.char_w * 2.0f;
    /* ASCII cell = jeden glyf široký (monospace). */
    metrics.ascii_w = metrics.char_w;

    if ( !ImGui::BeginTable ( "##mb_hex_table", n_cols, flags,
                              ImVec2 ( 0, avail_h ) ) ) {
        if ( mono ) ImGui::PopFont ( );
        return;
    }

    ImGui::TableSetupColumn ( "##mb_addr", ImGuiTableColumnFlags_WidthFixed );
    ImGui::TableSetupColumn ( "##mb_hex", ImGuiTableColumnFlags_WidthFixed );
    if ( st->ascii_column_visible ) {
        ImGui::TableSetupColumn ( "##mb_ascii", ImGuiTableColumnFlags_WidthStretch );
    }

    /* Adaptivní počet hex digitů adresy podle velikosti regionu. */
    int addr_digits = hexview_addr_digits ( total );

    /* Auto-scroll na cursor pokud je mimo viditelnou oblast. */
    int cursor_row = ( int ) ( ( uint64_t ) st->cursor_addr / ( uint64_t ) bpr );

    /* Per-row buffer (max 32 byte). */
    uint8_t row_buf[64];

    ImGuiListClipper clipper;
    clipper.Begin ( n_rows, row_h );

    /* Pokud se cursor přesunul mimo viewport, force-scroll. */
    static int s_last_cursor_row = -1;
    if ( s_last_cursor_row != cursor_row ) {
        float target_y = ( float ) cursor_row * row_h;
        /* Pokud výrazně off-screen, scroll. */
        float scroll_max = ImGui::GetScrollMaxY ( );
        if ( target_y < ImGui::GetScrollY ( ) || target_y > ImGui::GetScrollY ( ) + avail_h - row_h ) {
            float new_scroll = target_y - avail_h * 0.5f;
            if ( new_scroll < 0 ) new_scroll = 0;
            if ( new_scroll > scroll_max ) new_scroll = scroll_max;
            ImGui::SetScrollY ( new_scroll );
        }
        s_last_cursor_row = cursor_row;
    }

    while ( clipper.Step ( ) ) {
        for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++ ) {
            uint64_t row_addr = ( uint64_t ) row * ( uint64_t ) bpr;
            int row_len = bpr;
            if ( row_addr + ( uint64_t ) row_len > total ) {
                row_len = ( int ) ( total - row_addr );
            }
            if ( row_len <= 0 ) continue;

            int got = be->read_bytes ( be->ctx, row_addr, row_buf, ( uint32_t ) row_len );
            if ( got < 0 ) got = 0;
            /* Pokud read vrátil méně, doplníme 0 (vizuálně viditelné jako "00"). */
            for ( int i = got; i < row_len; i++ ) row_buf[i] = 0;

            hexview_render_row ( st, be, row_addr, row_buf, row_len,
                                 addr_digits, &metrics, pcsp, extras );
        }
    }
    clipper.End ( );

    ImGui::EndTable ( );

    if ( mono ) ImGui::PopFont ( );

    /* V1-polish-2 Bug 6+7: dispatch context menu popup mimo PushID scope
     * (= stabilní ID nezávislé na row_addr). RMB hit-test nastavil
     * s_ctx_open_requested + s_ctx_target_addr. Otevírá se vždy, i pokud
     * je RMB na stejném bajtu kde už cursor je (= reopen po výběru). */
    if ( s_ctx_open_requested ) {
        ImGui::OpenPopup ( "##mb_row_ctx" );
        s_ctx_open_requested = false;
    }
    if ( extras && ImGui::BeginPopup ( "##mb_row_ctx" ) ) {
        uint32_t target = s_ctx_target_addr;
        /* Bookmark sekce - jen Logical region (= 16-bit CPU adresa). */
        if ( extras->region_kind == REGION_KIND_LOGICAL ) {
            char hex_buf[16];
            std::snprintf ( hex_buf, sizeof ( hex_buf ), "%04X", target );
            if ( ImGui::MenuItem ( _( "Add bookmark for current cursor" ) ) ) {
                bookmarks_add ( hex_buf, NULL );
            }
            /* V2: cross-window Show in Memory Map.
             *
             * Otevře (pokud zavřeno) Memory Map debug okno a vyžádá focus
             * + pulse highlight řádku odpovídajícího 4 KB stránce target
             * adresy. Pouze pro Logical region (= Z80 adresa má smysl).
             *
             * memmap_window_request_focus_at() je no-op pro adresy mimo
             * 0..0xFFFF, takže explicit guard target<=0xFFFF je opt. */
            if ( ImGui::MenuItem ( _( "Show in Memory Map" ) ) ) {
                if ( g_gui ) g_gui->showMemoryMapWindow = true;
                memmap_window_request_focus_at ( ( unsigned ) target );
            }
            ImGui::Separator ( );
        }
        /* Freeze sekce - dostupné pro libovolný writable region. */
        uint8_t cur_val = 0;
        bool have_val = false;
        if ( be && be->read_bytes ) {
            int got = be->read_bytes ( be->ctx, ( uint64_t ) target, &cur_val, 1 );
            have_val = ( got == 1 );
        }
        bool is_frz = freeze_is_frozen ( extras->region_kind, extras->sub_id,
                                          target, NULL );
        if ( is_frz ) {
            if ( ImGui::MenuItem ( _( "Unfreeze byte at cursor" ) ) ) {
                freeze_remove ( extras->region_kind, extras->sub_id, target );
            }
        } else {
            char fz_label[64];
            std::snprintf ( fz_label, sizeof ( fz_label ),
                            _( "Freeze byte at cursor (= 0x%02X)" ),
                            ( unsigned ) cur_val );
            bool fz_dis = !have_val;
            if ( fz_dis ) ImGui::BeginDisabled ( );
            if ( ImGui::MenuItem ( fz_label ) ) {
                freeze_add ( extras->region_kind, extras->sub_id,
                             target, cur_val );
            }
            if ( fz_dis ) ImGui::EndDisabled ( );
        }

        /* V1.5+: Insert character... - otevře Char Inserter okno s
         * paletou znaků (tabs SharpMZ EU / JP / KOI8-CS). Cíl write =
         * právě tato MB instance (st->instance_idx). Položka dostupná
         * jen pokud je region writable + edit_enabled (jinak by clicks
         * v paletě jen reportovaly chybu). */
        if ( st->edit_enabled ) {
            ImGui::Separator ( );
            if ( ImGui::MenuItem ( _( "Insert character..." ) ) ) {
                membrowser_char_inserter_open ( st->instance_idx );
            }
        }

        /* V6: PCG glyph editor entry - jen pokud current region je
         * PCG_1500 (MZ-1500). Otevře separátní okno + focus na byte
         * pod kurzorem (přepočet byte offset -> char_idx). */
        if ( extras->region_kind == REGION_KIND_PCG_1500 ) {
            ImGui::Separator ( );
            if ( ImGui::MenuItem ( _( "Open in PCG editor..." ) ) ) {
                int char_idx = ( int ) ( target / 8 );
                if ( char_idx < 0 ) char_idx = 0;
                if ( char_idx > 255 ) char_idx = 255;
                membrowser_pcg_window_focus_at ( extras->sub_id, char_idx );
                if ( g_gui ) g_gui->showMembrowserPcgEditor = true;
            }
        }

        /* V6: Annotation sekce - Add / Edit / Delete pro target byte. */
        ImGui::Separator ( );
        const st_MB_ANNOTATION *ex_annot = membrowser_annotations_find (
            extras->region_kind, extras->sub_id, ( uint32_t ) target );
        const char *annot_label = ex_annot
            ? _( "Edit annotation at cursor..." )
            : _( "Add annotation at cursor..." );
        if ( ImGui::MenuItem ( annot_label ) ) {
            membrowser_annot_dialog_request_open ( extras->region_kind,
                                                     extras->sub_id,
                                                     ( uint32_t ) target );
        }
        if ( ex_annot ) {
            if ( ImGui::MenuItem ( _( "Delete annotation at cursor" ) ) ) {
                membrowser_annotations_remove ( extras->region_kind,
                                                  extras->sub_id,
                                                  ( uint32_t ) target );
            }
        }

        /* V6: Fill memory + Undo/Redo sekce.
         *
         * Fill: vyžádá modal dialog skrz request_open; vlastní render
         * dialogu běží v hlavní mb okenní render funkci (po EndChild
         * hex view scope, aby modal nepřebral input focus pod hex view).
         *
         * Undo/Redo: zobrazí počet úrovní v label. Disabled pokud 0
         * v daném směru. */
        ImGui::Separator ( );
        if ( ImGui::MenuItem ( _( "Fill at cursor..." ) ) ) {
            membrowser_fill_dialog_request_open ( extras->region_kind,
                                                    extras->sub_id,
                                                    ( uint64_t ) target );
        }
        int undo_n = 0, redo_n = 0;
        membrowser_undo_stats ( extras->region_kind, extras->sub_id,
                                  &undo_n, &redo_n );
        char undo_label[64];
        char redo_label[64];
        std::snprintf ( undo_label, sizeof ( undo_label ),
                        _( "Undo  (%d levels)\tCtrl+Z" ), undo_n );
        std::snprintf ( redo_label, sizeof ( redo_label ),
                        _( "Redo  (%d levels)\tCtrl+Y" ), redo_n );
        bool undo_dis = ( undo_n == 0 );
        if ( undo_dis ) ImGui::BeginDisabled ( );
        if ( ImGui::MenuItem ( undo_label ) ) {
            membrowser_fill_dialog_do_undo ( be, extras->region_kind,
                                               extras->sub_id,
                                               st->instance_idx );
        }
        if ( undo_dis ) ImGui::EndDisabled ( );
        bool redo_dis = ( redo_n == 0 );
        if ( redo_dis ) ImGui::BeginDisabled ( );
        if ( ImGui::MenuItem ( redo_label ) ) {
            membrowser_fill_dialog_do_redo ( be, extras->region_kind,
                                               extras->sub_id,
                                               st->instance_idx );
        }
        if ( redo_dis ) ImGui::EndDisabled ( );

        ImGui::EndPopup ( );
    }
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
