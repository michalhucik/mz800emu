/**
 * @file   mztape.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.4.0
 * @brief  Veřejné API knihovny mztape — konverze MZF souborů na CMT audio streamy.
 *
 * Definuje typy, enumy, struktury a funkce pro práci s kazetovým záznamem
 * počítačů Sharp MZ. Podporuje více modelů (MZ-700, MZ-800, MZ-1500, MZ-80B)
 * a různé rychlosti záznamu.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 * - 2026-11-09: Doplněny vlastní délky pulzů a hlavička CMTSPEED (UniCMT); pulzní
 *   sady převzaty z mz800emu: výchozí sada pro vstream změřená v emulátoru z výstupu
 *   Intercopy (takt GDG), sada cmt.com změřená stejně, údaj servisního manuálu jen
 *   jako záložní sada bitstreamu.
 * - 2026-11-09: kontrola rychlosti v bitstreamu (2.3.1)
 * - 2026-11-10: mztape_get_format() - definice formátové varianty jen pro čtení (2.4.0)
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */


#ifndef MZTAPE_H
#define MZTAPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#include "libs/mzf/mzf.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "libs/generic_driver/generic_driver.h"

#include "libs/cmtspeed/cmtspeed.h"


    /** @brief Typy bloků tvořících CMT záznam na pásce. */
    typedef enum en_MZTAPE_BLOCK {
        MZTAPE_BLOCK_LGAP = 0, /**< dlouhý GAP = 22 000 short pulzů (10 000 u MZ-80B!) */
        MZTAPE_BLOCK_SGAP,     /**< krátký GAP = 11 000 short pulzů */
        MZTAPE_BLOCK_LTM,      /**< dlouhý tapemark = 40 long + 40 short */
        MZTAPE_BLOCK_STM,      /**< krátký tapemark = 20 long + 20 short */
        MZTAPE_BLOCK_HDR,      /**< hlavička pásky = 128 bajtů (MZF header) */
        MZTAPE_BLOCK_HDRC,     /**< kopie hlavičky pásky */
        MZTAPE_BLOCK_FILE,     /**< tělo souboru = MZF body */
        MZTAPE_BLOCK_FILEC,    /**< kopie těla souboru */
        MZTAPE_BLOCK_CHKH,     /**< 2bajtový checksum hlavičky = 16bit součet jedničkových bitů */
        MZTAPE_BLOCK_CHKF,     /**< 2bajtový checksum těla = 16bit součet jedničkových bitů */
        MZTAPE_BLOCK_2L,       /**< 2 long pulzy (náběžné hrany) */
        MZTAPE_BLOCK_256S,     /**< 256 short pulzů */
        MZTAPE_BLOCK_STOP,     /**< terminátor — ukončení pole bloků (není skutečný blok) */
    } en_MZTAPE_BLOCK;


    /** @brief Délky jednoho pulzu (high + low část) v sekundách. */
    typedef struct st_MZTAPE_PULSE_LENGTH {
        double high;  /**< doba 1. části pulzu (H) */
        double low;   /**< doba 2. části pulzu (L) */
        double total; /**< celková doba pulzu (H + L) */
    } st_MZTAPE_PULSE_LENGTH;


    /** @brief Pulzní sady pro různé modely Sharp MZ. */
    typedef enum en_MZTAPE_PULSESET {
        MZTAPE_PULSESET_700 = 0, /**< MZ-700, MZ-80K, MZ-80A */
        MZTAPE_PULSESET_800,     /**< MZ-800, MZ-1500 */
        MZTAPE_PULSESET_80B,     /**< MZ-80B */
        MZTAPE_PULSESET_COUNT    /**< počet platných hodnot (sentinel) */
    } en_MZTAPE_PULSESET;


    /** @brief Dvojice délek pulzů — long (bit "1") a short (bit "0"). */
    typedef struct st_MZTAPE_PULSES_LENGTH {
        st_MZTAPE_PULSE_LENGTH long_pulse;  /**< pulz reprezentující "1" */
        st_MZTAPE_PULSE_LENGTH short_pulse; /**< pulz reprezentující "0" */
    } st_MZTAPE_PULSES_LENGTH;


    /** @brief Definice formátu CMT záznamu — délky mezer, pulzní sada a sekvence bloků. */
    typedef struct st_MZTAPE_FORMAT {
        uint32_t lgap;             /**< počet short pulzů v dlouhém GAPu */
        uint32_t sgap;             /**< počet short pulzů v krátkém GAPu */
        en_MZTAPE_PULSESET pulseset; /**< pulzní sada pro daný model */
        en_MZTAPE_BLOCK *blocks;   /**< pole bloků zakončené MZTAPE_BLOCK_STOP */
    } st_MZTAPE_FORMAT;


    /** @brief Formátové varianty CMT záznamu. */
    typedef enum en_MZTAPE_FORMATSET {
        MZTAPE_FORMATSET_MZ800_SANE = 0, /**< header + body, kratší GAP (6400/11000), pulseset MZ-800 */
        MZTAPE_FORMATSET_MZ800 = 1,      /**< 2x header + 2x body, plný GAP (22000/11000), pulseset MZ-800 */
        MZTAPE_FORMATSET_MZ80B = 2,      /**< header + body, GAP 10000/11000, pulseset MZ-80B (1800 Bd) */
        MZTAPE_FORMATSET_COUNT           /**< počet platných hodnot (sentinel) */
    } en_MZTAPE_FORMATSET;


