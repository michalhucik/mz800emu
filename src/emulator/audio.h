/*
 * File:   audio.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 28. července 2015, 13:39
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

#ifndef AUDIO_H
#define AUDIO_H

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>
#include <stdbool.h>
#include "app/app_thread.h"

#include "mzarch/mzarch_config.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/psg/psg.h"

    /* Pocet zdrojovych audio kanalu, parametrizovany pres HAVE_PSG:
     *   HAVE_PSG = 0  →  1 kanal (jen CTC0)         - MZ-700
     *   HAVE_PSG = 1  →  5 kanalu (CTC0 + 4 PSG0)   - MZ-800
     *   HAVE_PSG = 2  →  9 kanalu (CTC0 + 4 PSG0 + 4 PSG1) - MZ-1500
     */
#define AUDIO_SRC_CHANNELS_COUNT (1 + 4 * HAVE_PSG)

    typedef int8_t AUDIO_BUF_t; // Musi to byt jednobajtovy typ !!!

/**
 * @brief Největší stupeň hlasitosti kanálu ve zvukovém logu (útlum 0).
 *
 * Hodnoty zvukového logu (st_AUDIO_LOG_EVENT::value):
 * - 0 = ticho (výstup v nule nebo vypnutý kanál),
 * - 1 až 15 = plná výchylka stupně hlasitosti `15 - útlum` (CTC0 jen 0 a 15),
 * - 16 až 30 = poloviční výchylka stupně `hodnota - 15`
 *   (AUDIO_LEVEL_HALF_FACTOR); píše ji jen tónový kanál PSG s děličem 001h,
 * - 31 (AUDIO_LEVEL_CTC0_DIV2) = výchylka AUDIO_CTC0_DIV2_FACTOR stupně 15;
 *   píše ji jen kanál CTC0, když čítač běží v režimu 3 s děličem 2.
 */
#define AUDIO_MAXVAL_PER_CHANNEL 15

/** @brief Posun hodnot poloviční výchylky: hodnota = AUDIO_LEVEL_HALF_BASE + stupeň (1 až 15). */
#define AUDIO_LEVEL_HALF_BASE AUDIO_MAXVAL_PER_CHANNEL

/**
 * @brief Hodnota zvukového logu kanálu CTC0 při režimu 3 s děličem 2.
 *
 * Výchylka AUDIO_CTC0_DIV2_FACTOR plného kroku CTC0 (hodnoty
 * AUDIO_MAXVAL_PER_CHANNEL). Do logu ji píše audio_ctc0_div2_changed() místo
 * hran obdélníku 554 kHz; kanály PSG ji nepoužívají.
 */
#define AUDIO_LEVEL_CTC0_DIV2 (AUDIO_LEVEL_HALF_BASE + AUDIO_MAXVAL_PER_CHANNEL + 1)

/** @brief Počet hodnot zvukového logu (0 až 31); velikost tabulek úrovní. */
#define AUDIO_LEVEL_VALUES_COUNT (AUDIO_LEVEL_CTC0_DIV2 + 1)

/**
 * @brief Výchylka tónového kanálu PSG s děličem 001h proti plné výchylce stupně.
 *
 * Čip při děliči 001h přepíná výstup každý krok (110,8 kHz), ve slyšitelném
 * pásmu je to stálá úroveň. Změřeno na MZ-800 (2026-11-18, nahrávka z vývodu
 * AUDIO OUT čipu): střídání hlasitosti k <-> vypnuto dává signál o 6,2 dB
 * slabší než běžný tón se stejným útlumem (úroveň 0,49) pro útlumy 0 až 14.
 */
#define AUDIO_LEVEL_HALF_FACTOR 0.49f

