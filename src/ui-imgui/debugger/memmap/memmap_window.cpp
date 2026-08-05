/*
 * File:   memmap_window.cpp
 *
 * Memory Map debug okno - implementace V0.
 *
 * Layout: 3 sloupce (Addr / Banking / MemExt) x 16 řádků (= 4 kB
 * granularita Z80 prostoru 0x0000-0xFFFF). Pro MZ-800 navíc horní
 * roletka pro DMD mode.
 *
 * Datové zdroje (čte se na každý frame):
 *   - g_memory.map (banking flagy per architektura)
 *   - g_gdg.regDMD (jen MZ-800, DMD bit 3 = 700 mode, bit 2 = SCRW640)
 *   - g_memext.connection / g_memext.type (typ memextu)
 *   - g_memext.map[] (16x raw 4 kB bank ID, bit 7 = FLASH)
 *
 * Per-arch dispatch: memmap_query() je definovaná v
 * src/emulator/mzarch/<arch>/memory/<arch>_memory.c, deklarace v
 * hw-generic/memory/memory.h. UI volá jednotné jméno.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */
#include "hw-generic/memory/memory_arch.h" /* per-arch memory - dříve tranzitivně přes memory.h (mzhal 11c-2c) */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "main.h"
#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "memory/memory.h"
#include "memory/memext.h"

#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_gdg.h"
#endif

#include "ui-imgui/bootstrap/myimgui.h"
#include "../debugger_state.h"
#include "debugger/debugger.h"
#include "ui-imgui/auto_layout.h"
#include "memmap_window.h"

#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

#include <stdio.h>
#include <stdint.h>
#include <float.h>


/* Globální perzistentní stav Memory Map okna. */
struct st_MEMMAP_WINDOW g_memmap_window = {};


/* Pending flag pro cross-window focus request. Nastaven externím callerem
 * přes memmap_window_request_focus(), aplikován v memmap_window_render()
 * PŘED ImGui::Begin (kombinace SetNextWindowFocus + viz dále). Pattern
 * shodný se stack_history_window. */
static bool s_focus_pending = false;

/* V2: highlight pending pro cross-window navigaci s konkrétní adresou.
 * Nastaveno přes memmap_window_request_focus_at(addr). Aplikuje se v
 * render path - daný řádek (page = addr>>12) dostane pulse highlight,
 * který vyprchá ~1.5 s (frame counter). -1 = žádný highlight active. */
static int s_focus_highlight_page = -1;          /**< 0..15 nebo -1. */
static double s_focus_highlight_until = 0.0;     /**< ImGui::GetTime() limit. */
#define MEMMAP_FOCUS_HIGHLIGHT_SEC 1.5


extern "C" void memmap_window_request_focus ( void )
{
    s_focus_pending = true;
}


extern "C" void memmap_window_request_focus_at ( unsigned addr )
{
    s_focus_pending = true;
    if ( addr <= 0xFFFFu ) {
        s_focus_highlight_page = ( int ) ( addr >> 12 );
        /* Time se aplikuje v render path (po Begin), ne tady - tady
         * jen flag že "highlight on" - render path nastaví "until" hodnotu
         * při prvním viditelném framu po request. Pro idempotentní volání
         * v rámci framu (= stejný flag se nepřepíše) toto stačí. */
        s_focus_highlight_until = -1.0;  /* sentinel "init at next render" */
    }
}


/**
 * @brief Aplikuje banking změnu - memory_reconnect_ram + případný screen refresh.
 *
 * Sdílený helper pro všechny banking-change callbacky v tomto okně
 * (banking cell levý klik + popup Mount/Umount/Mount-All/Umount-All).
 * Po update mapovacích pointerů zavolá debugger_screen_refresh_if_enabled() -
 * při zapnutém "Auto refresh on edit" v Settings -> Screen se obraz překreslí,
 * protože banking změna může přemapovat 4 KB stránku mezi RAM/VRAM/ROM/PCG
 * a obsah obrazu se může změnit i bez emulace.
 */
static void memmap_apply_banking_change ( void )
{
    memory_reconnect_ram ( );
    debugger_screen_refresh_if_enabled ( );
}


extern "C" void memmap_window_state_init ( void )
{
    if ( g_memmap_window.initialized ) return;
    /* Default po fresh installu: kompaktní režim (užší okno). */
    g_memmap_window.compact_mode = true;
    g_memmap_window.layout_dirty = false;
    g_memmap_window.initialized = true;
}


extern "C" void memmap_window_register_persistence ( void *cmod_void )
{
    if ( !cmod_void ) return;
    /* Inicializace defaultů PŘED registrací cfgelementů. */
    memmap_window_state_init ( );

    st_CFGMODULE *cmod = (st_CFGMODULE *)cmod_void;
    st_CFGELEMENT *elm;

    /* Compact mode persistence. Default true (= fresh install startuje
     * v kompaktním režimu, per Michalovo zadání). */
    elm = cfgmodule_register_new_element ( cmod, (char *)"compact_mode",
                                           CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm,
                              (void *)&g_memmap_window.compact_mode,
                              (void *)&g_memmap_window.compact_mode );
}


/**
 * @brief Vrátí dvojici (text label, ImU32 RGBA color) pro daný region kind.
 *
 * Centrální paleta sdílená napříč všemi architekturami. Konkrétní RGBA
 * hodnoty jsou V0 placeholder (V4 polish doladí).
 *
 * @param kind         Region kind z memmap_query().
 * @param[out] label   Statický string ukazatel s textem buňky.
 * @param[out] color   ImU32 (RGBA, packed via IM_COL32) pro background tint.
 */
static void memmap_kind_get_visual ( en_MEMMAP_REGION_KIND kind,
                                     const char **label, ImU32 *color )
{
    switch ( kind ) {
        case MEMMAP_KIND_RAM:
            *label = "RAM";
            *color = IM_COL32 ( 40, 110, 40, 255 );      /* zelená - autoritativní */
            break;
        case MEMMAP_KIND_ROM_LOW:
            /* Sjednocený label "ROM" (per review V0) - barva odlišuje
             * (low = žlutá) vs upper. */
            *label = "ROM";
            *color = IM_COL32 ( 140, 110, 30, 255 );     /* žlutá */
            break;
        case MEMMAP_KIND_ROM_HIGH:
            /* Sjednocený label "ROM" (per review V0) - barva odlišuje
             * (high = oranžová) vs lower. */
            *label = "ROM";
            *color = IM_COL32 ( 160, 80, 30, 255 );      /* oranžová */
            break;
        case MEMMAP_KIND_CGROM:
            *label = "CG-ROM";
            /* Drive cyan (30, 110, 130) - moc podobné zelené RAM (G kanál
             * 110 oba). Nově teal (cyan posunuté k tyrkysové) - jasně
             * odlišná od RAM zelené i VRAM modré (R kanál vyšší). */
            *color = IM_COL32 ( 50, 140, 150, 255 );     /* teal */
            break;
        case MEMMAP_KIND_VRAM_I:
        case MEMMAP_KIND_VRAM_II:
            /* Sjednocený label "VRAM" + sjednocená barva pro VRAM I a II
             * (per druhé kolo review V0). VRAM_TEXT (MZ-700/MZ-800 700 mode
             * + MZ-1500) zůstává pro UI také pod stejnou modrou. */
            *label = "VRAM";
            *color = IM_COL32 ( 40, 90, 150, 255 );      /* modrá */
            break;
        case MEMMAP_KIND_VRAM_TEXT:
            /* Sjednocená barva s VRAM_I/II (per druhé kolo review V0). */
            *label = "VRAM";
            *color = IM_COL32 ( 40, 90, 150, 255 );      /* modrá */
            break;
        case MEMMAP_KIND_CGRAM:
            *label = "CG-RAM";
            *color = IM_COL32 ( 110, 50, 130, 255 );     /* purple */
            break;
        case MEMMAP_KIND_PCG_1:
            *label = "PCG1";
            *color = IM_COL32 ( 130, 60, 150, 255 );
            break;
        case MEMMAP_KIND_PCG_2:
            *label = "PCG2";
            *color = IM_COL32 ( 140, 50, 140, 255 );
            break;
        case MEMMAP_KIND_PCG_3:
            *label = "PCG3";
            *color = IM_COL32 ( 150, 40, 130, 255 );
            break;
        case MEMMAP_KIND_MAPPED_PORTS:
            /* MZ-700 / MZ-800 v MZ-700 modu / MZ-1500: 0xE000-0xE00F mapped
             * PIO/CTC/GDG ports + zbytek 0xE000-0xFFFF jako Monitor ROM
             * (per memmap_query()). Label "MMIO/ROM". Barva sjednocena
             * s ROM_HIGH (= dominantní obsah stránky je ROM E800-EFFF,
             * porty jsou jen prvních 16 bajtů; per druhé kolo review V0). */
            *label = "MMIO/ROM";
            *color = IM_COL32 ( 160, 80, 30, 255 );      /* oranžová = ROM_HIGH */
            break;
        case MEMMAP_KIND_PROHIBITED:
            *label = "PROHIB";
            *color = IM_COL32 ( 150, 30, 30, 255 );      /* červená */
            break;
        case MEMMAP_KIND_UNMAPPED:
            *label = "(0xFF)";
            *color = IM_COL32 ( 70, 70, 70, 255 );
            break;
        default:
            *label = "?";
            *color = IM_COL32 ( 100, 100, 100, 255 );
            break;
    };
}


