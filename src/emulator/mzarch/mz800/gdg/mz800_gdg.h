/*
 * File:   mz800_gdg.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 18:38
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

#ifndef MZ800_GDG_H
#define MZ800_GDG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "mzarch/mzarch_config.h"

#include "hw-generic/gdg/video.h"
#include "mzarch/mzevent.h"
#include "mzarch/mzarch.h"
#include "hw-generic/gdg/gdgclk.h"


// Tohle jsem meril jenom na MZ-800, nicmene ty rozdilz meyi platformami budou asi marginalni, proto jsem to pouzil globalne pro vsechny MZ-architektury.
#define IORQ_RD_TICKS 12   /* Delka IORQ RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define IORQ_WR_TICKS 9    /* ??? TODO: zmerit ??? Delka IORQ WR pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_RD_M1_TICKS 7 /* Delka MREQ M1 RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_RD_TICKS 9    /* Delka MREQ DATA RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_WR_TICKS 9    /* ??? TODO: zmerit ??? Delka MREQ DATA WR pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */

#define REGISTER_DMD_FLAG_MZ700 (1 << 3)
#define REGISTER_DMD_FLAG_SCRW640 (1 << 2)
#define REGISTER_DMD_FLAG_HICOLOR (1 << 1)
#define REGISTER_DMD_FLAG_VBANK (1 << 0)

