/**
 * @file   videorec_config.c
 * @brief  INI modul `[VIDEOREC]` - registrace nastavení video záznamu do cfgmain.
 *
 * Vzor snapshot_config.c: každý prvek má propagate callback (INI ->
 * g_videorec_settings) a save callback (g_videorec_settings -> INI). Neplatné
 * hodnoty se při propagaci nahradí výchozími (audio_rate, default_transition).
 *
 * @par Licence: GPLv3
 */

#include "videorec_config.h"
#include "videorec.h"
#include "videorec_sidecar.h"
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

#include <glib.h>
#include <string.h>

/* ========================================================================= */
/*                       Propagate callbacky                                 */
/* ========================================================================= */

/** @brief INI -> output_dir. @param e Prvek (st_CFGELEMENT*). @param data Nepoužito. */
static void propagatecfg_output_dir(void *e, void *data)
{
    (void)data;
    char *dir = cfgelement_get_text_value((st_CFGELEMENT *)e);
    g_strlcpy(g_videorec_settings.output_dir, dir ? dir : "", sizeof(g_videorec_settings.output_dir));
}

/** @brief INI -> audio_rate (jen 44100 nebo 48000, jinak 48000). @param e Prvek. @param data Nepoužito. */
static void propagatecfg_audio_rate(void *e, void *data)
{
    (void)data;
    unsigned rate = cfgelement_get_unsigned_value((st_CFGELEMENT *)e);
    g_videorec_settings.audio_rate = (rate == 44100) ? 44100 : 48000;
}

/** @brief INI -> retake_mode. @param e Prvek. @param data Nepoužito. */
static void propagatecfg_retake_mode(void *e, void *data)
{
    (void)data;
    g_videorec_settings.retake_mode = (en_VIDEOREC_RETAKE)cfgelement_get_unsigned_value((st_CFGELEMENT *)e);
}

/** @brief INI -> default_transition ("none" a neznámé jméno -> fade). @param e Prvek. @param data Nepoužito. */
static void propagatecfg_default_transition(void *e, void *data)
{
    (void)data;
    en_VIDEOREC_TRANSITION t = VIDEOREC_TRANS_FADE;
    char *name = cfgelement_get_text_value((st_CFGELEMENT *)e);
    if (!name || !videorec_transition_from_name(name, &t) || t == VIDEOREC_TRANS_NONE) t = VIDEOREC_TRANS_FADE;
    g_videorec_settings.default_transition = t;
}

/** @brief INI -> transition_ms. @param e Prvek. @param data Nepoužito. */
static void propagatecfg_transition_ms(void *e, void *data)
{
    (void)data;
    g_videorec_settings.transition_ms = cfgelement_get_unsigned_value((st_CFGELEMENT *)e);
}

/** @brief INI -> keyframe_interval. @param e Prvek. @param data Nepoužito. */
static void propagatecfg_keyframe_interval(void *e, void *data)
{
    (void)data;
    g_videorec_settings.keyframe_interval = cfgelement_get_unsigned_value((st_CFGELEMENT *)e);
}

/** @brief INI -> timebase (neznámé jméno -> emulated). @param e Prvek. @param data Nepoužito. */
static void propagatecfg_timebase(void *e, void *data)
{
    (void)data;
    en_VIDEOREC_TIMEBASE v = VIDEOREC_TIMEBASE_EMULATED;
    char *name = cfgelement_get_text_value((st_CFGELEMENT *)e);
    if (!name || !videorec_timebase_from_name(name, &v)) v = VIDEOREC_TIMEBASE_EMULATED;
    g_videorec_settings.timebase = v;
}

/** @brief INI -> realtime_pause (neznámé jméno -> skip). @param e Prvek. @param data Nepoužito. */
static void propagatecfg_realtime_pause(void *e, void *data)
{
    (void)data;
    en_VIDEOREC_RT_PAUSE v = VIDEOREC_RT_PAUSE_SKIP;
    char *name = cfgelement_get_text_value((st_CFGELEMENT *)e);
    if (!name || !videorec_rt_pause_from_name(name, &v)) v = VIDEOREC_RT_PAUSE_SKIP;
    g_videorec_settings.realtime_pause = v;
}

/** @brief INI -> realtime_pause_cap_s. @param e Prvek. @param data Nepoužito. */
static void propagatecfg_realtime_pause_cap_s(void *e, void *data)
{
    (void)data;
    g_videorec_settings.realtime_pause_cap_s = cfgelement_get_unsigned_value((st_CFGELEMENT *)e);
}

/** @brief INI -> realtime_speed (neznámé jméno -> as_seen). @param e Prvek. @param data Nepoužito. */
static void propagatecfg_realtime_speed(void *e, void *data)
{
    (void)data;
    en_VIDEOREC_RT_SPEED v = VIDEOREC_RT_SPEED_AS_SEEN;
    char *name = cfgelement_get_text_value((st_CFGELEMENT *)e);
    if (!name || !videorec_rt_speed_from_name(name, &v)) v = VIDEOREC_RT_SPEED_AS_SEEN;
    g_videorec_settings.realtime_speed = v;
}

