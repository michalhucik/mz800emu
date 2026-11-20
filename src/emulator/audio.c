/*
 * File:   audio.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 28. července 2015, 13:38
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <glib.h>

// Lokalizace
#include "i18n.h"

#include "audio.h"
#include "hw-generic/psg/psg.h"

#include "cfgmain.h"
#include "emulator.h"
#include "iface/iface_audio.h"
#include "videorec/videorec.h"
#include "audio_capture.h"

st_AUDIO g_audio;

#define MAX_UINT32 0xFFFFFFFFu // Maximální hodnota unsigned int (32bit)
#define DEFAULT_VOLUME (1 * MAX_UINT32)

uint32_t audio_float01_to_uint32(float value)
{
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;
    uint64_t ret = (uint64_t)roundf(value * MAX_UINT32);
    return (uint32_t) (ret < MAX_UINT32) ? ret : MAX_UINT32;
}

float audio_uint32_to_float01(uint32_t value)
{
    return (float)value / (float)MAX_UINT32;
}

static inline st_AUDIO_LOG *audio_log_create(uint64_t first_timestamp, bool stereo)
{
    st_AUDIO_LOG *log = (st_AUDIO_LOG *)malloc(sizeof(st_AUDIO_LOG));
    log->stereo = stereo;
    log->first_timestamp = first_timestamp;
    log->last_timestamp = first_timestamp;
    log->last_psg_timestamp = first_timestamp;
    log->ctc0_ac_mean = 0.0;
    memset(log->out_ac_mean, 0, sizeof(log->out_ac_mean));

    for (int src_id = 0; src_id < AUDIO_SRC_CHANNELS_COUNT; src_id++)
    {
        st_AUDIO_SOURCE_LOG *src = (st_AUDIO_SOURCE_LOG *)malloc(sizeof(st_AUDIO_SOURCE_LOG));
        src->samples = NULL;
        src->samples_count = 0;
        src->last_timestamp = first_timestamp;
        src->last_value = 0;
        log->src[src_id] = src;
    };

    return log;
}

static inline void audio_log_init_src(st_AUDIO_LOG *log, int src_id, AUDIO_BUF_t sample_value)
{
    st_AUDIO_SOURCE_LOG *src = log->src[src_id];
    src->last_value = sample_value;
}

void audio_log_destroy(st_AUDIO_LOG *audio_log)
{
    if (audio_log == NULL)
    {
        return;
    };

    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++)
    {
        if (audio_log->src[i] != NULL)
        {
            if (audio_log->src[i]->samples != NULL)
            {
                free(audio_log->src[i]->samples);
            };
            free(audio_log->src[i]);
            audio_log->src[i] = NULL;
        };
    };

    free(audio_log);
}

void audio_propagatecfg_volume(void *e, void *data)
{
    uint32_t volume = cfgelement_get_unsigned_value((CFGELM *)e);
    float *gain = (float *)data;
    *gain = audio_uint32_to_float01(volume);
}

void audio_savecfg_volume(void *e, void *data)
{
    float *gain = (float *)data;
    uint32_t volume = audio_float01_to_uint32(*gain);
    cfgelement_set_unsigned_value((CFGELM *)e, volume);
}

/** @brief Největší hodnota INI klíče [AUDIO] volume_master_boost: procenta hlavní hlasitosti nad 100 %. */
#define AUDIO_MASTER_BOOST_MAX_PERCENT 100u

G_STATIC_ASSERT(AUDIO_MASTER_GAIN_MAX == 1.0f + (float)AUDIO_MASTER_BOOST_MAX_PERCENT / 100.0f);

/**
 * @brief Část hlavní hlasitosti z INI klíče volume_master (0 až 1) naposledy přečtená z konfigurace.
 *
 * Hlavní hlasitost je v INI rozdělená do dvou klíčů (viz
 * audio_master_gain_to_cfg()); každý má vlastní propagaci a jejich pořadí
 * není zaručené, proto si obě části drží poslední hodnotu a hlasitost se
 * nastaví z jejich součtu. Jen vlákno, které konfiguraci propaguje.
 */
static float s_cfg_master_base = 1.0f;
/** @brief Část hlavní hlasitosti z INI klíče volume_master_boost (0 až 1) naposledy přečtená z konfigurace. */
static float s_cfg_master_boost = 0.0f;

/* Doxygen viz audio.h. */
float audio_master_gain_from_cfg(uint32_t volume_master, uint32_t boost_percent)
{
    if (boost_percent > AUDIO_MASTER_BOOST_MAX_PERCENT)
        boost_percent = AUDIO_MASTER_BOOST_MAX_PERCENT;
    float gain = audio_uint32_to_float01(volume_master) + (float)boost_percent / 100.0f;
    return (gain > AUDIO_MASTER_GAIN_MAX) ? AUDIO_MASTER_GAIN_MAX : gain;
}

