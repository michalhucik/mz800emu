#include "main.h"
#ifndef GDG_EVENT_C
#define GDG_EVENT_C
#include "mz800_gdg.h"
#include "mzarch/mzarch.h"
#include "hw-generic/gdg/video.h"

/**
 * Search for the nearest following GDG event and set it to g_gdg.event
 *
 *      HBLN
 *      HSYNC
 *      VSYNC
 *      LAST_VISIBLE_PIXEL
 *
 * @param
 * @return
 */
static inline void gdg_event_set_next(void)
{

    if (g_gdg.event.event_name == MZEVENT_GDG_SCREEN_ROW_END)
    {
        g_gdg.event.event_name = 0;
    }
    else
    {
        g_gdg.event.event_name++;
    };

    while (1)
    {
        if ((g_gdgevent[g_gdg.event.event_name].start_row <= g_gdg.beam_row) &&
            ((g_gdgevent[g_gdg.event.event_name].start_row + g_gdgevent[g_gdg.event.event_name].num_rows - 1) >= g_gdg.beam_row))
        {

            g_gdg.event.ticks = g_gdg.beam_row * VIDEO_SCREEN_WIDTH + g_gdgevent[g_gdg.event.event_name].event_column;

            break;
        };
        g_gdg.event.event_name++;
    };
}

#include "mz800_vramctrl.h"
#include "mz800_framebuffer.h"
#include "mz800_framebuffer_done.h"

#include "hw-generic/pioz80/pioz80.h"
#include "hw-generic/ctc8253/ctc8253.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/hwlog.h"
#include "debugger/bp_event.h"
#include "debugger/io_activity.h"
#endif
// #include "hw-generic/pio8255/pio8255.h"

#ifndef INCLUDED_FROM_MZARCH_C
void mz800_main_event_callback_screen_done(void);
#endif // INCLUDED_FROM_MZARCH_C

static inline void gdg_process_events(void)
{
    switch (g_mzarch_main.event.event_name)
    {

    case MZEVENT_GDG_HBLN_END:
        g_gdg.hbln = HBLN_OFF;

        g_gdg.screen_is_already_rendered_at_beam_pos = g_mzarch_main.event.ticks;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* trace-suite hwlog: HBLN end edge (decimovaný kvůli ~31000/sec). */
        if ( TEST_TRACE_HWLOG_DISPATCH ) {
            hwlog_record_gdg_video_hs_edge ( HWLOG_GDG_VIDEO_HBLN_END );
        }
        /* HWE - HW event BP hook (hbln). HBLN_OFF = signal logicky 0
         * (= blanking neaktivni). enforce vrstva trigger condition vyhodnoti. */
        if ( g_bp_event_active[ BP_EVENT_GDG_HBLN ] ) {
            bp_event_fire ( BP_EVENT_GDG_HBLN, 0 );
        }
#endif

        /* V rezimu MZ-700 aktualizujeme screen framebuffer jakmile skonci HBLN. */
        if ((GDG_DMD_TEST_MODE700) && ((g_gdg.beam_row >= VIDEO_BEAM_CANVAS_FIRST_ROW) && (g_gdg.beam_row <= VIDEO_BEAM_CANVAS_LAST_ROW)))
        {
            if (framebuffer_get_screen_changes())
            {
                framebuffer_update_MZ700_current_screen_row();
            };

            /* Pokud jsme na poslednim pixelu screen area */
            if (g_gdg.beam_row == VIDEO_BEAM_CANVAS_LAST_ROW)
            {
                if (framebuffer_get_screen_changes())
                {
                    g_framebuffer.screen_changes--;
                    g_framebuffer.framebuffer_state |= FB_STATE_SCREEN_CHANGED;
                };
            };
        };
        break;

    case MZEVENT_GDG_HBLN_START:
        g_gdg.hbln = HBLN_ACTIVE;
        g_vramctrl.mz700_wr_latch_is_used = 0;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* trace-suite hwlog: HBLN start edge (decimovaný). */
        if ( TEST_TRACE_HWLOG_DISPATCH ) {
            hwlog_record_gdg_video_hs_edge ( HWLOG_GDG_VIDEO_HBLN_START );
        }
        /* HWE - HW event BP hook (hbln). HBLN_ACTIVE = signal logicky 1
         * (= blanking aktivni). */
        if ( g_bp_event_active[ BP_EVENT_GDG_HBLN ] ) {
            bp_event_fire ( BP_EVENT_GDG_HBLN, 1 );
        }
#endif

        unsigned last_vbln_state = g_gdg.vbln;

        if (g_gdg.beam_row == VIDEO_BEAM_CANVAS_LAST_ROW)
        {
            g_gdg.vbln = VBLN_ACTIVE;
        }
        else if (g_gdg.beam_row == VIDEO_BEAM_CANVAS_FIRST_ROW - 1)
        {
            g_gdg.vbln = VBLN_OFF;
        };

        if (last_vbln_state != g_gdg.vbln)
        {
            /* udalost pro PIO-Z80 - VBLN */
            pioz80_port_id_event(PIOZ80_PORT_A, PIOZ80_PORT_EVENT_PA5_VBLN, g_gdg.vbln);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            /* trace-suite hwlog: VBLN edge event (start nebo end). */
            if ( TEST_TRACE_HWLOG_DISPATCH ) {
                uint8_t sub = ( g_gdg.vbln == VBLN_ACTIVE )
                                  ? HWLOG_GDG_VIDEO_VBLN_START
                                  : HWLOG_GDG_VIDEO_VBLN_END;
                hwlog_record ( HWLOG_CHIP_GDG_VIDEO, sub, NULL );
            }
            /* HWE - HW event BP hook (vbln). VBLN_ACTIVE = signal logicky 1
             * (= vertikalni blanking aktivni), jinak 0. */
            if ( g_bp_event_active[ BP_EVENT_GDG_VBLN ] ) {
                bp_event_fire ( BP_EVENT_GDG_VBLN,
                                 ( g_gdg.vbln == VBLN_ACTIVE ) ? 1 : 0 );
            }
#endif
        };
        break;

    case MZEVENT_GDG_STS_VSYNC_END:
        g_gdg.sts_vsync = VSYN_OFF;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        if ( TEST_TRACE_HWLOG_DISPATCH ) {
            hwlog_record ( HWLOG_CHIP_GDG_VIDEO, HWLOG_GDG_VIDEO_VS_END, NULL );
        }
        /* HWE - HW event BP hook (vsync). VSYN_OFF = signal logicky 0. */
        if ( g_bp_event_active[ BP_EVENT_GDG_VSYNC ] ) {
            bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );
        }
#endif
        break;

    case MZEVENT_GDG_STS_VSYNC_START:
        g_gdg.sts_vsync = VSYN_ACTIVE;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        if ( TEST_TRACE_HWLOG_DISPATCH ) {
            hwlog_record ( HWLOG_CHIP_GDG_VIDEO, HWLOG_GDG_VIDEO_VS_START, NULL );
        }
        /* HWE - HW event BP hook (vsync). VSYN_ACTIVE = signal logicky 1.
         * Default trigger=RISING zachova legacy chovani fire-on-vsync-start. */
        if ( g_bp_event_active[ BP_EVENT_GDG_VSYNC ] ) {
            bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );
        }