/** @brief INI -> realtime_turbo_audio (neznámé jméno -> as_heard). @param e Prvek. @param data Nepoužito. */
static void propagatecfg_realtime_turbo_audio(void *e, void *data)
{
    (void)data;
    en_VIDEOREC_TURBO_AUDIO v = VIDEOREC_TURBO_AUDIO_AS_HEARD;
    char *name = cfgelement_get_text_value((st_CFGELEMENT *)e);
    if (!name || !videorec_turbo_audio_from_name(name, &v)) v = VIDEOREC_TURBO_AUDIO_AS_HEARD;
    g_videorec_settings.realtime_turbo_audio = v;
}

/** @brief INI -> state_marks (neznámé jméno -> sidecar). @param e Prvek. @param data Nepoužito. */
static void propagatecfg_state_marks(void *e, void *data)
{
    (void)data;
    en_VIDEOREC_STATE_MARKS v = VIDEOREC_STATE_MARKS_SIDECAR;
    char *name = cfgelement_get_text_value((st_CFGELEMENT *)e);
    if (!name || !videorec_state_marks_from_name(name, &v)) v = VIDEOREC_STATE_MARKS_SIDECAR;
    g_videorec_settings.state_marks = v;
}

/** @brief INI -> auto_markers. @param e Prvek. @param data Nepoužito. */
static void propagatecfg_auto_markers(void *e, void *data)
{
    (void)data;
    g_videorec_settings.auto_markers = cfgelement_get_bool_value((st_CFGELEMENT *)e) != 0;
}

/** @brief INI -> record_debugger_steps. @param e Prvek. @param data Nepoužito. */
static void propagatecfg_record_debugger_steps(void *e, void *data)
{
    (void)data;
    g_videorec_settings.record_debugger_steps = cfgelement_get_bool_value((st_CFGELEMENT *)e) != 0;
}

/* ========================================================================= */
/*                         Save callbacky                                    */
/* ========================================================================= */

/** @brief output_dir -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_output_dir(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e, g_videorec_settings.output_dir);
}

/** @brief audio_rate -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_audio_rate(void *e, void *data)
{
    (void)data;
    cfgelement_set_unsigned_value((st_CFGELEMENT *)e, g_videorec_settings.audio_rate);
}

/** @brief retake_mode -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_retake_mode(void *e, void *data)
{
    (void)data;
    cfgelement_set_unsigned_value((st_CFGELEMENT *)e, (unsigned)g_videorec_settings.retake_mode);
}

/** @brief default_transition -> INI (jméno přechodu). @param e Prvek. @param data Nepoužito. */
static void savecfg_default_transition(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e, videorec_transition_name(g_videorec_settings.default_transition));
}

/** @brief transition_ms -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_transition_ms(void *e, void *data)
{
    (void)data;
    cfgelement_set_unsigned_value((st_CFGELEMENT *)e, g_videorec_settings.transition_ms);
}

/** @brief keyframe_interval -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_keyframe_interval(void *e, void *data)
{
    (void)data;
    cfgelement_set_unsigned_value((st_CFGELEMENT *)e, g_videorec_settings.keyframe_interval);
}

/** @brief timebase -> INI (jméno). @param e Prvek. @param data Nepoužito. */
static void savecfg_timebase(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e, videorec_timebase_name(g_videorec_settings.timebase));
}

/** @brief realtime_pause -> INI (jméno). @param e Prvek. @param data Nepoužito. */
static void savecfg_realtime_pause(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e, videorec_rt_pause_name(g_videorec_settings.realtime_pause));
}

/** @brief realtime_pause_cap_s -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_realtime_pause_cap_s(void *e, void *data)
{
    (void)data;
    cfgelement_set_unsigned_value((st_CFGELEMENT *)e, g_videorec_settings.realtime_pause_cap_s);
}

/** @brief realtime_speed -> INI (jméno). @param e Prvek. @param data Nepoužito. */
static void savecfg_realtime_speed(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e, videorec_rt_speed_name(g_videorec_settings.realtime_speed));
}

/** @brief realtime_turbo_audio -> INI (jméno). @param e Prvek. @param data Nepoužito. */
static void savecfg_realtime_turbo_audio(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e, videorec_turbo_audio_name(g_videorec_settings.realtime_turbo_audio));
}

/** @brief state_marks -> INI (jméno). @param e Prvek. @param data Nepoužito. */
static void savecfg_state_marks(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e, videorec_state_marks_name(g_videorec_settings.state_marks));
}

/** @brief auto_markers -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_auto_markers(void *e, void *data)
{
    (void)data;
    cfgelement_set_bool_value((st_CFGELEMENT *)e, g_videorec_settings.auto_markers ? 1 : 0);
}

/** @brief record_debugger_steps -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_record_debugger_steps(void *e, void *data)
{
    (void)data;
    cfgelement_set_bool_value((st_CFGELEMENT *)e, g_videorec_settings.record_debugger_steps ? 1 : 0);
}

/* ========================================================================= */
/*                              Init                                         */
/* ========================================================================= */