/** @brief Výchozí délka dlouhého GAPu — 22 000 short pulzů. */
#define MZTAPE_LGAP_LENGTH_DEFAULT 22000
/** @brief Zkrácená délka dlouhého GAPu — 6 400 short pulzů (SANE varianta). */
#define MZTAPE_LGAP_LENGTH_SANE 6400
/** @brief Délka dlouhého GAPu pro MZ-80B — 10 000 short pulzů. */
#define MZTAPE_LGAP_LENGTH_MZ80B 10000
/** @brief Délka krátkého GAPu — 11 000 short pulzů. */
#define MZTAPE_SGAP_LENGTH 11000
    //#define MZTAPE_SGAP_LENGTH_SANE 6400
/** @brief Délka krátkého GAPu pro SANE variantu — 11 000 short pulzů. */
#define MZTAPE_SGAP_LENGTH_SANE 11000
/** @brief Počet long pulzů v dlouhém tapemarku. */
#define MZTAPE_LTM_LLENGTH 40
/** @brief Počet short pulzů v dlouhém tapemarku. */
#define MZTAPE_LTM_SLENGTH 40
/** @brief Počet long pulzů v krátkém tapemarku. */
#define MZTAPE_STM_LLENGTH 20
/** @brief Počet short pulzů v krátkém tapemarku. */
#define MZTAPE_STM_SLENGTH 20