/**
 * @brief Výchylka CTC0 v režimu 3 s děličem 2 proti plnému kroku CTC0.
 *
 * CTC0 s děličem 2 dává obdélník 554 kHz, který výstup zvukového čipu
 * nestíhá; ve slyšitelném pásmu z něj zbyde stálá úroveň. Změřeno na MZ-800
 * (2026-11-19): klíčování bitem PC0 na 440 Hz dává čistý tón o úrovni 0,64
 * téhož klíčování při OUT0 trvale v jedničce (nahrávka přes line-in,
 * -3,92 dB, tři běhy; osciloskop na vývodu čipu 0,62). Stálý signál slyšet
 * není. MZ-1500 neměřen, hodnota je převzatá z MZ-800 [neověřeno].
 *
 * Samostatný MZ-700 (MZARCH 700) zvukový čip v cestě nemá (OUT0 jde přes
 * hradlo LS00 na potenciometr, servisní manuál MZ-700 str. 31), takže tam
 * zbyde prostá střední hodnota obdélníku se střídou 1:1, tedy 0,50. Odvozeno
 * ze schématu, neměřeno [neověřeno].
 */
#if MZARCH == 700
#define AUDIO_CTC0_DIV2_FACTOR 0.50f
#else
#define AUDIO_CTC0_DIV2_FACTOR 0.64f
#endif

/**
 * @brief Poměr výchylky CTC0 k výchylce jednoho kanálu PSG s útlumem 0.
 *
 * Změřeno na MZ-800 (2026-11-18, vývod AUDIO OUT čipu a horní konec
 * potenciometru hlasitosti): tón CTC0 je o 3,5 dB silnější než tón jednoho
 * kanálu PSG s útlumem 0.
 */
#define AUDIO_CTC0_TO_PSG_CHANNEL_RATIO 1.49f

/**
 * @brief Časová konstanta střídavé vazby CTC0 do zvukového výstupu [ms].
 *
 * Na MZ-800 je CTC0 (OUT0 hradlovaný PC0, i echo kazety) do výstupu čipu PSG
 * vázaný střídavě: obdélník je souměrný kolem klidové úrovně a stálá úroveň
 * se ke klidu vrací s časovou konstantou 0,8 až 0,9 s (změřeno osciloskopem
 * 2026-11-18). PSG je vázaný stejnosměrně. MZ-1500 je odvozený z MZ-800
 * (neměřeno).
 *
 * Na samostatném MZ-700 (MZARCH 700) to vlastnost stroje není: podle schématu
 * jde OUT0 přes hradlo LS00 a potenciometr 10 kΩ na emitorový sledovač
 * a reproduktor je připojený stejnosměrně (na skutečném MZ-700 neměřeno).
 * Stejná vazba tam zůstává jako pomůcka emulátoru: zvuková karta stálou
 * úroveň nepřenese a bez vazby by při skoku na stálou úroveň lupalo.
 */
#define AUDIO_CTC0_AC_TAU_MS 850

/**
 * @brief Časová konstanta dolní propusti mezi čipem PSG a zesilovačem [us].
 *
 * První řád, 460 us (zlom 346 Hz) při hlasitosti na maximu; změřeno
 * osciloskopem na potenciometru hlasitosti MZ-800 2026-11-18, ze schématu vychází
 * 350 Hz. Prochází jí PSG i CTC0. Volba [AUDIO] output_filter
 * (jen při AUDIO_HAVE_OUTPUT_FILTER).
 */
#define AUDIO_OUT_LP_TAU_US 460

/**
 * @brief Časová konstanta střídavé vazby celého zvukového výstupu [ms].
 *
 * Na MZ-800 je mezi výstupem čipu PSG a potenciometrem hlasitosti v sérii
 * kondenzátor C16 10 uF; s R6 47 kΩ a potenciometrem 10 kΩ dává horní propust
 * 1. řádu s časovou konstantou 10 uF x 57 kΩ = 0,57 s (asi 0,28 Hz). Prochází
 * jí PSG i CTC0 (CTC0 má před ní ještě vlastní vazbu AUDIO_CTC0_AC_TAU_MS).
 * Hodnota je spočtená ze schématu (10 uF x 57 kΩ = 0,57 s), měření osciloskopem
 * tomu odpovídá (0,55 s, rozsah 0,44 až 0,69 s; 2026-11-18, pomalý obdélník
 * z PC0 na horním konci potenciometru po odečtení vazby CTC0). Díky ní na
 * výstupu nezůstává stálé napětí, když počítač mlčí; nahrazuje dřívější
 * parkování stálé úrovně kanálů PSG. Jen při AUDIO_HAVE_OUTPUT_COUPLING.
 */