void videorec_config_init(void)
{
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "VIDEOREC");
    CFGELM *elm;

    elm = cfgmodule_register_new_element(cmod, "output_dir", CFGENTYPE_TEXT, "");
    cfgelement_set_propagate_cb(elm, propagatecfg_output_dir, NULL);
    cfgelement_set_save_cb(elm, savecfg_output_dir, NULL);

    elm = cfgmodule_register_new_element(cmod, "audio_rate", CFGENTYPE_UNSIGNED, 48000, 44100, 48000);
    cfgelement_set_propagate_cb(elm, propagatecfg_audio_rate, NULL);
    cfgelement_set_save_cb(elm, savecfg_audio_rate, NULL);

    elm = cfgmodule_register_new_element(cmod, "retake_mode", CFGENTYPE_UNSIGNED, VIDEOREC_RETAKE_DISCARD,
                                         VIDEOREC_RETAKE_OFF, VIDEOREC_RETAKE_SEAM);
    cfgelement_set_propagate_cb(elm, propagatecfg_retake_mode, NULL);
    cfgelement_set_save_cb(elm, savecfg_retake_mode, NULL);

    elm = cfgmodule_register_new_element(cmod, "default_transition", CFGENTYPE_TEXT, "fade");
    cfgelement_set_propagate_cb(elm, propagatecfg_default_transition, NULL);
    cfgelement_set_save_cb(elm, savecfg_default_transition, NULL);

    elm = cfgmodule_register_new_element(cmod, "transition_ms", CFGENTYPE_UNSIGNED, 500, 0, 5000);
    cfgelement_set_propagate_cb(elm, propagatecfg_transition_ms, NULL);
    cfgelement_set_save_cb(elm, savecfg_transition_ms, NULL);

    elm = cfgmodule_register_new_element(cmod, "keyframe_interval", CFGENTYPE_UNSIGNED, 250, 1, 3000);
    cfgelement_set_propagate_cb(elm, propagatecfg_keyframe_interval, NULL);
    cfgelement_set_save_cb(elm, savecfg_keyframe_interval, NULL);

    /* režim podle reality (tabulka V1.3 v PLAN-V1.md) */
    elm = cfgmodule_register_new_element(cmod, "timebase", CFGENTYPE_TEXT, "emulated");
    cfgelement_set_propagate_cb(elm, propagatecfg_timebase, NULL);
    cfgelement_set_save_cb(elm, savecfg_timebase, NULL);

    elm = cfgmodule_register_new_element(cmod, "realtime_pause", CFGENTYPE_TEXT, "skip");
    cfgelement_set_propagate_cb(elm, propagatecfg_realtime_pause, NULL);
    cfgelement_set_save_cb(elm, savecfg_realtime_pause, NULL);

    elm = cfgmodule_register_new_element(cmod, "realtime_pause_cap_s", CFGENTYPE_UNSIGNED, VIDEOREC_RT_PAUSE_CAP_DEFAULT_S,
                                         VIDEOREC_RT_PAUSE_CAP_MIN_S, VIDEOREC_RT_PAUSE_CAP_MAX_S);
    cfgelement_set_propagate_cb(elm, propagatecfg_realtime_pause_cap_s, NULL);
    cfgelement_set_save_cb(elm, savecfg_realtime_pause_cap_s, NULL);

    elm = cfgmodule_register_new_element(cmod, "realtime_speed", CFGENTYPE_TEXT, "as_seen");
    cfgelement_set_propagate_cb(elm, propagatecfg_realtime_speed, NULL);
    cfgelement_set_save_cb(elm, savecfg_realtime_speed, NULL);

    elm = cfgmodule_register_new_element(cmod, "realtime_turbo_audio", CFGENTYPE_TEXT, "as_heard");
    cfgelement_set_propagate_cb(elm, propagatecfg_realtime_turbo_audio, NULL);
    cfgelement_set_save_cb(elm, savecfg_realtime_turbo_audio, NULL);

    elm = cfgmodule_register_new_element(cmod, "state_marks", CFGENTYPE_TEXT, "sidecar");
    cfgelement_set_propagate_cb(elm, propagatecfg_state_marks, NULL);
    cfgelement_set_save_cb(elm, savecfg_state_marks, NULL);

    elm = cfgmodule_register_new_element(cmod, "auto_markers", CFGENTYPE_BOOL, 1);
    cfgelement_set_propagate_cb(elm, propagatecfg_auto_markers, NULL);
    cfgelement_set_save_cb(elm, savecfg_auto_markers, NULL);

    elm = cfgmodule_register_new_element(cmod, "record_debugger_steps", CFGENTYPE_BOOL, 0);
    cfgelement_set_propagate_cb(elm, propagatecfg_record_debugger_steps, NULL);
    cfgelement_set_save_cb(elm, savecfg_record_debugger_steps, NULL);

    /* Načíst sekci z INI a propagovat (vzor mcp_config_init): globální
     * cfgroot_propagate se v emulátoru nevolá - bez tohoto by se uložené
     * hodnoty nikdy nenačetly. Chybí-li sekce, propagují se výchozí hodnoty. */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}