#define GDG_MZ800_DMD_TEST_MZ700 (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ700)
#define GDG_MZ800_DMD_TEST_SCRW640 (g_gdg.regDMD & REGISTER_DMD_FLAG_SCRW640)
#define GDG_MZ800_DMD_TEST_HICOLOR (g_gdg.regDMD & REGISTER_DMD_FLAG_HICOLOR)
#define GDG_MZ800_DMD_TEST_VBANK (g_gdg.regDMD & REGISTER_DMD_FLAG_VBANK)

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

    typedef struct st_GDGEVENT
    {
        en_MZEVENT event;
        unsigned start_row;    /* od ktereho radku event volame */
        unsigned num_rows;     /* pocet radku na kterych se volani opakuje */
        unsigned event_column; /* na kterem sloupci se event zavola */
    } st_GDGEVENT;

    typedef struct st_GDG_TIMESTAMP
    {
        unsigned screens; /* celkovy pocet vykonanych obrazovek */
        unsigned ticks;   /* celkovy pocet vykonanych pixelu z posledniho nedokonceneho screenu
                           * Hodnota 0 odpovida 1. pixelu viditelneho obrazu <0; 354431>
                           */

    } st_GDG_TIMESTAMP;

    typedef struct st_GDG
    {
        st_EMUEVENT event;

        st_GDG_TIMESTAMP total_elapsed; /* Celkovy pocet vykonanych snimku a pixelu */

        unsigned beam_row;
        unsigned screen_is_already_rendered_at_beam_pos; /* pokud byla pauza a probehnul render obrazovky, tak tady mame posledmi pozici paprsku, ktera uz je zobrazena */

        unsigned screen_need_update_from;   /* od ktereho pixelu aktualniho radku je potreba updatovat framebuffer */
        unsigned last_updated_border_pixel; /* od ktereho pixelu aktualniho radku je potreba updatovat framebuffer */

        unsigned sts_vsync; /* STS Vsync, ktery vidime na status registru - neodpovida skutecnemu VS */
        unsigned sts_hsync; /* STS Hsync, ktery vidime na status registru - neodpovida skutecnemu HS */
        unsigned hbln;      /* HBLN: 0 - pokud se sloupec paprsku nachazi mimo screen */
        unsigned vbln;      /* VBLN: 0 - pokud se radek paprsku nachazi mimo screen */
        //        unsigned hsync;     /* Skutecny Hsync, ktery se posila na vstup CTC1 */

        unsigned regDMD;    /* Display Mode register */
        unsigned regBOR;    /* Border register */
        unsigned regPALGRP; /* Palette Group register */
        unsigned regPAL0;   /* Palette0 register */
        unsigned regPAL1;   /* Palette1 register */
        unsigned regPAL2;   /* Palette2 register */
        unsigned regPAL3;   /* Palette3 register */

        unsigned regct53g7; /* rizeni GATE pro CTC0 v ctc8253 v rezimu MZ700 */

        /** CKSW (Superimpose) bit: zápis na 0CF07h bit 7, čitelný v bitu 2
         *  Status registru (0CEh i 0E008h). Hodnota 1 zkracuje řádek
         *  o GDG_CKSW_ROW_SHRINK pxCLK; čip si bit načítá jednou za řádek
         *  (viz row_shrink). Vždy 0 nebo 1. */
        unsigned cksw;

        /** Zkrácení aktuálního řádku v pxCLK: 0, nebo GDG_CKSW_ROW_SHRINK.
         *  Nastavuje událost MZEVENT_GDG_HBLN_START z cksw, nuluje konec
         *  řádku. O tuto hodnotu jsou dřív události řádku za sloupcem
         *  GDG_CKSW_SHRINK_FIRST_COLUMN (konec viditelné části, hrana HSYNC,
         *  konec řádku). */
        unsigned row_shrink;

        /** Součet zkrácení dokončených řádků aktuálního snímku (pxCLK).
         *  Začátek aktuálního řádku je
         *  beam_row * VIDEO_SCREEN_WIDTH - frame_short_ticks tiků od začátku
         *  snímku. Na konci snímku se přičte do short_ticks_total a vynuluje.
         *  Bez CKSW trvale 0. */
        unsigned frame_short_ticks;

        /** Součet zkrácení všech dokončených snímků (pxCLK). Absolutní čas je
         *  ticks + screens * VIDEO_SCREEN_TICKS - short_ticks_total
         *  (gdg_compute_total_ticks()). Bez CKSW trvale 0. */
        uint64_t short_ticks_total;

        unsigned tempo;
        unsigned tempo_divider;

        /** Počet řádků, po kterém se překlopí TEMPO. Běžně
         *  GDG_TEMPO_DIVIDER_ROWS; po načtení snapshotu starší verze (bez
         *  klíče tempo_rows) GDG_TEMPO_DIVIDER_ROWS_LEGACY, aby se záznamy
         *  vstupů pořízené starší verzí přehrály se stejnými otisky stavu.
         *  Nastavuje gdg_init() a načtení snapshotu; reset hodnotu nemění.
         *  Invariant: jedna z obou konstant. */
        unsigned tempo_rows;

#ifdef MZ800EMU_CFG_CLK1M1_SLOW
        unsigned ctc0clk;
#endif

        /** Stav modelu WAIT u přístupu CPU do VRAM (mz800_vramwait.h, režim
         *  MZ-800 i MZ-700): okamžik, kdy se uvolní WRITE FF posledního zápisu
         *  (zápis přes jednobajtový buffer dokončený v CPU slotu). Absolutní
         *  čas v CLK0 na ose modelu (gdg_get_total_ticks() +
         *  GDG_VRAMWAIT_TICKS_TO_LINE_OFFSET), 0 = žádný zápis.
         *
         *  Reset: gdg_init(), gdg_reset(), načtení snapshotu (starší snapshot
         *  bez klíče = 0). Mění se jen při zápisu CPU do VRAM; čtení ho jen
         *  čte. */
        uint64_t vram_wrff_release;

    } st_GDG;

    extern st_GDG g_gdg;

    extern const struct st_GDGEVENT g_gdgevent[];

