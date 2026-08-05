/*
 * File:   psg.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 23. července 2015, 7:32
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

#ifndef PSG_H
#define PSG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "mzarch/mzcommon_config.h"
#include "audio.h"

#include <stdio.h>

    // pokud je definovano, tak pri inicializaci emulujeme nahodnou desynchronizaci psg.process_clock
#define PSG_USE_RANDOM_INIT 1

#define PSG_CH_LEFT   0x01   /* bit 0 - levý PSG (PSG0) */
#define PSG_CH_RIGHT  0x02   /* bit 1 - pravý PSG (PSG1) */
#define PSG_MAX_COUNT 2

    typedef enum en_PSG_CHANNELS
    {
        PSG_CHANNEL_0 = 0,
        PSG_CHANNEL_1,
        PSG_CHANNEL_2,
        PSG_CHANNEL_3,
        PSG_CHANNELS_COUNT
    } en_PSG_CHANNELS;

    typedef enum en_PSG_CHTYPE
    {
        PSG_CHTYPE_TONE,  /* square wave tone generator */
        PSG_CHTYPE_NOISE, /* periodic noise | white noise */
    } en_PSG_CHTYPE;

    typedef enum en_ATTENUATOR
    {
        PSG_OUT_MAX = 0, /* hodnota 0, utlum 0 db = max. hlasitost */
        PSG_OUT_LVL_14,  /* utlum 2 db */
        PSG_OUT_LVL_13,  /* utlum 4 db */
        PSG_OUT_LVL_12,  /* utlum 6 db */
        PSG_OUT_LVL_11,  /* utlum 8 db */
        PSG_OUT_LVL_10,  /* utlum 10 db */
        PSG_OUT_LVL_9,   /* utlum 12 db */
        PSG_OUT_LVL_8,   /* utlum 14 db */
        PSG_OUT_LVL_7,   /* utlum 16 db */
        PSG_OUT_LVL_6,   /* utlum 18 db */
        PSG_OUT_LVL_5,   /* utlum 20 db */
        PSG_OUT_LVL_4,   /* utlum 22 db */
        PSG_OUT_LVL_3,   /* utlum 24 db */
        PSG_OUT_LVL_2,   /* utlum 26 db */
        PSG_OUT_LVL_1,   /* utlum 28 db */
        PSG_OUT_OFF      /* hodnota 15, zvukovy vystup kanalu je vypnuty */
    } en_ATTENUATOR;

    typedef enum en_NOISE_DIV_TYPE
    {
        NOISE_DIV_TYPE0 = 0, /* noise divider 0x10, 6.928 kHz pri CPUDIV = 5 */
        NOISE_DIV_TYPE1,     /* noise divider 0x20, 3.464 kHz pri CPUDIV = 5 */
        NOISE_DIV_TYPE2,     /* noise divider 0x40, 1.732 kHz pri CPUDIV = 5 */
        NOISE_DIV_TYPE3,     /* noise divider nastavit podle channel 2 */
    } en_NOISE_DIV_TYPE;

    typedef enum en_NOISE_TYPE
    {
        NOISE_TYPE_PERIODIC = 0,
        NOISE_TYPE_WHITE
    } en_NOISE_TYPE;

    typedef struct st_PSG_NOISE
    {
        en_NOISE_DIV_TYPE div_type;
        en_NOISE_TYPE type; /* periodic noise | white noise */
        en_NOISE_TYPE last_noise_type;
        uint16_t shiftregister;
    } st_PSG_NOISE;

    typedef struct st_PSG_TONE
    {
        unsigned divider;
        unsigned latch_divider;
    } st_PSG_TONE;

    typedef struct st_PSG_CHANNEL
    {
        en_PSG_CHTYPE type;
        unsigned timer;
        en_ATTENUATOR attn;
        st_PSG_TONE tone;
        st_PSG_NOISE noise;
        unsigned output_signal;
    } st_PSG_CHANNEL;

    typedef struct st_PSG
    {
        unsigned latch_cs;
        unsigned latch_attn;
        st_PSG_CHANNEL channel[PSG_CHANNELS_COUNT];
    } st_PSG;

    typedef struct st_PSG_MODULE {
        bool stereo;                     /* false = mono (1 PSG), true = stereo (2 PSG) */
        st_PSG psg[PSG_MAX_COUNT];       /* psg[0] = levý/mono, psg[1] = pravý */
    } st_PSG_MODULE;

    extern st_PSG_MODULE g_psg_module;

    /* Zpětná kompatibilita - g_psg odkazuje na PSG0 (levý/mono) */
