/**
 * @file   videorec_platform.h
 * @brief  Parametry platformy pro video záznam (rozměry, canvas, takty, fps, zvukové kanály) a jejich kontrola.
 *
 * Lepidlo video záznamu (videorec.c) je jedno pro všechny platformy
 * (MZ-700 PAL/NTSC, MZ-800, MZ-1500); vše, čím se platformy liší, je
 * v jedné hodnotové struktuře st_VIDEOREC_PLATFORM. Hodnoty pro právě
 * překládanou platformu skládá makro VIDEOREC_PLATFORM_CURRENT_INIT
 * z per-arch maker (videorec_platform_arch.h); tento modul je na MZARCH
 * nezávislý, takže ho jde testovat samostatně pro všechny platformy
 * (tests/videorec/test_videorec_platform.c).
 *
 * @par Vlákna
 * Struktura je neměnná hodnota; funkce jsou čisté (bez globálního stavu)
 * a smí se volat z libovolného vlákna.
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_PLATFORM_H
#define VIDEOREC_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parametry platformy, na které se nahrává.
 *
 * Invarianty platné platformy (ověřuje videorec_platform_check()):
 * - `fps_num > 0`, `fps_den > 0`;
 * - `ticks_per_frame * fps_num == clk_hz * fps_den` (snímek trvá přesně
 *   celý počet taktů, časová osa zvuku tak na snímky navazuje bez driftu);
 * - canvas leží celý uvnitř framebufferu;
 * - `audio_channels == 1 + 4 * psg_count` (CTC0 + 4 kanály na každý PSG).
 *
 * Řetězce `name` a `tv_system` jsou statické literály (struktura je nevlastní).
 */
typedef struct st_VIDEOREC_PLATFORM {
    const char *name;         /**< Platforma: "mz700", "mz800" nebo "mz1500" (MZARCH_NAME). */
    const char *tv_system;    /**< TV norma: "pal" nebo "ntsc" (MZTVSYS). */
    unsigned fb_width;        /**< Šířka framebufferu [px] (VIDEO_DISPLAY_WIDTH, včetně borderu). */
    unsigned fb_height;       /**< Výška framebufferu [px] (VIDEO_DISPLAY_HEIGHT, včetně borderu). */
    unsigned canvas_x;        /**< Levý okraj canvasu ve framebufferu [px] (VIDEO_BORDER_LEFT_WIDTH). */
    unsigned canvas_y;        /**< Horní okraj canvasu ve framebufferu [px] (VIDEO_BORDER_TOP_HEIGHT). */
    unsigned canvas_w;        /**< Šířka canvasu [px] (VIDEO_CANVAS_WIDTH). */
    unsigned canvas_h;        /**< Výška canvasu [px] (VIDEO_CANVAS_HEIGHT). */
    uint64_t ticks_per_frame; /**< GDG takty na snímek (VIDEO_SCREEN_TICKS). */
    uint64_t clk_hz;          /**< Frekvence GDG taktů časových značek [Hz] (GDGCLK_BASE). */
    unsigned fps_num;         /**< Čitatel snímkové frekvence (VIDEO_SCREENS_PER_SEC). */
    unsigned fps_den;         /**< Jmenovatel snímkové frekvence (1). */
    unsigned audio_channels;  /**< Počet zdrojových zvukových kanálů (AUDIO_SRC_CHANNELS_COUNT). */
    unsigned psg_count;       /**< Max. počet PSG na platformě (HAVE_PSG: 0, 1 nebo 2). */
} st_VIDEOREC_PLATFORM;

/**
 * @brief Ověří invarianty platformy a celočíselnost vzorků zvuku na snímek.
 *
 * Kromě invariantů st_VIDEOREC_PLATFORM kontroluje, že
 * `audio_rate * fps_den` je dělitelné `fps_num` (např. 48000/60 = 800,
 * 44100/60 = 735, 48000/50 = 960): každý snímek AVI pak nese stejný celý
 * počet vzorků a zvuk se proti obrazu neposouvá.
 *
 * @param p          Platforma (nesmí být NULL).
 * @param audio_rate Vzorkovací frekvence záznamu [Hz]; 0 = kontrolovat jen platformu.
 * @param err        Buffer na anglický popis první nalezené chyby, nebo NULL.
 * @param err_size   Velikost bufferu `err` (0 = nic nezapisovat).
 * @return true = platforma (a frekvence) jsou použitelné; false = chyba
 *         (text v `err`, je-li zadaný).
 * @note Nemá vedlejší efekty kromě zápisu do `err`.
 */
bool videorec_platform_check(const st_VIDEOREC_PLATFORM *p, unsigned audio_rate, char *err, size_t err_size);

/**
 * @brief Počet zvukových vzorků na snímek `audio_rate * fps_den / fps_num`.
 * @param p          Platforma (nesmí být NULL, `fps_num > 0`).
 * @param audio_rate Vzorkovací frekvence [Hz].
 * @return Vzorků na snímek (celočíselné dělení; celé je jen u frekvence,
 *         kterou přijme videorec_platform_check()).
 */
unsigned videorec_platform_samples_per_frame(const st_VIDEOREC_PLATFORM *p, unsigned audio_rate);

#ifdef __cplusplus
}
#endif

#endif /* VIDEOREC_PLATFORM_H */
