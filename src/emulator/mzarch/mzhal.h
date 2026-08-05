/**
 * @file mzhal.h
 * @brief HW layer specifikace platformy - struktura g_mzhal.
 *
 * Jediné veřejné rozhraní runtime popisu emulované platformy. Struktura
 * koncentruje platformně závislé hodnoty (identita, hodinové domény,
 * video geometrie, capability, sběrnicové timingy), které dnes žijí
 * jako per-arch #define konstanty. Cílový stav (mzhal experiment):
 * sdílený (compile-once) kód čte VÝHRADNĚ g_mzhal a per-arch hlavičky
 * includuje už jen per-EXE kompilovaná množina.
 *
 * KONTRAKT:
 *  - g_mzhal je `const` s compile-time designated inicializátorem
 *    v per-EXE kompilovaném mzhal.c (plněno z per-arch maker, nikdy
 *    opisem literálů). Platí od load-time: žádné init-ordering okno,
 *    žádný zápis za běhu, žádné synchronizační požadavky. Kterékoliv
 *    vlákno smí kdykoliv číst.
 *  - Write-once vynucuje překladač (const v .rodata). Požadavek na
 *    mutable pole = re-adjudikace návrhu (viz STUDIE.md experimentu),
 *    ne cast-away-const.
 *  - Do g_mzhal NEPATŘÍ runtime-mutable HW konfigurace (DIP přepínače:
 *    switch700, allow_psg1, PSG stereo, CMT polarita, ...). Test:
 *    "zapisuje to snapshot load nebo UI menu?" -> pak to sem nepatří.
 *  - Zákaz vzoru `if (g_mzhal.X)` v per-instrukční hot smyčce - hot
 *    core (mzarch.c + mz*_gdg_event.c) zůstává per-EXE s compile-time
 *    konstantami. Ve smyčkách mimo hot core čti pole do loop-invariant
 *    lokálu PŘED smyčkou.
 *
 * Tato hlavička NESMÍ includovat žádné per-arch hlavičky (musí být
 * bezpečná pro budoucí compile-once TU bez -DMZARCH).
 *
 * Pole jsou generovaná X-macrem MZHAL_FIELDS - přidání pole bez
 * doplnění inicializační hodnoty MZHAL_INIT_<pole> v mzhal.c je
 * compile error ve všech 4 EXE (žádná tichá nula).
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

#ifndef MZHAL_H
#define MZHAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief X-macro seznam polí struktury st_MZHAL.
 *
 * Význam polí (X-macro Doxygen členů negeneruje, dokumentace zde):
 *
 * IDENTITA (osa platformy je VŽDY dvojice arch + tvsys!):
 *  - arch              700 / 800 / 1500 (= MZARCH)
 *  - tvsys             50 = PAL / 60 = NTSC (= MZTVSYS)
 *  - arch_name         "mz700" / "mz800" / "mz1500" (= MZARCH_NAME;
 *                      PAL/NTSC NErozlišuje - oba mz700 targety "mz700";
 *                      klíč gettext domény, prefix uživatelských souborů)
 *  - tvsys_name        "PAL" / "NTSC" (= MZTVSYS_NAME)
 *  - full_name         "MZ-700 (PAL)" / "MZ-800" / ... (UI zobrazení)
 *  - arch_display_name "MZ-700" / "MZ-800" / "MZ-1500" (trace manifesty)
 *
 * HODINY (GDG pixel clock doména):
 *  - gdgclk_base         simulovaná frekvence GDG [Hz]; invariant
 *                        gdgclk_base == screen_ticks * screens_per_sec
 *  - gdgclk_real_base    fyzický krystal [Hz] - NIKDY nezaměňovat
 *                        s gdgclk_base (jen výpočet native_fps)
 *  - gdgclk2cpu_divider  GDG taktů na 1 CPU T-stav (4 nebo 5)
 *  - gdgclk_ctc0_divider GDG taktů na 1 takt CTC0 (13 nebo 16)
 *  - cpu_hz              odvozené: gdgclk_base / gdgclk2cpu_divider
 *  - ctc0_input_hz       odvozené: gdgclk_base / gdgclk_ctc0_divider
 *
 * VIDEO GEOMETRIE (v GDG taktech = pixelech; odvozené beam meze se
 * doplní až s migrací konzumentů):
 *  - video_border_{left,right}_width, video_border_{top,bottom}_height
 *  - video_canvas_{width,height}     aktivní plocha (640x200 ekviv.)
 *  - video_display_{width,height}    border + canvas (viditelný obraz)
 *  - video_screen_{width,height}     celý raster včetně sync/porch
 *  - video_screen_ticks              screen_width * screen_height
 *  - video_screens_per_sec           50 / 60 (== tvsys)
 *  - video_h_{sync,back_porch,front_porch}_ticks   horizontální
 *                 sync/porch složky rasteru (screen_width = sync +
 *                 back porch + display_width + front porch)
 *  - psg_divider  GDG taktů na 1 takt PSG (= 16 * gdgclk2cpu_divider;
 *                 na platformě bez PSG bez významu, hodnota přesto
 *                 vyplněná ze stejného vzorce)
 *
 * CAPABILITY (co platforma HW má; capability NENÍ runtime connect stav):
 *  - psg_count    max počet PSG SN76489 (0 = MZ-700, 2 = MZ-800/1500)
 *  - audio_src_channels  počet zdrojových audio kanálů (1 + 4*psg_count;
 *                 pole dimenzována compile-time AUDIO_SRC_CHANNELS_MAX)
 *  - audio_ctc0_gate_pc00  beeper (CTC0 OUT) hradlován signálem PC00
 *                 z 8255 (MZ-800/MZ-1500 ano, MZ-700 ne)
 *  - has_key_tab   klávesa TAB v HW matici (col 0, bit 3) - jen MZ-800;
 *                 MZ-700/MZ-1500 ji nemají
 *  - has_rear_dip_switch700   zadní DIP přepínač MZ-700 modu (MZ-800/
 *                 MZ-1500 ano, MZ-700 nativní nemá) - mzhal 11c-2b
 *  - have_pioz80  Z80 PIO přítomno (0/1)
 *  - have_fdc, have_ide8, have_ramdisk, have_qdisk   volitelné HWEXT
 *  - mem_vram_size, mem_exvram_size, mem_pcg_size   skutečné velikosti
 *                 pamětí platformy v bajtech (superset st_MEMORY má MAX
 *                 dimenze, mzhal 9d); 0 = paměť na platformě neexistuje.
 *                 Zároveň zmrazené velikosti bin bloků snapshotu
 *                 (vram.bin, exvram.bin, pcg.bin).
 *  (joystick topologie záměrně chybí - per-arch IORQ/MEMOP kontrakt se
 *  vyřeší v capability vlně, ne boolean)
 *
 * SBĚRNICOVÉ TIMINGY (délky pulzů v GDG taktech; u deličky 4 platforem
 * [neověřeno] - převzato z MZ-800, viz mz*_bus_timing.h):
 *  - iorq_rd_ticks, iorq_wr_ticks, mreq_rd_m1_ticks, mreq_rd_ticks,
 *    mreq_wr_ticks
 */
