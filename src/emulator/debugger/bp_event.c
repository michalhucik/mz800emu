/*
 * File:   bp_event.c
 *
 * Implementace HW event vocabulary + dispatch (V1.5 HWE redesign).
 *
 * - String tabulka pro stabilní názvy (JSON serializace, UI dropdown).
 * - bp_event_fire() = dispatch z emu hook sites do breakpoints_enforce_hw_event.
 * - g_bp_event_active[] bitmap pro fast-skip v hot path.
 * - g_bp_event_state[] prev signal level pro edge detection (signal events).
 * - bp_event_get_kind() = klasifikace eventu pro UI/enforce dispatch.
 * - bp_event_trigger_to_string / from_string = trigger condition serializace.
 *
 * BC mapování legacy persistence stringů:
 *   - "pio:porta_int" / "pio:portb_int" -> IRQ_PIOZ80_A / IRQ_PIOZ80_B
 *   - "fdc:irq" -> IRQ_FDC
 *   - "tape:edge" -> CMT_IN
 *   - vyřazené eventy (psg:int_pa5, fdc:command/drq, mmio:*, tape:read/write_byte,
 *     ppi:*, ctc:gate0_edge) -> parse failure (= invalid event v BP, fire never).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPLv3, see LICENSE.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"
#include "mzarch/mzhal.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bp_event.h"


/* Forward decl - implementace v breakpoints.c. */
extern void breakpoints_enforce_hw_event ( en_BP_EVENT event, int32_t value );


/* Bitmap aktivních eventů (default OFF). */
bool g_bp_event_active [ BP_EVENT_COUNT ];

/* Per-event prev signal value pro edge detection (default 0). */
uint8_t g_bp_event_state [ BP_EVENT_COUNT ];

/* Ambient reason hodnota pro DSL identifikátor `Reason`. Default
 * BP_REASON_NONE; nastavuje callsite (mzarch_iff_change_cb) těsně před
 * bp_event_fire a vrací na NONE po dokončení fire stacku. Viz bp_event.h
 * doc pro detaily pattern. */
uint8_t g_bp_fire_reason = (uint8_t)BP_REASON_NONE;


/*
 * Stabilní řetězcové názvy eventů. Index = en_BP_EVENT hodnota.
 * BP_EVENT_NONE = "" (= žádný event nastaven).
 */
static const char *s_event_names [ BP_EVENT_COUNT ] = {
    [ BP_EVENT_NONE ]                = "",

    /* Signal events */
    [ BP_EVENT_GDG_VSYNC ]           = "vsync",
    [ BP_EVENT_GDG_HSYNC ]           = "hsync",
    [ BP_EVENT_GDG_VBLN ]            = "vbln",
    [ BP_EVENT_GDG_HBLN ]            = "hbln",
    [ BP_EVENT_CTC_ZC0 ]             = "ctc:zc0",
    [ BP_EVENT_CTC_ZC1 ]             = "ctc:zc1",
    [ BP_EVENT_CTC_ZC2 ]             = "ctc:zc2",
    [ BP_EVENT_IRQ_CTC2 ]            = "irq:ctc2",
    [ BP_EVENT_IRQ_PIOZ80_A ]        = "irq:pioz80_a",
    [ BP_EVENT_IRQ_PIOZ80_B ]        = "irq:pioz80_b",
    [ BP_EVENT_IRQ_FDC ]             = "irq:fdc",
    [ BP_EVENT_TEMPO ]               = "tempo",
    [ BP_EVENT_CURSOR ]              = "cursor",
    [ BP_EVENT_CMT_IN ]              = "cmt:in",
    [ BP_EVENT_CMT_OUT ]             = "cmt:out",
    [ BP_EVENT_CMT_MSTATE ]          = "cmt:mstate",
    [ BP_EVENT_CMT_MOTOR ]           = "cmt:motor",

    /* Change events */
    [ BP_EVENT_GDG_MODE_CHANGE ]     = "mode_change",
    [ BP_EVENT_GDG_PALETTE_CHANGE ]  = "palette_change",
    [ BP_EVENT_GDG_PALGRP_CHANGE ]   = "palgrp_change",
    [ BP_EVENT_GDG_BORDER_CHANGE ]   = "border_change",
    [ BP_EVENT_CMT_STATE_CHANGE ]    = "cmt:state_change",

    /* Point event s parametrem */
    [ BP_EVENT_GDG_RASTER ]          = "raster",       /* parametrizováno: "raster:N" */

    /* CPU events */
    [ BP_EVENT_CPU_NMI ]             = "cpu:nmi",
    [ BP_EVENT_CPU_DI ]              = "cpu:di",
    [ BP_EVENT_CPU_IM_CHANGE ]       = "cpu:im_change",
    [ BP_EVENT_CPU_IFF_CHANGE ]      = "cpu:iff_change",
    [ BP_EVENT_CPU_IFF1_CHANGE ]     = "cpu:iff1_change",
    [ BP_EVENT_CPU_IFF2_CHANGE ]     = "cpu:iff2_change",
    [ BP_EVENT_CPU_HALT ]            = "cpu:halt",
    [ BP_EVENT_CPU_RESET ]           = "cpu:reset",
};