#define GDG_TEST_VBLN (g_gdg.vbln == 0)

    extern void gdg_init(void);
    extern void gdg_reset(void);
    extern uint8_t gdg_read_dmd_status_memop(void);

    /**
     * @brief Status 0xE008 (čtení v režimu MZ-700) bez vedlejšího efektu.
     *
     * Stejná hodnota jako gdg_read_dmd_status_memop() pro stejný stav; na
     * MZ-700 a MZ-1500 bez načtení joysticku z hostitele (bity 1-4 ze stavu
     * páček z posledního čtení CPU, joymz_peek_status_bits14()). Na MZ-800
     * je čtení statusu samo bez vedlejšího efektu, funkce je shodná.
     * Pro debugger a mapované čtení memory_read_byte_no_se().
     *
     * @return Status byte (HBLNK, TEMPO, ...).
     * @pre Emu vlákno, nebo pauza.
     */
    extern uint8_t gdg_peek_dmd_status_memop(void);
    extern uint8_t gdg_read_dmd_status_ioop(void);
    extern void gdg_write_byte(unsigned addr, uint8_t value);

    /**
     * @brief Nastaví registr DMD stejnou cestou jako OUT (CEh), pro
     *        debugger.
     *
     * Provede totéž co větev CEh v gdg_write_byte() - maskování na dolní
     * 4 bity, nic při beze změny, aktualizace framebufferu do aktuální
     * pozice paprsku a gdg_set_regDMD(): ctc82530_on_regDMD_changed()
     * (GATE0 CTC0 = 1 v 800 módu, = regct53g7 v 700 módu) a přepočet RAM
     * fast-path tabulky (MZ800EMU_CFG_RAM_FASTPATH).
     *
     * Na rozdíl od gdg_write_byte(0x00CE, value) NEzapíše záznam hwlog
     * (GDG_MODE) a NEspustí HW event breakpoint BP_EVENT_GDG_MODE_CHANGE -
     * zásah z debuggeru není OUT programu a nemá se v trace ani
     * v breakpointech tvářit jako OUT.
     *
     * Čas změny (event_ticks pro CTC0) = gdg_get_insigeop_ticks(), tedy
     * aktuální g_gdg.total_elapsed.ticks (mezi instrukcemi konec
     * předchozí instrukce).
     *
     * @param value Nová hodnota DMD (použijí se bity 0-3).
     *
     * @pre Volat jen z emu vlákna mezi instrukcemi (drain fronty dbgapi)
     *      nebo když emu vlákno neběží.
     * @post g_gdg.regDMD == (value & 0x0F).
     */
    extern void gdg_debug_set_regDMD(uint8_t value);

/** @brief Zkrácení řádku při CKSW = 1 v pxCLK (netlist GDG: 1136 -> 1120). */
#define GDG_CKSW_ROW_SHRINK 16u

/**
 * @brief O kolik pxCLK před koncem M-cyklu zápisu se překlopí klopný obvod CKSW.
 *
 * Emulátor zná u zápisu na port čas konce M-cyklu. Klopný obvod bitu se
 * podle netlistu překlápí 0,5 CLK0 po náběžné hraně /WR (sestupná hrana T3),
 * tedy 2 CLK0 před koncem M-cyklu; fáze CPU vůči CLK0 je pevná, hodnota nemá
 * rozptyl. Zápis, jehož M-cyklus končí méně než tolik tiků po načtení bitu
 * (MZEVENT_GDG_HBLN_START), proto ještě platí pro tento řádek. Zdroj:
 * simulace netlistu GDG (emu-experiments superimpose/gdgsim,
 * probe_cksw_wr.c, 2160 zápisů). Měření na MZ-800 analyzátorem 2026-11-21
 * (hodiny = pixel clock, 14 227 zápisů): náběžná hrana /IORQ 975 px po
 * sestupné hraně /HSYNC ještě změní tentýž řádek, 976 px už ne, v obou
 * směrech; netlist dává 974,5 a 975,5, shoda je na půl pixelu, tedy na
 * rozlišení měření.
 */
#define GDG_CKSW_WR_EDGE_TICKS 2u

/**
 * @brief První sloupec, který krátkému řádku chybí.
 *
 * Zkrácení jde celé z pravého borderu: viditelná část krátkého řádku končí
 * tímto sloupcem (912 místo 928) a všechny události řádku za ním nastanou
 * o GDG_CKSW_ROW_SHRINK pxCLK dřív. Zdroj: simulace netlistu GDG
 * (emu-experiments superimpose/gdgsim, probe_cksw_edges.c). Měření na MZ-800
 * analyzátorem 2026-11-21: řádek 1120 px, 312 řádků, border na výstupu RGBI
 * od 186 px po sestupné hraně /HSYNC do 1114 px (CKSW = 0) nebo 1098 px
 * (CKSW = 1), tedy sloupce 0 až 928 a 0 až 912 emulátoru.
 */
#define GDG_CKSW_SHRINK_FIRST_COLUMN ( VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1u - GDG_CKSW_ROW_SHRINK )

/**
 * @brief Absolutní čas (pxCLK od startu emulace) pro tiky aktuálního snímku.
 *
 * Snímek s krátkými řádky (CKSW = 1) trvá méně než VIDEO_SCREEN_TICKS;
 * rozdíl za dokončené snímky drží g_gdg.short_ticks_total. Čas je souvislý
 * i přes konec snímku. Bez CKSW je výsledek stejný jako dřív
 * (screens * VIDEO_SCREEN_TICKS + now_ticks).
 */