/* Doxygen viz audio.h. */
void audio_master_gain_to_cfg(float gain, uint32_t *volume_master, uint32_t *boost_percent)
{
    if (!(gain >= 0.0f))
        gain = 0.0f; /* i NaN */
    if (gain > AUDIO_MASTER_GAIN_MAX)
        gain = AUDIO_MASTER_GAIN_MAX;
    uint32_t boost = 0;
    if (gain > 1.0f)
    {
        boost = (uint32_t)lroundf((gain - 1.0f) * 100.0f);
        if (boost > AUDIO_MASTER_BOOST_MAX_PERCENT)
            boost = AUDIO_MASTER_BOOST_MAX_PERCENT;
    };
    if (volume_master)
        *volume_master = audio_float01_to_uint32(gain);
    if (boost_percent)
        *boost_percent = boost;
}

/**
 * @brief INI -> hlavní hlasitost poslechu, část do 100 % ([AUDIO] volume_master).
 * @param e    Prvek konfigurace (unsigned, stejné měřítko jako hlasitosti kanálů).
 * @param data Nepoužito.
 * @post Hlavní hlasitost je součet obou částí (audio_master_gain_from_cfg()).
 */
static void audio_propagatecfg_master_volume(void *e, void *data)
{
    (void)data;
    s_cfg_master_base = audio_uint32_to_float01(cfgelement_get_unsigned_value((CFGELM *)e));
    iface_audio_set_master_gain(s_cfg_master_base + s_cfg_master_boost);
}

/**
 * @brief Hlavní hlasitost poslechu -> INI, část do 100 %.
 * @param e    Prvek konfigurace (unsigned).
 * @param data Nepoužito.
 */
static void audio_savecfg_master_volume(void *e, void *data)
{
    (void)data;
    uint32_t volume = 0;
    audio_master_gain_to_cfg(iface_audio_get_master_gain(), &volume, NULL);
    cfgelement_set_unsigned_value((CFGELM *)e, volume);
}

/**
 * @brief INI -> hlavní hlasitost poslechu, část nad 100 % ([AUDIO] volume_master_boost).
 * @param e    Prvek konfigurace (unsigned, procenta 0 až AUDIO_MASTER_BOOST_MAX_PERCENT).
 * @param data Nepoužito.
 * @post Hlavní hlasitost je součet obou částí (audio_master_gain_from_cfg()).
 */
static void audio_propagatecfg_master_boost(void *e, void *data)
{
    (void)data;
    uint32_t boost = (uint32_t)cfgelement_get_unsigned_value((CFGELM *)e);
    if (boost > AUDIO_MASTER_BOOST_MAX_PERCENT)
        boost = AUDIO_MASTER_BOOST_MAX_PERCENT;
    s_cfg_master_boost = (float)boost / 100.0f;
    iface_audio_set_master_gain(s_cfg_master_base + s_cfg_master_boost);
}

/**
 * @brief Hlavní hlasitost poslechu -> INI, část nad 100 %.
 * @param e    Prvek konfigurace (unsigned).
 * @param data Nepoužito.
 */
static void audio_savecfg_master_boost(void *e, void *data)
{
    (void)data;
    uint32_t boost = 0;
    audio_master_gain_to_cfg(iface_audio_get_master_gain(), NULL, &boost);
    cfgelement_set_unsigned_value((CFGELM *)e, boost);
}

#if AUDIO_HAVE_OUTPUT_FILTER
/**
 * @brief INI -> volba věrné výstupní propusti ([AUDIO] output_filter).
 * @param e    Prvek konfigurace (bool).
 * @param data Nepoužito.
 */
static void audio_propagatecfg_output_filter(void *e, void *data)
{
    (void)data;
    iface_audio_set_output_filter(cfgelement_get_bool_value((CFGELM *)e) != 0);
}

/**
 * @brief Volba věrné výstupní propusti -> INI.
 * @param e    Prvek konfigurace (bool).
 * @param data Nepoužito.
 */
static void audio_savecfg_output_filter(void *e, void *data)
{
    (void)data;
    cfgelement_set_bool_value((CFGELM *)e, iface_audio_output_filter_enabled() ? 1 : 0);
}
#endif /* AUDIO_HAVE_OUTPUT_FILTER */

