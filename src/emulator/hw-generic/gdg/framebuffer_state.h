/**
 * @file framebuffer_state.h
 * @brief Sdílený superset layout framebufferu GDG (compile-once TU).
 *
 * Struktura gdg_framebuffer_t je napříč architekturami identická, liší
 * se jen velikost pixel bufferu (display plocha). Superset dimenzuje
 * buffer na největší architekturu, aby byl layout binárně shodný pro
 * knihovnu mz_emucore i per-EXE TU:
 *
 *   - MZ-800:          928 x 288 = 267264 px (mz800_video.h)
 *   - MZ-700/MZ-1500:  704 x 232 = 163328 px (mz700_video_*.h,
 *                      mz1500_video.h)
 *
 * Per-arch hlavičky mz*_framebuffer.h tento header includují, přidávají
 * per-arch fill rutiny (hot, per-EXE) a compile-time assertem hlídají,
 * že jejich display plocha do supersetu vejde. Skutečné rozměry pro
 * runtime čtení: g_mzhal.video_display_width/height.
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

#ifndef FRAMEBUFFER_STATE_H
#define FRAMEBUFFER_STATE_H
#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Stav změn obrazu v aktuálním / předchozím snímku.
 */
typedef enum en_SCRSTS
{
    SCRSTS_IS_NOT_CHANGED = 0,    /**< Beze změn. */
    SCRSTS_PREVIOUS_IS_CHANGED,   /**< Změny proběhly v předchozím snímku. */
    SCRSTS_THIS_IS_CHANGED,       /**< Změny probíhají v tomto snímku. */
} en_SCRSTS;

/**
 * @brief Bitové příznaky "framebuffer obsahuje nový obsah k zobrazení".
 */
typedef enum en_FBSTATE
{
    FB_STATE_NOT_CHANGED = 0,             /**< Není co překreslovat. */
    FB_STATE_SCREEN_CHANGED = (1 << 0),   /**< Změna v canvas oblasti. */
    FB_STATE_BORDER_CHANGED = (1 << 1),   /**< Změna borderu. */
} en_FBSTATE;

/** @brief Počet pixel bufferů (identické u všech architektur). */
#define GDG_FRAMEBUFFER_PIXBUF_COUNT 3

/**
 * @brief Superset velikosti pixel bufferu = display plocha MZ-800
 * (928 x 288); MZ-700/MZ-1500 (704 x 232) využívají jen část.
 * Skutečná velikost pro runtime: g_mzhal.video_display_width * height.
 */
#define GDG_FRAMEBUFFER_PIXBUF_SIZE_MAX (928 * 288)

/**
 * @brief Framebuffer GDG - pixel buffery a příznaky změn.
 *
 * Invarianty: pixels ukazuje vždy do pixbuff[pixels_id]; pixels_id je
 * v rozsahu 0..GDG_FRAMEBUFFER_PIXBUF_COUNT-1. Plnění provádí emulační
 * vlákno (per-arch fill rutiny), čtení pro zobrazení iface_video.
 */
typedef struct gdg_framebuffer_t
{
    uint8_t pixbuff[GDG_FRAMEBUFFER_PIXBUF_COUNT][GDG_FRAMEBUFFER_PIXBUF_SIZE_MAX];
    uint8_t *pixels;          /**< Aktivní buffer (== pixbuff[pixels_id]). */
    int pixels_id;            /**< Index aktivního bufferu. */

    en_FBSTATE framebuffer_state; /**< Nenulová hodnota, pokud je potřeba zobrazit nový obsah framebufferu. */
    en_SCRSTS screen_changes; /**< Nenulová hodnota, pokud v tomto nebo předchozím snímku proběhly změny canvasu. */
    en_SCRSTS border_changes; /**< Nenulová hodnota, pokud v tomto nebo předchozím snímku proběhly změny borderu. */

    bool flag_20ms_passed;    /**< Pokud právě neběžíme v Normal speed, tento flag zajistí, že iface_video provede aktualizaci snímku. */
} gdg_framebuffer_t;

extern gdg_framebuffer_t g_framebuffer;

/** @brief Inicializace framebufferu (nulování, pixels -> pixbuff[0]). */
extern void framebuffer_init(void);

/* Fill rutiny obrazu - jména nesou zobrazovací režim (MZ-800 mód /
 * MZ-700 mód), ne architekturu; deklarace jsou u všech architektur
 * identické, implementace je per-arch v mz*_framebuffer.c (hot core,
 * per-EXE). */

/** @brief Dokreslí aktuální řádek canvasu do pozice last_pixel. */
extern void framebuffer_MZ800_current_screen_row_fill(unsigned last_pixel);
/** @brief Vyplní celou canvas oblast. */
extern void framebuffer_MZ800_all_screen_rows_fill(void);
/** @brief Aktualizuje aktuální řádek v MZ-700 textovém režimu. */
extern void framebuffer_update_MZ700_current_screen_row(void);
/** @brief Aktualizuje všechny řádky v MZ-700 textovém režimu. */
extern void framebuffer_update_MZ700_all_rows(void);
/** @brief Ohlásí změnu obsahu canvasu (nastaví příznaky změn). */
extern void framebuffer_MZ800_screen_changed(void);

/** @brief Vyplní border v aktuálním řádku paprsku. */
extern void framebuffer_border_current_row_fill(void);
/** @brief Vyplní border v celé display ploše. */
extern void framebuffer_border_all_rows_fill(void);
/** @brief Ohlásí změnu barvy borderu (nastaví příznaky změn). */
extern void framebuffer_border_changed(void);

/** @brief Vynutí kompletní překreslení obrazu (screen + border). */
extern void framebuffer_flush_full_screen(void);

#define framebudef_set_flag_20ms_passed() (g_framebuffer.flag_20ms_passed = true)
#define framebudef_clear_flag_20ms_passed() (g_framebuffer.flag_20ms_passed = false)
#define framebudef_get_flag_20ms_passed() (g_framebuffer.flag_20ms_passed)

#define framebuffer_get_state() (g_framebuffer.framebuffer_state)
#define framebuffer_get_screen_changes() (g_framebuffer.screen_changes)
#define framebuffer_get_border_changes() (g_framebuffer.border_changes)

#ifdef __cplusplus
}
#endif

#endif /* FRAMEBUFFER_STATE_H */