#define gdg_compute_total_ticks(now_ticks) (now_ticks + ((uint64_t)g_gdg.total_elapsed.screens * VIDEO_SCREEN_TICKS) - g_gdg.short_ticks_total)
#define gdg_get_total_ticks() gdg_compute_total_ticks(g_gdg.total_elapsed.ticks)

/** @brief Tiky od začátku snímku, ve kterých začíná aktuální řádek (sloupec 0). */
#define gdg_get_row_start_ticks() ( g_gdg.beam_row * VIDEO_SCREEN_WIDTH - g_gdg.frame_short_ticks )

/**
 * @brief Délka právě končícího snímku v pxCLK.
 *
 * Platí při zpracování konce posledního řádku (po přičtení jeho zkrácení do
 * frame_short_ticks, před gdg_on_screen_done_event()). O tuto hodnotu posouvá
 * konec snímku čas GDG i události časované od začátku snímku (CTC0, PIO-Z80,
 * 20ms časovač).
 */
#define gdg_get_ending_frame_ticks() ( VIDEO_SCREEN_TICKS - g_gdg.frame_short_ticks )

/**
 * @brief Počet řádků, po kterém se překlopí signál TEMPO (bit 0 Status registru).
 *
 * Změřeno na MZ-800 analyzátorem: 227,960 řádku, 34,24 Hz (emu-experiments
 * gdg-meas, druhé měření). Dělič běží po řádcích bez ohledu na délku řádku.
 * Platná hodnota za běhu je g_gdg.tempo_rows.
 */
#define GDG_TEMPO_DIVIDER_ROWS 228u

/**
 * @brief Perioda TEMPO ve verzích do mz800new 8e1e2b09 včetně (odhad, ne měření).
 *
 * Používá se jen po načtení snapshotu bez klíče tempo_rows, viz g_gdg.tempo_rows.
 */
#define GDG_TEMPO_DIVIDER_ROWS_LEGACY 229u

/**
 * @brief Posun tiků GDG na časovou osu modelu WAIT u VRAM (mz800_vramwait.h).
 *
 * Tik GDG = 1 CLK0, sloupec 0 emulátoru je začátek levého borderu. Model
 * počítá pozici v řádku od sestupné hrany nHSYN.
 *
 * Kotvou je sestupná hrana nHSYN na sloupci VIDEO_REAL_HSYNC_START_COLUMN
 * (posun 186 CLK0). Určeno měřením na MZ-800 (emu-experiments gdg-meas, třetí
 * měření): 18 000 přístupů do VRAM ve všech fázích řádku. Model sedí se
 * strojem jen pro jedno zarovnání osy (posun o 1 CLK0 na kteroukoli stranu
 * dá o pětinu až třetinu shod méně) a emulátor s touto kotvou dává v téže
 * poloze stejné čekací takty jako stroj. Dřívější kotva podle začátku
 * viditelné části HBLN (CLK0 335 netlistu na sloupci
 * VIDEO_BEAM_CANVAS_FIRST_COLUMN - 4) dávala posun o 1 CLK0 menší.
 */
#define GDG_VRAMWAIT_TICKS_TO_LINE_OFFSET ( VIDEO_SCREEN_WIDTH - VIDEO_REAL_HSYNC_START_COLUMN )

/** @brief Aktuální čas GDG na časové ose modelu WAIT u VRAM (CLK0). */
#define gdg_get_vramwait_ticks() ( gdg_get_total_ticks() + GDG_VRAMWAIT_TICKS_TO_LINE_OFFSET )

/**
 * @brief Okamžik právě probíhajícího přístupu na sběrnici (IORQ, MMIO) v tikách GDG.
 *
 * Callback sběrnice nejdřív zavolá mzarch_main_insideop_*(), která posune
 * g_gdg.total_elapsed.ticks na aktuální T-stav instrukce (cpu->op_tstate,
 * u PSG a VRAM včetně čekacích stavů) a stejný posun zapíše do
 * g_mzarch_main.instruction_insideop_sync_ticks. Čas přístupu je proto
 * přímo g_gdg.total_elapsed.ticks. Dřívější přičtení
 * instruction_insideop_sync_ticks započítalo provedenou část instrukce
 * dvakrát a datovalo zápisy do CTC, PIO 8255 (beeper) a DMD o tuto část
 * později (u přístupu v posledním M-cyklu až za konec instrukce).
 *
 * Mezi instrukcemi (drain fronty debuggeru, dbgapi) je posun nulový
 * a výsledkem je konec předchozí instrukce.
 *
 * @return Tiky od začátku aktuálního snímku (bez g_gdg.total_elapsed.screens;
 *         na celkový čas převádí gdg_compute_total_ticks()).
 * @pre Volat z emu vlákna.
 */