#if MZARCH == 800

/**
 * @brief Položky DMD roletky.
 *
 * 9 položek mapuje hodnotu g_gdg.regDMD & 0x0F na human-readable label.
 * Indexace: 0 = MZ-700, 1..8 = DMD 0..7.
 *
 * Klik = direct write do g_gdg.regDMD (bez IORQ funkce). U "MZ-700"
 * se zapíše 0x08 (= bity 0-2 nezachovává, autoritativní volba).
 */
static const struct {
    const char *label;
    uint8_t dmd_value;
} k_dmd_combo_items[ 9 ] = {
    { "MZ-700",                       0x08 },
    { "MZ-800 320x200 @ 4 / A",       0x00 },
    { "MZ-800 320x200 @ 4 / B",       0x01 },
    { "MZ-800 320x200 @ 16",          0x02 },
    { "MZ-800 320x200 @ 16 / U",      0x03 },
    { "MZ-800 640x200 @ 2 / A",       0x04 },
    { "MZ-800 640x200 @ 2 / B",       0x05 },
    { "MZ-800 640x200 @ 4",           0x06 },
    { "MZ-800 640x200 @ 4 / U",       0x07 },
};


/**
 * @brief Spočítá index aktuální položky v k_dmd_combo_items podle DMD reg.
 *
 * Mapování DMD -> položka:
 *   - bit 3 = 1 -> "MZ-700" (= index 0, bity 0-2 ignorovány)
 *   - bit 3 = 0 -> položka 1 + (DMD & 0x07)
 */
static int memmap_dmd_to_combo_idx ( uint8_t dmd )
{
    if ( dmd & 0x08 ) return 0;
    return 1 + ( dmd & 0x07 );
}


/**
 * @brief Renderuje DMD roletku na vrcholu okna (jen MZ-800).
 *
 * Klik na položku zapíše příslušnou hodnotu DMD (0x00-0x07 nebo 0x08)
 * přímo do g_gdg.regDMD (= žádná IORQ funkce, bez side-effectů které
 * IORQ dispatcher provádí). Po zápisu se zavolá memory_reconnect_ram()
 * pro aktualizaci memram_read[]/write[] dispatch tabulek (DMD bit 3
 * 700 vs 800 mode ovlivňuje zda VRAM/CGRAM v 8000-CFFF přepíše RAM).
 *
 * Vizuální layout (per review V0):
 *   - žádný viditelný label před comboboxem
 *   - šířka roletky natažená na celou šířku okna (-FLT_MIN)
 *   - skrytý label "##memmap_mode" (= stable ImGui ID bez visible textu)
 */
static void memmap_render_dmd_combo ( void )
{
    uint8_t dmd = g_gdg.regDMD & 0x0F;
    int current_idx = memmap_dmd_to_combo_idx ( dmd );

    ImGui::SetNextItemWidth ( -FLT_MIN );
    if ( ImGui::BeginCombo ( "##memmap_mode",
                             k_dmd_combo_items[ current_idx ].label ) ) {
        for ( int i = 0; i < 9; i++ ) {
            bool selected = ( i == current_idx );
            if ( ImGui::Selectable ( k_dmd_combo_items[ i ].label, selected ) ) {
                /* Direct write - bez IORQ funkce. */
                g_gdg.regDMD = k_dmd_combo_items[ i ].dmd_value;
                /* Aktualizace dispatch tabulek RAM/VRAM (DMD bit 3 přepíná
                 * 700 vs 800 mode v 8000-CFFF). Side-effect-free na úrovni
                 * Z80 - pouze přepojí pointery v g_memory.memram_*. Helper
                 * navíc trigeruje screen refresh při zapnutém "Auto refresh
                 * on edit" - DMD mode změna typicky mění video renderer
                 * (700 vs 800, HICOLOR/SCRW640, ...) i sadu mapovaných stránek. */
                memmap_apply_banking_change ( );
            };
            if ( selected ) ImGui::SetItemDefaultFocus ( );
        };
        ImGui::EndCombo ( );
    };
}

#endif /* MZARCH == 800 */


/* ---------------------------------------------------------------------------
 *  Banking sloupec - interakce (levý klik rotace, pravý klik popup menu).
 *
 *  Per-arch implementace dispatch přes #if MZARCH. Klik handlery zapisují
 *  direct do g_memory.map a volají memory_reconnect_ram() (= analogicky
 *  DMD roletka, bez IORQ funkce). Side-effect-free vůči Z80 vrstvě.
 * --------------------------------------------------------------------------- */

/* Forward decl - popup content je definován níže, ale volá se z
 * memmap_render_banking_cell přes BeginPopupContextItem. */
static void memmap_banking_render_popup_content ( void );


/**
 * @brief Vrátí true pokud daná 4 kB stránka má klikatelnou banking buňku.
 *
 * Klikatelnost se vyhodnocuje runtime z aktuálního DMD modu (MZ-800)
 * a g_memory.map. Stránky které jsou v daném modu vždy RAM (= žádný
 * banking flag jejich obsah nemění) vrací false. Volající používá pro
 * potlačení levého kliku a marker v levém sub-sloupci.
 *
 * MZ-800 native mode (DMD bit 3 = 0):
 *   - $0000-$0FFF: ROM <-> RAM (ROM_0000)
 *   - $1000-$1FFF: CG-ROM <-> RAM (ROM_1000)
 *   - $8000-$9FFF: VRAM I <-> RAM (CGRAM_VRAM)
 *   - $A000-$BFFF: VRAM II <-> RAM jen v SCRW640 modu (DMD bit 2 = 1)
 *   - $E000-$FFFF: ROM <-> RAM <-> Prohibited
 *   - ostatní vždy RAM
 *
 * MZ-800 emulating MZ-700 (DMD bit 3 = 1):
 *   - $0000-$0FFF: ROM <-> RAM
 *   - $1000-$1FFF: CG-ROM <-> RAM
 *   - $C000-$CFFF: CG-RAM/PCG <-> RAM (CGRAM_VRAM v 700 modu)
 *   - $D000-$DFFF: VRAM text+attr <-> RAM (ROM_E000 ovládá VRAM D000)
 *   - $E000-$FFFF: ROM <-> RAM <-> Prohibited
 *   - $8000-$BFFF NEklikatelné (vždy RAM v 700 modu)
 *
 * MZ-700 standalone:
 *   - jen $0000, $D000, $E000, $F000 (žádný CG-ROM v $1000 ani CG-RAM
 *     v $C000 - na MZ-700 jsou tyto stránky vždy RAM, banking flagy
 *     pro ně neexistují).
 *
 * MZ-1500:
 *   - $0000, $D000, $E000 (SPEC rotace), $F000 (ROM_UPPER toggle).
 */
