/**
 * @file   wav_analyzer.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.15.0
 * @brief  Hlavní API knihovny wav_analyzer - orchestrace všech vrstev.
 *
 * Poskytuje vysokoúrovňové API pro analýzu WAV nahrávek magnetofonových
 * kazet Sharp MZ. Interně orchestruje vrstvy 0-4:
 *   0. Preprocessing (DC offset, HP filtr, normalizace)
 *   1. Pulse Extraction (zero-crossing / Schmitt trigger)
 *   2. Leader Detection (detekce leader tónů)
 *   2b. Histogram Analysis (diagnostika, auto prahy)
 *   3. Format Classification (NORMAL/SINCLAIR/CPM/MZ-80B/TURBO/FASTIPL/BSD/FSK/SLOW/DIRECT)
 *   4. Specialized Decoding (FM, TURBO, FASTIPL, BSD, CPM-TAPE, FSK, SLOW, DIRECT dekodéry)
 *
 * Výstupem jsou dekódované MZF soubory a diagnostické informace.
 *
 * Vstupem je soubor WAV (wav_analyzer_analyze(), vrstvy 0-4), nebo hotová
 * posloupnost pulzů (wav_analyzer_analyze_pulses(), vrstvy 2-4), např.
 * hrany výstupu magnetofonu zaznamenané emulátorem s libovolnou frekvencí.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
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


#ifndef WAV_ANALYZER_H
#define WAV_ANALYZER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/wav/wav.h"
#include "libs/mzf/mzf.h"

#include "wav_analyzer_types.h"
#include "wav_preprocess.h"
#include "wav_pulse.h"
#include "wav_leader.h"
#include "wav_histogram.h"
#include "wav_classify.h"
#include "wav_decode_fm.h"
#include "wav_decode_turbo.h"
#include "wav_decode_fastipl.h"
#include "wav_decode_bsd.h"
#include "wav_decode_cpmtape.h"
#include "wav_decode_cpmtape.h"
#include "wav_decode_fsk.h"
#include "wav_decode_slow.h"
#include "wav_decode_direct.h"
#include "wav_decode_zx.h"


    /**
     * @brief Program, který záznam zapsal (rozpoznaný z délky tónu a parametrů na pásce).
     *
     * Slouží k volbě profilu bloku 0x46 (TC-1.21, IC-10.2-*). UNKNOWN = zapisovač
     * nejde určit nebo na volbě bloku nezáleží (ROM, BASIC, mzftools, ...).
     */
    typedef enum en_WAV_WRITER {
        WAV_WRITER_UNKNOWN = 0,   /**< neurčen; blok podle formátu jako dřív */
        WAV_WRITER_TURBOCOPY,     /**< TurboCopy V1.2x: tón počet x S = 3 495 500, poměr v tc_a/tc_b */
        WAV_WRITER_INTERCOPY,     /**< Intercopy V10.2: tón 11000 (hlavička) / 5500 (tělo), rychlost v ic_bd */
    } en_WAV_WRITER;

    /**
     * @brief Výsledek analýzy jednoho MZF souboru z WAV.
     *
     * Obsahuje dekódovaný MZF soubor, detekovaný formát,
     * CRC status hlavičky a těla a diagnostické informace.
     */
    typedef struct st_WAV_ANALYZER_FILE_RESULT {
        st_MZF *mzf;                       /**< dekódovaný MZF soubor (vlastní paměť, NULL pro ZX bloky) */
        uint8_t *tap_data;                 /**< surová TAP data pro ZX bloky (flag + payload + checksum, heap) */
        uint32_t tap_data_size;            /**< velikost TAP dat v bajtech (0 pro MZ formáty) */
        en_WAV_TAPE_FORMAT format;          /**< detekovaný kazetový formát */
        en_WAV_CRC_STATUS header_crc;       /**< CRC status hlavičky */
        en_WAV_CRC_STATUS body_crc;         /**< CRC status těla */
        st_WAV_LEADER_INFO leader;          /**< informace o leader tónu */
        en_WAV_SPEED_CLASS speed_class;     /**< rychlostní třída */
        int copy2_used;                     /**< 1 = data pochází z Copy2 (druhé kopie) */
        uint32_t header_leader_pulse;      /**< pozice header leaderu na pasce (start zaznamu).
                                                Pro dvoudilne formaty (TURBO/FASTIPL) se lisi
                                                od leader.start_index, ktery obsahuje datovy leader. */
        uint32_t consumed_until_pulse;     /**< pozice za posledním zpracovaným pulzem
                                                (pro přeskočení leaderů spotřebovaných
                                                dvoudílnými formáty jako TURBO/FASTIPL) */
        uint32_t recovery_status;          /**< bitové OR z en_WAV_RECOVERY_STATUS (0 = kompletní) */
        uint32_t recovered_bytes;          /**< skutečně dekódovaných bajtů při recovery (0 = kompletní) */
        uint32_t expected_bytes;           /**< očekávaných bajtů (0 = neznáme, např. BSD) */
        double short_pulse_us;             /**< naměřená délka krátkého pulzu (us), 0 = neznámo */
        double long_pulse_us;              /**< naměřená délka dlouhého pulzu (us), 0 = neznámo */
        double start_time_sec;             /**< absolutní čas začátku bloku ve WAV (sekundy od začátku souboru) */
        double duration_sec;               /**< délka bloku v sekundách (od header leaderu po consumed_until) */
        uint8_t cpmtape_variant;           /**< CPM-TAPE: en_WAV_CPMTAPE_VARIANT (jinak WAV_CPMTAPE_NONE) */
        uint16_t cpmtape_bd;               /**< CPM-TAPE: rychlost 2400/3200 Bd (jinak 0) */
        uint32_t bsd_chunks;               /**< BSD: počet přečtených kusů (jinak 0) */
        uint32_t bsd_crc_errors;           /**< BSD: kusy bez platné kopie (chyba CRC v obou kopiích nebo kopie chybí) */
        uint32_t bsd_from_copy;            /**< BSD: kusy, jejichž data jsou z kopie (1. kopie s chybou CRC) */
        int fsl_speed;                     /**< FSK/SLOW: úroveň rychlosti (FSK 0-6, SLOW 0-4) podle bajtu
                                                zpoždění v těle zavaděče a vzorkovací frekvence WAV;
                                                -1 = bajt neodpovídá žádné úrovni. Pro jiné formáty
                                                nemá význam (0). */
        en_WAV_WRITER writer;              /**< rozpoznaný zapisovač (NORMAL, TURBO, FASTIPL), jinak UNKNOWN */
        uint8_t tc_a;                      /**< TURBOCOPY: čitatel poměru a/b v základním tvaru (1-9) */
        uint8_t tc_b;                      /**< TURBOCOPY: jmenovatel poměru a/b (1-9) */
        uint16_t ic_bd;                    /**< INTERCOPY: rychlost 600, 1200, 2400, 2800, 3200 (popisek v Bd) */
        uint16_t header_crc_stored;        /**< checksum hlavičky z pásky; platí při header_crc_stored_valid */
        uint16_t body_crc_stored;          /**< checksum těla z pásky; platí při body_crc_stored_valid */
        int header_crc_stored_valid;       /**< záznam FM s chybou CRC hlavičky: 1 = header_crc_stored je z pásky */
        int body_crc_stored_valid;         /**< záznam FM s chybou CRC těla, jehož checksum se přečetl: 1 = body_crc_stored je z pásky */
        uint32_t copy_diff_bytes;          /**< záznam FM: počet bajtů, ve kterých se liší kopie, ačkoli součet je platný (0 = shoda) */
        uint32_t merged_bytes;             /**< záznam FM složený ze dvou kopií: bajty, které pocházejí z 2. kopie */
        uint32_t bsd_gap_chunks;           /**< BSD: chybějící kusy doplněné nulami (jen s obnovou) */
    } st_WAV_ANALYZER_FILE_RESULT;


    /**
     * @brief Výsledek kompletní analýzy WAV souboru.
     *
     * WAV soubor může obsahovat více MZF souborů (více leader tónů).
     * Výsledek obsahuje seznam všech nalezených souborů.
     *
     * @par Invarianty:
     * - file_count <= file_capacity
     * - pokud file_count > 0, files != NULL
     */
    typedef struct st_WAV_ANALYZER_RESULT {
        st_WAV_ANALYZER_FILE_RESULT *files;     /**< pole výsledků (alokováno na heapu) */
        uint32_t file_count;                    /**< počet nalezených souborů */
        uint32_t file_capacity;                 /**< alokovaná kapacita pole */
        st_WAV_LEADER_LIST leaders;             /**< všechny nalezené leader tóny */
        uint32_t total_pulses;                  /**< celkový počet extrahovaných pulzů */
        uint32_t sample_rate;                   /**< vzorkovací frekvence WAV */
        double wav_duration_sec;                /**< délka WAV souboru v sekundách */
        st_WAV_PULSE_STATS pulse_stats;         /**< statistiky pulzů (průměr, stddev, min, max) */
        st_WAV_ANALYZER_RAW_BLOCK *raw_blocks;  /**< neidentifikované bloky (alokováno na heapu) */
        uint32_t raw_block_count;               /**< počet neidentifikovaných bloků */
        uint32_t raw_block_capacity;            /**< alokovaná kapacita pole raw bloků */
    } st_WAV_ANALYZER_RESULT;


    /**
     * @brief Analyzuje WAV soubor a dekóduje všechny nalezené MZF soubory.
     *
     * Hlavní API funkce. Provádí kompletní pipeline:
     * 1. Parsování WAV hlavičky
     * 2. Preprocessing (dle konfigurace)
     * 3. Extrakce pulzů
     * 4. Detekce všech leader tónů
     * 5. Pro každý leader: klasifikace formátu + dekódování
     *
     * @param h Otevřený handler s WAV daty. Nesmí být NULL.
     * @param config Konfigurace analyzéru. Nesmí být NULL.
     * @param[out] out_result Výstupní výsledek. Nesmí být NULL.
     *             Volající musí uvolnit přes wav_analyzer_result_destroy().
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @post Při úspěchu out_result obsahuje všechny nalezené soubory.
     *       I bez nalezených souborů je výsledek platný (file_count=0).
     */
    extern en_WAV_ANALYZER_ERROR wav_analyzer_analyze (
        st_HANDLER *h,
        const st_WAV_ANALYZER_CONFIG *config,
        st_WAV_ANALYZER_RESULT *out_result
    );


    /**
     * @brief Nejdelší pulz, který wav_analyzer_analyze_pulses() předá analýze (us, 3600 s).
     *
     * Delší pulz (dlouhé ticho, i vzniklé sloučením přes vynechaný nulový
     * pulz) se na tuto délku zkrátí: duration_us na mez, duration_samples
     * nejvýš na odpovídající počet vzorků. Datová půlvlna je i v bloku 0x46
     * nejvýš 1 s (spec v1.1, kap. 5.7), zkrácení se tedy týká jen ticha;
     * posunou se jen časy za ním (start_time_sec, wav_duration_sec). Mez
     * zaručuje, že další vrstvy nedostanou nekonečno ani délku, která by
     * přetekla převody na celá čísla. Hodnota odpovídá nejdelší pauze části
     * bloku 0x46 (tamtéž).
     */