#define gdg_get_insigeop_ticks() (g_gdg.total_elapsed.ticks)

#define gdg_proximate_clk1m1_event(now_ticks) (now_ticks + (GDGCLK_CTC0_DIVIDER - (gdg_compute_total_ticks(now_ticks) % GDGCLK_CTC0_DIVIDER)))
#define gdg_get_event_pointer() (&g_gdg.event)
#define gdg_get_regct53g7() (g_gdg.regct53g7)

#ifdef MZ800EMU_CFG_CLK1M1_FAST
#define gdg_1m1_on_screen_done_event()
#else
#define gdg_1m1_on_screen_done_event() {g_gdg.ctc0clk++;}
#endif

/**
 * @brief Uzavře snímek: zvýší čítač snímků a odečte délku snímku od tiků.
 *
 * Volá se jen z gdg_process_events() při zpracování události konce
 * posledního řádku. Délka snímku je gdg_get_ending_frame_ticks()
 * (VIDEO_SCREEN_TICKS minus zkrácení řádků při CKSW = 1); zkrácení se
 * přičte do short_ticks_total, aby absolutní čas zůstal souvislý. V tu
 * chvíli platí g_gdg.total_elapsed.ticks >= délka snímku (událost má ticks
 * rovné délce snímku). Pokud by to neplatilo (konec snímku zpracovaný
 * dvakrát), unsigned odečet by přetekl na ~2^32 a emulace by minuty
 * zpracovávala události bez instrukcí. Pojistka to nahlásí přes
 * mzarch_main_report_screen_done_underflow() a tiky srovná na 0.
 * Kontrola je jednou za snímek, ne v per-instruction hot path.
 */
#define gdg_on_screen_done_event()                       \
    {                                                    \
        g_gdg.total_elapsed.screens++;                   \
        g_mzarch_main.cursor_timer++;                          \
        if (g_gdg.total_elapsed.ticks >= gdg_get_ending_frame_ticks()) \
        {                                                \
            g_gdg.total_elapsed.ticks -= gdg_get_ending_frame_ticks(); \
        }                                                \
        else                                             \
        {                                                \
            mzarch_main_report_screen_done_underflow(g_gdg.total_elapsed.ticks); \
            g_gdg.total_elapsed.ticks = 0;               \
        }                                                \
        g_gdg.short_ticks_total += g_gdg.frame_short_ticks; \
        g_gdg.frame_short_ticks = 0;                     \
        g_gdg.beam_row = 0;                              \
        gdg_1m1_on_screen_done_event();                  \
    }

    /**
     * @brief Pozice paprsku pro čas @p ticks v aktuálním snímku.
     *
     * Vychází z g_gdg.beam_row (řádek, jehož události jsou zpracované)
     * a počítá sloupec od začátku tohoto řádku. Pokud je @p ticks až za
     * koncem řádku (konec řádku ještě není zpracovaný), vrátí pozici
     * v následujícím řádku; za posledním řádkem snímku řádek 0.
     *
     * @param ticks Tiky od začátku snímku (>= začátek aktuálního řádku).
     * @param row [out] Řádek 0..VIDEO_SCREEN_HEIGHT-1; smí být NULL.
     * @return Sloupec od začátku řádku (0 = první viditelný bod).
     * @pre Emu vlákno, nebo pauza.
     */
    static inline unsigned gdg_get_beam_pos(unsigned ticks, unsigned *row);

    /**
     * @brief Čas události aktuálního řádku v tikách od začátku snímku.
     *
     * Jediné místo, které převádí sloupec z tabulky g_gdgevent na čas:
     * používá ho plánovač událostí (gdg_event_set_next()) i obnova čekající
     * události po načtení snapshotu. V krátkém řádku (g_gdg.row_shrink != 0,
     * bit CKSW) nastanou události za sloupcem GDG_CKSW_SHRINK_FIRST_COLUMN
     * o row_shrink dřív: konec viditelné části, hrana HSYNC a konec řádku.
     *
     * @param column Jmenovitý sloupec události (0..VIDEO_SCREEN_WIDTH).
     * @return Tiky od začátku snímku.
     * @pre g_gdg.beam_row, frame_short_ticks a row_shrink odpovídají řádku,
     *      pro který se čas počítá.
     */
    static inline unsigned gdg_get_event_ticks(unsigned column)
    {
        if (column > GDG_CKSW_SHRINK_FIRST_COLUMN)
        {
            column -= g_gdg.row_shrink;
        };
        return gdg_get_row_start_ticks() + column;
    }

    static inline unsigned gdg_get_beam_pos(unsigned ticks, unsigned *row)
    {
        unsigned r = g_gdg.beam_row;
        unsigned col = ticks - gdg_get_row_start_ticks();
        unsigned width = VIDEO_SCREEN_WIDTH - g_gdg.row_shrink;
        if (col >= width)
        {
            col -= width;
            r = (r + 1 < VIDEO_SCREEN_HEIGHT) ? r + 1 : 0;
        };
        if (row)
        {
            *row = r;
        };
        return col;
    }

