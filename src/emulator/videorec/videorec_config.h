/**
 * @file   videorec_config.h
 * @brief  INI modul `[VIDEOREC]` - perzistence nastavení video záznamu (g_videorec_settings).
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_CONFIG_H
#define VIDEOREC_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zaregistruje INI modul `VIDEOREC` do g_cfgmain.
 *
 * Klíče (propagace do g_videorec_settings, uložení z něj):
 * | Klíč                 | Typ      | Default  | Rozsah / poznámka                              |
 * |----------------------|----------|----------|------------------------------------------------|
 * | `output_dir`         | TEXT     | ""       | "" = `<home_dir>/videos`                       |
 * | `audio_rate`         | UNSIGNED | 48000    | 44100 nebo 48000 (jiná hodnota -> 48000)       |
 * | `retake_mode`        | UNSIGNED | 1        | 0-2 (en_VIDEOREC_RETAKE)                       |
 * | `default_transition` | TEXT     | "fade"   | jméno z videorec_transition_name(); "none" a neznámé -> "fade" |
 * | `transition_ms`      | UNSIGNED | 500      | 0-5000                                         |
 * | `keyframe_interval`  | UNSIGNED | 250      | 1-3000                                         |
 * | `timebase`           | TEXT     | "emulated" | "emulated" / "realtime"; neznámé -> "emulated" |
 * | `realtime_pause`     | TEXT     | "skip"   | "skip" / "freeze" / "freeze_capped"; neznámé -> "skip" |
 * | `realtime_pause_cap_s` | UNSIGNED | 3      | 1-60 [s]                                       |
 * | `realtime_speed`     | TEXT     | "as_seen" | "as_seen" / "emulated_when_fast"; neznámé -> "as_seen" |
 * | `realtime_turbo_audio` | TEXT   | "as_heard" | "as_heard" / "silence" / "attenuate"; neznámé -> "as_heard" |
 * | `state_marks`        | TEXT     | "sidecar" | "none" / "sidecar"; neznámé -> "sidecar"     |
 * | `auto_markers`       | BOOL     | 1        | 0 / 1                                          |
 * | `record_debugger_steps` | BOOL  | 0        | 0 / 1                                          |
 *
 * Význam voleb režimu podle reality viz videorec.h (tabulka V1.3 v plánu
 * PLAN-V1.md). Volby s názvy jsou TEXT (čitelné v INI), převod přes
 * videorec_*_name() / videorec_*_from_name().
 *
 * UNSIGNED hodnoty jsou v INI hexadecimální (cfgfile je zapisuje jako 0x..;
 * čte vždy hex, i bez prefixu). Hodnotu mimo rozsah cfgfile ignoruje
 * (zůstane výchozí).
 *
 * Funkce sekci hned načte z INI souboru g_cfgmain a propaguje ji
 * (cfgmodule_parse + cfgmodule_propagate; globální cfgroot_propagate se
 * v emulátoru nevolá).
 *
 * @pre g_cfgmain existuje (volá cfgmain_init() vedle snapshot_config_init()).
 * @post Modul je zaregistrovaný a g_videorec_settings obsahuje hodnoty z INI
 *       (chybějící sekce nebo klíč = výchozí hodnota); uložení
 *       (cfgroot_save při ukončení) čte z g_videorec_settings.
 * @par Side effects Čte INI soubor g_cfgmain (chybějící soubor není chyba).
 * @par Vlákna Hlavní vlákno při inicializaci konfigurace.
 */
void videorec_config_init(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEOREC_CONFIG_H */
