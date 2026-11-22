/**
 * @file gdg_raster_snapshot.h
 * @brief Side-effect free read API pro raster pozici GDG - arch-independent
 *        (gdg-panel F6).
 *
 * Raster pozice (scanline, pixel clock, VBLN/HBLN flagy, VBLN counter, tempo,
 * CTC0 divider) je společný pojem pro MZ-700, MZ-800 i MZ-1500. Per-arch
 * mirror moduly (`mz{800,700,1500}_gdg_mirror.{h,c}`) drží své vlastní
 * snapshot struktury pro arch-specific obsah panelu (DMD bity, palette, PCG
 * atd.), ale raster fieldy z `g_gdg` jsou ve všech 3 architekturách shodné
 * jak významem, tak jménem fieldu - lze je proto sdílet přes jednu společnou
 * snapshot strukturu a jeden symbol.
 *
 * Header definuje POD strukturu `st_GDG_RASTER_SNAPSHOT` a jeden symbol
 * `gdg_mirror_raster_snapshot()`. Implementace symbolu sídlí v per-arch
 * mirror souboru (`mz{800,700,1500}_gdg_mirror.c`) a do překladu vstupuje
 * podle `MZARCH` (linker proto vidí právě jednu definici).
 *
 * Side-effect free invariant (identický s `*_gdg_mirror.h`):
 *  - Funkce POUZE čte fieldy `g_gdg`. Žádný state mutation, žádné scheduling.
 *
 * Význam fieldů:
 *  - `beam_row`     - aktuální řádek paprsku v rámci snímku (`g_gdg.beam_row`,
 *                    rozsah 0..VIDEO_SCREEN_HEIGHT-1).
 *  - `screen_ticks` - pixel clock pozice v aktuálním snímku
 *                    (`g_gdg.total_elapsed.ticks`, 0..VIDEO_SCREEN_TICKS-1;
 *                    při screen done event se odečte VIDEO_SCREEN_TICKS, takže
 *                    relativně k aktuálnímu screenu).
 *  - `screens_done` - monotonic counter snímků od reset/load
 *                    (`g_gdg.total_elapsed.screens`).
 *  - `hbln`         - HBLN signál (0 = mimo horizontální blanking, 1 = on).
 *  - `vbln`         - VBLN signál (0 = mimo vertikální blanking, 1 = on).
 *  - `sts_hsync`    - HSYNC bit viditelný v GDG Status registru.
 *  - `sts_vsync`    - VSYNC bit viditelný v GDG Status registru.
 *  - `tempo`        - tempo signál (1 Hz bit). Live výstup low-bit pro Status.
 *  - `tempo_divider`- živý čítač pro generování tempo signálu
 *                    (`g_gdg.tempo_divider`; MZ-800 0..227, po 228 reset na 0
 *                    (po načtení snapshotu starší verze 229),
 *                    MZ-700 a MZ-1500 0..228, po 229 reset na 0).
 *                    POZN: jiný pojem než `ctc0_divider` níže.
 *  - `ctc0_divider` - compile-time konstanta `GDGCLK_CTC0_DIVIDER` (MZ-800=16,
 *                    MZ-1500=16, MZ-700 NTSC=13, MZ-700 PAL=16). Udává kolik
 *                    master GDGCLK ticků = 1 CTC0 tick. Všechny 3 archy mají
 *                    konstantu definovanou v `*_gdgclk.h`.
 *
 * License: GPLv3.
 */

#ifndef GDG_RASTER_SNAPSHOT_H
#define GDG_RASTER_SNAPSHOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Snapshot raster pozice GDG, arch-independent.
 *
 * POD struktura, kopie by value. Žádné pointery na živá data, konzument může
 * držet kopii libovolně dlouho. Hodnoty jsou snapshot v okamžiku volání
 * `gdg_mirror_raster_snapshot()`; mezi voláními může emulační vlákno
 * `g_gdg` libovolně mutovat - konzistence napříč voláními NENÍ garantována.
 */
typedef struct st_GDG_RASTER_SNAPSHOT
{
    /* ---- Raster pozice ------------------------------------------------- */
    uint32_t beam_row;        /**< Aktuální scanline (0..VIDEO_SCREEN_HEIGHT-1). */
    uint32_t screen_ticks;    /**< Pixel clock v aktuálním screenu
                               *   (0..VIDEO_SCREEN_TICKS-1). */
    uint64_t screens_done;    /**< Monotonic VBLN counter (snímky od resetu). */

    /* ---- Blanking + sync signály ------------------------------------- */
    uint8_t  hbln;            /**< HBLN flag (0/1). */
    uint8_t  vbln;            /**< VBLN flag (0/1). */
    uint8_t  sts_hsync;       /**< HSYNC v GDG Status registru (0/1). */
    uint8_t  sts_vsync;       /**< VSYNC v GDG Status registru (0/1). */

    /* ---- Tempo + divider --------------------------------------------- */
    uint8_t  tempo;           /**< Tempo signál (1 Hz bit, 0/1). */
    uint32_t tempo_divider;   /**< Živý čítač pro tempo (MZ-800 0..227, jinak 0..228). */
    uint32_t ctc0_divider;    /**< Compile-time GDGCLK_CTC0_DIVIDER pro
                               *   aktuální arch (MZ-800=16, MZ-1500=16,
                               *   MZ-700 NTSC=13, MZ-700 PAL=16). */

    /* ---- Délka řádku --------------------------------------------------- */
    uint16_t line_ticks;      /**< Délka aktuálního řádku v pxCLK:
                               *   VIDEO_SCREEN_WIDTH, na MZ-800 o 16 méně,
                               *   když řádek zkrátil bit CKSW (platí od
                               *   načtení bitu ve sloupci 790 do konce
                               *   řádku). Stejný význam jako pole
                               *   line_ticks příkazu get_periph_gdg. */
} st_GDG_RASTER_SNAPSHOT;

/**
 * @brief Vyplní snapshot strukturu aktuálním stavem raster fieldů `g_gdg`.
 *
 * Per-arch implementace sídlí v `mz{800,700,1500}_gdg_mirror.c` - do překladu
 * vstupuje právě jedna podle MZARCH. Side-effect free: čte pouze fieldy
 * `g_gdg` a compile-time konstantu `GDGCLK_CTC0_DIVIDER` z arch konfigurace.
 *
 * @param out  Pointer na cílovou strukturu (nesmí být NULL).
 *
 * @pre  `out != NULL`. Emulátor inicializován (`gdg_init()` proběhl).
 * @post `*out` obsahuje aktuální snapshot.
 */
void gdg_mirror_raster_snapshot(st_GDG_RASTER_SNAPSHOT *out);

#ifdef __cplusplus
}
#endif

#endif /* GDG_RASTER_SNAPSHOT_H */