void audio_print_volume_info(void)
{
    g_print("\n");
    const char *volume_info = _("Audio volume settings:");
    g_print("%s\n", volume_info);
    GString *str = g_string_new(NULL);
    for (size_t i = 0; i < strlen(volume_info); i++)
    {
        g_string_append_c(str, ' ');
    };
    g_print("%sCTC0  : %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SRC_CTC0] * 100.0f);
#if HAVE_PSG >= 1
    g_print("%sPSG0_0: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_0] * 100.0f);
    g_print("%sPSG0_1: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_1] * 100.0f);
    g_print("%sPSG0_2: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_2] * 100.0f);
    g_print("%sPSG0_3: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_3] * 100.0f);
#endif
#if HAVE_PSG == 2
    /* PSG1: MZ-1500 vzdy stereo, MZ-800 jen kdyz allow_psg1=YES */
    if (g_psg_module.stereo)
    {
        g_print("%sPSG1_0: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_0] * 100.0f);
        g_print("%sPSG1_1: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_1] * 100.0f);
        g_print("%sPSG1_2: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_2] * 100.0f);
        g_print("%sPSG1_3: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_3] * 100.0f);
    }
#endif
    g_print("\n");
    g_string_free(str, TRUE);
}

/** @brief Mez podílu doba / časová konstanta, pod kterou se 1 - exp(-d) nahrazuje d (chyba pod d^2 / 2). */
#define AUDIO_CTC0_AC_LINEAR_LIMIT 1.0e-3

/**
 * @brief Dopočítá střední hodnotu CTC0 (střídavá vazba) do času @p ticks.
 *
 * Mezi hranami je úroveň stálá, střední hodnota se k ní blíží exponenciálně
 * s časovou konstantou AUDIO_CTC0_AC_TAU_MS. Krátké úseky (hrany tónu) se
 * počítají lineárně bez exp().
 *
 * @param ticks Čas (tiky GDG); starší než poslední dopočet se ignoruje.
 * @pre Vlákno emulátoru.
 */
static inline void audio_ctc0_ac_advance(uint64_t ticks)
{
    st_AUDIO_CTC0_AC *ac = &g_audio.ctc0_ac;
    if (ticks <= ac->ticks)
        return;
    double d = (double)(ticks - ac->ticks) * 1000.0 / ((double)AUDIO_CTC0_AC_TAU_MS * (double)GDGCLK_BASE);
#if AUDIO_HAVE_OUTPUT_COUPLING
    /* Druhý stupeň (vazba celého výstupu): vstupem je výstup vazby CTC0,
     * který mezi hranami klesá jako a * exp(-t / tau1). Řešení rovnice
     * out_mean' = (a * exp(-t / tau1) - out_mean) / tau2:
     * out_mean(t) = out_mean(0) * e2 + a * tau1 / (tau1 - tau2) * (e1 - e2). */
    {
        double a = ac->level - ac->mean;
        double d2 = d * (double)AUDIO_CTC0_AC_TAU_MS / (double)AUDIO_OUT_AC_TAU_MS;
        if (d2 < AUDIO_CTC0_AC_LINEAR_LIMIT)
        {
            ac->out_mean += (a - ac->out_mean) * d2;
        }
        else
        {
            double e1 = exp(-d), e2 = exp(-d2);
            double c = (double)AUDIO_CTC0_AC_TAU_MS / ((double)AUDIO_CTC0_AC_TAU_MS - (double)AUDIO_OUT_AC_TAU_MS);
            ac->out_mean = ac->out_mean * e2 + a * c * (e1 - e2);
        };
    }
#endif /* AUDIO_HAVE_OUTPUT_COUPLING */
    double f = (d < AUDIO_CTC0_AC_LINEAR_LIMIT) ? d : (1.0 - exp(-d));
    ac->mean += (ac->level - ac->mean) * f;
    ac->ticks = ticks;
}

/**
 * @brief Změna úrovně kanálu CTC0 pro střídavou vazbu.
 * @param level Nová úroveň: výchylka hodnoty kanálu ve zvukovém logu
 *              (0, 1 nebo AUDIO_CTC0_DIV2_FACTOR).
 * @param ticks Čas změny (tiky GDG).
 * @pre Vlákno emulátoru.
 */
static inline void audio_ctc0_ac_set_level(double level, uint64_t ticks)
{
    audio_ctc0_ac_advance(ticks);
    g_audio.ctc0_ac.level = level;
}

/**
 * @brief Hodnota, kterou má kanál CTC0 nést ve zvukovém logu.
 *
 * - CTC0 v režimu 3 s děličem 2 (ctc0_div2) a PC0 = 1: AUDIO_LEVEL_CTC0_DIV2
 *   bez ohledu na echo kazety (viz audio_ctc0_div2_changed()),
 * - jinak ctc0_output XOR cmt_echo: AUDIO_MAXVAL_PER_CHANNEL nebo 0.
 *
 * @return 0, AUDIO_MAXVAL_PER_CHANNEL nebo AUDIO_LEVEL_CTC0_DIV2.
 */
static inline AUDIO_BUF_t audio_ctc0_channel_value(void)
{
    if (g_audio.ctc0_div2 && g_audio.ctc0_output)
        return AUDIO_LEVEL_CTC0_DIV2;
    return (g_audio.ctc0_output ^ g_audio.cmt_echo) ? AUDIO_MAXVAL_PER_CHANNEL : 0;
}

/**
 * @brief Výchylka hodnoty kanálu CTC0 pro střídavou vazbu.
 * @param value Hodnota z audio_ctc0_channel_value().
 * @return 0, 1 nebo AUDIO_CTC0_DIV2_FACTOR.
 */
static inline double audio_ctc0_value_level(AUDIO_BUF_t value)
{
    if (value == AUDIO_LEVEL_CTC0_DIV2)
        return (double)AUDIO_CTC0_DIV2_FACTOR;
    return value ? 1.0 : 0.0;
}

/**
 * @brief Uvede střídavou vazbu CTC0 do ustáleného stavu v čase @p ticks.
 *
 * Střední hodnota se položí rovna aktuální úrovni (výstup vazby 0), jako by
 * úroveň trvala dlouho. Po startu, resetu časové osy a načtení snapshotu.
 *
 * @param ticks Čas (tiky GDG).
 */
static void audio_ctc0_ac_settle(uint64_t ticks)
{
    g_audio.ctc0_ac.level = audio_ctc0_value_level(audio_ctc0_channel_value());
    g_audio.ctc0_ac.mean = g_audio.ctc0_ac.level;
    g_audio.ctc0_ac.out_mean = 0.0;
    g_audio.ctc0_ac.ticks = ticks;
}

/** @brief Výchylky hodnot zvukového logu (audio_level_amplitude()), spočtené při prvním použití. */
static double s_audio_level_amp[AUDIO_LEVEL_VALUES_COUNT];
/** @brief s_audio_level_amp je spočtené. */
static bool s_audio_level_amp_ready = false;

/* Doxygen viz audio.h. */
double audio_level_amplitude(unsigned value)
{
    if (!s_audio_level_amp_ready)
    {
        s_audio_level_amp[0] = 0.0;
        for (unsigned v = 1; v <= AUDIO_MAXVAL_PER_CHANNEL; v++)
        {
            s_audio_level_amp[v] = pow(10.0, -((double)(AUDIO_MAXVAL_PER_CHANNEL - v) / 10.0));
        };
        for (unsigned v = AUDIO_MAXVAL_PER_CHANNEL + 1; v < AUDIO_LEVEL_CTC0_DIV2; v++)
        {
            s_audio_level_amp[v] = (double)AUDIO_LEVEL_HALF_FACTOR * s_audio_level_amp[v - AUDIO_LEVEL_HALF_BASE];
        };
        s_audio_level_amp[AUDIO_LEVEL_CTC0_DIV2] = (double)AUDIO_CTC0_DIV2_FACTOR;
        s_audio_level_amp_ready = true;
    };
    return (value < AUDIO_LEVEL_VALUES_COUNT) ? s_audio_level_amp[value] : 0.0;
}

#if AUDIO_HAVE_OUTPUT_COUPLING
/**
 * @brief Dopočítá střední výchylku kanálu PSG (vazba výstupu) do času @p ticks.
 * @param src_id Id zdroje (1 až AUDIO_SRC_CHANNELS_COUNT - 1).
 * @param ticks  Čas (tiky GDG); starší než poslední dopočet se ignoruje.
 * @pre Vlákno emulátoru.
 */
static inline void audio_psg_ac_advance(int src_id, uint64_t ticks)
{
    st_AUDIO_PSG_AC *ac = &g_audio.psg_ac[src_id];
    if (ticks <= ac->ticks)
        return;
    double d = (double)(ticks - ac->ticks) * 1000.0 / ((double)AUDIO_OUT_AC_TAU_MS * (double)GDGCLK_BASE);
    double f = (d < AUDIO_CTC0_AC_LINEAR_LIMIT) ? d : (1.0 - exp(-d));
    ac->mean += (ac->amp - ac->mean) * f;
    ac->ticks = ticks;
}
#endif /* AUDIO_HAVE_OUTPUT_COUPLING */

/**
 * @brief Uvede vazbu výstupu u kanálů PSG do ustáleného stavu v čase @p ticks.
 *
 * Střední výchylka každého kanálu se položí rovna výchylce, kterou kanál
 * podle stavu PSG právě má (stálá úroveň pak nedá žádný skok); zvukový log
 * přitom začíná od nuly, proto `amp` = 0 do první změny kanálu. Po startu,
 * resetu časové osy a načtení snapshotu.
 *
 * @param ticks Čas (tiky GDG).
 */
static void audio_psg_ac_settle(uint64_t ticks)
{
    memset(g_audio.psg_ac, 0, sizeof(g_audio.psg_ac));
#if AUDIO_HAVE_OUTPUT_COUPLING
    for (int src_id = 1; src_id < AUDIO_SRC_CHANNELS_COUNT; src_id++)
    {
        const st_PSG *psg = &g_psg_module.psg[(src_id - 1) / PSG_CHANNELS_COUNT];
        const st_PSG_CHANNEL *channel = &psg->channel[(src_id - 1) % PSG_CHANNELS_COUNT];
        unsigned value = 0;
        if (channel->attn != PSG_OUT_OFF)
        {
            unsigned step = AUDIO_MAXVAL_PER_CHANNEL - channel->attn;
            if ((channel->type == PSG_CHTYPE_TONE) && (channel->tone.divider == 1))
                value = AUDIO_LEVEL_HALF_BASE + step;
            else if (channel->output_signal)
                value = step;
        };
        g_audio.psg_ac[src_id].mean = audio_level_amplitude(value);
        g_audio.psg_ac[src_id].ticks = ticks;
    };
#else
    (void)ticks;
#endif /* AUDIO_HAVE_OUTPUT_COUPLING */
}

/* Doxygen viz audio.h. */
void audio_out_ac_means(uint64_t now_ticks, double out[AUDIO_SRC_CHANNELS_COUNT])
{
#if AUDIO_HAVE_OUTPUT_COUPLING
    audio_ctc0_ac_advance(now_ticks);
    out[0] = g_audio.ctc0_ac.out_mean;
    for (int src_id = 1; src_id < AUDIO_SRC_CHANNELS_COUNT; src_id++)
    {
        audio_psg_ac_advance(src_id, now_ticks);
        out[src_id] = g_audio.psg_ac[src_id].mean;
    };
#else
    (void)now_ticks;
    for (int src_id = 0; src_id < AUDIO_SRC_CHANNELS_COUNT; src_id++)
        out[src_id] = 0.0;
#endif /* AUDIO_HAVE_OUTPUT_COUPLING */
}

/* Doxygen viz audio.h. */
double audio_ctc0_ac_mean(uint64_t now_ticks)
{
    audio_ctc0_ac_advance(now_ticks);
    return g_audio.ctc0_ac.mean;
}

void audio_init(void)
{

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "AUDIO");

    CFGELM *elm;

    elm = cfgmodule_register_new_element(cmod, "volume_8253", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SRC_CTC0]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SRC_CTC0]);

#if HAVE_PSG >= 1
    elm = cfgmodule_register_new_element(cmod, "volume_psg0", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_0]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_0]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg1", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_1]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_1]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg2", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_2]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_2]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg3", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_3]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_3]);
#endif /* HAVE_PSG >= 1 */

#if HAVE_PSG == 2
    /* PSG1 volume konfigurace — jen pro stereo PSG (MZ-1500) */
    elm = cfgmodule_register_new_element(cmod, "volume_psg1_0", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_0]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_0]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg1_1", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_1]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_1]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg1_2", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_2]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_2]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg1_3", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_3]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_3]);
#endif /* HAVE_PSG == 2 */

#if AUDIO_HAVE_OUTPUT_FILTER
    /* Věrná výstupní propust (dolní propust 460 us jako na skutečném MZ-800); výchozí zapnuto.
     * Samostatný MZ-700 ji nemá, prvek se tam neregistruje (ani v INI, ani pro MCP). */
    elm = cfgmodule_register_new_element(cmod, "output_filter", CFGENTYPE_BOOL, 1);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_output_filter, NULL);
    cfgelement_set_save_cb(elm, audio_savecfg_output_filter, NULL);