#define AUDIO_OUT_AC_TAU_MS 570

/**
 * @brief Má emulovaný počítač střídavou vazbu celého výstupu (AUDIO_OUT_AC_TAU_MS)?
 *
 * 1 = MZ-800 (ze schématu, měření tomu odpovídá) a MZ-1500 (odvozeno z MZ-800). 0 =
 * samostatný MZ-700 (MZARCH 700): reproduktor je tam vázaný stejnosměrně
 * a stejnosměrnou složku odstraňuje jen pomůcka emulátoru, vazba CTC0.
 */
#if MZARCH == 700
#define AUDIO_HAVE_OUTPUT_COUPLING 0
#else
#define AUDIO_HAVE_OUTPUT_COUPLING 1
#endif

/**
 * @brief Má emulovaný počítač výstupní dolní propust (volbu [AUDIO] output_filter)?
 *
 * 1 = MZ-800 (změřeno) a MZ-1500 (odvozeno z MZ-800, neměřeno). 0 = samostatný
 * MZ-700 (MZARCH 700): podle schématu tam mezi hradlem za OUT0 a emitorovým
 * sledovačem žádná propust není (jen potenciometr; kondenzátor 0,1 uF je až
 * paralelně k reproduktoru), na skutečném MZ-700 neměřeno. Při 0 se volba
 * neregistruje v INI, nenabízí v okně Audio ani přes MCP a
 * iface_audio_output_filter_enabled() vrací vždy false (dosavadní filtry).
 * MZ-800 v režimu MZ-700 je binárka MZ-800 a volbu má.
 */
#if MZARCH == 700
#define AUDIO_HAVE_OUTPUT_FILTER 0
#else
#define AUDIO_HAVE_OUTPUT_FILTER 1
#endif

/**
 * @brief Dorovnání hlasitosti výstupu se zapnutou výstupní propustí [dB].
 *
 * Volba emulátoru, NE změřená vlastnost stroje: absolutní hlasitost
 * skutečného počítače určuje knoflík hlasitosti a zesilovač LM386 (zesílení
 * asi 21), který úbytek za propustí dohání; v emulátoru za propustí žádný
 * zesilovač není. Poměry mezi zdroji (CTC0 proti PSG, dělič 001h, CTC0
 * s děličem 2) se dorovnáním nemění, násobí se až hotový součet.
 *
 * Hodnota 8 dB (2,51x) je zvolená podle měřicího programu audio_meas
 * (záznam reference.wav): tón CTC0 440 Hz má pak efektivní hodnotu
 * -11,6 dBFS a kanál PSG 433 Hz naplno -15,0 dBFS; kanál PSG, který začne
 * hrát naplno z ticha (jednostranná výchylka 0,25, než se střídavá vazba
 * výstupu ustálí), dá 0,63 a zůstane pod kolenem AUDIO_OUT_LIMIT_KNEE.
 * S 9 dB by už koleno překročil.
 */
#define AUDIO_OUT_MAKEUP_FILTER_ON_DB 8.0

/**
 * @brief Dorovnání hlasitosti výstupu s vypnutou výstupní propustí [dB].
 *
 * Platí i pro samostatný MZ-700 (MZARCH 700), který propust nemá. Volba
 * emulátoru jako AUDIO_OUT_MAKEUP_FILTER_ON_DB. Hodnota 4 dB (1,58x) je
 * zvolená tak, aby tón 440 Hz měl se zapnutou i vypnutou propustí stejnou
 * efektivní hodnotu (propust na 440 Hz ubírá 3,9 až 4,0 dB, změřeno
 * v emulátoru programem audio_meas): přepnutí propusti mění barvu zvuku,
 * ne hlasitost.
 */
#define AUDIO_OUT_MAKEUP_FILTER_OFF_DB 4.0