static bool memmap_banking_is_clickable ( int row )
{
#if MZARCH == 800
    bool mz700_mode = GDG_DMD_TEST_MODE700 ? true : false;
    bool scrw640    = GDG_MZ800_DMD_TEST_SCRW640 ? true : false;

    if ( mz700_mode ) {
        /* MZ-800 v MZ-700 emulačním modu. */
        switch ( row ) {
            case 0x00: case 0x01:
            case 0x0c: case 0x0d:
            case 0x0e: case 0x0f:
                return true;
            default:
                return false;
        };
    } else {
        /* MZ-800 native mode. */
        switch ( row ) {
            case 0x00: case 0x01:
            case 0x08: case 0x09:
            case 0x0e: case 0x0f:
                return true;
            case 0x0a: case 0x0b:
                /* $A000-$BFFF: VRAM II jen v SCRW640. */
                return scrw640;
            default:
                return false;
        };
    };
#elif MZARCH == 700
    /* MZ-700 standalone: žádný CG-ROM/CG-RAM banking, jen ROM_0000
     * a ROM_E000 (= VRAM D000 + porty E000 + ROM E000) a Prohibited
     * mode na $F000. */
    switch ( row ) {
        case 0x00:
        case 0x0d: case 0x0e: case 0x0f:
            return true;
        default:
            return false;
    };
#elif MZARCH == 1500
    switch ( row ) {
        case 0x00:
        case 0x0d: case 0x0e: case 0x0f:
            return true;
        default:
            return false;
    };
#else
    return false;
#endif
}


/**
 * @brief Provede rotaci banking stavu pro danou 4 kB stránku.
 *
 * Per-arch logika:
 *  - MZ-800: $0000 toggle ROM_0000.
 *    $1000 (CG-ROM) a $8000-$BFFF (VRAM) toggle SPOLEČNĚ ROM_1000 +
 *    CGRAM_VRAM - per HW model (IORQ IN E0/E1 řídí obě oblasti
 *    najednou, oddělená volba není možná). Toggle jen jednoho z
 *    bitů by uvedl emulátor do stavu, kterého reálné HW není
 *    schopno dosáhnout.
 *    $C000 (CG-RAM) v 700 modu - taktéž společný flag s $1000.
 *    $D000 (VRAM text) v 700 modu - ROM_E000 (řídí i ROM $E000).
 *    $E000-$FFFF rotace ROM <-> RAM <-> Prohibited (3-stav).
 *  - MZ-700: $0000 toggle ROM_0000; $D000 toggle ROM_E000 (= horní
 *    oblast); $E000 toggle ROM_E000; $F000 rotace ROM/RAM/Prohibited.
 *  - MZ-1500: $0000 toggle ROM_0000; $D000-$EFFF rotace SPEC
 *    (NONE/CGROM/PCG1/PCG2/PCG3); $F000 toggle ROM_UPPER.
 *
 * Side effects: zápis do g_memory.map + memory_reconnect_ram().
 * Pokud stránka není klikatelná (viz memmap_banking_is_clickable), no-op.
 *
 * @param row  4 kB stránka 0..15.
 */
static void memmap_banking_left_click ( int row )
{
    if ( !memmap_banking_is_clickable ( row ) ) return;

#if MZARCH == 800
    switch ( row ) {
        case 0x00:
            g_memory.map ^= MEMORY_MZ800_MAP_FLAG_ROM_0000;
            break;
        case 0x01:
        case 0x08: case 0x09: case 0x0a: case 0x0b:
        case 0x0c: case 0x0d:
            /* IORQ IN E0/E1 řídí ROM_1000 + CGRAM_VRAM SPOLEČNĚ - per HW
             * model. V 800 modu CG-ROM $1000 + VRAM $8000-$BFFF, v 700
             * modu CG-ROM $1000 + CG-RAM $C000. UI musí toggle obě bity
             * naráz, jinak by se emul. dostal do stavu nedostupného HW.
             * Výjimka: v 700 modu pro $D000 platí ROM_E000 flag
             * (= VRAM D000 + ROM E000 jeden bit). */
            if ( row == 0x0d && GDG_DMD_TEST_MODE700 ) {
                g_memory.map ^= MEMORY_MZ800_MAP_FLAG_ROM_E000;
            } else {
                g_memory.map ^= ( MEMORY_MZ800_MAP_FLAG_ROM_1000
                                | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM );
            };
            break;
        case 0x0e: case 0x0f: {
            /* 3-stav rotace pro horní oblast: ROM -> RAM -> Prohibited -> ROM.
             * Stav definuje (ROM_E000, PROHIBITED): ROM = (1,0), RAM = (0,0),
             * Prohibited = (1,1) (= per banking-e800 v0.5 model: Prohibited
             * vyžaduje ROM_E000 set, jinak by se neaktivoval správně). */
            int rom_e = MEMORY_MZ800_MAP_TEST_ROM_E000 ? 1 : 0;
            int prh   = MEMORY_MZ800_MAP_TEST_PROHIBITED ? 1 : 0;
            if ( rom_e && !prh ) {
                /* ROM -> RAM */
                g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_ROM_E000;
                g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
            } else if ( !rom_e && !prh ) {
                /* RAM -> Prohibited (= ROM_E000 zpět + Prohibited set) */
                g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_E000;
                g_memory.map |= MEMORY_MZ800_MAP_FLAG_PROHIBITED;
            } else {
                /* Prohibited -> ROM (clear Prohibited, ponechat ROM_E000) */
                g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
                g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_E000;
            };
            break;
        }
        default: return;
    };
#elif MZARCH == 700
    switch ( row ) {
        case 0x00:
            g_memory.map ^= MEMORY_MZ700_MAP_FLAG_ROM_0000;
            break;
        case 0x0d: case 0x0e:
            /* $D000 (VRAM) i $E000 (ports + horní ROM) jsou řízeny stejným
             * flagem ROM_E000 v MZ-700 modelu. */
            g_memory.map ^= MEMORY_MZ700_MAP_FLAG_ROM_E000;
            break;
        case 0x0f: {
            /* 3-stav rotace pro $F000 (ROM <-> RAM <-> Prohibited).
             * Stav (ROM_E000, PROHIBITED): ROM=(1,0), RAM=(0,0), PROH=(1,1). */
            int rom_e = MEMORY_MZ700_MAP_TEST_ROM_E000 ? 1 : 0;
            int prh   = MEMORY_MZ700_MAP_TEST_PROHIBITED ? 1 : 0;
            if ( rom_e && !prh ) {
                g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_ROM_E000;
                g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_PROHIBITED;
            } else if ( !rom_e && !prh ) {
                g_memory.map |= MEMORY_MZ700_MAP_FLAG_ROM_E000;
                g_memory.map |= MEMORY_MZ700_MAP_FLAG_PROHIBITED;
            } else {
                g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_PROHIBITED;
                g_memory.map |= MEMORY_MZ700_MAP_FLAG_ROM_E000;
            };
            break;
        }
        default: return;
    };
#elif MZARCH == 1500
    switch ( row ) {
        case 0x00:
            g_memory.map ^= MEMORY_MZ1500_MAP_FLAG_ROM_0000;
            break;
        case 0x0d: case 0x0e: {
            /* SPEC rotace 0..4: NONE -> CGROM -> PCG1 -> PCG2 -> PCG3 -> NONE.
             * SPEC pole jen v rámci D000 mask (3 bity, hodnoty 0..7;
             * legitimní 0..4). */
            uint8_t spec = ( g_memory.map & MEMORY_MZ1500_MAP_D000_MASK )
                           >> MEMORY_MZ1500_FLAG_SPEC_BITPOS;
            spec = ( spec + 1 ) % 5;
            g_memory.map &= ~MEMORY_MZ1500_MAP_D000_MASK;
            g_memory.map |= ( spec << MEMORY_MZ1500_FLAG_SPEC_BITPOS )
                            & MEMORY_MZ1500_MAP_D000_MASK;
            break;
        }
        case 0x0f:
            /* $F000 - toggle ROM_UPPER (= mapování horní ROM E800-FFFF).
             * Pozn: ROM_UPPER ovládá také $D000-$EFFF (vrátí RAM pokud OFF). */
            g_memory.map ^= MEMORY_MZ1500_MAP_FLAG_ROM_UPPER;
            break;
        default: return;
    };
#endif

    memmap_apply_banking_change ( );
}