#endif /* AUDIO_HAVE_OUTPUT_FILTER */

    /* Hlavní hlasitost poslechu (násobitel hotového mixu, mimo záznam); výchozí 100 %, rozsah 0 až 200 %.
     * Dva klíče kvůli zpětné kompatibilitě: volume_master (0 až 4294967295 = 0 až 100 %, význam se nemění)
     * a volume_master_boost (procenta nad 100 %, 0 až 100); hlasitost je jejich součet.
     * Ztlumení (iface_audio_set_mute()) se do INI neukládá: emulátor startuje vždy neztlumený. Klíč "mute"
     * ze staršího INI se při čtení tiše přeskočí (cfgmodule_parse() neznámé klíče ignoruje) a při příštím
     * uložení ze souboru zmizí (cfgroot_save() zapisuje jen registrované prvky). */
    elm = cfgmodule_register_new_element(cmod, "volume_master", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_master_volume, NULL);
    cfgelement_set_save_cb(elm, audio_savecfg_master_volume, NULL);

    elm = cfgmodule_register_new_element(cmod, "volume_master_boost", CFGENTYPE_UNSIGNED, 0, 0, AUDIO_MASTER_BOOST_MAX_PERCENT);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_master_boost, NULL);
    cfgelement_set_save_cb(elm, audio_savecfg_master_boost, NULL);

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);

    g_audio.log = audio_log_create(0, g_psg_module.stereo);
    for (int src_id = 0; src_id < AUDIO_SRC_CHANNELS_COUNT; src_id++)
    {
        audio_log_init_src(g_audio.log, src_id, 0);
    };

    g_audio.old_log = NULL;
    APP_MUTEX_CREATE(g_audio.old_log_mutex);

    g_audio.ctc0_output = 0;
    g_audio.ctc0_div2 = 0;
    g_audio.cmt_echo = 0;
    audio_ctc0_ac_settle(0);
    audio_psg_ac_settle(0);
    g_audio.log->ctc0_ac_mean = g_audio.ctc0_ac.mean;
    audio_out_ac_means(0, g_audio.log->out_ac_mean);

    audio_print_volume_info();
}