/**
 * @brief Koleno měkkého omezovače výstupu (výchylka v rozsahu 0 až 1).
 *
 * Do této výchylky je výstup přesně lineární, nad ní se špičky plynule
 * stlačují k 1,0 (videorec_audio_soft_limit()); tvrdý ořez nenastane ani při
 * všech zdrojích naplno. Volba emulátoru: skutečný MZ-800 plný signál také
 * stlačuje (zesilovač, asi 0,8 dB u jednoho kanálu PSG naplno), ale jeho
 * křivka změřená není a omezovač ji nenapodobuje. Tóny CTC0 a jeden až dva
 * kanály PSG naplno zůstávají pod kolenem; tři kanály PSG naplno, CTC0
 * s PSG a začátek tónu CTC0 (než se jeho střídavá vazba ustálí) koleno
 * překračují a stlačí se.
 */
#define AUDIO_OUT_LIMIT_KNEE 0.7f

/**
 * @brief Největší hlavní hlasitost poslechu (násobitel; 2,0 = 200 %, +6 dB).
 *
 * Hodnoty nad 1,0 přidávají: blok pro zvukové zařízení pak projde druhým
 * měkkým omezovačem (iface_audio_apply_master()), takže se neořízne.
 */
#define AUDIO_MASTER_GAIN_MAX 2.0f

    // extern AUDIO_BUF_t g_attenuator_volume_value [ PSG_OUT_OFF + 1 ];
    extern AUDIO_BUF_t g_attenuator_volume_value[4][16];

#define AUDIO_SCAN_PERIOD PSG_DIVIDER
    // #define AUDIO_SCAN_PERIOD   GDGCLK_CTC0_DIVIDER

