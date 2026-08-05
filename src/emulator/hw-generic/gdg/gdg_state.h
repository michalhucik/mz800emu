/**
 * @file gdg_state.h
 * @brief Superset layout stavových struktur GDG - jednotný pro všechny
 *        architektury (mzhal dávka 9c-2).
 *
 * Do kroku 9c-2 definovala každá per-arch hlavička (mz700/mz800/mz1500
 * _gdg.h) vlastní st_GDGEVENT / st_GDG_TIMESTAMP / st_GDG. st_GDGEVENT
 * a st_GDG_TIMESTAMP byly textově identické; st_GDG se lišila jen
 * per-arch poli. Tato hlavička definuje SUPERSET: společný (hot) blok
 * má identické pořadí ve všech EXE, per-arch specifická (cold) pole
 * jsou sdružena na konci. Per-arch typedefy byly smazány ve stejném
 * commitu - zapomenutý include je compile error, ne tichá ODR korupce.
 *
 * Invarianty:
 *  - Pole společného bloku čtená v per-instrukční smyčce (event,
 *    total_elapsed, beam_row) jsou na začátku (hot-fields-first);
 *    pořadí hlídají _Static_assert canary níže.
 *  - Layout NEZÁVISÍ na žádné build volbě (pravidlo mzhal kroku 5) -
 *    všechna pole existují vždy, build volby přepínají jen kód.
 *  - Persistence je na layoutu nezávislá: snapshot čte/zapisuje per
 *    XML klíč, mirror struktury kopírují per-field (LAYOUT-INVENTURA.md).
 *  - Per-arch pole smí plnit/číst jen kód příslušné architektury;
 *    na ostatních EXE zůstávají v klidové hodnotě z gdg_init() memset.
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

#ifndef GDG_STATE_H
#define GDG_STATE_H

#include <stdint.h>
#include <stddef.h>

#include "mzarch/mzevent.h"
#include "mzarch/mzhal.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Definice jednoho GDG eventu v rastru (řádkové okno + sloupec).
     *
     * Tabulku g_gdgevent[] plní per-arch event kód; struktura je společná.
     */
    typedef struct st_GDGEVENT
    {
        en_MZEVENT event;
        unsigned start_row;    /* od ktereho radku event volame */
        unsigned num_rows;     /* pocet radku na kterych se volani opakuje */
        unsigned event_column; /* na kterem sloupci se event zavola */
    } st_GDGEVENT;

    /**
     * @brief Časové razítko GDG: počet dokončených obrazovek + pixel
     *        clock pozice v aktuálním (nedokončeném) snímku.
     *
     * ticks je v rozsahu <0; VIDEO_SCREEN_TICKS-1> dané architektury;
     * hodnota 0 odpovídá 1. pixelu viditelného obrazu.
     */
    typedef struct st_GDG_TIMESTAMP
    {
        unsigned screens; /* celkovy pocet vykonanych obrazovek */
        unsigned ticks;   /* pixel clock pozice v aktualnim snimku */
    } st_GDG_TIMESTAMP;

    /**
     * @brief Stav zobrazovacího řadiče - superset všech architektur.
     *
     * Společný blok (hot -> cold), pak per-arch sekce. Významy polí viz
     * komentáře; per-arch pole jsou označena. Invarianty viz hlavička
     * souboru.
     */
    typedef struct st_GDG
    {
        /* --- HOT: čteno per instrukce / per event (canary níže) --- */
        st_EMUEVENT event;

        st_GDG_TIMESTAMP total_elapsed; /* Celkovy pocet vykonanych snimku a pixelu */

        unsigned beam_row;
        unsigned screen_is_already_rendered_at_beam_pos; /* pokud byla pauza a probehnul render obrazovky, tak tady mame posledni pozici paprsku, ktera uz je zobrazena */

        unsigned screen_need_update_from;   /* od ktereho pixelu aktualniho radku je potreba updatovat framebuffer */
        unsigned last_updated_border_pixel; /* posledni aktualizovany border pixel aktualniho radku */

        unsigned sts_vsync; /* STS Vsync, ktery vidime na status registru - neodpovida skutecnemu VS */
        unsigned sts_hsync; /* STS Hsync, ktery vidime na status registru - neodpovida skutecnemu HS */
        unsigned hbln;      /* HBLN: 0 - pokud se sloupec paprsku nachazi mimo screen */
        unsigned vbln;      /* VBLN: 0 - pokud se radek paprsku nachazi mimo screen */

        unsigned regDMD;    /* Display Mode register */
        unsigned regBOR;    /* Border register (HW port jen MZ-800; 700/1500 vzdy 0) */

        /* MZ-800 palety hned u regDMD - horka renderovaci skupina; poradi
         * horkeho prefixu odpovida puvodnimu per-arch layoutu (perf:
         * prvni dve cache lajny drzi kompletni render stav). */
        unsigned regPALGRP; /* MZ-800: Palette Group register */
        unsigned regPAL0;   /* MZ-800: Palette0 register */
        unsigned regPAL1;   /* MZ-800: Palette1 register */
        unsigned regPAL2;   /* MZ-800: Palette2 register */
        unsigned regPAL3;   /* MZ-800: Palette3 register */

        unsigned regct53g7; /* rizeni GATE pro CTC0 v ctc8253 v rezimu MZ700 */

        unsigned tempo;
        unsigned tempo_divider;

        unsigned cksw;      /* MZ-800: CKSW (Superimpose) bit, set zapisem na 0xCF07 bit 7.
                             * Citelny v Status registru bit 2 (0xCE i 0xE008).
                             * Emulator hodnotu udrzuje, ale zmenu HSYN timingu
                             * (CKSW=1 zkracuje horizontalni periodu o 16 pxCLK)
                             * NEemulujeme - viz mz800-knowledge hw/08a-video-timing.md.
                             */

        /* MZ-800: stav pro HDL-presny WAIT model 800 grafickych rezimu
         * (DMD bit 3 = 0, VRAM 0x8000-0xBFFF). Po WRITE do VRAM bezi
         * "horka faze" delky 32 CLK0 (320x200) nebo 17 CLK0 (640x200),
         * behem ktere dalsi VRAM pristup dostane WAIT podle lookup
         * tabulek tw_wr[]/tw_ww[].
         *
         * vram800_hot_phase_end_total_ticks: kumulativni timestamp
         *   (gdg_total_ticks) konce horke faze (= T3f posledniho WRITE
         *   + 32 nebo 17 CLK0). Hodnota 0 znamena "zadny predchozi
         *   WRITE / mimo horkou fazi".
         * vram800_hot_phase_clk0_phase: pozice prvniho WRITE v ramci
         *   radku (mod 80), index do tw_wr[]/tw_ww[]. Pro 640x200 se
         *   pri lookup pricte rotace +16.
         *
         * Reset: gdg_init() / gdg_reset() / snapshot load. Aktualizace:
         * pri kazdem WRITE z mzarch_main_insideop_mreq_mz800_vramctrl_
         * write(). READ stav neaktualizuje (READ negeneruje VRAM
         * strobe). */
        uint64_t vram800_hot_phase_end_total_ticks;
        unsigned vram800_hot_phase_clk0_phase;

        /* --- Per-arch sekce: MZ-700 / MZ-1500 --- */
        int mode_color[8]; /* barevná paleta (port 0xF1); společné jméno
                              700/1500 (mzhal 9c), XML klíče snapshotu
                              zůstávají per-arch */

        /* Redzony (mzhal 9c-3): superset mění out-of-bounds zápis na
         * tichou in-bounds korupci sousedních polí - redzony vrací
         * detekovatelnost. Leží pohromadě NA KONCI struktury (mimo
         * horké cache lajny; perf rozhodnutí krok 15) - chytají přeběh
         * za konec logického stavu, jemné mezisekční hlídání bylo
         * záměrně obětováno lokalitě. Plní gdg_redzone_fill()
         * (gdg_init), kontroluje gdg_redzone_check() (gdg_reset,
         * snapshot load). */
        uint32_t redzone_a[4];
        uint32_t redzone_b[4];
        uint32_t redzone_c[4];

    } st_GDG;