void audio_reset_log(uint64_t new_timestamp)
{
    /* Zničit starý log a vytvořit čistý s novým timestampem */
    audio_log_destroy(g_audio.log);
    g_audio.log = audio_log_create(new_timestamp, g_psg_module.stereo);
    for (int src_id = 0; src_id < AUDIO_SRC_CHANNELS_COUNT; src_id++) {
        audio_log_init_src(g_audio.log, src_id, 0);
    }
    /* Kanál CTC0 nese hned svou úroveň: na rozdíl od PSG, které log doplní
     * prvním krokem, nemusí další událost CTC0 dlouho přijít a log s nulou
     * by se proti ustálené vazbě lišil o stálou úroveň (skok ve zvuku). */
    audio_log_init_src(g_audio.log, audio_source_id(AUDIO_SRC_CTC0), audio_ctc0_channel_value());
    /* Střídavá vazba CTC0 není ve snapshotu: začíná ustálená na aktuální úrovni. */
    audio_ctc0_ac_settle(new_timestamp);
    audio_psg_ac_settle(new_timestamp);
    g_audio.log->ctc0_ac_mean = g_audio.ctc0_ac.mean;
    audio_out_ac_means(new_timestamp, g_audio.log->out_ac_mean);

    /* Zahodit i old_log — obsahuje zastaralá data */
    APP_MUTEX_LOCK(g_audio.old_log_mutex);
    audio_log_destroy(g_audio.old_log);
    g_audio.old_log = NULL;
    APP_MUTEX_UNLOCK(g_audio.old_log_mutex);
}

