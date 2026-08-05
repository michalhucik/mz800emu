/*
 * File:   psg.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 23. července 2015, 7:33
 *
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

/* Emulace PSG - SN76489AN */

#include "mzarch/mzcommon_config.h"

#include "psg.h"
#include "hw-generic/gdg/gdg_state.h"
#include "mzarch/mzhal.h"
#include "audio.h"
#include "mzarch/mzarch.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdint.h>

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/hwlog.h"
#include "debugger/bp_event.h"
#endif

st_PSG_MODULE g_psg_module;

/* ========================================================================= */
/* PSG Write Log - autoritativní 1:1 záznam register zápisů.                */
/*                                                                           */
/* Datový tok:                                                               */
/*   psg_write_byte() -> if (enabled) psg_write_log_record()                 */
/*                       -> mutex lock -> fprintf -> mutex unlock            */
/*                                                                           */
/* Thread safety:                                                            */
/*   - `enabled` je gboolean modifikovaný jen pod mutexem; fast-path check  */
/*     v hot pathu (psg_write_byte) je nezamknutý - benigní race: nejhůř   */
/*     se vynechá jeden zápis kolem disable transition, ne crash.           */
/*   - file pointer `fp` je čten/měněn jen pod mutexem.                     */
/*                                                                           */
/* Performance:                                                              */
/*   - mutex lock per write, ale PSG zápisy jsou ~100-1000/s (= hluboko     */
/*     pod CPU instruction rate), režie zanedbatelná.                       */
/*   - fflush per řádek = odolnost proti crash emulátoru (TSV usable i bez  */
/*     graceful exit).                                                       */
/*                                                                           */
/* Lifetime: globální statická instance, mutex inicializován při prvním    */
/* enable přes g_once_init.                                                  */
/* ========================================================================= */

/**
 * @brief Stav PSG write log subsystému.
 *
 * Invarianty:
 *   - `enabled == TRUE`  => `fp != NULL`
 *   - `enabled == FALSE` => `fp == NULL` (po disable cleanup)
 *   - mutex_inited == TRUE je terminální (jednou inicialovaný mutex
 *     se nedealokuje, žije po celou dobu procesu).
 */
typedef struct st_PSG_WRITE_LOG
{
    gboolean enabled;        /**< Atomic-read fast-path flag. */
    FILE    *fp;             /**< Otevřený TSV soubor; NULL = uzavřeno. */
    GMutex   mutex;          /**< Chrání fp + enabled při write/enable/disable. */
    gsize    mutex_inited;   /**< g_once_init flag (= 1 po inicializaci). */
    unsigned row_count;      /**< Počet řádků zapsaných od posledního enable. */
} st_PSG_WRITE_LOG;

static st_PSG_WRITE_LOG g_psg_write_log = { FALSE, NULL, { 0 }, 0, 0u };

/**
 * @brief Lazy init GMutex (idempotent, thread-safe).
 *
 * Používá g_once_init_enter/leave - bezpečné při souběhu UI a emu vlákna
 * při prvním enable.
 */
static inline void psg_write_log_ensure_mutex_init(void)
{
    if (g_once_init_enter(&g_psg_write_log.mutex_inited))
    {
        g_mutex_init(&g_psg_write_log.mutex);
        g_once_init_leave(&g_psg_write_log.mutex_inited, 1);
    }
}

/**
 * @brief Vrací nominální emulátor clock (pxCLK) v Hz.
 *
 * Pro TSV header metadata. Hodnota je arch-specifická (PAL/NTSC, MZ-800/
 * MZ-1500); čte se z runtime gdgclk konfigurace.
 */
static inline uint32_t psg_write_log_pxclk_hz(void)
{
    /* g_mzhal.gdgclk_base = nominální pxCLK kompilovaný per architektura
     * (MZ-800: ~17.73 MHz, MZ-1500/MZ-700-NTSC: ~14.32 MHz). */
    return (uint32_t)g_mzhal.gdgclk_base;
}