#define MZHAL_FIELDS(X) \
    X(int,          arch) \
    X(int,          tvsys) \
    X(const char *, arch_name) \
    X(const char *, tvsys_name) \
    X(const char *, full_name) \
    X(const char *, arch_display_name) \
    X(uint32_t,     gdgclk_base) \
    X(uint32_t,     gdgclk_real_base) \
    X(unsigned,     gdgclk2cpu_divider) \
    X(unsigned,     gdgclk_ctc0_divider) \
    X(uint32_t,     cpu_hz) \
    X(uint32_t,     ctc0_input_hz) \
    X(unsigned,     video_border_left_width) \
    X(unsigned,     video_border_right_width) \
    X(unsigned,     video_border_top_height) \
    X(unsigned,     video_border_bottom_height) \
    X(unsigned,     video_canvas_width) \
    X(unsigned,     video_canvas_height) \
    X(unsigned,     video_display_width) \
    X(unsigned,     video_display_height) \
    X(unsigned,     video_screen_width) \
    X(unsigned,     video_screen_height) \
    X(uint32_t,     video_screen_ticks) \
    X(unsigned,     video_screens_per_sec) \
    X(unsigned,     video_h_sync_ticks) \
    X(unsigned,     video_h_back_porch_ticks) \
    X(unsigned,     video_h_front_porch_ticks) \
    X(unsigned,     psg_divider) \
    X(unsigned,     psg_count) \
    X(unsigned,     have_pioz80) \
    X(unsigned,     have_fdc) \
    X(unsigned,     have_ide8) \
    X(unsigned,     have_ramdisk) \
    X(unsigned,     have_qdisk) \
    X(unsigned,     audio_src_channels) \
    X(unsigned,     audio_ctc0_gate_pc00) \
    X(unsigned,     has_key_tab) \
    X(unsigned,     has_rear_dip_switch700) \
    X(uint32_t,     mem_vram_size) \
    X(uint32_t,     mem_exvram_size) \
    X(uint32_t,     mem_pcg_size) \
    X(unsigned,     iorq_rd_ticks) \
    X(unsigned,     iorq_wr_ticks) \
    X(unsigned,     mreq_rd_m1_ticks) \
    X(unsigned,     mreq_rd_ticks) \
    X(unsigned,     mreq_wr_ticks)

    /**
     * @brief HW layer specifikace platformy.
     *
     * Členy generuje MZHAL_FIELDS - význam a invarianty viz dokumentace
     * X-macra výše. Invarianty hodnot vynucují _Static_assert v mzhal.c
     * (kompile-time) a mzhal_validate() (runtime cross-check).
     */
    typedef struct st_MZHAL
    {
#define MZHAL_FIELD_DECL(type, name) type name;
        MZHAL_FIELDS(MZHAL_FIELD_DECL)
#undef MZHAL_FIELD_DECL
    } st_MZHAL;

    /**
     * @brief HW layer aktuální platformy - jediná instance.
     *
     * Const objekt v .rodata, definovaný v per-EXE mzhal.c. Platný od
     * load-time, čtení je thread-safe bez synchronizace.
     */
    extern const st_MZHAL g_mzhal;

    /**
     * @brief Runtime konzistenční kontrola g_mzhal.
     *
     * Cross-check proti druhému zdroji platformní identity
     * (g_mzarch_platform_* globály z mzarch_platform.c) - chytá
     * rozjetí obou zdrojů při budoucích refaktorech. Kompile-time
     * invarianty hodnot řeší _Static_assert v mzhal.c.
     *
     * Volat jako první věc v main() / main_pipe() / mztest_init().
     *
     * Chování při chybě: vypíše popis na stderr a volá abort()
     * (fail-fast - emulátor s nekonzistentní HW specifikací nesmí
     * pokračovat). Bez chyby žádné vedlejší efekty.
     */
    void mzhal_validate(void);

    /* ------------------------------------------------------------------
     * Beam geometrie (souřadnice paprsku v rámci display plochy).
     *
     * Derivace jsou napříč architekturami identické (ověřeno proti
     * mz800_video.h / mz700_video_pal.h / mz700_video_ntsc.h /
     * mz1500_video.h), liší se jen vstupní rozměry z g_mzhal.
     * ------------------------------------------------------------------ */

    /** @brief První sloupec canvas oblasti (za levým borderem). */
    static inline unsigned mzhal_beam_canvas_first_column(void)
    {
        return g_mzhal.video_border_left_width;
    }

    /** @brief První řádek canvas oblasti (za horním borderem). */
    static inline unsigned mzhal_beam_canvas_first_row(void)
    {
        return g_mzhal.video_border_top_height;
    }

    /** @brief Poslední sloupec canvas oblasti. */
    static inline unsigned mzhal_beam_canvas_last_column(void)
    {
        return g_mzhal.video_border_left_width + g_mzhal.video_canvas_width - 1u;
    }

    /** @brief Poslední řádek canvas oblasti. */
    static inline unsigned mzhal_beam_canvas_last_row(void)
    {
        return g_mzhal.video_border_top_height + g_mzhal.video_canvas_height - 1u;
    }

    /** @brief Poslední sloupec viditelné display plochy. */
    static inline unsigned mzhal_beam_display_last_column(void)
    {
        return g_mzhal.video_display_width - 1u;
    }

    /** @brief Poslední řádek viditelné display plochy. */
    static inline unsigned mzhal_beam_display_last_row(void)
    {
        return g_mzhal.video_display_height - 1u;
    }

#ifdef __cplusplus
}
#endif

#endif /* MZHAL_H */