uint64_t audio_psg_phase_ticks(uint64_t now_ticks)
{
    if (!g_audio.log || now_ticks < g_audio.log->last_psg_timestamp)
        return 0;
    return now_ticks - g_audio.log->last_psg_timestamp;
}


void audio_exit(void)
{
    audio_log_destroy(g_audio.log);
    g_audio.log = NULL;

    if (g_audio.old_log_mutex)
    {
        APP_MUTEX_LOCK(g_audio.old_log_mutex);
        audio_log_destroy(g_audio.old_log);
        g_audio.old_log = NULL;
        APP_MUTEX_UNLOCK(g_audio.old_log_mutex);

        APP_MUTEX_DESTROY(g_audio.old_log_mutex);
        g_audio.old_log_mutex = NULL;
    };
}

static inline void audio_changed(st_AUDIO_LOG *log, en_AUDIO_SOURCE audio_source, AUDIO_BUF_t value, uint64_t total_event_ticks)
{
    // st_AUDIO_LOG *log = g_audio.log;

    int src_id = audio_source_id(audio_source);

    st_AUDIO_SOURCE_LOG *src = log->src[src_id];

    if (src->last_value == value)
    {
        return;
    };

    st_AUDIO_LOG_EVENT *last_sample = NULL;

    if (src->samples_count == 0)
    {
        src->samples = (st_AUDIO_LOG_EVENT *)malloc(sizeof(st_AUDIO_LOG_EVENT));
        last_sample = &src->samples[0];
        src->samples_count = 1;
        last_sample->value = src->last_value;
    }
    else
    {
        last_sample = &src->samples[src->samples_count - 1];
    };

    last_sample->count_ticks = total_event_ticks - src->last_timestamp;
    src->last_timestamp = total_event_ticks;

    st_AUDIO_LOG_EVENT *new_samples = (st_AUDIO_LOG_EVENT *)realloc(src->samples, (src->samples_count + 1) * sizeof(st_AUDIO_LOG_EVENT));
    if (new_samples == NULL)
    {
        fprintf(stderr, "%s():%d - Failed to reallocate memory for audio log\n", __func__, __LINE__);
        return;
    };
    src->samples = new_samples;
    st_AUDIO_LOG_EVENT *sample = &src->samples[src->samples_count];
    src->samples_count++;

    sample->value = value;
    sample->count_ticks = 0;
    src->last_value = value;

    /* Video záznam a zachycení zvuku: jen živé změny (log == g_audio.log).
     * Události přesouvané v audiolog_finish_20ms_frame() do nového logu už
     * tapnuté byly. */
    if (log == g_audio.log)
    {
#if AUDIO_HAVE_OUTPUT_COUPLING
        /* střídavá vazba výstupu: střední výchylka kanálu PSG v emulovaném čase */
        if (src_id != 0)
        {
            audio_psg_ac_advance(src_id, total_event_ticks);
            g_audio.psg_ac[src_id].amp = audio_level_amplitude((unsigned)value);
        };
#endif /* AUDIO_HAVE_OUTPUT_COUPLING */
        if (videorec_wants_emu_audio())
            videorec_audio_tap((unsigned)src_id, (uint8_t)value, total_event_ticks);
        if (audio_capture_wanted())
            audio_capture_on_change((unsigned)src_id, (uint8_t)value, total_event_ticks);
    };
}

/**
 * @brief Zapíše do kanálu CTC0 zvukového logu hodnotu audio_ctc0_channel_value()
 *        a ohlásí ji střídavé vazbě (audio_ctc0_ac_set_level()).
 *
 * @param total_event_ticks Čas události; starší než poslední událost kanálu
 *        se posune na ni (echo kazety doručuje hrany zpětně).
 * @pre Vlákno emulátoru; g_audio.ctc0_output, g_audio.ctc0_div2
 *      a g_audio.cmt_echo jsou 0/1.
 */
static inline void audio_ctc0_channel_update(uint64_t total_event_ticks)
{
    st_AUDIO_SOURCE_LOG *src = g_audio.log->src[audio_source_id(AUDIO_SRC_CTC0)];
    if (total_event_ticks < src->last_timestamp)
        total_event_ticks = src->last_timestamp;
    AUDIO_BUF_t value = audio_ctc0_channel_value();
    audio_ctc0_ac_set_level(audio_ctc0_value_level(value), total_event_ticks);
    audio_changed(g_audio.log, AUDIO_SRC_CTC0, value, total_event_ticks);
}

