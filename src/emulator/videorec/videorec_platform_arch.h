/**
 * @file   videorec_platform_arch.h
 * @brief  Parametry video záznamu pro právě překládanou platformu (z per-arch maker).
 *
 * Jediné místo, kde se per-arch makra (MZARCH_NAME, MZTVSYS, VIDEO_*,
 * GDGCLK_BASE, HAVE_PSG) převádějí na hodnotu st_VIDEOREC_PLATFORM.
 * Hlavičku includuje lepidlo (videorec.c) a testovací sondy
 * (tests/videorec/videorec_platform_probe.c), které ji překládají pro
 * každou platformu zvlášť.
 *
 * @pre Překlad s definovanými MZARCH, MZARCH_NAME, MZTVSYS, MZTVSYS_PAL
 *      a MZTVSYS_NTSC (cmake/AddMzEmu.cmake, cmake/AddMzTest.cmake).
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_PLATFORM_ARCH_H
#define VIDEOREC_PLATFORM_ARCH_H

#include "videorec_platform.h"

#include "mzarch/mzarch_config.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/gdg/gdgclk.h"

#if !defined(MZTVSYS) || !defined(MZTVSYS_PAL) || !defined(MZTVSYS_NTSC) || !defined(MZARCH_NAME)
#error "videorec_platform_arch.h needs MZARCH_NAME, MZTVSYS, MZTVSYS_PAL and MZTVSYS_NTSC"
#endif

/** @brief Název TV normy platformy pro sidecar ("pal" / "ntsc"). */
#if MZTVSYS == MZTVSYS_NTSC
#define VIDEOREC_PLATFORM_TV_NAME "ntsc"
#else
#define VIDEOREC_PLATFORM_TV_NAME "pal"
#endif

/** @brief Počet zdrojových zvukových kanálů platformy (= AUDIO_SRC_CHANNELS_COUNT z audio.h). */
#define VIDEOREC_PLATFORM_AUDIO_CHANNELS (1 + 4 * HAVE_PSG)

/**
 * @brief Inicializátor st_VIDEOREC_PLATFORM pro právě překládanou platformu.
 *
 * Konstantní výraz (lze použít pro `static const` proměnnou). Canvas je
 * obdélník mezi bordery v nativních souřadnicích framebufferu
 * (VIDEO_BORDER_LEFT_WIDTH, VIDEO_BORDER_TOP_HEIGHT, VIDEO_CANVAS_*).
 */
#define VIDEOREC_PLATFORM_CURRENT_INIT                                                                   \
    {                                                                                                    \
        MZARCH_NAME, VIDEOREC_PLATFORM_TV_NAME, VIDEO_DISPLAY_WIDTH, VIDEO_DISPLAY_HEIGHT,               \
            VIDEO_BORDER_LEFT_WIDTH, VIDEO_BORDER_TOP_HEIGHT, VIDEO_CANVAS_WIDTH, VIDEO_CANVAS_HEIGHT,   \
            (uint64_t)VIDEO_SCREEN_TICKS, (uint64_t)GDGCLK_BASE, VIDEO_SCREENS_PER_SEC, 1,               \
            VIDEOREC_PLATFORM_AUDIO_CHANNELS, HAVE_PSG                                                   \
    }

#endif /* VIDEOREC_PLATFORM_ARCH_H */