/**
 * @brief Per-arch obsah popup menu pro pravý klik nad sloupcem Banking.
 *
 * Volá se z renderu okna při OpenPopup. Obsahuje sub-menu mount/umount
 * pro klíčové banking položky + Mount All / Umount All globální akce.
 * Po každé změně g_memory.map zavolá memory_reconnect_ram().
 *
 * MZ-800 záležitosti:
 *  - "CG-ROM $1000" a "CG-RAM/VRAM" jsou v HW propojeny stejným flagem
 *    CGRAM_VRAM (= IN E0/E1). Položky v menu mají oddělená pojmenování,
 *    ale obě toggle stejný flag (= UX volba per Michalovo zadání).
 */
static void memmap_banking_render_popup_content ( void )
{
#if MZARCH == 800
    if ( ImGui::BeginMenu ( _L( "ROM $0000" ) ) ) {
        if ( ImGui::MenuItem ( _L( "Mount" ) ) ) {
            g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_0000;
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Umount" ) ) ) {
            g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_ROM_0000;
            memmap_apply_banking_change ( );
        };
        ImGui::EndMenu ( );
    };
    /* CG-ROM $1000 a CG-RAM/VRAM jsou v HW propojeny společným IORQ IN
     * (E0 = mount obě, E1 = umount obě). UI nabídne dvě položky pro
     * orientaci uživatele, ale obě toggle stejnou kombinaci bitů
     * ROM_1000 + CGRAM_VRAM. */
    if ( ImGui::BeginMenu ( _L( "CG-ROM $1000" ) ) ) {
        if ( ImGui::MenuItem ( _L( "Mount" ) ) ) {
            g_memory.map |= ( MEMORY_MZ800_MAP_FLAG_ROM_1000
                            | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM );
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Umount" ) ) ) {
            g_memory.map &= ~( MEMORY_MZ800_MAP_FLAG_ROM_1000
                             | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM );
            memmap_apply_banking_change ( );
        };
        ImGui::EndMenu ( );
    };
    if ( ImGui::BeginMenu ( _L( "CG-RAM/VRAM" ) ) ) {
        if ( ImGui::MenuItem ( _L( "Mount" ) ) ) {
            g_memory.map |= ( MEMORY_MZ800_MAP_FLAG_ROM_1000
                            | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM );
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Umount" ) ) ) {
            g_memory.map &= ~( MEMORY_MZ800_MAP_FLAG_ROM_1000
                             | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM );
            memmap_apply_banking_change ( );
        };
        ImGui::EndMenu ( );
    };
    if ( ImGui::BeginMenu ( _L( "ROM $E000" ) ) ) {
        if ( ImGui::MenuItem ( _L( "Mount" ) ) ) {
            g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_E000;
            g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Umount" ) ) ) {
            g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_ROM_E000;
            g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Inhibit" ) ) ) {
            /* Prohibited mode aktivuje OUT E5 (= ROM_E000 zůstává set,
             * stránky $E000-$FFFF vrací 0x1A shadow). */
            g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_E000;
            g_memory.map |= MEMORY_MZ800_MAP_FLAG_PROHIBITED;
            memmap_apply_banking_change ( );
        };
        ImGui::EndMenu ( );
    };
    ImGui::Separator ( );
    if ( ImGui::MenuItem ( _L( "Mount All" ) ) ) {
        /* Vše mounted, Prohibited clear. */
        g_memory.map = MEMORY_MZ800_MAP_FLAG_ROM_0000
                     | MEMORY_MZ800_MAP_FLAG_ROM_1000
                     | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM
                     | MEMORY_MZ800_MAP_FLAG_ROM_E000;
        memmap_apply_banking_change ( );
    };
    if ( ImGui::MenuItem ( _L( "Umount All" ) ) ) {
        g_memory.map = 0;
        memmap_apply_banking_change ( );
    };
#elif MZARCH == 700
    if ( ImGui::BeginMenu ( _L( "ROM $0000" ) ) ) {
        if ( ImGui::MenuItem ( _L( "Mount" ) ) ) {
            g_memory.map |= MEMORY_MZ700_MAP_FLAG_ROM_0000;
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Umount" ) ) ) {
            g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_ROM_0000;
            memmap_apply_banking_change ( );
        };
        ImGui::EndMenu ( );
    };
    if ( ImGui::BeginMenu ( _L( "ROM $E000" ) ) ) {
        if ( ImGui::MenuItem ( _L( "Mount" ) ) ) {
            g_memory.map |= MEMORY_MZ700_MAP_FLAG_ROM_E000;
            g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_PROHIBITED;
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Umount" ) ) ) {
            g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_ROM_E000;
            g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_PROHIBITED;
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Inhibit" ) ) ) {
            g_memory.map |= MEMORY_MZ700_MAP_FLAG_ROM_E000;
            g_memory.map |= MEMORY_MZ700_MAP_FLAG_PROHIBITED;
            memmap_apply_banking_change ( );
        };
        ImGui::EndMenu ( );
    };
    ImGui::Separator ( );
    if ( ImGui::MenuItem ( _L( "Mount All" ) ) ) {
        g_memory.map = MEMORY_MZ700_MAP_FLAG_ROM_0000
                     | MEMORY_MZ700_MAP_FLAG_ROM_E000;
        memmap_apply_banking_change ( );
    };
    if ( ImGui::MenuItem ( _L( "Umount All" ) ) ) {
        g_memory.map = 0;
        memmap_apply_banking_change ( );
    };
#elif MZARCH == 1500
    if ( ImGui::BeginMenu ( _L( "ROM $0000" ) ) ) {
        if ( ImGui::MenuItem ( _L( "Mount" ) ) ) {
            g_memory.map |= MEMORY_MZ1500_MAP_FLAG_ROM_0000;
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Umount" ) ) ) {
            g_memory.map &= ~MEMORY_MZ1500_MAP_FLAG_ROM_0000;
            memmap_apply_banking_change ( );
        };
        ImGui::EndMenu ( );
    };
    if ( ImGui::BeginMenu ( _L( "Upper area" ) ) ) {
        /* "Upper area" = ROM_UPPER flag - ovládá VRAM $D000-$D7FF + ports
         * $E000-$E008 + ROM E800-FFFF najednou (per mz1500_memory.h
         * MEMORY_MZ1500_MAP_TEST_E800_ROM = MEMORY_MZ1500_MAP_TEST_D000_VRAM
         * = ROM_UPPER && SPEC=0). */
        if ( ImGui::MenuItem ( _L( "Mount" ) ) ) {
            g_memory.map |= MEMORY_MZ1500_MAP_FLAG_ROM_UPPER;
            memmap_apply_banking_change ( );
        };
        if ( ImGui::MenuItem ( _L( "Umount" ) ) ) {
            g_memory.map &= ~MEMORY_MZ1500_MAP_FLAG_ROM_UPPER;
            memmap_apply_banking_change ( );
        };
        ImGui::EndMenu ( );
    };
    if ( ImGui::BeginMenu ( _L( "D000 SPEC" ) ) ) {
        uint8_t cur = ( g_memory.map & MEMORY_MZ1500_MAP_D000_MASK )
                      >> MEMORY_MZ1500_FLAG_SPEC_BITPOS;
        struct {
            const char *label;
            uint8_t value;
        } items[ 5 ] = {
            { _L( "NONE (VRAM)" ),  0 },
            { _L( "CGROM" ),        1 },
            { _L( "PCG1" ),         2 },
            { _L( "PCG2" ),         3 },
            { _L( "PCG3" ),         4 },
        };
        for ( int i = 0; i < 5; i++ ) {
            if ( ImGui::MenuItem ( items[ i ].label, NULL,
                                   cur == items[ i ].value ) ) {
                g_memory.map &= ~MEMORY_MZ1500_MAP_D000_MASK;
                g_memory.map |= ( items[ i ].value
                                  << MEMORY_MZ1500_FLAG_SPEC_BITPOS )
                                & MEMORY_MZ1500_MAP_D000_MASK;
                memmap_apply_banking_change ( );
            };
        };
        ImGui::EndMenu ( );
    };
    ImGui::Separator ( );
    if ( ImGui::MenuItem ( _L( "Mount All" ) ) ) {
        /* All ROM/upper, SPEC = 0 (= VRAM, ne PCG). */
        g_memory.map = MEMORY_MZ1500_MAP_FLAG_ROM_0000
                     | MEMORY_MZ1500_MAP_FLAG_ROM_UPPER;
        memmap_apply_banking_change ( );
    };
    if ( ImGui::MenuItem ( _L( "Umount All" ) ) ) {
        g_memory.map = 0;
        memmap_apply_banking_change ( );
    };
#endif
}