/* Doxygen viz audio.h. */
void audio_ctc0_changed(bool ctc0_state, uint64_t total_event_ticks)
{
    g_audio.ctc0_output = ctc0_state;
    g_audio.ctc0_div2 = 0;
    if (!g_audio.cmt_echo)
    {
        /* bez echa beze změny: čas události se neupravuje */
        audio_ctc0_ac_set_level(ctc0_state ? 1.0 : 0.0, total_event_ticks);
        audio_changed(g_audio.log, AUDIO_SRC_CTC0, (ctc0_state) ? AUDIO_MAXVAL_PER_CHANNEL : 0, total_event_ticks);
        return;
    };
    audio_ctc0_channel_update(total_event_ticks);
}

/* Doxygen viz audio.h. */
void audio_ctc0_div2_changed(bool pc0, uint64_t total_event_ticks)
{
    g_audio.ctc0_output = pc0;
    g_audio.ctc0_div2 = 1;
    audio_ctc0_channel_update(total_event_ticks);
}

/* Doxygen viz audio.h. */
void audio_ctc0_restore(bool ctc0_state, bool div2)
{
    g_audio.ctc0_output = ctc0_state;
    g_audio.ctc0_div2 = div2;
}

/* Doxygen viz audio.h. */
void audio_cmt_echo_changed(bool level, uint64_t total_event_ticks)
{
    if (g_audio.cmt_echo == (unsigned)level)
        return;
    g_audio.cmt_echo = level;
    audio_ctc0_channel_update(total_event_ticks);
}

#if HAVE_PSG >= 1
/**
 * @brief Hodnota kanálu PSG pro zvukový log.
 *
 * Vypnutý kanál (útlum 15) dává 0. Tónový kanál s děličem 001h dává stálou
 * poloviční výchylku svého stupně hlasitosti (AUDIO_LEVEL_HALF_BASE + stupeň):
 * čip přepíná výstup každý krok (110,8 kHz), do logu se místo toho píše
 * střední úroveň (změřeno 0,49, viz AUDIO_LEVEL_HALF_FACTOR). Ostatní kanály
 * dávají stupeň hlasitosti při výstupu v 1 a 0 při výstupu v 0.
 *
 * @param channel Kanál PSG po kroku psg_step().
 * @return Hodnota zvukového logu 0 až 30 (hodnotu 31 kanály PSG nepíšou).
 */
static inline AUDIO_BUF_t audio_psg_channel_log_value(const st_PSG_CHANNEL *channel)
{
    if (channel->attn == PSG_OUT_OFF)
        return 0;
    AUDIO_BUF_t step = (AUDIO_BUF_t)(AUDIO_MAXVAL_PER_CHANNEL - channel->attn);
    if ((channel->type == PSG_CHTYPE_TONE) && (channel->tone.divider == 1))
        return (AUDIO_BUF_t)(AUDIO_LEVEL_HALF_BASE + step);
    return channel->output_signal ? step : 0;
}
#endif /* HAVE_PSG >= 1 */

/**
 * @brief Dokrokuje PSG do zadaného času a zapíše úrovně kanálů do zvukového logu.
 *
 * PSG se krokuje po PSG_DIVIDER ticích od last_psg_timestamp logu; po každém
 * kroku se do logu zapíše hodnota každého kanálu (audio_psg_channel_log_value()).
 * Bez PSG (MZ-700) jen posune last_psg_timestamp.
 *
 * @param total_event_ticks Čas, do kterého se má PSG dokrokovat (tiky GDG).
 * @pre Vlákno emulátoru; total_event_ticks >= last_psg_timestamp logu.
 * @post last_psg_timestamp je nejvýš o PSG_DIVIDER - 1 tiků před total_event_ticks.
 */
void audio_log_fill_psg(uint64_t total_event_ticks)
{
#if HAVE_PSG == 0
    /* MZ-700 nema PSG - pouze udrzime last_psg_timestamp v synchronizaci,
     * abychom v audiolog_finish_20ms_frame mohli pouzit log->last_psg_timestamp
     * jako konzistentni casovou znacku zacatku noveho frame logu. */
    g_audio.log->last_psg_timestamp = total_event_ticks;
#else
    st_AUDIO_LOG *log = g_audio.log;

    while ((total_event_ticks - log->last_psg_timestamp) >= PSG_DIVIDER)
    {
        psg_step();   /* Steppuje PSG0 (a PSG1 pokud stereo) */

        for (int channel = 0; channel < PSG_CHANNELS_COUNT; channel++)
        {
            /* PSG0 logování */
            AUDIO_BUF_t scan_value = audio_psg_channel_log_value(&g_psg.channel[channel]);
            audio_changed(g_audio.log, (en_AUDIO_SOURCE)(AUDIO_SOURCE_FLAG_PSG0 | channel), scan_value, log->last_psg_timestamp);

#if HAVE_PSG == 2
            /* PSG1 logování — stereo PSG (MZ-1500 nativne, MZ-800 pres allow_psg1).
             * Runtime check g_psg_module.stereo pro MZ-800 dynamic case. */
            if (g_psg_module.stereo)
            {
                AUDIO_BUF_t scan_value1 = audio_psg_channel_log_value(&g_psg_module.psg[1].channel[channel]);
                audio_changed(g_audio.log, (en_AUDIO_SOURCE)(AUDIO_SOURCE_FLAG_PSG1 | channel), scan_value1, log->last_psg_timestamp);
            }
#endif
        };

        log->last_psg_timestamp += PSG_DIVIDER;
    };
#endif /* HAVE_PSG */
}