#define WAV_ANALYZER_PULSE_MAX_US 3600000000.0


    /**
     * @brief Analyzuje posloupnost pulzů (místo WAV) a dekóduje všechny nalezené záznamy.
     *
     * Vstupem je signál už rozložený na pulzy (půlvlny), např. hrany výstupu
     * magnetofonu zaznamenané emulátorem s přesností na takt. Provede tytéž
     * kroky jako wav_analyzer_analyze() od detekce leaderů dál (statistiky
     * pulzů, leadery, CPM-TAPE, klasifikace a dekodéry, surové bloky, časy);
     * pro posloupnost, kterou dává extrakce pulzů z WAV, je výsledek shodný.
     * Předzpracování a extrakce pulzů se nepoužijí, volby konfigurace pro ně
     * (pulse_mode, schmitt_*, channel, polarity, enable_*) nemají vliv.
     *
     * Délky se čtou z duration_us (rozhoduje o všem kromě dekodéru DIRECT
     * a surových bloků), duration_samples a level používá dekodér DIRECT
     * (1 vzorek = 1 bit) a sběr surových bloků (keep_unknown, 1 bit na vzorek).
     * Signál DIRECT je definovaný ve vzorcích frekvence, pro kterou vznikl;
     * při jiné frekvenci posloupnosti se jeho data nedekódují.
     * sample_rate smí být 1 až 2^32-1 Hz: tolerance periody tónu zapisovače
     * (1,5 vzorku) má spodní mez 2 us, kvantizační podlaha detektoru leaderu
     * (1 vzorek) se s frekvencí zmenšuje a rozhoduje relativní tolerance,
     * takže přesné pulzy při 17 721 600 Hz se dekódují jako WAV.
     *
     * Pulz s délkou duration_us, která není kladné konečné číslo (0, záporná,
     * NaN, nekonečno), se přeskočí: nenese hranu, a pulz za ním se stejnou
     * úrovní jako předchozí ponechaný pulz se k němu přičte. Pulz delší než
     * WAV_ANALYZER_PULSE_MAX_US (i po sloučení) se na tuto mez zkrátí (zvoleno
     * místo odmítnutí: dlouhé ticho je v záznamu běžné). Bez takových pulzů
     * se posloupnost nekopíruje.
     *
     * @param seq Posloupnost pulzů. Nesmí být NULL; sample_rate > 0; při
     *            count > 0 pulses != NULL. Funkce ji nevlastní a nemění.
     * @param config Konfigurace analyzéru. Nesmí být NULL.
     * @param[out] result Výstupní výsledek. Nesmí být NULL. Volající ho uvolní
     *             přes wav_analyzer_result_destroy().
     * @return WAV_ANALYZER_OK při úspěchu (i bez nalezených záznamů),
     *         WAV_ANALYZER_ERROR_INVALID_PARAM při neplatném parametru,
     *         WAV_ANALYZER_ERROR_ALLOC při selhání alokace.
     *
     * @post result->sample_rate = seq->sample_rate, result->wav_duration_sec
     *       je součet délek pulzů po přípravě (kladné délky, nejvýš
     *       WAV_ANALYZER_PULSE_MAX_US každá), result->total_pulses počet pulzů
     *       po vynechání nulových. Indexy pulzů ve výsledku (leadery,
     *       header_leader_pulse, consumed_until_pulse, raw_blocks) se vztahují
     *       k posloupnosti po vynechání nulových pulzů; bez nich k @p seq.
     *       Časy (start_time_sec, duration_sec) vynechání pulzů s nulovou
     *       délkou nemění; zkrácení pulzu nad mez je zkrátí.
     * @post Při chybě (každý návrat jiný než WAV_ANALYZER_OK, s result != NULL)
     *       je celý *result vynulovaný (memset 0, nic alokovaného); volání
     *       wav_analyzer_result_destroy() je i tak bezpečné.
     *
     * @note keep_unknown při vysoké frekvenci dá velké surové bloky (1 bit
     *       na vzorek: 17 721 600 Hz = asi 2,2 MB na sekundu nedekódovaného
     *       úseku); úsek nad 2^35 vzorků se jako surový blok nevrátí. Pod
     *       touto mezí se blok alokuje celý (paměť úměrná vzorkům úseku),
     *       volající s vysokou frekvencí má proto volit keep_unknown = 0.
     * @note Pulz s duration_samples 0 (a kladným duration_us, např. při
     *       nízké sample_rate) čte dekodér DIRECT při čtení bitu jako
     *       1 vzorek (při přeskakování synchronizace jako 0 vzorků) a do
     *       surového bloku nepřispěje žádným bitem.
     * @note Úroveň rychlosti FSK/SLOW (fsl_speed) se určuje z tabulky podle
     *       sample_rate (od 46 050 Hz tabulka 48 000 Hz); u posloupnosti,
     *       která nepochází z WAV, může vyjít -1. Data na ní nezávisí.
     * @note Synchronizace: posloupnost se jen čte, výsledek se zapisuje;
     *       volající zajistí, že je během volání nemění jiné vlákno.
     */
    extern en_WAV_ANALYZER_ERROR wav_analyzer_analyze_pulses (
        const st_WAV_PULSE_SEQUENCE *seq,
        const st_WAV_ANALYZER_CONFIG *config,
        st_WAV_ANALYZER_RESULT *result
    );


    /**
     * @brief Uvolní výsledek analýzy včetně všech MZF souborů.
     *
     * Bezpečné volání s NULL (no-op).
     *
     * @param result Ukazatel na výsledek k uvolnění (může být NULL).
     */
    extern void wav_analyzer_result_destroy ( st_WAV_ANALYZER_RESULT *result );


    /**
     * @brief Vypíše shrnutí výsledku analýzy na výstupní proud.
     *
     * Diagnostický výpis s přehledem nalezených souborů,
     * formátů, CRC statusů a leader informací.
     *
     * @param result Výsledek analýzy. Nesmí být NULL.
     * @param stream Výstupní proud (typicky stdout nebo stderr).
     * @param verbose 1 = podrobný výpis (reálná rychlost Bd, přibližná
     *                rychlost, pulzní sada), 0 = stručný.
     */
    extern void wav_analyzer_print_summary (
        const st_WAV_ANALYZER_RESULT *result,
        FILE *stream,
        int verbose
    );


    /** @brief Verze knihovny wav_analyzer. */
#define WAV_ANALYZER_VERSION "1.15.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny wav_analyzer.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* wav_analyzer_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* WAV_ANALYZER_H */