/**
 * @brief Šířka marker sub-části Banking buňky v px.
 *
 * Dynamicky závislá na aktuálním fontu (= font_h * 1.2). Použita jak při
 * setup Banking sloupce ve vnější tabulce, tak při manuálním layoutu
 * uvnitř buňky (kolečko markeru se vystředí v této šířce).
 */
static inline float memmap_banking_marker_width ( void )
{
    return ImGui::GetFontSize ( ) * 1.2f;
}


/**
 * @brief Šířka content sub-části Banking buňky v px.
 *
 * Dynamicky závislá na aktuálním fontu - drží konstantní šířku napříč
 * všemi řádky bez ohledu na konkrétní region label.
 *
 * Normal mode: spočítaná z nejdelšího možného labelu napříč všemi MZARCH
 *              ("MMIO/ROM") + horizontální padding.
 *
 * Compact mode: 0 (= žádný content text, buňka má jen marker; sloupec
 *               je zúžen na šířku BNK header textu, viz
 *               memmap_banking_total_width()).
 */
static inline float memmap_banking_content_width ( void )
{
    if ( g_memmap_window.compact_mode ) {
        return 0.0f;
    };
    return ImGui::CalcTextSize ( "MMIO/ROM" ).x
           + ImGui::GetStyle ( ).CellPadding.x * 2.0f
           + ImGui::GetFontSize ( ) * 0.5f;
}


/**
 * @brief Celková šířka Banking sloupce vnější tabulky v px.
 *
 * Normal mode: marker_w + content_w + 2x CellPadding.x.
 *
 * Compact mode: max ( marker_w, CalcTextSize("BNK").x + 2x CellPadding.x ).
 *               Tj. sloupec má aspoň šířku BNK header textu, ale ne víc
 *               než marker (kolečko clickability) potřebuje.
 */
static inline float memmap_banking_total_width ( void )
{
    const float marker_w = memmap_banking_marker_width ( );
    const float content_w = memmap_banking_content_width ( );
    const float cell_pad_x2 = ImGui::GetStyle ( ).CellPadding.x * 2.0f;
    if ( g_memmap_window.compact_mode ) {
        const float bnk_w = ImGui::CalcTextSize ( "BNK" ).x + cell_pad_x2;
        return ( marker_w > bnk_w ) ? marker_w : bnk_w;
    };
    return marker_w + content_w + cell_pad_x2;
}


/**
 * @brief Renderuje sjednocenou Banking buňku (marker + content) ve 3-col layoutu.
 *
 * Layout uvnitř buňky vnější tabulky (= jeden 3. sloupec "Banking"):
 *
 *      +-----+--------------+
 *      | mk  | content text |     <- pevné šířky sub-částí dle fontu
 *      +-----+--------------+
 *
 * Celá tato dvojice je klikatelná jako jeden item:
 *   - Levý klik (clickable řádky): rotace banking stavu
 *   - Pravý klik (vždy): popup menu Banking sloupce
 *   - Hover (clickable řádky): jemný highlight přes obě sub-části
 *
 * Implementace: žádná nested BeginTable - po vzoru "padni na manuální
 * layout pokud nested glitchuje". Místo toho:
 *   1) jeden InvisibleButton přes obě sub-části (marker + content)
 *   2) hover/click events z tohoto InvisibleButton
 *   3) vizuální obsah (background, kolečko, text) přes ImDrawList
 *
 * Sub-části mají pevné šířky závislé na fontu:
 *   - marker:  memmap_banking_marker_width()  (= font_h * 1.2)
 *   - content: memmap_banking_content_width() (= CalcTextSize nejdelšího)
 *
 * Total šířka Banking buňky vnější tabulky musí být součet obou + tiny
 * padding (= setup viz hlavní render).
 *
 * @param kind      Druh regionu (z memmap_query).
 * @param row       4 kB stránka 0..15.
 * @param bg_color  Barva pozadí (sdílená přes obě sub-části).
 */