#define g_psg (g_psg_module.psg[0])

#define PSG_COUNT_VOLUME_LEVELS 16

    extern void psg_instance_init(st_PSG *psg);
    extern void psg_instance_step(st_PSG *psg);
    extern void psg_init(bool stereo);
    extern void psg_write_byte(unsigned channel, uint8_t value);
    extern void psg_step(void);

    /* ===================================================================== */
    /* Debug/UI mirror API - side-effect free čtení stavu PSG.               */
    /*                                                                       */
    /* Tyto gettery NEMODIFIKUJÍ žádný field st_PSG. Jsou určené pro IO      */
    /* Ports debug okno a budoucí per-chip detail panel (V1.7+ 2.4).         */
    /* SN76489 je write-only chip - mirror je čistá snapshot interních       */
    /* registrů, žádný side-effect od skutečného IORQ READ neexistuje (PSG  */
    /* read na portu 0xE9/F2/F3 vrací open bus 0xFFh).                       */
    /* ===================================================================== */

    /**
     * @brief Aktuální latched channel select (CS) pro další DATA byte.
     *
     * Bity [6:5] posledního LATCH bytu (= byte s bit 7 = 1). Určuje
     * který ze 4 kanálů přijme následující DATA byte.
     *
     * @param psg ukazatel na PSG instanci (NESMÍ být NULL).
     * @return 0..3 (en_PSG_CHANNELS)
     */
    extern unsigned psg_mirror_latch_cs(const st_PSG *psg);

    /**
     * @brief Aktuální latched attn flag pro další DATA byte.
     *
     * Bit [4] posledního LATCH bytu. 1 = další DATA byte aktualizuje
     * channel attenuation; 0 = další DATA byte aktualizuje tone divider
     * (TONE kanály) resp. noise div_type+type (NOISE kanál).
     *
     * @param psg ukazatel na PSG instanci (NESMÍ být NULL).
     * @return 0 nebo nenulová hodnota (interně byte hodnota bitu 4)
     */
    extern unsigned psg_mirror_latch_attn(const st_PSG *psg);

    /**
     * @brief Typ kanálu (TONE / NOISE).
     *
     * Per-channel statická informace nastavená v `psg_instance_init`:
     * kanály 0-2 jsou TONE, kanál 3 je NOISE.
     *
     * @param psg ukazatel na PSG instanci (NESMÍ být NULL).
     * @param ch index kanálu (0..3).
     * @return en_PSG_CHTYPE (PSG_CHTYPE_TONE nebo PSG_CHTYPE_NOISE)
     */
    extern en_PSG_CHTYPE psg_mirror_channel_type(const st_PSG *psg, unsigned ch);

    /**
     * @brief Attenuation kanálu (4-bit volume).
     *
     * 0 = max volume (0 dB), 0xF = OFF (kanál vypnutý), krok -2 dB.
     * Hodnota je platná pro VŠECHNY kanály (TONE i NOISE).
     *
     * @param psg ukazatel na PSG instanci (NESMÍ být NULL).
     * @param ch index kanálu (0..3).
     * @return 0..15 (en_ATTENUATOR)
     */
    extern unsigned psg_mirror_channel_attn(const st_PSG *psg, unsigned ch);

    /**
     * @brief Tone divider TONE kanálu (10-bit).
     *
     * Frekvence square wave = INPUT_CLK / (divider * 32). Smysluplné
     * jen pro TONE kanály (0..2). Pro NOISE kanál (3) vrací raw obsah
     * `.tone.divider` field (typicky 0 / nedefinováno).
     *
     * @param psg ukazatel na PSG instanci (NESMÍ být NULL).
     * @param ch index kanálu (0..3).
     * @return 0..0x3FF (10-bit)
     */
    extern uint16_t psg_mirror_channel_tone_divider(const st_PSG *psg, unsigned ch);

    /**
     * @brief Noise divider type (NOISE kanál).
     *
     * 2-bit hodnota: 0 = /16 (6.928 kHz @ CPUDIV=5), 1 = /32 (3.464 kHz),
     * 2 = /64 (1.732 kHz), 3 = řízeno tone divider channel 2. Smysluplné
     * jen pro NOISE kanál (3); pro TONE kanály (0..2) vrací raw obsah
     * `.noise.div_type` field.
     *
     * @param psg ukazatel na PSG instanci (NESMÍ být NULL).
     * @param ch index kanálu (0..3).
     * @return en_NOISE_DIV_TYPE (0..3)
     */
    extern en_NOISE_DIV_TYPE psg_mirror_channel_noise_div_type(const st_PSG *psg, unsigned ch);

    /**
     * @brief Noise feedback type (NOISE kanál).
     *
     * 1-bit: 0 = periodic noise, 1 = white noise. Smysluplné jen pro
     * NOISE kanál (3); pro TONE kanály (0..2) vrací raw obsah
     * `.noise.type` field.
     *
     * @param psg ukazatel na PSG instanci (NESMÍ být NULL).
     * @param ch index kanálu (0..3).
     * @return en_NOISE_TYPE (NOISE_TYPE_PERIODIC nebo NOISE_TYPE_WHITE)
     */
    extern en_NOISE_TYPE psg_mirror_channel_noise_type(const st_PSG *psg, unsigned ch);

    /* ===================================================================== */
    /* PSG Write Log - autoritativní 1:1 záznam každého register zápisu.    */
    /*                                                                       */
    /* Záznam se odebírá přímo v `psg_write_byte()` (= hook), tj. zachytí    */
    /* každý byte přesně tak, jak ho CPU zapíše na PSG datový port. Slouží  */
    /* pro offline rekonstrukci PSG stavu (replay v external tool) - krok   */
    /* po kroku, bit po bitu.                                                */
    /*                                                                       */
    /* Kontrast vůči polling sample/event logu v UI vrstvě: tyto polling    */
    /* logy zachycují stav PSG mezi UI tick (= ~60 Hz), takže sub-tick      */
    /* eventy propadnou. Write-log je autoritativní (= 1:1).                */
    /*                                                                       */
    /* Formát: TSV s headerem (emulator_clock_hz, stereo flag), per řádek   */
    /*   pxclk_ticks <TAB> channel_mask <TAB> raw_byte_hex                   */
    /*                                                                       */
    /* Thread safety: enable/disable se volá z UI vlákna; record z emu      */
    /* vlákna. Vnitřní GLib GMutex chrání file pointer a enabled flag.      */
    /* Performance: PSG zápisy jsou low-frequency (~100-1000/s), režie mutex */
    /* je zanedbatelná. Při disabled se vrací z record-path bez mutexu      */
    /* (atomic check g_psg_write_log.enabled).                               */
    /* ===================================================================== */

    /**
     * @brief Zapne write log a otevře cílový soubor.
     *
     * Pokud je již log aktivní, nejdřív zavře předchozí soubor a otevře
     * nový s daným path. Zapíše TSV header (metadata + sloupce). Voláno
     * typicky z UI vlákna.
     *
     * Vedlejší efekty: vytvoří/přepíše soubor na disku, alokuje GMutex
     * (jednorázově, lazy).
     *
     * @param path  Cílová cesta TSV souboru (UTF-8). NESMÍ být NULL.
     * @return  true = otevřeno OK; false = open selhal (errno nastaveno).
     */
    extern bool psg_write_log_enable(const char *path);

    /**
     * @brief Vypne write log a zavře soubor.
     *
     * Po návratu jsou všechny zápisy zahozeny (no-op). Bezpečné volat
     * opakovaně. Voláno typicky z UI vlákna.
     *
     * Vedlejší efekty: fflush + fclose souboru.
     */
    extern void psg_write_log_disable(void);

    /**
     * @brief Vrací true pokud je write log právě aktivní.
     *
     * Atomic snapshot enabled flagu; bezpečné z libovolného vlákna.
     */
    extern bool psg_write_log_is_enabled(void);

    /**
     * @brief Vrací počet řádků zapsaných od posledního enable.
     *
     * Pro UI status indikátor. Atomic snapshot; bezpečné z UI vlákna.
     */
    extern unsigned psg_write_log_row_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PSG_H */