/** @brief Poison vzor redzone polí st_GDG (viz redzone_a komentář). */
#define GDG_REDZONE_PATTERN 0x5AC0DE5Au

    /**
     * @brief Naplní redzone pole poison vzorem.
     *
     * Volat jednou z gdg_init(); bez vedlejších efektů na ostatní pole.
     *
     * @param g Stav GDG (typicky &g_gdg), nesmí být NULL.
     */
    static inline void gdg_redzone_fill(st_GDG *g)
    {
        for (int i = 0; i < 4; i++) {
            g->redzone_a[i] = GDG_REDZONE_PATTERN;
            g->redzone_b[i] = GDG_REDZONE_PATTERN;
            g->redzone_c[i] = GDG_REDZONE_PATTERN;
        }
    }

    /* Zarovnani na cache lajnu: horky prefix (event, total_elapsed,
     * beam stav, regDMD, palety) zacina na hranici 64 B. */
    extern st_GDG g_gdg __attribute__((aligned(64)));

    /** @brief Tabulka GDG eventů rastru (per-arch data, společný typ). */
    extern const struct st_GDGEVENT g_gdgevent[];

/* Sdílené signálové predikáty (mzhal 11c-2b: přesunuto z per-arch
 * mz*_gdg.h, kde byly tři identické kopie) - čtou superset pole g_gdg. */
#define SIGNAL_GDG_HBLNK (g_gdg.hbln)
#define SIGNAL_GDG_VBLNK (g_gdg.vbln)
#define SIGNAL_GDG_STS_HS (g_gdg.sts_hsync)
#define SIGNAL_GDG_STS_VS (g_gdg.sts_vsync)
#define SIGNAL_GDG_TEMPO (g_gdg.tempo & 1)

#define HBLN_ACTIVE 0
#define HBLN_OFF 1
#define VBLN_ACTIVE 0
#define VBLN_OFF 1
#define HSYN_ACTIVE 0
#define HSYN_OFF 1
#define VSYN_ACTIVE 0
#define VSYN_OFF 1