void audiolog_finish_20ms_frame(uint64_t total_event_ticks)
{
    // dokoncime PSG
    audio_log_fill_psg(total_event_ticks);

    /* Video záznam: všechny události do last_psg_timestamp jsou doručené
     * (CTC0 živě až do total_event_ticks, PSG po krocích do last_psg_timestamp). */
    if (videorec_wants_emu_audio())
        videorec_audio_horizon(g_audio.log->last_psg_timestamp);
    /* Zachycení zvuku: začátek naplánovaného, render reference.wav, konec. */
    if (audio_capture_wanted())
        audio_capture_on_frame(g_audio.log->last_psg_timestamp);

    // zaevidujeme konec frame
    st_AUDIO_LOG *log = g_audio.log;
    log->last_timestamp = total_event_ticks;

    // uzavreme posledni eventy
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++)
    {
        st_AUDIO_SOURCE_LOG *src = log->src[i];
        if (src->samples_count > 0)
        {
            st_AUDIO_LOG_EVENT *last_sample = &src->samples[src->samples_count - 1];
            last_sample->count_ticks = total_event_ticks - src->last_timestamp;
        };
    };

    // Vytvorime log pro nasledujici frame
    st_AUDIO_LOG *new_log = audio_log_create(log->last_psg_timestamp, g_psg_module.stereo);
    /* Stav střídavé vazby CTC0 na začátku nového logu (hrany do konce snímku
     * už v něm jsou; rozdíl proti last_psg_timestamp je pod jeden krok PSG). */
    new_log->ctc0_ac_mean = audio_ctc0_ac_mean(total_event_ticks);
    audio_out_ac_means(total_event_ticks, new_log->out_ac_mean);

    // Zkopirujeme posledni hodnoty do noveho frame
    int first_src_id = 1; // Preskocime CTC0?
    uint32_t ticks_remaining = total_event_ticks - log->last_psg_timestamp;
    if ((ticks_remaining == 0) || (log->src[0]->samples_count == 0))
    {
        first_src_id = 0; // Nepreskocime CTC0
    }
    else
    {
        // Budeme muset zkopirovat i presahujici eventy z CTC0
        st_AUDIO_SOURCE_LOG *src = log->src[0];
        // Nejprve najdeme prvni event, ktery se nevesel do uzavreneho frame
        unsigned first_event = src->samples_count - 1;
        uint64_t event_timestamp = 0;
        while (first_event > 0)
        {
            st_AUDIO_LOG_EVENT *e = &src->samples[first_event];
            if (e->count_ticks >= ticks_remaining)
            {
                event_timestamp = new_log->first_timestamp + ticks_remaining;
                break;
            };
            ticks_remaining -= e->count_ticks;
            first_event--;
        };

        // Zinicializujeme novy log CTC0 podle posledniho nedokonceneho eventu
        audio_log_init_src(new_log, 0, src->samples[first_event].value);

        // Zkopirujeme posledni eventy CTC0
        first_event++;
        if (first_event < src->samples_count)
        {
            for (unsigned i = first_event; i < src->samples_count; i++)
            {
                st_AUDIO_LOG_EVENT *e = &src->samples[i];
                audio_changed(new_log, AUDIO_SRC_CTC0, e->value, event_timestamp);
                event_timestamp += e->count_ticks;
            };

            // odstranime posledni eventy
            src->samples_count = first_event;
        };
    };

    // Zkopirujeme posledni hodnoty PSG
    for (int i = first_src_id; i < AUDIO_SRC_CHANNELS_COUNT; i++)
    {
        st_AUDIO_SOURCE_LOG *src = log->src[i];
        audio_log_init_src(new_log, i, src->last_value);
    };

    // Swap logu
    if (EMULATOR_TEST_NORMAL_SPEED)
    {
        if (g_audio.old_log != NULL)
        {
            audio_log_destroy(g_audio.old_log);
        };
        g_audio.old_log = log;
    }
    else // MZ800_EMULATION_SPEED_MAX
    {
        APP_MUTEX_LOCK(g_audio.old_log_mutex);
        if (g_audio.old_log != NULL)
        {
            audio_log_destroy(g_audio.old_log);
        };
        g_audio.old_log = log;
        APP_MUTEX_UNLOCK(g_audio.old_log_mutex);
    };
    g_audio.log = new_log;
}
