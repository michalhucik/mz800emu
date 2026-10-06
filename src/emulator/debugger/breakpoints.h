/*
 * File:   breakpoints.h
 *
 * Modul breakpoints — vyšší vrstva nad bptmap.
 * Vlastní kompletní data breakpointů a skupin, řídí jejich životní cyklus,
 * ukládá konfiguraci do JSON souboru (.bpt).
 *
 * Datový model V1 (smart BP fáze D.1):
 *   - st_BPT obsahuje typ (en_BPT_TYPE), zónu paměti (en_BP_ZONE),
 *     range adresy (addr/addr_end), I/O port, název HW eventu, SP
 *     threshold, condition výraz, action mini-DSL skript, hit/skip
 *     counter a edge_triggered flag.
 *   - V1 implementace zapisuje data do souboru (JSON), backend pro
 *     vyhodnocení condition/action a hooky pro non-PC typy přijdou
 *     v sub-fázích D.1.2 - D.5.
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

#ifndef BREAKPOINTS_H
#define BREAKPOINTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

#include "bp_event.h"
#include "dbgapi_cmdrq.h"

/* Forward decl - opaque AST handle z bp_expr.h. Plný include zde
 * neuvádíme, abychom udrželi breakpoints.h štíhlý. */
struct st_BP_EXPR;
/* Forward decl - opaque AST handle z bp_action.h (D.1.3). */
struct st_BP_ACTION;


    /* Výchozí barvy pro breakpointy a skupiny */
#define BREAKPOINTS_DEFAULT_BG_RGB  0x000000
#define BREAKPOINTS_DEFAULT_FG_RGB  0xFFFFFF


    /*
     * Aktuální verze .bpt JSON schématu (V1.6+ TODO 4.7, fáze 1).
     *
     * Řetězcový tag pro klíč "schema_version" v JSON root. Liší se od
     * číselného `BREAKPOINTS_JSON_FORMAT_VERSION` (interní makro v
     * breakpoints.c) tím, že:
     *
     * - je veřejné API (lze použít v testech),
     * - je readable string (snadnější diagnostika v JSON souboru),
     * - umožňuje semantic versioning ("V1.5", "V1.6", "V2.0-pre", ...).
     *
     * Loader toleruje:
     * - missing klíč → fallback "V1.0-pre-versioning" + info log,
     * - neznámou hodnotu → warning na stderr, ale parse pokračuje
     *   (= forward-compat: starší emu načte novější soubor best-effort).
     *
     * Při změně schématu (= incompatible structural change) inkrementovat
     * verzi a popsat migration v `persistence.md`.
     */
#define BREAKPOINTS_SCHEMA_VERSION_CURRENT  "V1.5"

    /*
     * Fallback hodnota pro .bpt soubory bez "schema_version" klíče.
     * Označuje soubory zapsané před zavedením verzování (= V1.0 - V1.5
     * pre-4.7).
     */