bool psg_write_log_enable(const char *path)
{
    if (path == NULL)
        return false;

    psg_write_log_ensure_mutex_init();
    g_mutex_lock(&g_psg_write_log.mutex);

    /* Re-enable: zavři předchozí session (= idempotentní enable). */
    if (g_psg_write_log.fp != NULL)
    {
        fclose(g_psg_write_log.fp);
        g_psg_write_log.fp = NULL;
    }
    g_psg_write_log.enabled = FALSE;
    g_psg_write_log.row_count = 0u;

    g_psg_write_log.fp = g_fopen(path, "w");
    if (g_psg_write_log.fp == NULL)
    {
        g_mutex_unlock(&g_psg_write_log.mutex);
        return false;
    }

    /* TSV header s metadaty (= external tool zná jak interpretovat). */
    fprintf(g_psg_write_log.fp,
            "# PSG write log\n"
            "# emulator_clock_hz=%u\n"
            "# stereo=%d\n"
            "# columns: pxclk_ticks\tchannel_mask\traw_byte_hex\n",
            (unsigned)psg_write_log_pxclk_hz(),
            g_psg_module.stereo ? 1 : 0);
    fflush(g_psg_write_log.fp);

    g_psg_write_log.enabled = TRUE;
    g_mutex_unlock(&g_psg_write_log.mutex);
    return true;
}

void psg_write_log_disable(void)
{
    /* Pokud mutex není inicializovaný, není ani co zavírat. */
    if (g_once_init_enter(&g_psg_write_log.mutex_inited))
    {
        g_mutex_init(&g_psg_write_log.mutex);
        g_once_init_leave(&g_psg_write_log.mutex_inited, 1);
    }

    g_mutex_lock(&g_psg_write_log.mutex);
    g_psg_write_log.enabled = FALSE;
    if (g_psg_write_log.fp != NULL)
    {
        fflush(g_psg_write_log.fp);
        fclose(g_psg_write_log.fp);
        g_psg_write_log.fp = NULL;
    }
    g_mutex_unlock(&g_psg_write_log.mutex);
}

bool psg_write_log_is_enabled(void)
{
    /* Atomic single-byte read; v case souběhu vrátí buď starou nebo novou
     * hodnotu, ale ne corrupted. */
    return g_psg_write_log.enabled ? true : false;
}

unsigned psg_write_log_row_count(void)
{
    return g_psg_write_log.row_count;
}

/**
 * @brief Zapíše jeden write event do TSV logu.
 *
 * Voláno z emu vlákna po každém PSG zápisu, ale jen pokud je `enabled`.
 * Pod mutexem znovu ověří enabled (= TOCTOU obrana při souběžném disable).
 *
 * @param total_ticks  pxCLK timestamp od startu emulace.
 * @param channel      PSG channel mask (PSG_CH_LEFT / PSG_CH_RIGHT / both).
 * @param value        Surový byte tak, jak ho CPU zapsal.
 */
static inline void psg_write_log_record(uint64_t total_ticks,
                                        unsigned channel,
                                        uint8_t value)
{
    g_mutex_lock(&g_psg_write_log.mutex);
    if (g_psg_write_log.enabled && g_psg_write_log.fp != NULL)
    {
        fprintf(g_psg_write_log.fp,
                "%" G_GUINT64_FORMAT "\t%u\t%02X\n",
                (guint64)total_ticks,
                (unsigned)(channel & 0xFFu),
                (unsigned)value);
        fflush(g_psg_write_log.fp);
        g_psg_write_log.row_count++;
    }
    g_mutex_unlock(&g_psg_write_log.mutex);
}

void psg_instance_init(st_PSG *psg)
{
    psg->channel[PSG_CHANNEL_0].type = PSG_CHTYPE_TONE;
    psg->channel[PSG_CHANNEL_1].type = PSG_CHTYPE_TONE;
    psg->channel[PSG_CHANNEL_2].type = PSG_CHTYPE_TONE;

    psg->channel[PSG_CHANNEL_3].type = PSG_CHTYPE_NOISE;
    psg->channel[PSG_CHANNEL_3].noise.shiftregister = 1 << 15;

    for (unsigned i = 0; i < PSG_CHANNELS_COUNT; i++)
    {
        psg->channel[i].attn = PSG_OUT_OFF;
    };
}