static void memmap_render_banking_combined_cell ( en_MEMMAP_REGION_KIND kind,
                                                  int row, ImU32 bg_color )
{
    const char *label = "?";
    ImU32 dummy_color = 0;
    memmap_kind_get_visual ( kind, &label, &dummy_color );

    /* Geometrie. */
    const bool compact = g_memmap_window.compact_mode;
    const float marker_w = memmap_banking_marker_width ( );
    const float content_w = memmap_banking_content_width ( );
    const float row_h = ImGui::GetTextLineHeight ( );

    /* Celková šířka buňky vnější tabulky musí pokrýt celou šířku sloupce
     * (= memmap_banking_total_width). V compact módu může být >marker_w,
     * pokud BNK header text vyžaduje širší sloupec. */
    const float cell_w = memmap_banking_total_width ( )
                         - ImGui::GetStyle ( ).CellPadding.x * 2.0f;

    /* Pozadí celé buňky (= obě sub-části najednou) přes table API. */
    ImGui::TableSetBgColor ( ImGuiTableBgTarget_CellBg, bg_color );

    /* Pozice cursoru pro vrácení a manuální layout přes drawlist. */
    ImVec2 cur_screen = ImGui::GetCursorScreenPos ( );

    /* Klikatelnost se vyhodnocuje runtime - viz memmap_banking_is_clickable. */
    const bool clickable = memmap_banking_is_clickable ( row );

    /* InvisibleButton přes celou šířku buňky - nese levý klik (jen pokud
     * clickable), pravý klik (vždy, popup Banking sloupce) a hover. */
    char id_buf[ 32 ];
    snprintf ( id_buf, sizeof id_buf, "##memmap_bank_%d", row );
    ImGui::InvisibleButton ( id_buf, ImVec2 ( cell_w, row_h ) );

    const bool hovered = clickable && ImGui::IsItemHovered ( );

    if ( clickable && ImGui::IsItemClicked ( ImGuiMouseButton_Left ) ) {
        memmap_banking_left_click ( row );
    };

    /* Hover tooltip - hint na chování (Left rotate clickable, Right
     * menu vždy). Drive byl tooltip na header, ale po revertu na čistý
     * TableHeader header tooltip zmizel - vrátíme ho do banking buněk. */
    if ( ImGui::IsItemHovered ( ) ) {
        if ( clickable )
            ImGui::SetTooltip ( "%s", _ ( "Left: rotate, Right: menu" ) );
        else
            ImGui::SetTooltip ( "%s", _ ( "Right: menu" ) );
    };

    /* Pravý klik = popup. Funguje vždy (i nad neklikatelnými řádky)
     * - popup je globální menu Banking sloupce. */
    if ( ImGui::BeginPopupContextItem ( ) ) {
        memmap_banking_render_popup_content ( );
        ImGui::EndPopup ( );
    };

    ImDrawList *dl = ImGui::GetWindowDrawList ( );

    /* Hover highlight přes celou šířku buňky. */
    if ( hovered ) {
        ImVec2 hi_min = cur_screen;
        ImVec2 hi_max ( cur_screen.x + cell_w, cur_screen.y + row_h );
        dl->AddRectFilled ( hi_min, hi_max, IM_COL32 ( 255, 255, 255, 40 ) );
    };

    /* Marker (bílé kolečko, jen pokud clickable).
     * Normal mode: vystředěný v marker_w sub-části (levá část buňky).
     * Compact mode: vystředěný v celé buňce cell_w (žádný content text). */
    if ( clickable ) {
        float marker_cx;
        if ( compact ) {
            marker_cx = cur_screen.x + cell_w * 0.5f;
        } else {
            marker_cx = cur_screen.x + marker_w * 0.5f;
        };
        ImVec2 center ( marker_cx, cur_screen.y + row_h * 0.5f );
        float radius = row_h * 0.15f;
        dl->AddCircleFilled ( center, radius, IM_COL32 ( 255, 255, 255, 255 ) );
    };

    /* Content text (jen v normal modu) - vystředěný v content_w sub-části. */
    if ( !compact ) {
        ImVec2 text_size = ImGui::CalcTextSize ( label );
        float text_x = cur_screen.x + marker_w
                       + ( content_w - text_size.x ) * 0.5f;
        float text_y = cur_screen.y + ( row_h - text_size.y ) * 0.5f;
        if ( text_x < cur_screen.x + marker_w ) text_x = cur_screen.x + marker_w;
        ImU32 text_col = IM_COL32 ( 255, 255, 255, 255 );
        dl->AddText ( ImVec2 ( text_x, text_y ), text_col, label );
    };
}


/**
 * @brief Vykreslí horizontálně vystředěné tlačítko v aktuální table buňce.
 *
 * Helper pro MExt sloupec. Tlačítko nese label (typicky "$xx") a v
 * současné V0 fázi nemá žádnou akci (= no-op). Stable ImGui ID je
 * tvořeno pomocí přivěšeného "##memext_<row>" suffixu, takže klik na
 * řádce N nezpůsobí ID kolizi s tlačítkem na jiném řádku.
 *
 * @param label    Text zobrazený na tlačítku (typicky "$xx" hex bank).
 * @param row      Řádek tabulky 0..15 (zdroj stable ImGui ID).
 * @return         true pokud bylo tlačítko stisknuto (V0: ignorováno).
 */
static bool memmap_render_memext_button ( const char *label, int row )
{
    char id_buf[ 32 ];
    snprintf ( id_buf, sizeof id_buf, "%s##memext_%d", label, row );

    /* Horizontální vycentrování v table buňce: spočítáme šířku tlačítka
     * (= šířka labelu + frame padding) a před něj vložíme spacer tak,
     * aby tlačítko bylo uprostřed dostupné šířky sloupce. */
    ImVec2 cell_size = ImGui::GetContentRegionAvail ( );
    ImVec2 text_size = ImGui::CalcTextSize ( label );
    float btn_w = text_size.x + ImGui::GetStyle ( ).FramePadding.x * 2.0f;
    float pad_x = ( cell_size.x - btn_w ) * 0.5f;
    if ( pad_x > 0.0f ) {
        ImGui::Dummy ( ImVec2 ( pad_x, 0.0f ) );
        ImGui::SameLine ( 0.0f, 0.0f );
    };

    bool pressed = ImGui::Button ( id_buf );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _ ( "MemExt remap..." ) );
    };
    return pressed;
}


/**
 * @brief Otevře/togglne existující MemExt Map Settings okno.
 *
 * Klik na buňku MExt sloupce nyní otevírá samostatné okno
 * `imgui_memext_map_window` (= stejné, jaké otevírá Devices -> MemExt ->
 * Memory Map Settings...). Tlačítko **nemění** MemExt mapu - jen
 * zobrazí setup dialog. Pokud je memext odpojen, tlačítko stále otevře
 * dialog (= dialog si poradí s NOT_CONNECTED stavem sám).
 *
 * Side effects: nastaví g_gui->showMemextMapWindow = true.
 */
static void memmap_open_memext_setup ( void )
{
    g_gui->showMemextMapWindow = true;
}


/**
 * @brief Renderuje buňku MExt sloupce pro daný 4 kB řádek.
 *
 * Tři stavy (per review V0 - tlačítka, label "$xx" raw 4K bank):
 *   - Memext nepřipojen: jen plain text "--" (žádné tlačítko, žádný
 *     klik handler, žádný tooltip - buňka je inertní).
 *   - Luftner (4K granularita): tlačítko "$XX" (raw bank včetně bit 7
 *     = FLASH/SRAM rozlišení v hodnotě, nezobrazuje se prefixem)
 *   - PEHU (8K granularita): tlačítko "$XX" na sudém řádku, lichý řádek
 *     prázdný (= patří k dvojbuňce nahoře). V0 separátor mezi sudým a
 *     lichým řádkem zůstává (ImGui table API neumí rowspan bez většího
 *     zásahu - V1 polish).
 *
 * Klik na tlačítko otevře MemExt Map Settings okno (g_gui->showMemextMapWindow).
 *
 * Sloupec MExt je nezávislý na sloupci Banking (= žádné "covered"
 * stavy, žádné šedé/proškrtnuté buňky).
 *
 * @param row  Řádek tabulky 0..15 (= 4 kB stránka).
 */
static void memmap_render_memext_cell ( int row )
{
    char buf[ 16 ];
    bool clicked = false;

    if ( !MEMEXT_TEST_CONNECTED ) {
        /* MemExt odpojen - buňka inertní. Plain text vystředěný,
         * bez tlačítka, bez klik handleru, bez tooltipu. */
        const char *plain = "--";
        ImVec2 cell_size = ImGui::GetContentRegionAvail ( );
        ImVec2 text_size = ImGui::CalcTextSize ( plain );
        float pad_x = ( cell_size.x - text_size.x ) * 0.5f;
        if ( pad_x > 0.0f ) {
            ImGui::Dummy ( ImVec2 ( pad_x, 0.0f ) );
            ImGui::SameLine ( 0.0f, 0.0f );
        };
        ImGui::TextUnformatted ( plain );
        return;
    } else if ( MEMEXT_TEST_TYPE_LUFTNER ) {
        uint32_t raw = g_memext.map[ row ];
        snprintf ( buf, sizeof buf, "$%02X", raw & 0xff );
        clicked = memmap_render_memext_button ( buf, row );
    } else if ( MEMEXT_TEST_TYPE_PEHU ) {
        /* PEHU: 8 dvojbuněk po 8 kB. Tlačítko na sudém řádku, lichý
         * řádek prázdný (= patří k dvojbuňce) - ale musí mít stejnou
         * výšku jako sudý řádek s buttonem, jinak ImGui table řádky
         * mají různou výšku a sloupce Addr/Banking se na lichých řádcích
         * vizuálně zúží. */
        if ( ( row & 1 ) == 0 ) {
            uint32_t raw = g_memext.map[ row ];
            snprintf ( buf, sizeof buf, "$%02X", raw & 0xff );
            clicked = memmap_render_memext_button ( buf, row );
        } else {
            /* Dummy se stejnou výškou jako Button ("$XX" + frame padding). */
            float btn_h = ImGui::GetTextLineHeight ( )
                          + ImGui::GetStyle ( ).FramePadding.y * 2.0f;
            ImGui::Dummy ( ImVec2 ( 0.0f, btn_h ) );
        };
        /* Lichý řádek: nic nekreslíme. */
    } else {
        /* Fallback - neznámý typ. */
        clicked = memmap_render_memext_button ( "$??", row );
    };

    /* Klik = otevře MemExt Map Settings okno (nemění mapu). */
    if ( clicked ) {
        memmap_open_memext_setup ( );
    };
}