#define BREAKPOINTS_SCHEMA_VERSION_FALLBACK "V1.0-pre-versioning"


    /*
     * Typ breakpointu (smart BP V1).
     *
     * Hodnoty jsou stabilní - serializace do .bpt JSON souboru používá
     * řetězcové názvy (viz bpt_type_to_string). Číselné hodnoty se v
     * souboru NEvyskytují.
     *
     * V1 (fáze D.1) ukládá typ do datového modelu, ale enforcement
     * non-PC typů přidává až D.2 - D.5.
     */
    typedef enum en_BPT_TYPE {
        BPT_TYPE_PC_EXEC = 0,       /* execution na CPU adrese (= dnešní default) */
        BPT_TYPE_MEM_R,             /* memory read na adrese / range */
        BPT_TYPE_MEM_W,             /* memory write na adrese / range */
        BPT_TYPE_IORQ_R,            /* I/O port read */
        BPT_TYPE_IORQ_W,            /* I/O port write */
        BPT_TYPE_IRQ,               /* CPU acknowledged INT (post-dispatch, IM mode + filter) */
        BPT_TYPE_HW_EVENT,          /* pojmenovaný HW event (vsync, ctc:zc0, ...) */
        BPT_TYPE_SP_THRESHOLD,      /* stack overflow detekce (SP < threshold) */
        BPT_TYPE_GLOBAL,            /* bez adresy, jen condition (= "kdykoliv X") */
        BPT_TYPE_IRQ_SIG,           /* V1.5.A8.5: peripheral INT line raise (pre-dispatch) */
        BPT_TYPE_COUNT              /* sentinel - počet platných hodnot */
    } en_BPT_TYPE;


    /*
     * IRQ_SIG source bity (V1.5.A8.5).
     *
     * Bitmask v st_BPT.irq_sig_source_mask. Multi-source BP fire pokud
     * libovolný z vybraných sources raise INT line (= OR semantics).
     *
     * Pokrývá MZ-800 / MZ-1500 (= shodný interrupt subsystem). Bit pozice
     * stabilní pro persistence (= JSON ukládá array of stable string
     * names přes bp_irq_sig_source_to_string).
     */
    typedef enum en_BP_IRQ_SIG_SOURCE {
        BP_IRQ_SIG_PIOZ80_PORT_A = 1 << 0,  /* Z80 PIO port A (joystick / FDC bridge) */
        BP_IRQ_SIG_PIOZ80_PORT_B = 1 << 1,  /* Z80 PIO port B (PSG INT, parallel) */
        BP_IRQ_SIG_CTC2          = 1 << 2,  /* CTC channel 2 (timer/counter) */
        BP_IRQ_SIG_FDC           = 1 << 3,  /* WD279x floppy controller */
        BP_IRQ_SIG_OTHER         = 1 << 4,  /* nedetekovatelný / bus latch */
        BP_IRQ_SIG_ALL           = 0x1F     /* mask všech 5 sources */
    } en_BP_IRQ_SIG_SOURCE;


    /*
     * Zóna paměti pro memory-aware BP (banking).
     *
     * V1 datový model uchovává hodnotu, banking-aware enforcement
     * přidává D.5. Default = BP_ZONE_CPU_VIEW (= co vidí PC v aktuálním
     * banking stavu, banking-agnostic).
     *
     * Hodnoty jsou stabilní - serializace přes bp_zone_to_string.
     */
    typedef enum en_BP_ZONE {
        BP_ZONE_CPU_VIEW = 0,       /* default - co vidí PC teď (banking-aware view) */
        BP_ZONE_ROM_LOWER,          /* MZ-800 dolní ROM (0x0000-0x0FFF Monitor) */
        BP_ZONE_ROM_UPPER,          /* MZ-800 horní ROM (0xE000-0xFFFF) */
        BP_ZONE_RAM,                /* hlavní RAM (banking-fixed) */
        BP_ZONE_VRAM_FB,            /* VRAM framebuffer (banking-aware okno) */
        BP_ZONE_PCG,                /* PCG (programmable character generator) */
        BP_ZONE_MMEXT_BANK,         /* memory expansion overlay bank (+ bank_id), bývalý PEHU_BANK */
        BP_ZONE_COUNT               /* sentinel - počet platných hodnot */
    } en_BP_ZONE;


    /*
     * Match mode pro adresové / portové / bank fieldy (V1.5.E).
     *
     * SINGLE = legacy V1 chování (= match jen na jednu hodnotu).
     * RANGE  = match pokud hodnota je v <ref, end> inclusive.
     * MASK   = match pokud (hodnota & mask) == (ref & mask).
     *
     * Hodnoty stabilní - serializace do .bpt JSON přes
     * bp_match_mode_to_string(). Číselné hodnoty se v souboru
     * NEvyskytují.
     */
    typedef enum en_BP_MATCH_MODE {
        BP_MATCH_SINGLE = 0,        /* default - jeden bod */
        BP_MATCH_RANGE,             /* start..end inclusive */
        BP_MATCH_MASK,              /* (x & mask) == (ref & mask) */
        BP_MATCH_COUNT              /* sentinel - počet platných hodnot */
    } en_BP_MATCH_MODE;


    /**
     * @brief Interpretace pole `addr` u MEM_R/W breakpointu v zóně
     *        BP_ZONE_MMEXT_BANK (feature D).
     *
     * CPU_VIEW    = default, zpětně kompatibilní: `addr` je Z80 adresa
     *               0x0000-0xFFFF (BP fajruje na write na tuto adresu,
     *               když je daná banka namapovaná).
     * BANK_OFFSET = `addr` je offset 0x0000-0x1FFF v rámci PEHU banky;
     *               BP fajruje na write do (bank_id, offset) bez ohledu
     *               na to, do kterého CPU okna je banka právě namapovaná.
     *
     * Pro ostatní zóny se pole ignoruje (ale persistuje se, aby přepnutí
     * zóny/typu neztratilo hodnotu). Hodnoty stabilní - serializace do
     * .bpt JSON přes bp_addr_space_to_string().
     */
    typedef enum en_BP_ADDR_SPACE {
        BP_ADDR_SPACE_CPU_VIEW = 0,     /* default - addr je Z80 adresa */
        BP_ADDR_SPACE_BANK_OFFSET,      /* addr je offset v PEHU bance */
        BP_ADDR_SPACE_COUNT             /* sentinel */
    } en_BP_ADDR_SPACE;


    /*
     * SP threshold mode (V1.5.E) - oddělený enum, sémantika jiná než
     * obecný match mode.
     *
     * SINGLE = legacy V1 chování (= trigger při sestupném crossingu
     *          jednoho prahu, stack overflow detekce).
     * WINDOW = trigger pokud SP opustí <sp_threshold .. sp_upper>
     *          (= stack corruption / cross-task switch detect).
     */
    typedef enum en_BP_SP_MODE {
        BP_SP_SINGLE = 0,           /* trigger při crossingu jedné hranice */
        BP_SP_WINDOW,               /* trigger pokud SP opustí [lower..upper] */
        BP_SP_MODE_COUNT            /* sentinel */
    } en_BP_SP_MODE;


    /*
     * Z80 I/O addressing šířka pro IORQ_R/W BP (V1.5.A7).
     *
     * Z80 má dva I/O addressing patterny:
     *   - 8-bit: IN A,(n) / OUT (n),A - port literal v opcode (0..0xFF),
     *            high byte adresové sběrnice je obvykle "don't care".
     *   - 16-bit: IN r,(C) / OUT (C),r - plný BC registr na adresové
     *             sběrnici (B = high, C = low).
     *
     * BP_PORT_8BIT (default) = match jen low byte hit_port (= zachová
     *   V1 chování, kde každý IORQ s odpovídajícím low byte fire).
     * BP_PORT_16BIT = match plný BC pattern (= rozliší IN A,(0xCE)
     *   vs IN r,(C) kde BC=0x42CE).
     *
     * Hodnoty stabilní - serializace .bpt JSON přes bp_port_mode_to_string.
     */
    typedef enum en_BP_PORT_MODE {
        BP_PORT_8BIT = 0,           /* default - obvyklý Z80 IORQ (low byte only) */
        BP_PORT_16BIT,              /* full BC pattern (16-bit port) */
        BP_PORT_MODE_COUNT          /* sentinel */
    } en_BP_PORT_MODE;


    /*
     * Skupina breakpointů — umožňuje hierarchické řazení a hromadné ovládání.
     */
    typedef struct st_BPTGROUP {
        int id;             /* kladné číslo, unikátní identifikátor */
        int parent;         /* -1 = root, jinak ID rodičovské skupiny */
        float order;        /* pořadí zobrazení v UI */
        bool enabled;
        char *name;         /* heap, g_strdup */
        uint32_t bg_rgb;    /* barva pozadí labelu */
        uint32_t fg_rgb;    /* barva textu labelu */
    } st_BPTGROUP;


    /*
     * Breakpoint — jeden konkrétní breakpoint.
     *
     * Datový model V1 (post fáze D.1):
     *   - Společná pole pro všechny typy: id, parent, enabled, name,
     *     auto_name, bg_rgb, fg_rgb, hits, type, zone, expr, action,
     *     hit_count, skip_count, edge_triggered.
     *   - Per-typ specifická pole (využití závisí na type):
     *       PC_EXEC, MEM_R/W, IRQ:    addr, addr_end, zone, bank_id
     *       IORQ_R/W:                  port (16-bit Z80 I/O adresa)
     *       HW_EVENT:                  event_name (heap string, např. "vsync")
     *       SP_THRESHOLD:              sp_threshold
     *       GLOBAL:                    žádná adresa, používá jen expr
     *
     *   Pole nepoužívaná pro daný typ jsou inicializována na 0/NULL/false
     *   a měla by být ignorována konzumenty BP - viz bp_field_used_by_type
     *   (TODO: helper, D.6 UI).
     *
     * POZN:
     *   - addr je dnes 16-bit (CPU view stačí pro V1). Banking
     *     awareness přes 24-bit physical adresy = V1.5/V2.
     *   - V1 je BPT_TYPE_PC_EXEC funkční via bptmap (= legacy code path);
     *     ostatní typy jsou enumeračně přípustné, ale nemají enforcement
     *     hook (= D.2 - D.5).
     */
    typedef struct st_BPT {
        /* === Identifikace a hierarchie (z fází A'/C) === */
        int id;             /* kladné číslo, unikátní identifikátor */
        int parent;         /* -1 = root, jinak ID rodičovské skupiny */
        bool enabled;
        bool auto_name;     /* true = jméno se generuje automaticky z adresy */
        char *name;         /* heap, g_strdup */
        uint32_t bg_rgb;
        uint32_t fg_rgb;
        uint64_t hits;      /* počítadlo aktivací (display only) */

        /* === Smart BP fields (V1, fáze D.1) === */
        en_BPT_TYPE type;       /* typ BP, default BPT_TYPE_PC_EXEC */
        uint16_t addr;          /* primární adresa (PC, MEM, IRQ vector) */
        uint16_t addr_end;      /* horní hranice range BP, default == addr */
        en_BP_ZONE zone;        /* memory zone, default BP_ZONE_CPU_VIEW */
        uint8_t bank_id;        /* bank index pro BP_ZONE_MMEXT_BANK */
        uint16_t port;          /* I/O port pro BPT_TYPE_IORQ_R/W */
        char *event_name;       /* heap, jen pro BPT_TYPE_HW_EVENT (např. "vsync" nebo "raster:192") */
        /* Parsovaný event_name do enum + parametru (D.3 cache, hot path).
         * BP_EVENT_NONE = nenastaveno nebo invalid. event_param = 0 pro
         * eventy bez parametru, jinak hodnota z "name:N" suffixu (raster). */
        en_BP_EVENT parsed_event;
        int32_t event_param;
        /* V1.5 HWE - trigger condition pro signal eventy (vsync, ctc:zc0, ...).
         * Aplikuje se jen pro BP_EVT_KIND_SIGNAL eventy. Pro CHANGE / POINT
         * je hodnota uložena, ale enforce ji ignoruje. Default RISING zachovává
         * legacy V1.5.A8 chování (= "fire on event happened"). */
        en_BP_EVENT_TRIGGER event_trigger;
        uint16_t sp_threshold;  /* jen pro BPT_TYPE_SP_THRESHOLD */
        char *expr;             /* heap, condition výraz (NULL = vždy true) */
        char *action;           /* heap, action mini-DSL skript (NULL = stop) */
        /* Cache parsovaného AST z expr (D.1.2). Plní setter
         * breakpoints_set_expr() a load. NULL = unparsed nebo invalid. */
        struct st_BP_EXPR *parsed_expr;
        /* Cache parsovaného AST z action (D.1.3). Plní setter
         * breakpoints_set_action() a load. NULL = unparsed nebo invalid
         * nebo prázdná action (= stop sémantika). */
        struct st_BP_ACTION *parsed_action;
        uint32_t hit_count;     /* trigger až po N. hitu (0 = trigger každý) */
        uint32_t skip_count;    /* skip prvních N hitů (0 = nic neskip) */
        bool edge_triggered;    /* true = trigger jen na přechod, false = level */

        /* === Match modes (V1.5.E) ============================================
         *
         * Defaults: SINGLE mode + max-mask (0xFFFF / 0xFF) zachovává V1
         * sémantiku (= žádné runtime chování změny při výchozích hodnotách).
         *
         * RANGE pro addr: používá legacy field addr_end (= úspora migrace).
         * RANGE pro port: používá nové port_end.
         * RANGE pro bank: používá nové bank_id_end.
         * RANGE pro SP:   sp_threshold = lower bound, sp_upper = upper bound.
         */
        en_BP_MATCH_MODE addr_match_mode;   /* PC_EXEC / MEM_R / MEM_W match mode */
        uint16_t addr_mask;                 /* AND mask pro MASK mode (default 0xFFFF) */

        en_BP_MATCH_MODE port_match_mode;   /* IORQ_R / IORQ_W match mode */
        uint16_t port_end;                  /* RANGE upper pro port */
        uint16_t port_mask;                 /* AND mask pro port (default 0xFFFF) */
        en_BP_PORT_MODE port_mode;          /* V1.5.A7: 8-bit vs 16-bit IORQ adresace */

        en_BP_MATCH_MODE bank_match_mode;   /* MMEXT_BANK zone match mode */
        uint8_t bank_id_end;                /* RANGE upper pro bank */
        uint8_t bank_id_mask;               /* AND mask pro bank (default 0xFF) */

        /* Feature D: interpretace addr pro BP_ZONE_MMEXT_BANK (CPU_VIEW
         * default / BANK_OFFSET = offset 0x0000-0x1FFF v PEHU bance). */
        en_BP_ADDR_SPACE bp_addr_space;

        en_BP_SP_MODE sp_mode;              /* SP_THRESHOLD mode (SINGLE / WINDOW) */
        uint16_t sp_upper;                  /* WINDOW upper bound (lower = sp_threshold) */

        /* === V1.5.A8 - IRQ vector + ISR address filter ====================
         *
         * Volitelné filtry pro BPT_TYPE_IRQ aplikované POST-dispatch
         * (= po z80_process_interrupt, kdy už je znám vector slot a ISR
         * jump target). Oba enabled = false (default) zachovává legacy
         * V1 chování (= fire na každý dispatch).
         *
         * im2_vector_addr je očekávaný IM2 vector table address ve tvaru
         * (I << 8) | (vec & 0xFE). Při comparison enforce maskuje low byte
         * 0xFE (= HW omezení vector page boundary), takže user může zadat
         * libovolnou hodnotu - bit 0 je při matchi ignorován.
         *
         * im2_isr_addr je očekávaná ISR jump target adresa (= cpu->pc
         * bezprostředně po dispatchi).
         *
         * Filter explicitně vyžaduje IM 2: v IM 0/1 dispatch BP s některým
         * z filtrů zapnutých NEFIRUJE (= vector ani ISR pro IM 0/1 nejsou
         * smysluplně definované).
         *
         * Match mode pro vector/ISR: SINGLE / RANGE / MASK (V1.6+ TODO 4.4).
         * Default SINGLE pro BC s V1.5 .bpt files.
         *
         * Pri SINGLE = porovnani exact (= V1.5 chovani).
         * Pri RANGE = vector_addr <= x <= vector_addr_end.
         * Pri MASK = (x & vector_mask) == (vector_addr & vector_mask).
         *
         * HW vector page boundary mask 0xFFFE (= bit 0 ignorovan) je
         * aplikovana na vector_addr PRED bp_match16 testem.
         */
        bool im2_vector_enabled;            /* true = filter aktivní (jen pro IM 2) */
        uint16_t im2_vector_addr;           /* očekávaný (I << 8) | (vec & 0xFE) */
        en_BP_MATCH_MODE im2_vector_match_mode; /* V1.6+ 4.4: SINGLE/RANGE/MASK */
        uint16_t im2_vector_addr_end;       /* V1.6+ 4.4: RANGE upper bound (inclusive) */
        uint16_t im2_vector_mask;           /* V1.6+ 4.4: MASK bitmask */
        bool im2_isr_enabled;               /* true = filter aktivní (jen pro IM 2) */
        uint16_t im2_isr_addr;              /* očekávaná ISR adresa = cpu->pc po dispatchu */
        en_BP_MATCH_MODE im2_isr_match_mode; /* V1.6+ 4.4: SINGLE/RANGE/MASK */
        uint16_t im2_isr_addr_end;          /* V1.6+ 4.4: RANGE upper bound (inclusive) */
        uint16_t im2_isr_mask;              /* V1.6+ 4.4: MASK bitmask */

        /* === V1.5.A8.5 - IRQ IM mode discriminator + IM 0 RST filter ======
         *
         * Per-IM enable flag. Aspoň jeden musí být true (= UI validation),
         * jinak BP nikdy nefire. Defaults pro nový BPT_TYPE_IRQ = all true
         * (= zachová legacy A8 chování fire-na-všechny IRQ dispatch).
         *
         * im0_rst_mask: bitmask filtruje IM 0 RST opcode na bus. Mask 0 =
         * match-all (= legacy). Mask != 0 = match jen flagged RSTs:
         *   bit 0 = RST 00h (opcode 0xC7)
         *   bit 1 = RST 08h (opcode 0xCF)
         *   bit 2 = RST 10h (opcode 0xD7)
         *   bit 3 = RST 18h (opcode 0xDF)
         *   bit 4 = RST 20h (opcode 0xE7)
         *   bit 5 = RST 28h (opcode 0xEF)
         *   bit 6 = RST 30h (opcode 0xF7)
         *   bit 7 = RST 38h (opcode 0xFF)
         */
        bool im0_enabled;                   /* fire na IM 0 dispatch */
        bool im1_enabled;                   /* fire na IM 1 dispatch */
        bool im2_enabled;                   /* fire na IM 2 dispatch */
        uint8_t im0_rst_mask;               /* IM 0 RST opcode filter mask */

        /* === V1.5.A8.5 - BPT_TYPE_IRQ_SIG (pre-dispatch peripheral signal) ===
         *
         * Bitmask z en_BP_IRQ_SIG_SOURCE. BP fires pokud aktuální edge raise
         * INT line zahrnuje libovolný z vybraných sources (OR semantics).
         * Default 0 = invalid (= UI validation requires aspoň 1).
         */
        uint8_t irq_sig_source_mask;        /* bitmask en_BP_IRQ_SIG_SOURCE */

        /* === V1.C.3 - Owner attribution (mutant mcp-server) ===============
         *
         * Indikuje, kdo BP vytvořil (= USER GUI klik / MCP klient / TEST /
         * INTERNAL emu). Pole je čisté metadata - žádný runtime efekt na
         * matching, fire ani persistenci v XML (v1.0).
         *
         * Default DBGAPI_CMD_ORIGIN_USER pro nově alokované BP (= legacy
         * chování = vytvořeno GUI). Dispatcher v dbgapi.c po úspěšném
         * breakpoints_add_auto() přepíše hodnotu na rq->cmd_origin.
         *
         * UI tabulka BP zobrazuje krátký badge přes owner_badge_render().
         */
        en_DBGAPI_CMD_ORIGIN cmd_origin;

        /* === 0019 vrstva 2 - per-BP rate-limit těžkých FWD akcí ===========
         *
         * Implicitní ochrana proti saturaci diskem/CPU opakovanými těžkými
         * forward akcemi (snapshot / trace_save) na horkém BP. Aplikuje se
         * JEN na FWD_SNAPSHOT a FWD_TRACE_SAVE (ostatní FWD jsou levné).
         *
         * Konfigurace (override; měněno uživatelem / MCP / .bpt):
         *   - fwd_min_interval_ms: minimální odstup mezi dvěma firy těžké
         *     akce v ms. 0 = použij vestavěný default
         *     BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS (bezpečný default i bez
         *     jakékoliv konfigurace). Při porušení = akce se TIŠE skipne.
         *   - fwd_max_fires: tvrdý strop počtu úspěšných firů těžké akce za
         *     session (od enable / posledního resetu). 0 = neomezeno. Při
         *     dosažení = BP se sám vypne (disable_self ekvivalent) + warning.
         *
         * Runtime stav (NE konfigurace; resetuje se při (re-)enable BP přes
         * breakpoints_set_enabled a při breakpoints_init):
         *   - fwd_last_fire_us: čas posledního úspěšného firu v mikrosekundách
         *     monotonního zdroje (g_get_monotonic_time). 0 = ještě nikdy.
         *   - fwd_fire_count: počet úspěšných firů těžké akce od resetu.
         */
        uint32_t fwd_min_interval_ms;   /* 0 = vestavěný default */
        uint32_t fwd_max_fires;         /* 0 = neomezeno */
        int64_t  fwd_last_fire_us;      /* runtime: monotonní čas posl. firu */
        uint32_t fwd_fire_count;        /* runtime: počet firů od resetu */
    } st_BPT;


    /*
     * Hlavní kontejner pro breakpointy a skupiny.
     *
     * POZOR na GArray invalidaci: g_array_remove_index() přesouvá elementy
     * v paměti — ukazatele získané z find_by_id() se tím invalidují!
     * Po jakékoliv operaci remove je nutné znovu volat find_by_id().
     */
    typedef struct st_BREAKPOINTS {
        /* Konfigurace z [BREAKPOINTS] sekce hlavního INI */
        char *default_file;     /* cesta k souboru s breakpointy */
        unsigned auto_save;     /* bool pro cfgfile handler */
        unsigned auto_load;
        unsigned groups_first;

        /* Data */
        GArray *groups;         /* GArray(st_BPTGROUP) */
        GArray *breakpoints;    /* GArray(st_BPT) */

        /* Čítač pro přidělování unikátních ID */
        int next_id;

        /* Verze dat — inkrementuje se po každé mutaci (add/remove/set/clear/load/sync).
         * UI cache porovnává s vlastní kopií a při neshodě přepočítá stav. */
        unsigned version;
    } st_BREAKPOINTS;

    extern st_BREAKPOINTS g_breakpoints;


    /* === Životní cyklus === */

    extern void breakpoints_init ( void );
    extern void breakpoints_exit ( void );


    /* === Persistence === */

    extern void breakpoints_save_to_file ( void );

    /**
     * @brief Nahradí BP, skupiny a $vars obsahem výchozího souboru.
     *
     * Cestu vyhodnotí z g_breakpoints.default_file proti konfiguračnímu
     * adresáři a zavolá breakpoints_load_from_filepath().
     *
     * @pre Volat jen z emu vlákna (z UI přes DBGAPI_CMD_BP_LOAD_FILE,
     *      helper dbg_ui_bp_load_from_file) nebo při init, kdy emu vlákno
     *      BP nevyhodnocuje. Přestavuje data, která emu vlákno čte.
     */
    extern void breakpoints_load_from_file ( void );

    /* Varianty se specifikovanou cestou (pro Load From... / Save As...) */
    extern void breakpoints_save_to_filepath ( const char *filepath );

    /**
     * @brief Nahradí BP, skupiny a $vars obsahem souboru @p filepath.
     *
     * Nejdřív vždy zavolá breakpoints_clear_all(); neexistující nebo
     * nečitelný soubor tiše zanechá prázdný stav. Po načtení validuje
     * rodiče a adresy, přestaví bptmap a zvýší g_breakpoints.version.
     *
     * @param filepath Cesta k souboru; NULL nebo "" = no-op (data se nemažou).
     *
     * @pre Volat jen z emu vlákna (z UI přes DBGAPI_CMD_BP_LOAD_FILE) nebo
     *      v jednovláknovém kontextu (init, testy). Uvolňuje stringy a AST
     *      BP a zkracuje pole, která emu vlákno čte při vyhodnocení BP.
     * @note Gating logging callbacků nepřepočítává; dělá to handler
     *       DBGAPI_CMD_BP_LOAD_FILE.
     */
    extern void breakpoints_load_from_filepath ( const char *filepath );


    /* === CRUD skupiny === */

    /* Vytvoří novou skupinu. Vrátí ID nové skupiny. */
    extern int breakpoints_group_add ( const char *name, int parent );

    /* Odstraní skupinu. Potomci (skupiny i BPT) se přesunou do root (-1). Vrátí true při úspěchu. */
    extern bool breakpoints_group_remove ( int group_id );

    /* Najde skupinu podle ID. Vrátí ukazatel nebo NULL. POZOR: invaliduje se po remove! */
    extern st_BPTGROUP* breakpoints_group_find_by_id ( int group_id );

    extern bool breakpoints_group_set_enabled ( int group_id, bool enabled );
    extern bool breakpoints_group_set_name ( int group_id, const char *name );
    extern bool breakpoints_group_set_colors ( int group_id, uint32_t bg_rgb, uint32_t fg_rgb );
    extern bool breakpoints_group_set_order ( int group_id, float order );
    extern unsigned breakpoints_group_count ( void );

    /* Nastaví nového rodiče pro skupinu. Validuje proti cyklům. */
    extern bool breakpoints_group_set_parent ( int group_id, int new_parent );


    /* === CRUD breakpointy === */

    /* Vytvoří nový breakpoint na adrese. Vrátí ID nebo -1 pokud na adrese již existuje jiný BPT. */
    extern int breakpoints_add ( uint16_t addr, const char *name, int parent );

    /* Vytvoří nový breakpoint na adrese. Pokud je adresa obsazena, automaticky nastaví enabled=false.
     * Vrátí ID nového BPT, nebo -1 při fatální chybě (přetečení ID). */
    extern int breakpoints_add_auto ( uint16_t addr, const char *name, int parent );

    /* Odstraní breakpoint a odregistruje ho z bptmap. Vrátí true při úspěchu. */
    extern bool breakpoints_remove ( int bpt_id );

    /* Najde BPT podle ID. Vrátí ukazatel nebo NULL. POZOR: invaliduje se po remove! */
    extern st_BPT* breakpoints_find_by_id ( int bpt_id );

    /* Najde BPT podle adresy. Vrátí ukazatel nebo NULL. */
    extern st_BPT* breakpoints_find_by_addr ( uint16_t addr );

    /**
     * @brief Najde všechny BP relevantní pro danou CPU adresu (range-aware).
     *
     * Match pravidla per typ:
     *   - BPT_TYPE_PC_EXEC, MEM_R, MEM_W: addr_match_mode (SINGLE / RANGE /
     *     MASK) přes bp_match16 nad bpt->addr / addr_end / addr_mask
     *   - BPT_TYPE_GLOBAL: vždy zahrnut (condition může referencovat cokoliv)
     *   - BPT_TYPE_IORQ_*, IRQ, IRQ_SIG, HW_EVENT, SP_THRESHOLD: nezahrnuje
     *     se (jejich match je na portu/eventu/IM/SP, ne na CPU addr)
     *
     * Použití: UI tooltipy, popup menu - "co je na řádku".
     * Komplexita: O(N) přes celý seznam BP. Pro UI hover OK.
     *
     * @param addr      CPU 16-bit adresa.
     * @param out_ids   Caller-owned GArray<int>, prázdné se vyplní. Funkce
     *                  appenduje bpt_id - žádné implicitní g_array_set_size,
     *                  caller může předem g_array_set_size(0) pro reuse.
     * @return Počet nalezených BP (= out_ids->len delta od entry).
     *
     * Thread safety: čte g_breakpoints.breakpoints. Bezpečné z UI vlákna
     * pokud emu paused, jinak nutno wrap do breakpoints lock.
     */
    extern int breakpoints_find_all_by_addr ( uint16_t addr, GArray *out_ids );

    /* Nastaví enabled/disabled a synchronizuje s bptmap. */
    extern bool breakpoints_set_enabled ( int bpt_id, bool enabled );

    extern bool breakpoints_set_name ( int bpt_id, const char *name );
    extern bool breakpoints_set_auto_name ( int bpt_id, bool auto_name );
    extern bool breakpoints_set_colors ( int bpt_id, uint32_t bg_rgb, uint32_t fg_rgb );

    /* Nastaví nového rodiče pro event. Provede resync bptmap. */
    extern bool breakpoints_set_parent ( int bpt_id, int new_parent );

    /* Nastaví novou adresu pro event. Provede resync bptmap. */
    extern bool breakpoints_set_addr ( int bpt_id, uint16_t new_addr );
    extern void breakpoints_increment_hits ( int bpt_id );

    /**
     * @brief Vynuluje počítadlo zásahů BP (bpt->hits) a zvýší g_breakpoints.version.
     *
     * Neexistující ID = no-op.
     *
     * @param bpt_id ID breakpointu.
     *
     * @pre Volat z EMU vlákna (nebo z jednovláknového kontextu). hits++
     *      a test hit_count dělá emu vlákno při vyhodnocení BP; UI posílá
     *      reset přes DBGAPI_CMD_BP_RESET_HITS (helper dbg_ui_bp_reset_hits).
     */
    extern void breakpoints_reset_hits ( int bpt_id );
    extern unsigned breakpoints_count ( void );


    /* === Hromadné operace === */

    /**
     * @brief Smaže všechny breakpointy, skupiny a $vars a vyčistí bptmap.
     *
     * Vyčistí i aktivní bitmapu HW eventů a dočasný BP (Run To / Step
     * Over) v bptmap; zvýší g_breakpoints.version.
     *
     * @pre Volat jen z emu vlákna (z UI přes DBGAPI_CMD_BP_CLEAR_ALL,
     *      helper dbg_ui_bp_clear_all) nebo v jednovláknovém kontextu
     *      (init, testy). Uvolňuje stringy a AST BP, které emu vlákno
     *      čte při vyhodnocení BP.
     * @note Gating logging callbacků nepřepočítává; dělá to handler
     *       DBGAPI_CMD_BP_CLEAR_ALL.
     */
    extern void breakpoints_clear_all ( void );

    /* Zkontroluje, zda je BPT efektivně povolený — tj. enabled on + všechny rodičovské skupiny enabled. */
    extern bool breakpoints_is_effectively_enabled ( int bpt_id );

    /* Resynchronizuje celý bptmap s aktuálním stavem breakpointů (zachovává temporary BPT). */
    extern void breakpoints_sync_bptmap ( void );

    /* Přepočte g_bptmap.has_enabled_bp (= existuje aspoň jeden efektivně
     * povolený BP). Volá se po BP CRUD mimo sync (add/remove/enable), aby
     * TEST_DEBUGGER_CPUHIST_ACTIVE viděl aktuální stav. */
    extern void breakpoints_recompute_has_enabled ( void );


    /* === Smart BP setters (V1, fáze D.1) =================================== */

    /* Nastaví typ BP. Vrátí true při úspěchu, false pokud BP neexistuje
     * nebo type je mimo rozsah BPT_TYPE_COUNT. */
    extern bool breakpoints_set_type ( int bpt_id, en_BPT_TYPE type );

    /* Nastaví memory zone. Vrátí true při úspěchu. */
    extern bool breakpoints_set_zone ( int bpt_id, en_BP_ZONE zone );

    /* Nastaví bank_id (pro BP_ZONE_MMEXT_BANK). Vrátí true při úspěchu. */
    extern bool breakpoints_set_bank_id ( int bpt_id, uint8_t bank_id );

    /* Nastaví range konec (addr_end). Vrátí true při úspěchu.
     * Pokud end < addr, hodnota se uloží beze změny (validace je věcí UI/parseru). */
    extern bool breakpoints_set_addr_end ( int bpt_id, uint16_t addr_end );

    /* Nastaví range najednou (addr i addr_end). Vrátí true při úspěchu.
     * NEsynchronizuje bptmap pro non-PC typy - addr stále jde přes
     * breakpoints_set_addr (pro PC-exec backend). */
    extern bool breakpoints_set_addr_range ( int bpt_id, uint16_t addr, uint16_t addr_end );

    /* Nastaví I/O port (pro IORQ types). Vrátí true při úspěchu. */
    extern bool breakpoints_set_port ( int bpt_id, uint16_t port );

    /* Nastaví event_name (pro BPT_TYPE_HW_EVENT). Předaný string se zkopíruje (g_strdup).
     * NULL = vyčistí předchozí hodnotu. Vrátí true při úspěchu. */
    extern bool breakpoints_set_event_name ( int bpt_id, const char *event_name );

    /**
     * @brief Nastaví trigger condition pro HW_EVENT BP (V1.5 HWE).
     *
     * Aplikuje se jen pro signal eventy (vsync, ctc:zc0, irq:fdc, ...).
     * Pro CHANGE / POINT eventy hodnota uložena ale enforce ji ignoruje.
     *
     * @param bpt_id ID breakpointu.
     * @param trig   Trigger condition (LOW / HIGH / RISING / FALLING / CHANGED).
     * @return false pokud BP neexistuje nebo trig mimo BP_EVT_TRIG_COUNT.
     */
    extern bool breakpoints_set_event_trigger ( int bpt_id, en_BP_EVENT_TRIGGER trig );

    /* Nastaví SP threshold (pro BPT_TYPE_SP_THRESHOLD). Vrátí true při úspěchu. */
    extern bool breakpoints_set_sp_threshold ( int bpt_id, uint16_t threshold );

    /* Nastaví condition expression. Předaný string se zkopíruje. NULL = vyčistí.
     * Vrátí true při úspěchu. */
    extern bool breakpoints_set_expr ( int bpt_id, const char *expr );

    /* Nastaví action mini-DSL skript. Předaný string se zkopíruje. NULL = vyčistí.
     * Vrátí true při úspěchu. */
    extern bool breakpoints_set_action ( int bpt_id, const char *action );

    /* Nastaví hit_count (trigger po N. hitu, 0 = každý hit). Vrátí true při úspěchu. */
    extern bool breakpoints_set_hit_count ( int bpt_id, uint32_t hit_count );

    /* Nastaví skip_count (skip prvních N hitů). Vrátí true při úspěchu. */
    extern bool breakpoints_set_skip_count ( int bpt_id, uint32_t skip_count );

    /* Nastaví edge_triggered flag. Vrátí true při úspěchu. */
    extern bool breakpoints_set_edge_triggered ( int bpt_id, bool edge_triggered );

    /**
     * @brief Nastaví per-BP override minimálního odstupu těžkých FWD akcí (0019 v2).
     *
     * Override rate-limitu pro forward akce snapshot / trace_save daného BP.
     * @param bpt_id    ID breakpointu.
     * @param interval_ms Minimální odstup mezi dvěma firy v ms. 0 = použij
     *        globální / vestavěný default (NE nula = bezpečný default zůstává).
     * @return false pokud BP s @p bpt_id neexistuje, jinak true.
     * @note Runtime stav (fwd_last_fire_us, fwd_fire_count) tento setter
     *       nemění; ten se resetuje při (re-)enable BP.
     */
    extern bool breakpoints_set_fwd_min_interval_ms ( int bpt_id, uint32_t interval_ms );

    /**
     * @brief Nastaví per-BP tvrdý strop počtu firů těžké FWD akce (0019 v2).
     *
     * @param bpt_id    ID breakpointu.
     * @param max_fires Maximální počet úspěšných firů za session (od enable /
     *        resetu). 0 = neomezeno. Při dosažení se BP sám vypne (disable_self).
     * @return false pokud BP s @p bpt_id neexistuje, jinak true.
     */
    extern bool breakpoints_set_fwd_max_fires ( int bpt_id, uint32_t max_fires );


    /* === Match mode settery (V1.5.E) ======================================= */

    /**
     * @brief Nastaví addr_match_mode (PC_EXEC / MEM_R / MEM_W).
     * @return false pokud BP neexistuje nebo mode mimo BP_MATCH_COUNT.
     */
    extern bool breakpoints_set_addr_match_mode ( int bpt_id, en_BP_MATCH_MODE mode );

    /** @brief Nastaví addr_mask (AND mask pro MASK mode). */
    extern bool breakpoints_set_addr_mask ( int bpt_id, uint16_t mask );

    /** @brief Nastaví port_match_mode (IORQ_R / IORQ_W). */
    extern bool breakpoints_set_port_match_mode ( int bpt_id, en_BP_MATCH_MODE mode );

    /** @brief Nastaví port_end (RANGE upper bound pro port). */
    extern bool breakpoints_set_port_end ( int bpt_id, uint16_t port_end );

    /** @brief Nastaví port_mask (AND mask pro port). */
    extern bool breakpoints_set_port_mask ( int bpt_id, uint16_t mask );

    /**
     * @brief Nastaví port_mode (8BIT / 16BIT) pro IORQ_R/W (V1.5.A7).
     * @return false pokud BP neexistuje nebo mode mimo BP_PORT_MODE_COUNT.
     */
    extern bool breakpoints_set_port_mode ( int bpt_id, en_BP_PORT_MODE mode );

    /**
     * @brief Nastaví bp_addr_space (feature D: CPU_VIEW / BANK_OFFSET) pro
     *        MEM_R/W v zóně MMEXT_BANK.
     * @return false pokud BP neexistuje nebo space mimo BP_ADDR_SPACE_COUNT.
     */
    extern bool breakpoints_set_bp_addr_space ( int bpt_id, en_BP_ADDR_SPACE space );

    /** @brief Nastaví bank_match_mode (MMEXT_BANK zone). */
    extern bool breakpoints_set_bank_match_mode ( int bpt_id, en_BP_MATCH_MODE mode );

    /** @brief Nastaví bank_id_end (RANGE upper bound pro bank). */
    extern bool breakpoints_set_bank_id_end ( int bpt_id, uint8_t bank_id_end );

    /** @brief Nastaví bank_id_mask (AND mask pro bank). */
    extern bool breakpoints_set_bank_id_mask ( int bpt_id, uint8_t mask );

    /** @brief Nastaví sp_mode (SP_THRESHOLD: SINGLE / WINDOW). */
    extern bool breakpoints_set_sp_mode ( int bpt_id, en_BP_SP_MODE mode );

    /** @brief Nastaví sp_upper (WINDOW upper bound). */
    extern bool breakpoints_set_sp_upper ( int bpt_id, uint16_t sp_upper );


    /* === IRQ vector / ISR filter settery (V1.5.A8) ========================= */

    /**
     * @brief Nastaví IM2 vector address filter (BPT_TYPE_IRQ).
     *
     * Filter aktivní jen v IM 2. Při comparison se aplikuje AND s 0xFE
     * (= vector page boundary), takže low bit je v match ignorován.
     *
     * @param bpt_id  ID breakpointu.
     * @param enabled true = filter zapnutý (BP fire jen pokud match).
     * @param addr    Očekávaný (I << 8) | (vec & 0xFE).
     * @return false pokud BP neexistuje.
     */
    extern bool breakpoints_set_im2_vector_filter ( int bpt_id, bool enabled, uint16_t addr );

    /**
     * @brief Nastaví IM2 ISR address filter (BPT_TYPE_IRQ).
     *
     * Filter aktivní jen v IM 2. Match je exact comparison s cpu->pc
     * po dispatchi.
     *
     * @param bpt_id  ID breakpointu.
     * @param enabled true = filter zapnutý.
     * @param addr    Očekávaná ISR adresa.
     * @return false pokud BP neexistuje.
     */
    extern bool breakpoints_set_im2_isr_filter ( int bpt_id, bool enabled, uint16_t addr );


    /* === IM2 vector / ISR Match modes (V1.6+ TODO 4.4) ===================== */

    /**
     * @brief Nastaví im2_vector match mode (BPT_TYPE_IRQ).
     *
     * @param bpt_id  ID breakpointu.
     * @param mode    SINGLE / RANGE / MASK.
     * @return false pokud BP neexistuje.
     */
    extern bool breakpoints_set_im2_vector_match_mode ( int bpt_id, en_BP_MATCH_MODE mode );

    /** @brief Nastaví im2_vector_addr_end (RANGE upper bound, inclusive). */
    extern bool breakpoints_set_im2_vector_addr_end ( int bpt_id, uint16_t addr_end );

    /** @brief Nastaví im2_vector_mask (MASK bitmask). */
    extern bool breakpoints_set_im2_vector_mask ( int bpt_id, uint16_t mask );

    /** @brief Nastaví im2_isr match mode (BPT_TYPE_IRQ). */
    extern bool breakpoints_set_im2_isr_match_mode ( int bpt_id, en_BP_MATCH_MODE mode );

    /** @brief Nastaví im2_isr_addr_end (RANGE upper bound, inclusive). */
    extern bool breakpoints_set_im2_isr_addr_end ( int bpt_id, uint16_t addr_end );

    /** @brief Nastaví im2_isr_mask (MASK bitmask). */
    extern bool breakpoints_set_im2_isr_mask ( int bpt_id, uint16_t mask );


    /* === IRQ IM mode discriminator + RST filter settery (V1.5.A8.5) ======== */

    /**
     * @brief Nastaví per-IM enable flag pro BPT_TYPE_IRQ.
     *
     * Aspoň jeden IM musí být enabled (= UI validation), jinak BP nikdy
     * nefire. Setter sám validaci nedělá - jen ukládá hodnotu.
     *
     * @param bpt_id  ID breakpointu.
     * @param im      Číslo IM mode (0 / 1 / 2).
     * @param enabled true = BP fire na dispatch v tomto IM.
     * @return false pokud BP neexistuje nebo im je mimo rozsah 0-2.
     */
    extern bool breakpoints_set_im_enabled ( int bpt_id, uint8_t im, bool enabled );

    /**
     * @brief Nastaví IM 0 RST opcode filter mask.
     *
     * Mask 0 = match-all (= legacy fire-na-každý RST). Bit i = match RST
     * opcode 0xC7 + i*8 (= RST 00..38 v 8-byte krokem).
     *
     * @param bpt_id ID breakpointu.
     * @param mask   Bitmask 8 RST opcodů.
     * @return false pokud BP neexistuje.
     */
    extern bool breakpoints_set_im0_rst_mask ( int bpt_id, uint8_t mask );


    /* === IRQ_SIG setter (V1.5.A8.5) ======================================== */

    /**
     * @brief Nastaví IRQ_SIG source mask (BPT_TYPE_IRQ_SIG).
     *
     * Bitmask z en_BP_IRQ_SIG_SOURCE. Pokud 0, BP nikdy nefire (= UI
     * validation requires aspoň 1 source).
     *
     * @param bpt_id ID breakpointu.
     * @param mask   Bitmask sources.
     * @return false pokud BP neexistuje.
     */
    extern bool breakpoints_set_irq_sig_source_mask ( int bpt_id, uint8_t mask );


    /* === IRQ_SIG source name konverze (V1.5.A8.5) ========================== */

    /**
     * @brief Vrátí stabilní jméno pro single-bit IRQ_SIG source.
     *
     * Akceptuje jeden bit z en_BP_IRQ_SIG_SOURCE (= power-of-2 hodnotu).
     * Pro multi-bit / 0 / neplatnou hodnotu vrátí NULL.
     *
     * Stabilní jména: "PIOZ80_A", "PIOZ80_B", "CTC2", "FDC", "OTHER".
     */
    extern const char* bp_irq_sig_source_to_string ( en_BP_IRQ_SIG_SOURCE src );

    /**
     * @brief Konverze stabilní jméno → en_BP_IRQ_SIG_SOURCE.
     *
     * @param s    Vstupní string (nesmí být NULL).
     * @param out  Výstup - jeden bit z en_BP_IRQ_SIG_SOURCE.
     * @return true při úspěchu, false pokud s je NULL nebo neznámé jméno.
     */
    extern bool bp_irq_sig_source_from_string ( const char *s, en_BP_IRQ_SIG_SOURCE *out );


    /* === Match helpery (V1.5.E) ============================================ */

    /**
     * @brief Generic match check pro 16-bit hodnoty (PC, MEM addr, IO port).
     *
     *   SINGLE: x == ref
     *   RANGE:  ref <= x <= end
     *   MASK:   (x & mask) == (ref & mask)
     *
     * @param mode  Match mode (BP_MATCH_*).
     * @param x     Aktuální hodnota (PC, addr, port).
     * @param ref   Referenční hodnota (= bpt->addr / bpt->port).
     * @param end   RANGE upper bound (relevant jen pro BP_MATCH_RANGE).
     * @param mask  MASK and-mask (relevant jen pro BP_MATCH_MASK).
     * @return true pokud x odpovídá pravidlu.
     */
    extern bool bp_match16 ( en_BP_MATCH_MODE mode, uint16_t x,
                              uint16_t ref, uint16_t end, uint16_t mask );

    /**
     * @brief Stejné jako bp_match16, ale pro 8-bit hodnoty (bank ID).
     */
    extern bool bp_match8 ( en_BP_MATCH_MODE mode, uint8_t x,
                             uint8_t ref, uint8_t end, uint8_t mask );


    /* === String konverze pro typ a zónu ==================================== */

    /* Vrátí stabilní řetězec pro daný typ (= JSON serializace).
     * Pro neznámý/COUNT typ vrátí "PC_EXEC" jako fallback. */
    extern const char* bpt_type_to_string ( en_BPT_TYPE type );

    /* Konverze řetězec → en_BPT_TYPE. Vrátí true při úspěchu, false pokud
     * je řetězec NULL/neznámý. Při chybě *out zůstane nezměněné. */
    extern bool bpt_type_from_string ( const char *s, en_BPT_TYPE *out );

    /* Vrátí stabilní řetězec pro danou zónu. Fallback "CPU_VIEW". */
    extern const char* bp_zone_to_string ( en_BP_ZONE zone );

    /* Konverze řetězec → en_BP_ZONE. Vrátí true při úspěchu. */
    extern bool bp_zone_from_string ( const char *s, en_BP_ZONE *out );


    /* === String konverze pro match mody (V1.5.E) =========================== */

    /** @brief Vrátí stabilní řetězec pro daný match mode ("SINGLE" / "RANGE" / "MASK"). */
    extern const char* bp_match_mode_to_string ( en_BP_MATCH_MODE mode );

    /** @brief Konverze řetězec → en_BP_MATCH_MODE. Vrátí true při úspěchu. */
    extern bool bp_match_mode_from_string ( const char *s, en_BP_MATCH_MODE *out );

    /** @brief Konverze en_BP_ADDR_SPACE → stabilní string ("cpu_view"/"bank_offset"). */
    extern const char* bp_addr_space_to_string ( en_BP_ADDR_SPACE space );

    /** @brief Konverze řetězec → en_BP_ADDR_SPACE. Vrátí true při úspěchu. */
    extern bool bp_addr_space_from_string ( const char *s, en_BP_ADDR_SPACE *out );

    /** @brief Vrátí stabilní řetězec pro SP mode ("SINGLE" / "WINDOW"). */
    extern const char* bp_sp_mode_to_string ( en_BP_SP_MODE mode );

    /** @brief Konverze řetězec → en_BP_SP_MODE. Vrátí true při úspěchu. */
    extern bool bp_sp_mode_from_string ( const char *s, en_BP_SP_MODE *out );

    /** @brief Vrátí stabilní řetězec pro IORQ port mode ("8BIT" / "16BIT"). */
    extern const char* bp_port_mode_to_string ( en_BP_PORT_MODE mode );

    /** @brief Konverze řetězec → en_BP_PORT_MODE. Vrátí true při úspěchu. */
    extern bool bp_port_mode_from_string ( const char *s, en_BP_PORT_MODE *out );


    /* === Condition evaluation (D.1.2) ====================================== */

    /* Forward decl - plný typ je v bp_expr.h. Caller voláním
     * breakpoints_eval_condition() musí #include "debugger/bp_expr.h". */
    struct bp_expr_ctx_s;

    /**
     * Vyhodnotí condition výraz BP proti aktuálnímu kontextu.
     * Pokud BP nemá expr (NULL nebo prázdný), vrátí true (= vždy fire).
     * Pokud parser cache je NULL ale expr není NULL (= parse selhalo),
     * vrátí true (= conservatively fire, problém je už zalogovaný
     * při set_expr).
     *
     * V1 NEpočítá hits/skip - jen čistá condition evaluation.
     * Hit/skip logiku přidá D.2 spolu s enforcement hooky.
     *
     * @param bpt   Breakpoint (musí být non-NULL)
     * @param ctx   Kontext pro eval (= naplněný caller-em)
     * @return true pokud condition fired (= akce má proběhnout)
     */
    extern bool breakpoints_eval_condition ( const st_BPT *bpt,
                                              const struct bp_expr_ctx_s *ctx );


    /* === Action execution (D.1.3) ========================================== */

    /**
     * Vykoná action skript BP (post-condition trigger).
     *
     * Implicit continue rule:
     *   - action == NULL nebo nikdy nebyla nastavená → vrátí false (= STOP)
     *   - action je nastavená a parser uspěl → vykoná příkazy a vrátí
     *     true (= CONTINUE, BP nezastavuje emulátor)
     *   - action je nastavená ale parser selhal → vrátí false (= STOP,
     *     conservatively - syntax chyba je už zalogovaná při set_action)
     *
     * V1 NEpočítá hits/skip - jen čistá akce. Hit/skip + edge logiku
     * doplní D.2 spolu s enforcement hooky.
     *
     * @param bpt   Breakpoint (musí být non-NULL)
     * @param ctx   Kontext pro eval argumentů (předává se shodně s
     *              bp_expr_ctx_s pro condition).
     * @return true = pokračuj (continue), false = zastav (stop)
     */
    extern bool breakpoints_run_action ( const st_BPT *bpt,
                                          const struct bp_expr_ctx_s *ctx );


    /* === Enforcement (D.2) ================================================= */

    /**
     * @brief Plná enforcement smyčka pro jeden BP (= D.2).
     *
     * Volá se z hooku v emu vrstvě (mzarch hot loop pro PC_EXEC,
     * memory/iorq read/write pro MEM/IORQ, interrupt manager pro IRQ,
     * per-instruction pro GLOBAL).
     *
     * Sekvence:
     *   1. Skip pokud BP není efektivně povolený (= enabled + group chain)
     *   2. skip_count: pokud > 0, dekrementuje a vrací (= ignoruj prvních N)
     *   3. condition eval (bp_expr): pokud false, vrací (žádný hit)
     *   4. hits++ (= statistika i řízení hit_count)
     *   5. hit_count: pokud > 0 a hits != hit_count, vrací (jen N. hit fires)
     *   6. action execute: NULL/empty = STOP, jinak skript rozhodne
     *   7. STOP → emulator_pause(true) + DBGAPI_MSG_BREAKPOINT_HIT (s id)
     *   8. CONTINUE → drain control-plane fronty (0019 vrstva 1) =
     *      pending PAUSE/STOP/... se obslouží na hranici akce (viz
     *      breakpoints_drain_control_plane_at_bp_boundary)
     *
     * @param bpt   Breakpoint (musí existovat v g_breakpoints).
     * @param ctx   Naplněný kontext. Caller musí nastavit ctx->self_id =
     *              bpt->id před voláním (D.2 self_id sémantika - umožňuje
     *              akci disable_self funkci).
     *
     * Side effects:
     *   - Pause emulátoru (= EMULATOR_PAUSED_BY_DEBUGGER) při STOP.
     *   - DBGAPI MSG send do UI vlákna.
     *   - Mutace bpt->hits (statistika).
     *   - Mutace user vars (= action storage).
     *   - Mutace memory/registers (= action poke/set).
     *   - Vlna 1 mutant event-viewer: pokud je @c TEST_TRACE_EVENTLOG_ACTIVE
     *     a kategorie @c EVENTLOG_CAT_BP_FIRE je v active masce, zapíše se
     *     1 záznam do Event Viewer ringu (po condition eval, před action
     *     execute). Subtype = @c en_EVENTLOG_BP_FIRE_SUB klasifikace AST,
     *     payload = @c bpt->id (low 16) | @c g_bp_fire_reason (next 8).
     *
     * Threading: jen z EMU vlákna (čte g_emulator + mutace BP state).
     */
    extern void breakpoints_enforce ( st_BPT *bpt,
                                       struct bp_expr_ctx_s *ctx );


    /**
     * @brief Drain control-plane fronty na hranici dokončené BP akce (0019).
     *
     * Vrstva 1 control-plane garance: zajišťuje, že pending řídicí příkazy
     * (PAUSE / STOP / STEP / ...) z UI/MCP se obslouží do jednoho dalšího
     * BP hitu i na horké smyčce, kde se k per-frame drainu (mzarch.c
     * screen-done) emulátor nemusí dostat (= těžká BP akce natahuje snímek
     * a synchronní submit z UI by jinak vytekl timeoutem).
     *
     * Volat výhradně PO dokončení BP akce, která vrací continue (= akce
     * reálně proběhla, jsme mimo per-instrukční horkou cestu). V OFF stavu
     * (žádný hit / hit bez akce) se sem nevstupuje. Náklad při hitu je jeden
     * @c dbgapi_emu_has_pending() atomic read + branch v případě prázdné
     * fronty (= stejně levné jako per-frame bod mzarch.c).
     *
     * Drainuje celou frontu stejnými primitivy jako per-frame bod
     * (dbgapi_emu_dequeue/dispatch/complete). Pokud některý příkaz nastaví
     * pauzu (emulator_pause(true)), enforce po návratu narazí na
     * EMULATOR_TEST_PAUSED v mzarch.c a vstoupí do paused-loop - žádný další
     * kód zde není potřeba.
     *
     * Pozn.: tato vrstva neřeší délku jedné dlouhé akce (ta vždy dobehne);
     * řeší responzivnost control-plane MEZI hity (= žádné hromadění).
     *
     * Side effects: dispatch řídicích příkazů z fronty (může nastavit
     * paused/run/reset stav, odeslat MSG/eventy). Žádný efekt při prázdné
     * frontě.
     *
     * Threading: jen z EMU vlákna (volá dbgapi_emu_* primitiva určená pro
     * emu stranu). No-op v buildu bez MZ800EMU_CFG_DEBUGGER_ENABLED a no-op
     * pokud CMDRQ fronta není inicializovaná (= dbgapi_init nebyl volán,
     * brzká fáze startu / test bez fronty).
     */
    extern void breakpoints_drain_control_plane_at_bp_boundary ( void );


    /* ===================================================================== */
    /*  0019 vrstva 3 - kumulativní byte backstop pro forward akce           */
    /* ===================================================================== */

    /**
     * @brief Nastaví práh kumulativního byte backstopu pro FWD akce (0019 v3).
     *
     * Backstop sčítá počet bajtů zapsaných těžkými forward akcemi
     * (snapshot / trace_save) napříč všemi BP. Po překročení prahu emulátor
     * sám zapauzuje (emulator_pause(true)) a vyemituje warning + "paused"
     * event s důvodem (= ochrana disku i při špatně nastavené vrstvě 2).
     *
     * @param limit_bytes Práh v bajtech. 0 = backstop vypnut (žádná
     *        auto-pauza). Default po @ref breakpoints_init je
     *        @c BP_ACTION_FWD_DEFAULT_BYTE_LIMIT.
     *
     * Side effects: pouze uloží práh. Neresetuje akumulátor.
     * Threading: měněno z UI/MCP vlákna (prostý uint64 zápis); čteno z emu
     * vlákna při FWD akci. Bez locku - hodnota je jen měkký práh, race na
     * čtení nevadí.
     */
    extern void breakpoints_fwd_set_byte_limit ( uint64_t limit_bytes );

    /**
     * @brief Vrátí aktuální práh byte backstopu v bajtech (0 = vypnuto).
     */
    extern uint64_t breakpoints_fwd_get_byte_limit ( void );

    /**
     * @brief Vrátí dosud naakumulovaný počet bajtů zapsaných FWD akcemi.
     *
     * Slouží pro diagnostiku / UI. Akumulátor se resetuje při
     * @ref breakpoints_init (= reset emulátoru).
     */
    extern uint64_t breakpoints_fwd_get_total_bytes ( void );

    /**
     * @brief Vynuluje byte akumulátor backstopu (0019 v3).
     *
     * Volá se z @ref breakpoints_init. Veřejné kvůli testům a možnému
     * ručnímu resetu z UI po vyřešení saturace.
     */
    extern void breakpoints_fwd_reset_byte_accounting ( void );

    /**
     * @brief Přičte bajty forward akce a vyhodnotí byte backstop (0019 v3).
     *
     * Volá bp_action.c po každém úspěšném zápisu těžké FWD akce
     * (snapshot / trace_save). Akumuluje bajty napříč všemi BP; po překročení
     * prahu (@ref breakpoints_fwd_set_byte_limit) emulátor sám zapauzuje
     * (emulator_pause(true)) + vyemituje warning a "paused" event s důvodem.
     *
     * @param bp_id ID breakpointu, jehož akce zápis provedla (do detailu eventu;
     *        -1 = neznámé / mimo enforcement kontext).
     * @param bytes Počet právě zapsaných bajtů (0 = no-op).
     *
     * Side effects: viz výše. Auto-pauza proběhne jen na hraně překročení.
     * Threading: jen z emu vlákna (safe-point mezi instrukcemi).
     */
    extern void breakpoints_fwd_account_bytes ( int bp_id, uint64_t bytes );


    /* ===================================================================== */
    /*  0019 vrstva 2 - globální default rate-limit intervalu pro FWD akce   */
    /* ===================================================================== */

    /**
     * @brief Nastaví globální default min. odstupu těžkých FWD akcí (0019 v2).
     *
     * Použije se pro BP, který nemá vlastní per-BP override
     * (fwd_min_interval_ms == 0). Inicializován z INI klíče
     * [BREAKPOINTS] fwd_default_min_interval_ms.
     *
     * @param interval_ms Globální default v ms. 0 = global default vypnut ->
     *        fallback na vestavěnou konstantu @c
     *        BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS (bezpečný default zůstává).
     *
     * Side effects: pouze uloží hodnotu.
     * Threading: měněno z UI/MCP vlákna, čteno z emu vlákna. Bez locku
     * (prostý uint32 read/write, měkký práh).
     */
    extern void breakpoints_fwd_set_default_min_interval_ms ( uint32_t interval_ms );

    /**
     * @brief Vrátí globální default min. odstupu těžkých FWD akcí v ms (0019 v2).
     */
    extern uint32_t breakpoints_fwd_get_default_min_interval_ms ( void );


    /**
     * @brief PC_EXEC dispatch z mzarch hot loop pro daný PC.
     *
     * Volá se z mzarch.c před vykonáním instrukce. Wrapper kolem
     * breakpoints_enforce s naplněným ctx pro PC_EXEC scénář.
     *
     * Pro temporary BP (= run-to-cursor / step-over CALL/RST/DJNZ/ED) si
     * zachovává legacy chování (= pause + show debugger bez BP okna).
     *
     * @param pc Aktuální g_mzarch_main.instruction_addr.
     *
     * Side effects: viz breakpoints_enforce.
     */
    extern void breakpoints_enforce_pc_exec ( uint16_t pc );


    /**
     * @brief V1.6+ TODO 4.3: enforce PC_EXEC MASK BP per-instruction.
     *
     * Iteruje per-type list BPTMAP_IDX_PC_EXEC_NONSINGLE (PC_EXEC s
     * MASK match mode). Volat **vedle** breakpoints_enforce_pc_exec
     * (= dual check). Hot loop test: g_bptmap.per_type_active[
     * BPTMAP_IDX_PC_EXEC_NONSINGLE] -> volat tuto fci.
     *
     * Vraci po prvnim BP ktery zafire pause (= EMULATOR_TEST_PAUSED).
     *
     * @param pc Aktualni instruction address.
     */
    extern void breakpoints_enforce_pc_exec_nonsingle ( uint16_t pc );


    /**
     * @brief MEM_R hook - volat z memory_read_byte() PŘED návratem.
     *
     * @param addr  Adresa čtení.
     * @param value Hodnota čtená (= byte, který volající právě nese).
     *
     * Iteruje per-typ list MEM_R, pro každý BP s addr v <bp.addr, bp.addr_end>
     * zavolá breakpoints_enforce. Hot path optimalizace: caller už musí
     * mít g_bptmap.per_type_active[BPTMAP_IDX_MEM_R] == true.
     */
    extern void breakpoints_enforce_mem_r ( uint16_t addr, uint8_t value );

    /**
     * @brief MEM_W hook - volat z memory_write_byte() PŘED zápisem.
     */
    extern void breakpoints_enforce_mem_w ( uint16_t addr, uint8_t value );

    /**
     * @brief IORQ_R hook - volat z port_read_with_logging_cb() po čtení.
     *
     * @param port  Plná 16-bit port adresa (= addr param z port_read_cb).
     * @param value Přečtená hodnota.
     */
    extern void breakpoints_enforce_iorq_r ( uint16_t port, uint8_t value );

    /**
     * @brief IORQ_W hook - volat z port_write_with_logging_cb() po zápisu.
     */
    extern void breakpoints_enforce_iorq_w ( uint16_t port, uint8_t value );

    /**
     * @brief IRQ enforce hook - volaný POST-dispatch (V1.5.A8 BREAKING vs V1).
     *
     * V1 chování: hook byl volán pre-dispatch z mzarch_interrupt_manager()
     * jen s 'raised' bitmapou. Vector ani ISR nebyly známy (peripheral
     * je dodá až při INTACK).
     *
     * V1.5.A8: hook se volá až POST-dispatch z mzarch.c (po
     * z80_process_interrupt) kde jsou dostupné vector_addr
     * (= (I << 8) | (vec & 0xFE)) a isr_addr (= cpu->pc po jumpu).
     * Tím získáme možnost filtrovat per IM2 slot nebo per ISR target.
     *
     * V1.5.A8.5: přidán int_vector_byte parameter (= raw cpu->int_vector
     * byte na bus při INTACK) a IM mode discriminator (im0/im1/im2_enabled
     * + im0_rst_mask). int_vector_byte je v IM 0 RST opcode (např. 0xCF
     * pro RST 08); v IM 2 = vec ze kterého se počítá vector_addr.
     *
     * Pro IM != 2 jsou vector_addr a isr_addr nedefinované (caller posílá
     * vector_addr=0 a isr_addr=cpu->pc). IRQ BP s im2_vector_enabled nebo
     * im2_isr_enabled v takovém případě NEFIRUJÍ (= filter explicit
     * vyžaduje IM 2). BP bez filtrů fire i pro IM 0/1 dispatch (pokud
     * příslušný im{N}_enabled je true).
     *
     * Vector match aplikuje AND s 0xFE (= HW vector page boundary), takže
     * uživatelská hodnota im2_vector_addr může mít libovolný bit 0.
     *
     * @param raised          INT bus mask před dispatch.
     * @param im_mode         Z80 IM mode (0/1/2) v okamžiku dispatchu.
     * @param vector_addr     IM2 vector table address (jen pro IM 2; jinak 0).
     * @param isr_addr        ISR jump target po dispatch (= cpu->pc).
     * @param int_vector_byte Raw cpu->int_vector byte (RST opcode v IM 0).
     */
    extern void breakpoints_enforce_irq ( uint8_t raised, uint8_t im_mode,
                                           uint16_t vector_addr, uint16_t isr_addr,
                                           uint8_t int_vector_byte );


    /**
     * @brief IRQ_SIG enforce hook - volaný PRE-dispatch (V1.5.A8.5).
     *
     * Volá se z interrupt.c z hot loop pre-dispatch (= ještě před
     * z80_process_interrupt). Detekuje edge raise (prev=0, curr=1) na INT
     * bus a fire BPT_TYPE_IRQ_SIG s odpovídajícím source mask.
     *
     * Caller povinen: hook je no-op pokud
     * g_bptmap.per_type_active[BPTMAP_IDX_IRQ_SIG] == false. Caller testuje
     * flag PŘED voláním (= zero overhead v default OFF).
     *
     * Per-source detekce z bus bitů:
     *   - PIOZ80 (sub-detekce PORT_A/B z g_pioz80.interrupt_port_id)
     *   - CTC2
     *   - FDC
     *   - Other = ostatní bity
     *
     * Match logic: BP fire pokud (bpt->irq_sig_source_mask & active_sources) != 0.
     *
     * @param raised_now   Aktuální INT bus mask (= g_mzarch_main.interrupt).
     * @param raised_prev  Předchozí INT bus mask (= snapshot z minulého volání).
     */
    extern void breakpoints_enforce_irq_sig ( uint8_t raised_now, uint8_t raised_prev );

    /**
     * @brief GLOBAL hook - volat per-instruction (dražší).
     *
     * Default OFF: hot loop zkontroluje
     * g_bptmap.per_type_active[BPTMAP_IDX_GLOBAL] FIRST a pouze v true
     * případě dispatchne. Bez aktivního GLOBAL BP = zero impact.
     *
     * Edge-triggered semantika: pokud bpt->edge_triggered, fires se jen
     * při přechodu condition false -> true (= V1 implementace via
     * statický prev_result hash per BP id, viz breakpoints.c).
     */
    extern void breakpoints_enforce_global ( void );


    /**
     * @brief SP_THRESHOLD hook - volat z mzarch hot loop po každé instrukci,
     *        pokud SP změnil hodnotu.
     *
     * Edge-triggered sémantika: BP fires JEN při přechodu
     * old_sp >= sp_threshold -> new_sp < sp_threshold (= "stack právě
     * překročil práh dolů"). Sustained stav SP < threshold po jedné fire
     * už znovu nefire dokud SP nestoupne nad threshold a znovu neklesne.
     *
     * Caller povinen: BP enforcement je no-op pokud
     * g_bptmap.per_type_active[BPTMAP_IDX_SP_THRESHOLD] == false. Hot path
     * tudíž testuje flag PŘED voláním (= zero overhead v default OFF).
     *
     * V1 ctx pro SP_THRESHOLD:
     *   ctx.Address = new_sp (= aktuální SP po překročení)
     *   ctx.Value   = bp->sp_threshold (= práh, který byl překročen)
     *
     * V1 omezení: jen "SP <" varianta. "SP >" / "delta > N" = V1.5.
     *
     * @param old_sp  Předchozí hodnota SP (před aktuální instrukcí).
     * @param new_sp  Aktuální hodnota SP (po aktuální instrukci).
     */
    extern void breakpoints_enforce_sp_threshold ( uint16_t old_sp,
                                                    uint16_t new_sp );


    /**
     * @brief HW_EVENT hook - dispatch z bp_event_fire().
     *
     * V1.5 HWE redesign: dispatch dle bp_event_get_kind(event):
     *   SIGNAL       - per-BP trigger condition check (low/high/rising/
     *                  falling/changed) proti g_bp_event_state[event] vs
     *                  current value (0/1).
     *   CHANGE       - implicit "happened" (žádný filter), value = new
     *                  hodnota (mode/palette/palgrp/border).
     *   POINT_PARAM  - filter na parametr (= raster row match).
     *   POINT_NOPARAM- implicit "happened", value = info data.
     *
     * Per-kind ctx pro condition expression:
     *   SIGNAL       Address=0,         Value=curr_state (0/1)
     *   CHANGE       Address=0,         Value=new value
     *   POINT_PARAM  Address=value,     Value=0
     *   POINT_NOPARAM Address=0,        Value=info
     *
     * State cache (g_bp_event_state[]) update PO iteraci pro signal
     * eventy - další fire pak vidí current jako prev pro edge detection.
     *
     * Hot path optimalizace: caller už musí mít g_bp_event_active[event]
     * == true (= je registrován alespoň 1 BP tohoto eventu).
     *
     * @param event Identifikace eventu (BP_EVENT_*).
     * @param value Per-kind value (signal level / change value / param / info).
     */
    extern void breakpoints_enforce_hw_event ( en_BP_EVENT event, int32_t value );


    /**
     * @brief Naplní bp_expr_ctx_t globálním emu state (cpu, BankPC).
     *
     * Helper pro UI Test Eval i pro enforce hooky. Caller předá zero-init
     * ctx (= bp_expr_ctx_zero); funkce přepíše jen pole nezávislá na typu
     * hooku (cpu, BankPC, BankAddr). Pole závislá na typu (Address, Value,
     * IsRead/Write/Exec/Port, self_id) plní caller sám.
     *
     * Cycle/Frame/Scanline zůstávají nuly - V1 nemá kanonické zdroje
     * globálního T-state / frame counteru přístupné přes mzarch API.
     *
     * Threading: jen z EMU vlákna v normálním BP enforce, z UI vlákna
     * pro Test Eval (= snapshot read, race tolerable při pause).
     *
     * @param ctx Cíl (NULL = no-op).
     */
    extern void breakpoints_fill_global_ctx ( struct bp_expr_ctx_s *ctx );


    /* === Per-typ index helper - rozšíření o HW_EVENT (D.3) ================== */

    /**
     * @brief Per-typ index pro HW_EVENT lookup (= analog en_BPTMAP_TYPE_IDX
     *        BPTMAP_IDX_*, ale interní pro D.3, samostatná tabulka).
     *
     * BP s typem BPT_TYPE_HW_EVENT se registrují do
     * g_bptmap.per_type_lists[BPTMAP_IDX_HW_EVENT] (nový slot v en_BPTMAP_TYPE_IDX).
     */


#ifdef __cplusplus
}
#endif

#endif /* BREAKPOINTS_H */