void psg_init(bool stereo)
{
    g_psg_module.stereo = stereo;
    psg_instance_init(&g_psg_module.psg[0]);
    if (stereo)
        psg_instance_init(&g_psg_module.psg[1]);
}

/* Zapíše byte do registrů jedné PSG instance */
static inline void psg_write_byte_core(st_PSG *psg, uint8_t value, uint64_t total_ticks)
{
    unsigned latch, attn, cs;

    latch = value & (1 << 7);

    if (latch)
    {
        cs = (value >> 5) & 0x03;
        attn = value & (1 << 4);
        psg->latch_cs = cs;
        psg->latch_attn = attn;
    }
    else
    {
        cs = psg->latch_cs;
        attn = psg->latch_attn;
    };

    st_PSG_CHANNEL *channel = &psg->channel[cs];

    if (attn)
    {
        en_ATTENUATOR new_attn = value & 0x0f;
        if (new_attn != channel->attn)
        {
            audio_log_fill_psg(total_ticks);
            channel->attn = new_attn;
        };
    }
    else if ((latch) && (channel->type == PSG_CHTYPE_TONE))
    {
        channel->tone.latch_divider = value & 0x0f;
    }
    else
    {
        if (channel->type == PSG_CHTYPE_TONE)
        {
            unsigned new_divider = (value << 4) | channel->tone.latch_divider;
            if (new_divider != channel->tone.divider)
            {
                audio_log_fill_psg(total_ticks);
                channel->tone.divider = new_divider;
            };
        }
        else
        {
            en_NOISE_DIV_TYPE new_div_type = value & 0x03;
            en_NOISE_TYPE new_type = (value >> 2) & 1;
            if ((new_div_type != channel->noise.div_type) || (new_type != channel->noise.type))
            {
                audio_log_fill_psg(total_ticks);
                channel->noise.div_type = new_div_type;
                channel->noise.type = new_type;
            };
        };
    };
}

void psg_write_byte(unsigned channel, uint8_t value)
{
    mzarch_main_insideop_iorq_psg_write();
    uint64_t total_ticks = gdg_compute_total_ticks(g_gdg.total_elapsed.ticks);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat write do PSG datového portu.
     *
     * Payload (per HW-log_format_CZ.md):
     *   [0] = raw byte zapsaný uživatelem
     *   [1] = channel mask (mono = 0x01, stereo bitmask PSG_CH_LEFT/RIGHT)
     *   [2] = stereo flag (0 = mono, 1 = stereo)
     *   [3..5] = rezervováno
     *
     * Decoded info (latch_cs, attn flag, čteno tone vs noise vs attn)
     * dopočítá externí parser z initial state + sumace eventů.
     */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        uint8_t payload[ 6 ] = {
            value,
            (uint8_t)( channel & 0xff ),
            (uint8_t)( g_psg_module.stereo ? 1 : 0 ),
            0, 0, 0
        };
        hwlog_record ( HWLOG_CHIP_PSG, HWLOG_PSG_REGISTER_WRITE, payload );
    }
    /* HWE: psg:reg_write vyřazený (= IORQ_W na PSG port je expressivnější).
     * psg:int_pa5 vyřazený - PSG INT je viditelný přes irq:pioz80_b nebo
     * IRQ_SIG. */
#endif

    if (!g_psg_module.stereo)
    {
        /* Mono režim — channel se ignoruje */
        psg_write_byte_core(&g_psg_module.psg[0], value, total_ticks);

        /* Write-log hook (mono path). Channel mask = PSG_CH_LEFT (= 0x01),
         * protože mono PSG zapisuje do psg[0]. Fast-path skip když disabled. */
        if (g_psg_write_log.enabled)
        {
            psg_write_log_record(total_ticks, PSG_CH_LEFT, value);
        }
        return;
    }

    /* Stereo režim — dispatch podle channel bitmask.
     * Obě podmínky jsou nezávislé, aby PSG_CH_LEFT | PSG_CH_RIGHT
     * správně zapsal do obou PSG (broadcast port). */
    if (channel & PSG_CH_LEFT)
    {
        psg_write_byte_core(&g_psg_module.psg[0], value, total_ticks);
    }
    if (channel & PSG_CH_RIGHT)
    {
        psg_write_byte_core(&g_psg_module.psg[1], value, total_ticks);
    }

    /* Write-log hook (stereo path): zaznamená každý zápis přesně jak ho
     * CPU provedl. Channel mask zachycuje cílový PSG (broadcast = 0x03).
     * Fast-path skip když disabled. */
    if (g_psg_write_log.enabled)
    {
        psg_write_log_record(total_ticks, channel, value);
    }
}