#endif
        break;

    case MZEVENT_GDG_AFTER_LAST_SCREEN_PIXEL:
        /* V rezimu MZ-800 aktualizujeme screen framebuffer az po dokoncenem radku. */
        if (!GDG_DMD_TEST_MODE700)
        {
            if (framebuffer_get_screen_changes())
            {
                framebuffer_MZ800_current_screen_row_fill(VIDEO_CANVAS_WIDTH);
            };

            /* Pokud jsme na poslednim pixelu screen area */
            if (g_gdg.beam_row == VIDEO_BEAM_CANVAS_LAST_ROW)
            {
                if (framebuffer_get_screen_changes())
                {
                    g_framebuffer.screen_changes--;
                    g_framebuffer.framebuffer_state |= FB_STATE_SCREEN_CHANGED;
                };
            };
        };
        break;

#if 0
            case MZEVENT_GDG_STS_HSYNC_START:
                g_gdg.sts_hsync = 0;
                break;
#endif

    case MZEVENT_GDG_AFTER_LAST_VISIBLE_PIXEL:
        g_gdg.sts_hsync = HSYN_ACTIVE; /* aktivni o par ticku drive */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* trace-suite hwlog: STS_HSYNC start edge (decimovaný). */
        if ( TEST_TRACE_HWLOG_DISPATCH ) {
            hwlog_record_gdg_video_hs_edge ( HWLOG_GDG_VIDEO_HS_START );
        }
        /* HWE - HW event BP hooks: hsync (signal level 1 = ACTIVE) +
         * raster:N (parametrizováno per row). Test obou v jednom branchi
         * pro lepší branch predictor. HSYNC end fire dole v SCREEN_ROW_END. */
        if ( g_bp_event_active[ BP_EVENT_GDG_HSYNC ] ) {
            bp_event_fire ( BP_EVENT_GDG_HSYNC, 1 );
        }
        if ( g_bp_event_active[ BP_EVENT_GDG_RASTER ] ) {
            bp_event_fire ( BP_EVENT_GDG_RASTER, (int32_t) g_gdg.beam_row );
        }