#define GDG_TEST_VBLN (g_gdg.vbln == 0)

    /**
     * @brief Studená inicializace GDG (poison redzone, výchozí stav).
     * Implementace per-arch (mz*_gdg.c), prototyp společný.
     */
    extern void gdg_init(void);
    /** @brief Teplá inicializace GDG (reset registrů, kontrola redzone). */
    extern void gdg_reset(void);
    /** @brief Čtení DMD/status přes MEMOP (0xE008); per-arch chování. */
    extern uint8_t gdg_read_dmd_status_memop(void);
    /** @brief Čtení DMD/status přes IORQ; per-arch chování. */
    extern uint8_t gdg_read_dmd_status_ioop(void);
    /** @brief Zápis do GDG registrů (DMD, paleta, ...); per-arch dekodér. */
    extern void gdg_write_byte(unsigned addr, uint8_t value);

    /**
     * @brief Celkový počet GDG ticků od startu pro danou pozici v snímku.
     *
     * Rodina ticks (mzhal 10d): dříve tři identické kopie maker v per-arch
     * gdg.h s compile-time VIDEO_SCREEN_TICKS; nyní runtime
     * z g_mzhal.video_screen_ticks (const .rodata). Perf dopad hlídá A/B
     * brána 10f - při měřitelném zpomalení fallback = per-EXE
     * specializace v mzarch.c.
     *
     * @param now_ticks Pixel clock pozice v aktuálním snímku (0 = jen
     *                  součet dokončených snímků).
     * @return Kumulativní počet ticků od startu emulace.
     */
    static inline uint64_t gdg_compute_total_ticks(uint64_t now_ticks)
    {
        return now_ticks
             + ((uint64_t)g_gdg.total_elapsed.screens * g_mzhal.video_screen_ticks);
    }

    /**
     * @brief Celkový počet GDG ticků od startu k aktuální pozici paprsku.
     * @return gdg_compute_total_ticks(g_gdg.total_elapsed.ticks).
     */
    static inline uint64_t gdg_get_total_ticks(void)
    {
        return gdg_compute_total_ticks(g_gdg.total_elapsed.ticks);
    }

    /**
     * @brief Kumulativní tick nejbližšího příštího CLK1M1 (CTC0) eventu.
     *
     * Modulo běží nad runtime deličkou g_mzhal.gdgclk_ctc0_divider
     * (16/16/13/16 per platforma - 13 není mocnina dvou, mask trik zde
     * nelze). Konzumenti jsou warm (per CTC event, ctc8253.c).
     *
     * @param now_ticks Pixel clock pozice v aktuálním snímku.
     * @return Kumulativní tick příští hrany CLK1M1.
     */
    static inline uint64_t gdg_proximate_clk1m1_event(uint64_t now_ticks)
    {
        const uint32_t d = g_mzhal.gdgclk_ctc0_divider;
        return now_ticks + (d - (gdg_compute_total_ticks(now_ticks) % d));
    }

    /**
     * @brief Ověří neporušenost redzone polí.
     *
     * @param g Stav GDG (typicky &g_gdg), nesmí být NULL.
     * @return 1 pokud jsou všechna redzone pole netknutá, 0 při korupci
     *         (= někdo přepsal paměť mezi sekcemi st_GDG; volající
     *         zaloguje a v debug buildu asertuje).
     */
    static inline int gdg_redzone_check(const st_GDG *g)
    {
        for (int i = 0; i < 4; i++) {
            if ((g->redzone_a[i] != GDG_REDZONE_PATTERN) ||
                (g->redzone_b[i] != GDG_REDZONE_PATTERN) ||
                (g->redzone_c[i] != GDG_REDZONE_PATTERN)) {
                return 0;
            }
        }
        return 1;
    }

    /* Canary: hot pole musí zůstat na začátku a per-arch cold sekce až
     * za společným blokem. Při reorderu spadne kompilace všech 4 EXE.
     * (static_assert pro C++ TU, _Static_assert pro C TU.) */
#ifdef __cplusplus
#define GDG_STATE_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define GDG_STATE_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
    GDG_STATE_ASSERT(offsetof(st_GDG, event) == 0,
                     "st_GDG: event musi byt prvni (hot)");
    GDG_STATE_ASSERT(offsetof(st_GDG, total_elapsed) == sizeof(st_EMUEVENT),
                     "st_GDG: total_elapsed musi nasledovat hned za event (hot)");
    GDG_STATE_ASSERT(offsetof(st_GDG, regPALGRP) == offsetof(st_GDG, regBOR) + sizeof(unsigned),
                     "st_GDG: palety musi sousedit s regDMD/regBOR (hot render)");
    GDG_STATE_ASSERT(offsetof(st_GDG, regPAL3) < 2 * 64,
                     "st_GDG: palety musi lezet v prvnich dvou cache lajnach");
    GDG_STATE_ASSERT(offsetof(st_GDG, redzone_a) > offsetof(st_GDG, mode_color),
                     "st_GDG: redzony patri na konec struktury (perf)");
#undef GDG_STATE_ASSERT

#ifdef __cplusplus
}
#endif

#endif /* GDG_STATE_H */