/*
 * Trigger condition stable names (= JSON serializace).
 */
static const char *s_trigger_names [ BP_EVT_TRIG_COUNT ] = {
    [ BP_EVT_TRIG_RISING ]   = "rising",
    [ BP_EVT_TRIG_FALLING ]  = "falling",
    [ BP_EVT_TRIG_CHANGED ]  = "changed",
    [ BP_EVT_TRIG_LOW ]      = "low",
    [ BP_EVT_TRIG_HIGH ]     = "high",
};


void bp_event_init ( void ) {
    bp_event_clear_all_active ( );
    memset ( g_bp_event_state, 0, sizeof ( g_bp_event_state ) );
}


void bp_event_clear_all_active ( void ) {
    memset ( g_bp_event_active, 0, sizeof ( g_bp_event_active ) );
}


void bp_event_set_active ( en_BP_EVENT event, bool active ) {
    if ( event <= BP_EVENT_NONE || event >= BP_EVENT_COUNT ) return;
    g_bp_event_active [ event ] = active;
}


bool bp_event_is_supported_on_current_arch ( en_BP_EVENT event ) {
    /* Runtime dle g_mzhal (mzhal krok 8) - drive compile-time #if. */
    switch ( event ) {
        case BP_EVENT_IRQ_PIOZ80_A:
        case BP_EVENT_IRQ_PIOZ80_B:
            return g_mzhal.have_pioz80 ? true : false;

        /* GDG change eventy - jen MZ-800 ma DMD/palette/palgrp/border ve
         * full forme. MZ-1500 a MZ-700 fire mode_change/palette_change s
         * jinou semantikou (jiny register layout) a nemaji palgrp/border
         * vubec - ze mz800 layoutu UI by byly matouci. */
        case BP_EVENT_GDG_MODE_CHANGE:
        case BP_EVENT_GDG_PALETTE_CHANGE:
        case BP_EVENT_GDG_PALGRP_CHANGE:
        case BP_EVENT_GDG_BORDER_CHANGE:
            return (g_mzhal.arch == 800) ? true : false;

        default:
            return true;
    }
}


en_BP_EVENT_KIND bp_event_get_kind ( en_BP_EVENT event ) {
    switch ( event ) {
        /* Signal events - mají trigger condition. */
        case BP_EVENT_GDG_VSYNC:
        case BP_EVENT_GDG_HSYNC:
        case BP_EVENT_GDG_VBLN:
        case BP_EVENT_GDG_HBLN:
        case BP_EVENT_CTC_ZC0:
        case BP_EVENT_CTC_ZC1:
        case BP_EVENT_CTC_ZC2:
        case BP_EVENT_IRQ_CTC2:
        case BP_EVENT_IRQ_PIOZ80_A:
        case BP_EVENT_IRQ_PIOZ80_B:
        case BP_EVENT_IRQ_FDC:
        case BP_EVENT_TEMPO:
        case BP_EVENT_CURSOR:
        case BP_EVENT_CMT_IN:
        case BP_EVENT_CMT_OUT:
        case BP_EVENT_CMT_MSTATE:
        case BP_EVENT_CMT_MOTOR:
            return BP_EVT_KIND_SIGNAL;

        /* Change events - implicit "changed". */
        case BP_EVENT_GDG_MODE_CHANGE:
        case BP_EVENT_GDG_PALETTE_CHANGE:
        case BP_EVENT_GDG_PALGRP_CHANGE:
        case BP_EVENT_GDG_BORDER_CHANGE:
        case BP_EVENT_CMT_STATE_CHANGE:
            return BP_EVT_KIND_CHANGE;

        /* Point s parametrem. */
        case BP_EVENT_GDG_RASTER:
            return BP_EVT_KIND_POINT_PARAM;

        /* Point bez parametru (CPU events). */
        case BP_EVENT_CPU_NMI:
        case BP_EVENT_CPU_DI:
        case BP_EVENT_CPU_IM_CHANGE:
        case BP_EVENT_CPU_IFF_CHANGE:
        case BP_EVENT_CPU_IFF1_CHANGE:
        case BP_EVENT_CPU_IFF2_CHANGE:
        case BP_EVENT_CPU_HALT:
        case BP_EVENT_CPU_RESET:
            return BP_EVT_KIND_POINT_NOPARAM;

        /* NONE / out-of-range = bezpecny default. */
        case BP_EVENT_NONE:
        case BP_EVENT_COUNT:
        default:
            return BP_EVT_KIND_POINT_NOPARAM;
    };
}


const char* bp_event_to_string ( en_BP_EVENT event ) {
    if ( event < 0 || event >= BP_EVENT_COUNT ) return "";
    const char *s = s_event_names [ event ];
    return s ? s : "";
}