/** @brief Výchozí přenosová rychlost — 1200 baudů. */
#define MZTAPE_DEFAULT_BDSPEED 1200


    /** @brief MZF data připravená pro konverzi na CMT stream — raw hlavička, tělo a checksums. */
    typedef struct st_MZTAPE_MZF {
        uint8_t header[sizeof ( st_MZF_HEADER )]; /**< raw hlavička uložená v originální endianitě */
        uint8_t *body;   /**< alokovaná data těla souboru */
        uint16_t size;   /**< velikost těla v bajtech */
        uint32_t chkh;   /**< checksum hlavičky (počet jedničkových bitů) */
        uint32_t chkb;   /**< checksum těla (počet jedničkových bitů) */
    } st_MZTAPE_MZF;

    /**
     * @brief Uvolní strukturu st_MZTAPE_MZF včetně alokovaného těla.
     * @param mztmzf Ukazatel na strukturu k uvolnění (NULL je bezpečné).
     */
    extern void mztape_mztmzf_destroy ( st_MZTAPE_MZF *mztmzf );

    /**
     * @brief Vytvoří st_MZTAPE_MZF načtením MZF hlavičky a těla z handleru.
     * @param mzf_handler Handler otevřeného MZF souboru.
     * @param offset Offset v handleru, odkud začíná MZF hlavička.
     * @return Ukazatel na novou strukturu, nebo NULL při chybě.
     */
    extern st_MZTAPE_MZF* mztape_create_mztapemzf ( st_HANDLER *mzf_handler, uint32_t offset );

    /**
     * @brief Vytvoří CMT bitstream přímou generací vzorků z MZF dat.
     *
     * Méně přesný path — zaokrouhlovací chyba se akumuluje pulz po pulzu.
     * Pro vyšší rychlosti doporučen vstream path.
     *
     * @param mztmzf MZF data.
     * @param mztape_format Formátová varianta záznamu.
     * @param mztape_speed Rychlost záznamu (platný poměr, cmtspeed_is_valid()).
     * @param sample_rate Vzorkovací frekvence výstupního bitstreamu (Hz).
     * @return Ukazatel na nový bitstream (vlastník je volající), nebo NULL při
     *         chybě. Chybou je i neplatný formát a neplatná rychlost
     *         (CMTSPEED_NONE, CMTSPEED_CUSTOM).
     */
    extern st_CMT_BITSTREAM* mztape_create_cmt_bitstream_from_mztmzf ( st_MZTAPE_MZF *mztmzf, en_MZTAPE_FORMATSET mztape_format, en_CMTSPEED mztape_speed, uint32_t sample_rate );

    /**
     * @brief Vytvoří CMT vstream z MZF dat (RLE kódování pulzů).
     *
     * Přesnější path — zaokrouhlení probíhá nezávisle pro každý pulz.
     *
     * @param mztmzf MZF data.
     * @param mztape_format Formátová varianta záznamu.
     * @param mztape_speed Rychlost záznamu.
     * @param rate Vzorkovací frekvence výstupního vstreamu (Hz).
     * @return Ukazatel na nový vstream, nebo NULL při chybě.
     */
    extern st_CMT_VSTREAM* mztape_create_cmt_vstream_from_mztmzf ( st_MZTAPE_MZF *mztmzf, en_MZTAPE_FORMATSET mztape_format, en_CMTSPEED mztape_speed, uint32_t rate );

    /**
     * @brief Jednotné API pro vytvoření CMT streamu (bitstream nebo vstream).
     *
     * Pro bitstream interně vytváří vstream a konvertuje (přesnější výsledek).
     *
     * @param mztmzf MZF data.
     * @param cmtspeed Rychlost záznamu.
     * @param type Typ výstupního streamu (bitstream/vstream).
     * @param mztape_fset Formátová varianta záznamu.
     * @param rate Vzorkovací frekvence (Hz).
     * @return Ukazatel na nový stream, nebo NULL při chybě.
     */
    extern st_CMT_STREAM* mztape_create_stream_from_mztapemzf ( st_MZTAPE_MZF *mztmzf, en_CMTSPEED cmtspeed, en_CMT_STREAM_TYPE type, en_MZTAPE_FORMATSET mztape_fset, uint32_t rate );

    /**
     * @brief Rozšířená verze s volitelným přepisem délek pulzů.
     *
     * Jsou-li všechny čtyři hodnoty long_high_us100 .. short_low_us100
     * nulové, chová se jako mztape_create_stream_from_mztapemzf() (délky
     * z pulzní sady a rychlosti). Jinak se délky převedou na µs
     * (hodnota / 10), naplní přes mztape_pulses_set_us() a záznam vznikne
     * přes mztape_create_stream_from_mztapemzf_pulses(). Hodnoty jsou
     * v jednotkách 0,1 µs (sekundy = hodnota / 1e7); přípona _us100 ve
     * jménech je historická. Částečně vyplněné hodnoty (některá nulová,
     * jiná ne) jsou chyba.
     *
     * @param mztmzf MZF data.
     * @param cmtspeed Rychlost záznamu (ignorována pokud pulse fields != 0).
     * @param type Typ výstupního streamu (bitstream/vstream).
     * @param mztape_fset Formátová varianta záznamu.
     * @param rate Vzorkovací frekvence (Hz).
     * @param long_high_us100  Délka HIGH části dlouhého pulzu (0,1 µs, 0 = výchozí).
     * @param long_low_us100   Délka LOW části dlouhého pulzu (0,1 µs, 0 = výchozí).
     * @param short_high_us100 Délka HIGH části krátkého pulzu (0,1 µs, 0 = výchozí).
     * @param short_low_us100  Délka LOW části krátkého pulzu (0,1 µs, 0 = výchozí).
     * @return Ukazatel na nový stream (vlastník je volající), nebo NULL při
     *         chybě (mimo jiné částečně nulové délky nebo část pulzu kratší
     *         než půl vzorku).
     */
    extern st_CMT_STREAM* mztape_create_stream_from_mztapemzf_ex ( st_MZTAPE_MZF *mztmzf, en_CMTSPEED cmtspeed, en_CMT_STREAM_TYPE type, en_MZTAPE_FORMATSET mztape_fset, uint32_t rate,
                                                                     uint16_t long_high_us100, uint16_t long_low_us100,
                                                                     uint16_t short_high_us100, uint16_t short_low_us100 );

    /**
     * @brief Spočítá celkový počet long a short pulzů pro daný MZF a formát.
     * @param mztmzf MZF data.
     * @param mztape_format Formátová varianta záznamu.
     * @param[out] long_pulses Výstup — celkový počet long pulzů.
     * @param[out] short_pulses Výstup — celkový počet short pulzů.
     */
    extern void mztape_compute_pulses ( st_MZTAPE_MZF *mztmzf, en_MZTAPE_FORMATSET mztape_format, uint64_t *long_pulses, uint64_t *short_pulses );

    /**
     * @brief Délky půlvln pulzní sady při rychlosti 1:1.
     *
     * Hodnoty tabulky pulzních sad (g_mztape_pulses_700, g_mztape_pulses_800,
     * g_mztape_pulses_80B), kterou používá bitstream path. Sada MZ-800 je
     * údaj servisního manuálu (LONG 470/494 µs, SHORT 240/278 µs); vstream
     * path MZ-800 hraje jinou sadu (viz mztape_get_speed_pulses()). Slouží
     * k zobrazení a pro nástroje. Profil TAPEMZ-V1-LEGACY má vlastní
     * historické sady v1.0 (viz tmz_profile_legacy_pulses()). Pulzy cmt.com
     * (rychlost 2:1 CPM) sem nepatří.
     *
     * @param pulseset Pulzní sada.
     * @param[out] out LONG H, LONG L, SHORT H, SHORT L v sekundách.
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE (out je NULL, neplatná sada).
     */
    extern int mztape_get_pulse_lengths ( en_MZTAPE_PULSESET pulseset, double out[4] );

    /**
     * @brief Vrátí definici formátové varianty záznamu (jen pro čtení).
     *
     * Tytéž hodnoty, podle kterých generují záznam
     * mztape_create_cmt_vstream_from_mztmzf() a ostatní cesty: počet SHORT
     * dlouhého a krátkého tónu (lgap, sgap), pulzní sadu a posloupnost bloků.
     * Počty pulzů tapemarků jsou společné všem variantám (MZTAPE_LTM_* a
     * MZTAPE_STM_*). Slouží knihovnám, které mají záznam mztape popsat
     * jinými prostředky (profil MZ800EMU-CMT v tmz_profile), aby hodnoty
     * měly jediný zdroj. Funkce nemá vedlejší efekty.
     *
     * @param mztape_format Formátová varianta záznamu.
     * @return Ukazatel na statickou definici platnou po celou dobu běhu
     *         programu (volající ji nesmí měnit ani uvolnit, ani pole blocks),
     *         nebo NULL pro neplatnou variantu.
     */
    extern const st_MZTAPE_FORMAT* mztape_get_format ( en_MZTAPE_FORMATSET mztape_format );

    /** @brief Pole podporovaných rychlostí záznamu, zakončené CMTSPEED_NONE. */
    extern const en_CMTSPEED g_mztape_speed[];