#endif
        /* Jsme skutecne jeste ve viditelne casti obrazu? */
        if (g_gdg.beam_row < VIDEO_DISPLAY_HEIGHT)
        {
            if (framebuffer_get_border_changes())
            {
                framebuffer_border_current_row_fill();
            };
            g_gdg.last_updated_border_pixel = VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1;

            /* Pokud jsme na poslednim pixelu visible area */
            if (g_gdg.beam_row == VIDEO_BEAM_DISPLAY_LAST_ROW)
            {
                if (framebuffer_get_border_changes())
                {
                    g_framebuffer.border_changes--;
                    g_framebuffer.framebuffer_state |= FB_STATE_BORDER_CHANGED;
                };
            };
        };
        break;

    case MZEVENT_GDG_REAL_HSYNC_START:
        /* tady zacina skutecny HSYNC, ktery je na RGBI a jeho sestupna hrana je CTC1_CLK */
        ctc8253_clkfall(CTC_CS1, g_mzarch_main.event.ticks);
        break;

    case MZEVENT_GDG_SCREEN_ROW_END:
        g_gdg.sts_hsync = HSYN_OFF;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* trace-suite hwlog: STS_HSYNC end edge (decimovaný). */
        if ( TEST_TRACE_HWLOG_DISPATCH ) {
            hwlog_record_gdg_video_hs_edge ( HWLOG_GDG_VIDEO_HS_END );
        }
        /* HWE - HW event BP hook (hsync). HSYN_OFF = signal logicky 0. */
        if ( g_bp_event_active[ BP_EVENT_GDG_HSYNC ] ) {
            bp_event_fire ( BP_EVENT_GDG_HSYNC, 0 );
        }
#endif
        g_gdg.tempo_divider++;
        /* Na mem MZ800 ma TEMPO pravdepodobne cca 34 Hz - tedy od oka :) */
        if (g_gdg.tempo_divider == 229)
        {
            g_gdg.tempo_divider = 0;
            g_gdg.tempo++;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            /* HWE - HW event BP hook (tempo). SIGNAL_GDG_TEMPO = tempo & 1
             * = toggle pri kazdem inkrementu (= ~32 Hz signal). */
            if ( g_bp_event_active[ BP_EVENT_TEMPO ] ) {
                bp_event_fire ( BP_EVENT_TEMPO, (int32_t) ( g_gdg.tempo & 1 ) );
            }
#endif
        };

        /* Muzeme vynulovat update pozici borderu ve framebufferu? */
        /* (Pokud ma jinou hodnotu, tak to znamena, ze pres OUT doslo ke zmene radku, ktery teprve nastane.) */
        if (g_gdg.last_updated_border_pixel == VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1)
        {
            g_gdg.last_updated_border_pixel = 0;
        };

        g_gdg.screen_need_update_from = 0;

        if (g_gdg.beam_row < VIDEO_SCREEN_HEIGHT - 1)
        {
            g_gdg.beam_row++;
        }
        else
        {
            if ((EMULATOR_TEST_NORMAL_SPEED) || (framebudef_get_flag_20ms_passed()))
            {
                framebudef_clear_flag_20ms_passed();

                if ((framebuffer_get_state()) || iface_video_get_redraw_full_screen_request())
                {
                    framebuffer_screen_done();
                    g_gdg.screen_is_already_rendered_at_beam_pos = g_mzarch_main.event.ticks; // TODO: je tohle vubec nutne?
                };
            };

            mz800_main_event_callback_screen_done();

            gdg_on_screen_done_event();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            /* V1.5 fáze 2.2: HWE BP_EVENT_CURSOR edge-based fire.
             * Cursor blink state = (cursor_timer / 25) & 1, cursor_timer
             * inkrementován v gdg_on_screen_done_event makru per snímek.
             * Fire po každém screen_done = zaručené zachycení edge nezávisle
             * na CPU PortC pollingu. Edge detection samotná je v
             * breakpoints_enforce_hw_event přes g_bp_event_state. */
            if ( g_bp_event_active[ BP_EVENT_CURSOR ] ) {
                bp_event_fire ( BP_EVENT_CURSOR,
                                (int32_t) ( mz800_main_get_cursor_timer_state ( ) & 1 ) );
            }

            /* V1.5 fáze 3.3: I/O Ports panel sliding window advance
             * (= rotace ringu hits_per_frame[60]). Gated default OFF
             * uvnitr io_activity_advance_frame(). */
            io_activity_advance_frame ( );
#endif
        };
        break;

        /* tyto eventy neni potreba zde resit */
    case MZEVENT_NO_GDG:
    case MZEVENT_BREAK:
    case MZEVENT_BREAK_MZARCH_INTERRUPT:
    case MZEVENT_BREAK_EMULATION_PAUSED:
    case MZEVENT_PIOZ80:
    case MZEVENT_CTC0:

    case MZEVENT_CUSTOM_SPEED_SYNCHRONISATION:
        break;
    };

    gdg_event_set_next();
}
#endif // GDG_EVENT_C