bool bp_event_from_string ( const char *name,
                             en_BP_EVENT *out_event,
                             int32_t *out_param ) {
    if ( !name || !out_event ) return false;

    /* Speciál: prázdný string = BP_EVENT_NONE. */
    if ( name[0] == '\0' ) {
        *out_event = BP_EVENT_NONE;
        if ( out_param ) *out_param = 0;
        return true;
    };

    /* BC aliasy: starší persistence (V1.5.A8 a starší).
     *
     * Mapování legacy stringů na nový enum. Vyřazené eventy
     * (psg:int_pa5, fdc:command/drq, mmio:*, tape:read/write_byte,
     * ppi:*, ctc:gate0_edge) NEJSOU v alias tabulce - parse selže
     * a UI/persist warn-loguje (= BP nikdy nezafire). */
    static const struct {
        const char *legacy;
        en_BP_EVENT event;
    } legacy_aliases[] = {
        /* HWE rename (V1.5.A8 -> V1.5 HWE) */
        { "pio:porta_int",  BP_EVENT_IRQ_PIOZ80_A },
        { "pio:portb_int",  BP_EVENT_IRQ_PIOZ80_B },
        { "fdc:irq",        BP_EVENT_IRQ_FDC },
        { "tape:edge",      BP_EVENT_CMT_IN },

        /* Pre-HWE CPU prefix legacy (V1.5.E a starší) */
        { "nmi",        BP_EVENT_CPU_NMI },
        { "di",         BP_EVENT_CPU_DI },
        { "im_change",  BP_EVENT_CPU_IM_CHANGE },
        { "iff_change", BP_EVENT_CPU_IFF_CHANGE },
        { "halt",       BP_EVENT_CPU_HALT },
        { "reset",      BP_EVENT_CPU_RESET },
    };
    for ( size_t i = 0; i < sizeof ( legacy_aliases ) / sizeof ( legacy_aliases [ 0 ] ); i++ ) {
        if ( strcmp ( name, legacy_aliases [ i ].legacy ) == 0 ) {
            *out_event = legacy_aliases [ i ].event;
            if ( out_param ) *out_param = 0;
            return true;
        };
    };

    /* Zkusit "raster:N" formát - hledej dvojtečku a stringif za ní je
     * čistě číslo. Tabulka má jen "raster" prefix; specifické subevent
     * names ("ctc:zc0", "irq:fdc", ...) jsou full match v tabulce. */
    const char *colon = strchr ( name, ':' );
    if ( colon ) {
        /* Nejdřív zkusit přesný match (= "ctc:zc0" apod. má colon). */
        for ( int i = 1; i < BP_EVENT_COUNT; i++ ) {
            const char *s = s_event_names [ i ];
            if ( s && strcmp ( name, s ) == 0 ) {
                *out_event = (en_BP_EVENT) i;
                if ( out_param ) *out_param = 0;
                return true;
            };
        };

        /* Fallback: prefix do colon = base name, suffix za colon = N. */
        size_t base_len = (size_t) ( colon - name );
        const char *suffix = colon + 1;
        if ( base_len == 0 || suffix[0] == '\0' ) return false;

        /* Validace, že suffix je dekadické číslo. */
        char *endp = NULL;
        long n = strtol ( suffix, &endp, 10 );
        if ( !endp || endp == suffix || *endp != '\0' ) return false;
        if ( n < 0 || n > 65535 ) return false;

        /* Hledej event jehož string == base_name (= prefix bez colon). */
        for ( int i = 1; i < BP_EVENT_COUNT; i++ ) {
            const char *s = s_event_names [ i ];
            if ( !s ) continue;
            if ( strlen ( s ) == base_len && strncmp ( name, s, base_len ) == 0 ) {
                *out_event = (en_BP_EVENT) i;
                if ( out_param ) *out_param = (int32_t) n;
                return true;
            };
        };
        return false;
    }

    /* Bez dvojtečky - přesný match. */
    for ( int i = 1; i < BP_EVENT_COUNT; i++ ) {
        const char *s = s_event_names [ i ];
        if ( s && strcmp ( name, s ) == 0 ) {
            *out_event = (en_BP_EVENT) i;
            if ( out_param ) *out_param = 0;
            return true;
        };
    };
    return false;
}


const char* bp_event_trigger_to_string ( en_BP_EVENT_TRIGGER trig ) {
    if ( trig < 0 || trig >= BP_EVT_TRIG_COUNT ) return "rising";
    const char *s = s_trigger_names [ trig ];
    return s ? s : "rising";
}


bool bp_event_trigger_from_string ( const char *s,
                                     en_BP_EVENT_TRIGGER *out ) {
    if ( !s || !out ) return false;
    for ( int i = 0; i < BP_EVT_TRIG_COUNT; i++ ) {
        const char *n = s_trigger_names [ i ];
        if ( n && strcmp ( s, n ) == 0 ) {
            *out = (en_BP_EVENT_TRIGGER) i;
            return true;
        };
    };
    return false;
}


void bp_event_fire ( en_BP_EVENT event, int32_t value ) {
    if ( event <= BP_EVENT_NONE || event >= BP_EVENT_COUNT ) return;
    /* Hot path defensiva - duplikát testu z hook site, ale chrání proti
     * přímému volání bez prior testu. */
    if ( !g_bp_event_active [ event ] ) return;

    breakpoints_enforce_hw_event ( event, value );
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