/** @brief Největší přípustná délka jedné části pulzu v µs (rozsah uint16 hlavičky CMTSPEED). */
#define MZTAPE_PULSE_US_MAX 65535.0

    /**
     * @brief Naplní délky pulzů z hodnot v mikrosekundách (vlastní délky pulzů).
     *
     * Pořadí parametrů odpovídá hlavičce CMTSPEED zařízení UniCMT: LONG high,
     * LONG low, SHORT high, SHORT low. Délky se uloží v sekundách včetně
     * součtů total.
     *
     * @param[out] pulses Cílová struktura (nesmí být NULL).
     * @param long_high_us Délka high části dlouhého pulzu (bit "1") v µs.
     * @param long_low_us Délka low části dlouhého pulzu v µs.
     * @param short_high_us Délka high části krátkého pulzu (bit "0") v µs.
     * @param short_low_us Délka low části krátkého pulzu v µs.
     * @return EXIT_SUCCESS; EXIT_FAILURE, pokud některá délka není v rozsahu
     *         (0, MZTAPE_PULSE_US_MAX] (i NaN) - pak se *pulses nemění.
     */
    extern int mztape_pulses_set_us ( st_MZTAPE_PULSES_LENGTH *pulses, double long_high_us, double long_low_us, double short_high_us, double short_low_us );

    /**
     * @brief Vrátí délky pulzů, se kterými se generuje záznam v daném poměru rychlosti.
     *
     * Jde o tytéž délky, které používá mztape_create_cmt_vstream_from_mztmzf()
     * (pro MZ-800 sada změřená v emulátoru z výstupu Intercopy, pro 2:1 CP/M
     * sada cmt.com), vydělené poměrem. Slouží k zobrazení a jako výchozí bod
     * pro vlastní délky pulzů.
     *
     * @param mztape_format Formátová varianta záznamu (určuje pulzní sadu).
     * @param mztape_speed Poměr rychlosti (cmtspeed_is_valid()).
     * @param[out] pulses Výstupní délky v sekundách (nesmí být NULL).
     * @return EXIT_SUCCESS; EXIT_FAILURE pro neplatný formát nebo rychlost
     *         (včetně CMTSPEED_CUSTOM) - pak se *pulses nemění.
     */
    extern int mztape_get_speed_pulses ( en_MZTAPE_FORMATSET mztape_format, en_CMTSPEED mztape_speed, st_MZTAPE_PULSES_LENGTH *pulses );

    /**
     * @brief Spočítá ekvivalentní poměr rychlosti vlastních délek pulzů.
     *
     * Poměr = (dlouhý + krátký pulz při 1:1) / (dlouhý + krátký pulz zadaný),
     * délky 1:1 podle mztape_get_speed_pulses(). Je to jen orientační údaj
     * pro zobrazení (UniCMT 3x: asi 3,03).
     *
     * @param mztape_format Formátová varianta záznamu (určuje délky 1:1).
     * @param pulses Délky pulzů (NULL vrátí 0).
     * @return Poměr > 0; 0 pro neplatný formát, NULL nebo nulové délky.
     */
    extern double mztape_pulses_get_ratio ( en_MZTAPE_FORMATSET mztape_format, const st_MZTAPE_PULSES_LENGTH *pulses );