/**
 * @brief Renderuje Memory Map okno.
 *
 * Hlavní entry point volaný z main_window každý frame, pokud
 * `*p_open` je true. Detail layoutu viz file-level komentář.
 */
extern "C" void memmap_window_render ( bool *p_open )
{
    if ( !p_open || !*p_open ) return;

    /* Title per architektura + stable ID přes ###. */
#if MZARCH == 800
    const char *title = _L ( "Memory Map - MZ-800###memmap_window" );
#elif MZARCH == 1500
    const char *title = _L ( "Memory Map - MZ-1500###memmap_window" );
#elif MZARCH == 700
    const char *title = _L ( "Memory Map - MZ-700###memmap_window" );
#else
#error "Unknown MZARCH"
#endif

    /* Velikost okna: on-demand autoresize při otevření okna (Appearing)
     * NEBO po toggle compact/normal (layout_dirty). Permanentní
     * AlwaysAutoResize jsme zkoušeli v kombinaci s SizingStretchProp
     * i SizingFixedFit - ani jedno nedalo autoshrink po toggle. Mimo
     * tyto eventy je okno user-resizable.
     *
     * ImGuiCond_Appearing = okno se otevřelo (= příchod přes p_open
     * false->true tranzici nebo první open). */
    /* Auto-layout při fresh open MUSÍ být před následujícím auto-fit
     * SetNextWindowSize - jeho ImGuiCond_Appearing přebije naši FirstUseEver
     * velikost, ale POZICE z auto-layout zůstane (= jen size se přepíše
     * na 0,0 auto-fit). */
    auto_layout_first_use_portrait ( title, 280.0f, 420.0f );

    ImGuiCond resize_cond = ImGuiCond_Appearing;
    if ( g_memmap_window.layout_dirty ) {
        resize_cond = ImGuiCond_Always;
        g_memmap_window.layout_dirty = false;
    };
    ImGui::SetNextWindowSize ( ImVec2 ( 0.0f, 0.0f ), resize_cond );

    /* Cross-window focus request: ImGui::SetNextWindowFocus() MUSÍ být
     * PŘED ImGui::Begin (= preview pro Platform_SetWindowFocus backend
     * v dalším renderu, OS-level raise + grab keyboard focus). Volání
     * po Begin v témž framu fakticky neprovede z-order change. */
    if ( s_focus_pending ) {
        ImGui::SetNextWindowFocus ( );
        s_focus_pending = false;
    };
    if ( !ImGui::Begin ( title, p_open, ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::End ( );
        return;
    };

    /* Auto-pauza (původně reagující na klik kamkoliv do okna) byla
     * v review V0 ZRUŠENA. Pokud uživatel přepíná DMD za běhu emu,
     * change proběhne přímo - drobnou race s emu vláknem nereší. */

    /* ---- 1) DMD roletka (jen MZ-800) ---- */
#if MZARCH == 800
    memmap_render_dmd_combo ( );
    ImGui::Separator ( );
#endif

    /* ---- 2) Hlavní 3-sloupcová tabulka ----
     *
     * Sloupce: Addr | Banking | MX
     *
     * Banking sloupec sám obsahuje vnořený manuální layout (= dvě
     * sub-části: marker + content), který se vykresluje přes drawlist
     * v memmap_render_banking_combined_cell. Šířka Banking sloupce je
     * pevná a odvozená dynamicky od fontu (marker_w + content_w +
     * malý padding pro vizuální odsazení).
     *
     * Důvod manuálního layoutu místo nested BeginTable: aby celá
     * dvojice (marker + content) byla klikatelná jako jeden item, je
     * potřeba jeden InvisibleButton přes obě sub-části. Nested tabulka
     * v ImGui by tento jeden hit-test region neumožnila bez výrazné
     * komplikace (= clip rects, item ID hierarchie). */
    /* SizingFixedFit (drive SizingStretchProp): tabulka má fit-content
     * width = každý sloupec přesně podle své WidthFixed velikosti, žádné
     * roztahování posledního sloupce do zbytku okna. To zajistí, že po
     * toggle compact/normal okno auto-resize zmenší šířku spolu se
     * sloupcem Banking. Předtím StretchProp dispatch alokoval ušetřený
     * prostor sloupci MX a okno zůstávalo na původní šířce. */
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_SizingFixedFit;

    if ( ImGui::BeginTable ( "##memmap_table", 3, table_flags ) ) {
        /* Banking sloupec: pevná šířka odvozená dynamicky od fontu, závisí
         * na compact_mode (= memmap_banking_total_width). */
        const float banking_total_w = memmap_banking_total_width ( );

        /* Banking header text dle compact_mode (Normal = "Banking",
         * Compact = "BNK"). Stejný string se použije pro setup columnu
         * i renderovaný header. */
        const char *banking_header = g_memmap_window.compact_mode
                                      ? _ ( "BNK" )
                                      : _ ( "Banking" );

        /* Šířka Addr sloupce dle nejširší hodnoty "$F000" (= 5 znaků
         * monospace). Bez explicit padding navíc - header text "Addr"
         * je užší než data, takže prostor stačí pro hodnoty. */
        float addr_w = ImGui::CalcTextSize ( "$F000" ).x
                       + ImGui::GetStyle ( ).CellPadding.x * 2.0f;
        /* NoSort flag na Addr a MX sloupcích = TableHeader nereaguje na
         * klik (= jediný klikatelný header zůstává Banking, který přepíná
         * compact_mode). */
        ImGui::TableSetupColumn ( _( "Addr" ),
                                  ImGuiTableColumnFlags_WidthFixed
                                  | ImGuiTableColumnFlags_NoSort,
                                  addr_w );
        ImGui::TableSetupColumn ( banking_header,
                                  ImGuiTableColumnFlags_WidthFixed,
                                  banking_total_w );
        /* Header zkrácen z "MemExt" na "MX" (per review V0). Pevná
         * šířka = CalcTextSize("$F0") button width + padding (= drive
         * mela WidthStretch ale to konfliktovalo s autoresize - MX si
         * při zmenšení Banking sloupce vzala ušetřený prostor a okno
         * se nezmenšilo). */
        float mx_btn_w = ImGui::CalcTextSize ( "$F0" ).x
                         + ImGui::GetStyle ( ).FramePadding.x * 2.0f
                         + ImGui::GetStyle ( ).CellPadding.x * 2.0f;
        ImGui::TableSetupColumn ( _( "MX" ),
                                  ImGuiTableColumnFlags_WidthFixed
                                  | ImGuiTableColumnFlags_NoSort,
                                  mx_btn_w );

        /* Custom header row.
         *
         * Layout:
         *   - col 0 (Addr):    text "Addr" vystředěný
         *   - col 1 (Banking): text "Banking" vystředěný + hover tooltip
         *   - col 2 (MX):      text "MX" vystředěný
         *
         * Pořadí v Banking headeru: InvisibleButton (hit-test) NEJDŘÍV,
         * pak TableHeader (background), pak text přes drawlist. Tím se
         * zajistí, že hit-test rect nevykukuje pod hranici header rowu
         * a neblokuje hover na první cell rowu. */
        ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );

        /* col 0: Addr header - non-klikatelný (žádný visual hover/active
         * highlight, matoucí pro uživatele protože header nereaguje na
         * klik). Push transparentní barvy pro HeaderHovered + HeaderActive
         * skryjí defaultní ImGui feedback. */
        ImGui::TableSetColumnIndex ( 0 );
        {
            const char *htext = _ ( "Addr" );
            float col_w = ImGui::GetContentRegionAvail ( ).x;
            float text_w = ImGui::CalcTextSize ( htext ).x;
            float pad = ( col_w - text_w ) * 0.5f;
            if ( pad > 0.0f ) {
                ImGui::SetCursorPosX ( ImGui::GetCursorPosX ( ) + pad );
            };
            ImGui::PushStyleColor ( ImGuiCol_HeaderHovered, IM_COL32 ( 0, 0, 0, 0 ) );
            ImGui::PushStyleColor ( ImGuiCol_HeaderActive,  IM_COL32 ( 0, 0, 0, 0 ) );
            ImGui::TableHeader ( htext );
            ImGui::PopStyleColor ( 2 );
        };

        /* col 1: Banking header - jediný klikatelný header v tabulce.
         * Klik přepne g_memmap_window.compact_mode (Normal <-> Compact),
         * což změní šířku sloupce a obsah buněk. Po toggle vyžádá re-fit
         * okna přes layout_dirty flag (= SetNextWindowSize 0,0 v dalším
         * framu).
         *
         * Header text se mění dle režimu: Normal = "Banking",
         * Compact = "BNK". Text vystředěn přes SetCursorPosX. */
        ImGui::TableSetColumnIndex ( 1 );
        {
            const char *htext = banking_header;
            float col_w = ImGui::GetContentRegionAvail ( ).x;
            float text_w = ImGui::CalcTextSize ( htext ).x;
            float pad = ( col_w - text_w ) * 0.5f;
            if ( pad > 0.0f ) {
                ImGui::SetCursorPosX ( ImGui::GetCursorPosX ( ) + pad );
            };
            ImGui::TableHeader ( htext );
            /* Klik na header = toggle compact_mode + re-fit okna. */
            if ( ImGui::IsItemClicked ( ImGuiMouseButton_Left ) ) {
                g_memmap_window.compact_mode = !g_memmap_window.compact_mode;
                g_memmap_window.layout_dirty = true;
            };
            if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenOverlapped ) ) {
                ImGui::SetTooltip ( "%s",
                    g_memmap_window.compact_mode
                    ? _ ( "Click: expand to normal mode" )
                    : _ ( "Click: collapse to compact mode" ) );
            };
        };

        /* col 2: MX header - non-klikatelný (viz Addr header výše). */
        ImGui::TableSetColumnIndex ( 2 );
        {
            const char *htext = _ ( "MX" );
            float col_w = ImGui::GetContentRegionAvail ( ).x;
            float text_w = ImGui::CalcTextSize ( htext ).x;
            float pad = ( col_w - text_w ) * 0.5f;
            if ( pad > 0.0f ) {
                ImGui::SetCursorPosX ( ImGui::GetCursorPosX ( ) + pad );
            };
            ImGui::PushStyleColor ( ImGuiCol_HeaderHovered, IM_COL32 ( 0, 0, 0, 0 ) );
            ImGui::PushStyleColor ( ImGuiCol_HeaderActive,  IM_COL32 ( 0, 0, 0, 0 ) );
            ImGui::TableHeader ( htext );
            ImGui::PopStyleColor ( 2 );
        };

        /* V2: cross-window highlight - init "until" time při prvním
         * viditelném framu po request, pak fading background nad target
         * row. Idempotentní volání memmap_window_request_focus_at()
         * v rámci jednoho framu (= flag se opakovaně nepřepíše). */
        if ( s_focus_highlight_page >= 0 && s_focus_highlight_until < 0.0 ) {
            s_focus_highlight_until = ImGui::GetTime ( )
                                       + MEMMAP_FOCUS_HIGHLIGHT_SEC;
        }
        double now = ImGui::GetTime ( );
        if ( s_focus_highlight_page >= 0 && now >= s_focus_highlight_until ) {
            s_focus_highlight_page = -1;  /* expire */
        }

        for ( int row = 0; row < 16; row++ ) {
            ImGui::TableNextRow ( );

            /* V2: pulse highlight target row z cross-window navigation. */
            if ( row == s_focus_highlight_page ) {
                double rem = s_focus_highlight_until - now;
                if ( rem < 0.0 ) rem = 0.0;
                if ( rem > MEMMAP_FOCUS_HIGHLIGHT_SEC ) rem = MEMMAP_FOCUS_HIGHLIGHT_SEC;
                float alpha = ( float ) ( rem / MEMMAP_FOCUS_HIGHLIGHT_SEC );
                /* Žluté pulse highlight - kontrast vůči RowBg defaultu.
                 * Linear fade z plné na 0 přes 1.5s = jasná pulse animace. */
                ImU32 hi = IM_COL32 ( 240, 200, 60,
                                       ( int ) ( 180.0f * alpha ) );
                ImGui::TableSetBgColor ( ImGuiTableBgTarget_RowBg0, hi );
            }

            /* Sloupec Addr - text label $XYZ0 přes InvisibleButton aby
             * fungoval pravý klik (popup Banking menu) a hover hint
             * konzistentně s buňkou Banking sloupce. */
            ImGui::TableSetColumnIndex ( 0 );
            {
                char addr_buf[ 8 ];
                snprintf ( addr_buf, sizeof addr_buf, "$%X000", row );

                ImVec2 cur_screen = ImGui::GetCursorScreenPos ( );
                ImVec2 cell_avail = ImGui::GetContentRegionAvail ( );
                float row_h = ImGui::GetTextLineHeight ( );

                char addr_id[ 32 ];
                snprintf ( addr_id, sizeof addr_id,
                            "##memmap_addr_%d", row );
                ImGui::InvisibleButton ( addr_id,
                                          ImVec2 ( cell_avail.x, row_h ) );

                if ( ImGui::IsItemHovered ( ) ) {
                    ImGui::SetTooltip ( "%s", _ ( "Right: menu" ) );
                };
                if ( ImGui::BeginPopupContextItem ( ) ) {
                    memmap_banking_render_popup_content ( );
                    ImGui::EndPopup ( );
                };

                /* Text label nad InvisibleButton - cursor vrátit a
                 * vykreslit. Adresa zarovnaná vlevo (default text flow). */
                ImGui::SetCursorScreenPos ( cur_screen );
                ImGui::TextUnformatted ( addr_buf );
            };

            /* Sloupec Banking - sjednocená buňka (marker + content)
             * s jedním klikatelným hit-testem. */
            en_MEMMAP_REGION_KIND kind = memmap_query ( ( uint8_t ) row );
            const char *dummy_label = NULL;
            ImU32 bg_color = IM_COL32 ( 80, 80, 80, 255 );
            memmap_kind_get_visual ( kind, &dummy_label, &bg_color );

            ImGui::TableSetColumnIndex ( 1 );
            memmap_render_banking_combined_cell ( kind, row, bg_color );

            /* Sloupec MemExt. */
            ImGui::TableSetColumnIndex ( 2 );
            memmap_render_memext_cell ( row );
        };

        ImGui::EndTable ( );
    };

    ImGui::End ( );
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