/** @brief Sloupec paprsku v aktuálním čase GDG (viz gdg_get_beam_pos()). */
#define gdg_get_beam_col() gdg_get_beam_pos(g_gdg.total_elapsed.ticks, NULL)

    /**
     * @brief Virtuální pozice paprsku: řádek * VIDEO_SCREEN_WIDTH + sloupec
     *        v mřížce nezkráceného řádku.
     *
     * Pro ladicí nástroje a záznamy trace, které pozici dekódují dělením
     * VIDEO_SCREEN_WIDTH. V krátkém řádku (CKSW = 1) se sloupce od
     * GDG_CKSW_SHRINK_FIRST_COLUMN posunou o GDG_CKSW_ROW_SHRINK, takže
     * hrana HSYNC a konec řádku mají stejný sloupec jako v dlouhém řádku
     * a sloupce 912 až 927 se nevyskytnou. Bez CKSW je výsledek roven
     * tikům od začátku snímku.
     *
     * @param ticks Tiky od začátku snímku (>= začátek aktuálního řádku).
     * @return Virtuální pozice 0..VIDEO_SCREEN_TICKS-1.
     * @pre Emu vlákno, nebo pauza.
     */
    static inline unsigned gdg_get_beam_vpos(unsigned ticks)
    {
        unsigned r = g_gdg.beam_row;
        unsigned col = ticks - gdg_get_row_start_ticks();
        if (g_gdg.row_shrink && (col >= GDG_CKSW_SHRINK_FIRST_COLUMN))
        {
            col += g_gdg.row_shrink;
        };
        if (col >= VIDEO_SCREEN_WIDTH)
        {
            col -= VIDEO_SCREEN_WIDTH;
            r = (r + 1 < VIDEO_SCREEN_HEIGHT) ? r + 1 : 0;
        };
        return r * VIDEO_SCREEN_WIDTH + col;
    }

    /**
     * @brief Tiky od poslední sestupné hrany skutečného HSYNC (CLK1 čítače 8253).
     *
     * Hrana je ve sloupci VIDEO_REAL_HSYNC_START_COLUMN, v krátkém řádku
     * (CKSW = 1) o row_shrink dřív. Od hrany do konce řádku je vždy stejně
     * daleko (zkrácení leží před hranou), takže před hranou aktuálního
     * řádku je výsledek sloupec plus tato pevná vzdálenost.
     *
     * @param ticks Tiky od začátku snímku (>= začátek aktuálního řádku).
     * @return Počet tiků od hrany, 0 až délka řádku - 1.
     * @pre Emu vlákno, nebo pauza.
     */
    static inline unsigned gdg_get_ticks_since_hsync_fall(unsigned ticks)
    {
        unsigned col = gdg_get_beam_pos(ticks, NULL);
        unsigned hsync_col = VIDEO_REAL_HSYNC_START_COLUMN - g_gdg.row_shrink;
        if (col >= hsync_col)
        {
            return col - hsync_col;
        };
        return col + (VIDEO_SCREEN_WIDTH - VIDEO_REAL_HSYNC_START_COLUMN);
    }

    static inline void gdg_get_timestamp(st_GDG_TIMESTAMP *tm)
    {
        tm->screens = g_gdg.total_elapsed.screens;
        tm->ticks = g_gdg.total_elapsed.ticks;
    }

#ifdef __cplusplus
}
#endif

#endif /* MZ800_GDG_H */