#define AUDIO_SOURCE_FLAG_PSG0 0x10
#define AUDIO_SOURCE_MASK_PSG0 0x03
#define AUDIO_SOURCE_ID_SHIFT_PSG0 1
// predbezne pro MZ-1500
#define AUDIO_SOURCE_FLAG_PSG1 0x20
#define AUDIO_SOURCE_MASK_PSG1 0x03
#define AUDIO_SOURCE_ID_SHIFT_PSG1 5

    typedef enum en_AUDIO_SOURCE
    {
        AUDIO_SRC_CTC0 = 0,
        AUDIO_SRC_PSG0_0 = AUDIO_SOURCE_FLAG_PSG0,
        AUDIO_SRC_PSG0_1,
        AUDIO_SRC_PSG0_2,
        AUDIO_SRC_PSG0_3,
        // predbezne pro MZ-1500
        AUDIO_SRC_PSG1_0 = AUDIO_SOURCE_FLAG_PSG1,
        AUDIO_SRC_PSG1_1,
        AUDIO_SRC_PSG1_2,
        AUDIO_SRC_PSG1_3,
    } en_AUDIO_SOURCE;

    static inline size_t audio_source_id(en_AUDIO_SOURCE source)
    {
        if (source == AUDIO_SRC_CTC0)
            return 0;
        else if (source & AUDIO_SOURCE_FLAG_PSG0)
        {
            return (source & AUDIO_SOURCE_MASK_PSG0) + AUDIO_SOURCE_ID_SHIFT_PSG0;
        };
        return (source & AUDIO_SOURCE_MASK_PSG1) + AUDIO_SOURCE_ID_SHIFT_PSG1;
    }

    /**
     * @brief Je hodnota zvukového logu poloviční výchylka (16 až 30)?
     * @param value Hodnota zvukového logu (0 až 31).
     * @return true = poloviční výchylka stupně audio_level_step(); hodnota
     *         AUDIO_LEVEL_CTC0_DIV2 mezi ně nepatří.
     */
    static inline bool audio_level_is_half(unsigned value)
    {
        return (value > AUDIO_MAXVAL_PER_CHANNEL) && (value < AUDIO_LEVEL_CTC0_DIV2);
    }

    /**
     * @brief Je hodnota zvukového logu úroveň CTC0 s děličem 2 (31)?
     * @param value Hodnota zvukového logu (0 až 31).
     * @return true = výchylka AUDIO_CTC0_DIV2_FACTOR stupně 15.
     */
    static inline bool audio_level_is_ctc0_div2(unsigned value)
    {
        return value == AUDIO_LEVEL_CTC0_DIV2;
    }

    /**
     * @brief Stupeň hlasitosti hodnoty zvukového logu.
     * @param value Hodnota zvukového logu (0 až 31).
     * @return Stupeň 0 až 15 (15 = útlum 0); u poloviční výchylky stupeň bez
     *         snížení, u AUDIO_LEVEL_CTC0_DIV2 stupeň 15.
     */
    static inline unsigned audio_level_step(unsigned value)
    {
        if (value <= AUDIO_MAXVAL_PER_CHANNEL)
            return value;
        return audio_level_is_half(value) ? (value - AUDIO_LEVEL_HALF_BASE) : AUDIO_MAXVAL_PER_CHANNEL;
    }

    /**
     * @brief Úsek stálé hodnoty kanálu ve zvukovém logu.
     *
     * Invariant: `value` je 0 až 31 (viz AUDIO_MAXVAL_PER_CHANNEL).
     */
    typedef struct st_AUDIO_LOG_EVENT
    {
        uint32_t count_ticks; /**< Délka úseku v ticích GDG. */
        AUDIO_BUF_t value;    /**< Hodnota kanálu (0 až 31). */
    } st_AUDIO_LOG_EVENT;

    typedef struct st_AUDIO_SOURCE_LOG
    {
        uint64_t last_timestamp;
        AUDIO_BUF_t last_value;
        st_AUDIO_LOG_EVENT *samples;
        unsigned samples_count;
    } st_AUDIO_SOURCE_LOG;

    typedef struct st_AUDIO_LOG
    {
        bool stereo;                /* true = stereo (2 PSG), false = mono */
        uint64_t first_timestamp;
        uint64_t last_timestamp;
        uint64_t last_psg_timestamp;
        /** Střední hodnota CTC0 (0..1) pro střídavou vazbu na začátku logu
         *  (audio_ctc0_ac_mean()); SDL cesta jí nastaví svůj stav, když log
         *  nenavazuje na předchozí zpracovaný. */
        double ctc0_ac_mean;
        /** Střední hodnoty kanálů pro střídavou vazbu výstupu na začátku logu
         *  (audio_out_ac_means()): [0] střední hodnota výstupu vazby CTC0
         *  (-1..1), [1..] střední výchylka kanálu PSG (0..1). Bez
         *  AUDIO_HAVE_OUTPUT_COUPLING nuly. */
        double out_ac_mean[AUDIO_SRC_CHANNELS_COUNT];
        st_AUDIO_SOURCE_LOG *src[AUDIO_SRC_CHANNELS_COUNT];
    } st_AUDIO_LOG;

    /**
     * @brief Stav střídavé vazby CTC0 vedený v emulovaném čase.
     *
     * Střední hodnota kanálu CTC0 (dolní propust 1. řádu s časovou konstantou
     * AUDIO_CTC0_AC_TAU_MS nad úrovní kanálu 0..1); výstup střídavé vazby je úroveň
     * minus střední hodnota. Počítá se přesně z časů hran, nezávisle na tom,
     * zda nějaká zvuková cesta právě běží, aby šlo stav kdykoli předat SDL
     * cestě i rendereru nahrávky.
     *
     * `out_mean` je druhý stupeň pro střídavou vazbu celého výstupu
     * (AUDIO_OUT_AC_TAU_MS): střední hodnota výstupu vazby CTC0
     * (`level - mean`), vedená se stejnými časy hran.
     *
     * Invarianty: `mean` je v 0..1 a platí k času `ticks`; `level` je výchylka
     * hodnoty, kterou kanál CTC0 nese ve zvukovém logu (0, 1 nebo
     * AUDIO_CTC0_DIV2_FACTOR); `out_mean` je v -1..1 (bez
     * AUDIO_HAVE_OUTPUT_COUPLING 0).
     */
    typedef struct st_AUDIO_CTC0_AC
    {
        uint64_t ticks;  /**< Čas, ke kterému platí `mean` a `out_mean` (tiky GDG). */
        double mean;     /**< Střední hodnota úrovně (0..1). */
        double out_mean; /**< Střední hodnota výstupu vazby CTC0 pro vazbu výstupu (-1..1). */
        double level;    /**< Úroveň kanálu od času `ticks` (0, 1 nebo AUDIO_CTC0_DIV2_FACTOR). */
    } st_AUDIO_CTC0_AC;

    /**
     * @brief Stav střídavé vazby výstupu pro jeden kanál PSG, vedený v emulovaném čase.
     *
     * Střední výchylka kanálu (dolní propust 1. řádu s časovou konstantou
     * AUDIO_OUT_AC_TAU_MS nad výchylkou audio_level_amplitude() hodnoty kanálu).
     * Vazba je lineární, střední hodnota smíchaného signálu je součet středních
     * hodnot kanálů vážený hlasitostmi (iface_audio_output_ac_means()).
     *
     * Invarianty: `mean` a `amp` jsou v 0..1; `mean` platí k času `ticks`.
     */
    typedef struct st_AUDIO_PSG_AC
    {
        uint64_t ticks; /**< Čas, ke kterému platí `mean` (tiky GDG). */
        double mean;    /**< Střední výchylka kanálu (0..1). */
        double amp;     /**< Výchylka kanálu od času `ticks` (0..1). */
    } st_AUDIO_PSG_AC;

    typedef struct st_AUDIO
    {
        /** Stav výstupu CTC0 po hradlování bitem PC0 (0/1). Při ctc0_div2 = 1
         *  je to samotný bit PC0 (OUT0 se nahrazuje stálou úrovní). */
        unsigned ctc0_output;
        /** 1 = CTC0 běží v režimu 3 s děličem 2 (audio_ctc0_div2_changed()):
         *  kanál CTC0 nese při ctc0_output = 1 hodnotu AUDIO_LEVEL_CTC0_DIV2
         *  místo hran OUT0. Nuluje ho audio_ctc0_changed(). Není ve snapshotu,
         *  po načtení se odvodí z čítače (audio_ctc0_restore()). */
        unsigned ctc0_div2;
        /** Úroveň echa kazety (0/1, audio_cmt_echo_changed()); v klidu 0.
         *  Kanál CTC0 zvukového logu nese ctc0_output XOR cmt_echo; při
         *  ctc0_div2 = 1 a ctc0_output = 1 echo hodnotu kanálu nemění. */
        unsigned cmt_echo;
        /** Střídavá vazba CTC0 (viz st_AUDIO_CTC0_AC); není ve snapshotu, po
         *  načtení a po resetu logu začíná ustálená. */
        st_AUDIO_CTC0_AC ctc0_ac;
        /** Střídavá vazba výstupu po kanálech PSG (index = id zdroje, [0]
         *  se nepoužívá); není ve snapshotu, po načtení začíná ustálená. */
        st_AUDIO_PSG_AC psg_ac[AUDIO_SRC_CHANNELS_COUNT];

        st_AUDIO_LOG *log;
        st_AUDIO_LOG *old_log;
        app_mutex_t *old_log_mutex;
    } st_AUDIO;

    extern st_AUDIO g_audio;

    extern void audio_init(void);

    /**
     * @brief Hlavní hlasitost poslechu z hodnot INI klíčů [AUDIO] volume_master a volume_master_boost.
     *
     * Hlasitost je `volume_master / 4294967295 + boost_percent / 100`,
     * nejvýš AUDIO_MASTER_GAIN_MAX. Klíč volume_master má stejný význam jako
     * před rozšířením rozsahu (0 až 4294967295 = 0 až 100 %), takže starší
     * INI bez klíče volume_master_boost (výchozí 0) dává stejnou hlasitost
     * jako dřív.
     *
     * @param volume_master Hodnota klíče volume_master.
     * @param boost_percent Hodnota klíče volume_master_boost (procenta nad 100 %); nad 100 se ořízne.
     * @return Hlasitost 0 až AUDIO_MASTER_GAIN_MAX.
     * @note Čistá funkce.
     */
    extern float audio_master_gain_from_cfg(uint32_t volume_master, uint32_t boost_percent);

    /**
     * @brief Rozloží hlavní hlasitost poslechu do INI klíčů volume_master a volume_master_boost.
     *
     * Do 100 % včetně: `volume_master = hlasitost * 4294967295`, boost 0.
     * Nad 100 %: volume_master je nejvyšší hodnota a boost jsou celá
     * procenta nad 100 % (zaokrouhleno). Starší verze emulátoru, která klíč
     * volume_master_boost nezná, z takového INI přečte 100 %.
     *
     * @param gain          Hlasitost; ořízne se na 0 až AUDIO_MASTER_GAIN_MAX, NaN se bere jako 0.
     * @param volume_master Výstup: hodnota klíče volume_master; NULL = nezapisovat.
     * @param boost_percent Výstup: hodnota klíče volume_master_boost (0 až 100); NULL = nezapisovat.
     * @note Čistá funkce.
     */
    extern void audio_master_gain_to_cfg(float gain, uint32_t *volume_master, uint32_t *boost_percent);

    /**
     * @brief Změna výstupu CTC0 po hradlování bitem PC0.
     *
     * Kanál CTC0 zvukového logu dostane hodnotu ctc0_state XOR echo kazety
     * (0 nebo AUDIO_MAXVAL_PER_CHANNEL). Ukončí stav "režim 3 s děličem 2"
     * (audio_ctc0_div2_changed()).
     *
     * @param ctc0_state OUT0 AND PC0 (na samostatném MZ-700 jen OUT0).
     * @param total_event_ticks Čas hrany (osa gdg_get_total_ticks()).
     * @pre Vlákno emulátoru.
     * @post g_audio.ctc0_output == ctc0_state, g_audio.ctc0_div2 == 0.
     */
    extern void audio_ctc0_changed(bool ctc0_state, uint64_t total_event_ticks);

    /**
     * @brief CTC0 běží v režimu 3 s děličem 2: kanál nese stálou úroveň místo hran.
     *
     * Obdélník 554 kHz se do zvukového logu nepíše. Při @p pc0 = true dostane
     * kanál hodnotu AUDIO_LEVEL_CTC0_DIV2 (výchylka AUDIO_CTC0_DIV2_FACTOR
     * plného kroku CTC0, změřeno na MZ-800), při false hodnotu echa kazety
     * (v klidu 0). Stálou úroveň pak nechá doznít střídavá vazba CTC0.
     *
     * Echo kazety při @p pc0 = true hodnotu kanálu nemění: XOR obdélníku
     * 554 kHz s úrovní pásky je zase obdélník 554 kHz (na HW neměřeno
     * [neověřeno]).
     *
     * @param pc0 Bit PC0 (na samostatném MZ-700 vždy true).
     * @param total_event_ticks Čas změny (osa gdg_get_total_ticks()); starší
     *        než poslední událost kanálu se posune na ni.
     * @pre Vlákno emulátoru. Volá jen 8253 (ctc8253.c), který stav čítače zná.
     * @post g_audio.ctc0_div2 == 1, g_audio.ctc0_output == pc0. Stav trvá do
     *       nejbližšího audio_ctc0_changed().
     */
    extern void audio_ctc0_div2_changed(bool pc0, uint64_t total_event_ticks);

    /**
     * @brief Nastaví stav kanálu CTC0 bez zápisu do zvukového logu.
     *
     * Po načtení snapshotu: g_audio.ctc0_output a g_audio.ctc0_div2 ve
     * snapshotu nejsou a odvozují se ze stavu 8253 a PC0
     * (ctc8253_ctc0_audio_restore()). Volat před audio_reset_log(), která
     * podle nich ustálí střídavou vazbu.
     *
     * @param ctc0_state OUT0 AND PC0; při @p div2 = true samotný PC0.
     * @param div2 true = CTC0 běží v režimu 3 s děličem 2.
     * @pre Vlákno emulátoru (nebo stojící emulace).
     */
    extern void audio_ctc0_restore(bool ctc0_state, bool div2);

    /**
     * @brief Změna úrovně echa kazety (signál pásky do reproduktoru).
     *
     * Echo nemá vlastní zvukový kanál: kanál CTC0 zvukového logu nese
     * ctc0_output XOR cmt_echo, takže hrana kteréhokoli z obou signálů je
     * hranou výstupu a v klidu echa (0) je výstup čistý CTC0. Čas události
     * starší než poslední událost kanálu se posune na ni (hrany pásky se
     * doručují zpětně, nejvýš o snímek).
     *
     * @param level Nová úroveň signálu pásky.
     * @param total_event_ticks Čas hrany (osa gdg_get_total_ticks()).
     * @pre Vlákno emulátoru.
     * @post g_audio.cmt_echo == level; g_audio.ctc0_output se nemění.
     */
    extern void audio_cmt_echo_changed(bool level, uint64_t total_event_ticks);
    /**
     * @brief Střední hodnota CTC0 pro střídavou vazbu v zadaném čase.
     *
     * Dopočítá stav st_AUDIO_CTC0_AC do `now_ticks` (čas před posledním
     * dopočtem stav nemění) a vrátí střední hodnotu.
     *
     * @param now_ticks Čas (osa gdg_get_total_ticks()).
     * @return Střední hodnota 0..1 (podíl času v jedničce vážený exponenciálně).
     * @pre Vlákno emulátoru.
     * @post g_audio.ctc0_ac.ticks >= now_ticks.
     */
    extern double audio_ctc0_ac_mean(uint64_t now_ticks);

    /**
     * @brief Výchylka hodnoty zvukového logu při plné hlasitosti kanálu.
     *
     * 0 = ticho, 15 = 1,0, mezi nimi krok 2 dB; hodnoty 16 až 30 výchylka
     * stupně `value - 15` krát AUDIO_LEVEL_HALF_FACTOR; hodnota 31
     * (AUDIO_LEVEL_CTC0_DIV2, jen kanál CTC0) AUDIO_CTC0_DIV2_FACTOR. Pro
     * hodnoty 0 až 30 stejný předpis jako tabulka SDL cesty
     * (iface_audio_set_src_volume()) při hlasitosti 100 %; hodnotu 31 má
     * tabulka kanálů PSG nulovou.
     *
     * @param value Hodnota zvukového logu 0 až 31; větší se bere jako 0.
     * @return Výchylka 0..1.
     */
    extern double audio_level_amplitude(unsigned value);

    /**
     * @brief Střední hodnoty kanálů pro střídavou vazbu výstupu v zadaném čase.
     *
     * Dopočítá stavy st_AUDIO_CTC0_AC a st_AUDIO_PSG_AC do `now_ticks`
     * a vrátí je: `out[0]` = střední hodnota výstupu vazby CTC0 (-1..1),
     * `out[1..]` = střední výchylky kanálů PSG (0..1). Bez
     * AUDIO_HAVE_OUTPUT_COUPLING samé nuly.
     *
     * @param now_ticks Čas (osa gdg_get_total_ticks()).
     * @param out       Výstup, AUDIO_SRC_CHANNELS_COUNT prvků.
     * @pre Vlákno emulátoru.
     */
    extern void audio_out_ac_means(uint64_t now_ticks, double out[AUDIO_SRC_CHANNELS_COUNT]);

    extern void audio_log_fill_psg(uint64_t total_event_ticks);
    extern void audiolog_finish_20ms_frame(uint64_t total_event_ticks);
    extern void audio_log_destroy(st_AUDIO_LOG *audio_log);
    extern void audio_reset_log(uint64_t new_timestamp);

    /**
     * @brief Fáze krokování PSG: tiky GDG od posledního kroku PSG.
     *
     * PSG se krokuje po PSG_DIVIDER ticích od g_audio.log->last_psg_timestamp,
     * líně (při zápisu do PSG a na konci snímku). Fáze je čas, který ještě
     * nebyl odkrokovaný; snapshot ji ukládá, aby po načtení PSG krokoval
     * ve stejné mřížce jako bez načtení (snap_audio.c).
     *
     * @param now_ticks Aktuální čas GDG (gdg_get_total_ticks()).
     * @return now_ticks - last_psg_timestamp, 0 když je log v budoucnosti.
     * Threading: emu vlákno.
     */
    extern uint64_t audio_psg_phase_ticks(uint64_t now_ticks);

    extern void audio_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