void psg_instance_step(st_PSG *psg)
{
    for (unsigned cs = 0; cs < PSG_CHANNELS_COUNT; cs++)
    {
        st_PSG_CHANNEL *channel = &psg->channel[cs];

        if (channel->attn != PSG_OUT_OFF)
        {
            if (channel->type == PSG_CHTYPE_TONE)
            {
                /* tone */
                st_PSG_TONE *tone = &channel->tone;
                if (tone->divider < 2)
                {
                    channel->output_signal = 1;
                }
                else if (0 == channel->timer--)
                {
                    channel->timer = tone->divider - 1;
                    channel->output_signal = (~channel->output_signal) & 0x01;
                };
            }
            else
            {
                /* noise */
                unsigned bit0, bit3;
                st_PSG_NOISE *noise = &channel->noise;
                if ((noise->div_type == 0x03) && (psg->channel[PSG_CHANNEL_2].tone.divider < 2))
                {
                    channel->output_signal = 1;
                }
                else if (0 == channel->timer--)
                {
                    if (noise->div_type == NOISE_DIV_TYPE3)
                    {
                        channel->timer = psg->channel[PSG_CHANNEL_2].tone.divider - 1;
                    }
                    else
                    {
                        channel->timer = (0x10 << noise->div_type) - 1;
                    };

                    bit0 = noise->shiftregister & 0x01;

                    if (noise->last_noise_type != noise->type)
                    {
                        noise->shiftregister = 1 << 15;
                        noise->last_noise_type = noise->type;
                    }
                    else if (noise->type == NOISE_TYPE_WHITE)
                    {
                        bit3 = (noise->shiftregister >> 3) & 0x01;
                        noise->shiftregister = noise->shiftregister >> 1;
                        noise->shiftregister |= (bit0 ^ bit3) << 15;
                    }
                    else
                    {
                        noise->shiftregister = noise->shiftregister >> 1;
                        noise->shiftregister |= bit0 << 15;
                    };
                    channel->output_signal = noise->shiftregister & 0x01;
                };
            };
        };
    };
}

void psg_step(void)
{
    psg_instance_step(&g_psg_module.psg[0]);
    if (g_psg_module.stereo)
        psg_instance_step(&g_psg_module.psg[1]);
}

/* ========================================================================= */
/* Debug/UI mirror API - side-effect free čtení stavu PSG.                   */
/*                                                                           */
/* Žádný z těchto getterů NEMODIFIKUJE st_PSG. Bezpečné pro volání z UI      */
/* vlákna v paralelním běhu emu vlákna (single-byte / single-field snapshot, */
/* x86 atomic).                                                              */
/* ========================================================================= */

unsigned psg_mirror_latch_cs(const st_PSG *psg)
{
    return psg->latch_cs;
}

unsigned psg_mirror_latch_attn(const st_PSG *psg)
{
    return psg->latch_attn;
}

en_PSG_CHTYPE psg_mirror_channel_type(const st_PSG *psg, unsigned ch)
{
    return psg->channel[ch].type;
}

unsigned psg_mirror_channel_attn(const st_PSG *psg, unsigned ch)
{
    return (unsigned)psg->channel[ch].attn;
}

uint16_t psg_mirror_channel_tone_divider(const st_PSG *psg, unsigned ch)
{
    return (uint16_t)(psg->channel[ch].tone.divider & 0x3FFu);
}

en_NOISE_DIV_TYPE psg_mirror_channel_noise_div_type(const st_PSG *psg, unsigned ch)
{
    return psg->channel[ch].noise.div_type;
}

en_NOISE_TYPE psg_mirror_channel_noise_type(const st_PSG *psg, unsigned ch)
{
    return psg->channel[ch].noise.type;
}