/** @brief Offset délek pulzů v hlavičce CMTSPEED (4x uint16 LE v µs). */
#define MZTAPE_UNICMT_PULSES_OFFSET 0x30

    /** @brief Výsledek rozpoznání hlavičky CMTSPEED zařízení UniCMT. */
    typedef enum en_MZTAPE_UNICMT_MARKER {
        MZTAPE_UNICMT_MARKER_INVALID = -1, /**< hlavička CMTSPEED s nepoužitelnými délkami (nula) */
        MZTAPE_UNICMT_MARKER_NONE = 0,     /**< nejde o hlavičku CMTSPEED (běžný MZF blok) */
        MZTAPE_UNICMT_MARKER_VALID = 1,    /**< hlavička CMTSPEED, délky pulzů načteny */
    } en_MZTAPE_UNICMT_MARKER;

    /**
     * @brief Rozpozná hlavičku CMTSPEED zařízení UniCMT a načte z ní délky pulzů.
     *
     * Hlavička CMTSPEED je 128bajtový MZF blok bez těla vložený do MZT mezi
     * díly pásky; mění rychlost všech dalších dílů. Za hlavičku se považuje
     * blok, který má zároveň typ 00h, jméno "CMTSPEED" ukončené 0Dh a délku
     * těla 0. Na offsetu MZTAPE_UNICMT_PULSES_OFFSET jsou 4x uint16 LE v µs:
     * LONG high, LONG low, SHORT high, SHORT low (formát ověřen hexdumpem
     * souborů 1xspeed.mzf, 2xspeed.mzf a 3xspeed.mzf z UniCMT; pořadí slov
     * je odvozené ze shody sady 1x s údajem servisního manuálu 470/494,
     * 240/278 µs). Bajt 20h (u všech známých souborů 01h, význam neznámý)
     * se nekontroluje.
     *
     * @param header Surová hlavička MZF, 128 bajtů v pořadí ze souboru (nesmí být NULL).
     * @param[out] pulses Při MZTAPE_UNICMT_MARKER_VALID délky pulzů; jinak se
     *             nemění. Smí být NULL (pak jen rozpoznání).
     * @return en_MZTAPE_UNICMT_MARKER.
     */
    extern en_MZTAPE_UNICMT_MARKER mztape_unicmt_speed_marker ( const uint8_t *header, st_MZTAPE_PULSES_LENGTH *pulses );

    /**
     * @brief Vytvoří CMT vstream z MZF dat s vlastními délkami pulzů.
     *
     * Stejný záznam jako mztape_create_cmt_vstream_from_mztmzf(), jen délky
     * pulzů nejsou dané poměrem, ale parametrem (např. hlavička CMTSPEED).
     * Každá část pulzu se zaokrouhlí na celý počet vzorků samostatně.
     *
     * @param mztmzf MZF data.
     * @param mztape_format Formátová varianta záznamu (bloky, délky GAPů).
     * @param pulses Délky pulzů v sekundách (NULL je chyba).
     * @param rate Vzorkovací frekvence výstupního vstreamu (Hz).
     * @return Ukazatel na nový vstream (vlastník je volající), nebo NULL při
     *         chybě - mimo jiné, když by některá část pulzu měla 0 vzorků.
     */
    extern st_CMT_VSTREAM* mztape_create_cmt_vstream_from_mztmzf_pulses ( st_MZTAPE_MZF *mztmzf, en_MZTAPE_FORMATSET mztape_format, const st_MZTAPE_PULSES_LENGTH *pulses, uint32_t rate );

    /**
     * @brief Jednotné API pro vytvoření CMT streamu s vlastními délkami pulzů.
     *
     * Obdoba mztape_create_stream_from_mztapemzf(); bitstream vzniká
     * převodem vstreamu.
     *
     * @param mztmzf MZF data.
     * @param pulses Délky pulzů v sekundách (NULL je chyba).
     * @param type Typ výstupního streamu (bitstream/vstream).
     * @param mztape_fset Formátová varianta záznamu.
     * @param rate Vzorkovací frekvence (Hz).
     * @return Ukazatel na nový stream (vlastník je volající), nebo NULL při chybě.
     */
    extern st_CMT_STREAM* mztape_create_stream_from_mztapemzf_pulses ( st_MZTAPE_MZF *mztmzf, const st_MZTAPE_PULSES_LENGTH *pulses, en_CMT_STREAM_TYPE type, en_MZTAPE_FORMATSET mztape_fset, uint32_t rate );


    /** @brief Uživatelský alokátor — umožňuje nahradit výchozí malloc/calloc/free. */
    typedef struct st_MZTAPE_ALLOCATOR {
        void* (*alloc)(size_t size);   /**< alokace paměti (jako malloc) */
        void* (*alloc0)(size_t size);  /**< alokace s nulováním (jako calloc) */
        void  (*free)(void *ptr);      /**< uvolnění paměti */
    } st_MZTAPE_ALLOCATOR;

    /**
     * @brief Nastaví vlastní alokátor pro knihovnu mztape.
     * @param allocator Ukazatel na strukturu alokátoru, nebo NULL pro reset na výchozí.
     */
    extern void mztape_set_allocator ( const st_MZTAPE_ALLOCATOR *allocator );


    /** @brief Typ callback funkce pro hlášení chyb. */
    typedef void (*mztape_error_cb)(const char *func, int line, const char *fmt, ...);

    /**
     * @brief Nastaví vlastní callback pro chybová hlášení.
     * @param cb Callback funkce, nebo NULL pro reset na výchozí (fprintf stderr).
     */
    extern void mztape_set_error_callback ( mztape_error_cb cb );

    /** @brief Verze knihovny mztape. */
#define MZTAPE_VERSION "2.4.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny mztape.
     * @return Statický řetězec s verzí (např. "2.0.0").
     */
    extern const char* mztape_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZTAPE_H */

