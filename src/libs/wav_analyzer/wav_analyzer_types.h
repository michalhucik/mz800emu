/**
 * @file   wav_analyzer_types.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.4.0
 * @brief  Společné datové typy, struktury a chybové kódy knihovny wav_analyzer.
 *
 * Definuje základní typy používané všemi vrstvami analyzéru:
 * pulzy, sekvence pulzů, informace o leaderech, výsledky dekódování,
 * konfiguraci a chybové kódy.
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


#ifndef WAV_ANALYZER_TYPES_H
#define WAV_ANALYZER_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>


    /** @brief Chybové kódy operací wav analyzéru. */
    typedef enum en_WAV_ANALYZER_ERROR {
        WAV_ANALYZER_OK = 0,                    /**< operace úspěšná */
        WAV_ANALYZER_ERROR_ALLOC,               /**< selhání alokace paměti */
        WAV_ANALYZER_ERROR_INVALID_PARAM,       /**< neplatný parametr (NULL, hodnota mimo rozsah) */
        WAV_ANALYZER_ERROR_IO,                  /**< chyba čtení/zápisu (generic_driver, WAV) */
        WAV_ANALYZER_ERROR_WAV_FORMAT,          /**< nepodporovaný WAV formát */
        WAV_ANALYZER_ERROR_NO_SIGNAL,           /**< WAV neobsahuje žádný detekovatelný signál */
        WAV_ANALYZER_ERROR_NO_LEADER,           /**< nebyl nalezen žádný leader tón */
        WAV_ANALYZER_ERROR_UNKNOWN_FORMAT,      /**< formát kazetového záznamu nebyl rozpoznán */
        WAV_ANALYZER_ERROR_DECODE_TAPEMARK,     /**< chyba při detekci tapemarku */
        WAV_ANALYZER_ERROR_DECODE_DATA,         /**< chyba při dekódování dat */
        WAV_ANALYZER_ERROR_DECODE_CHECKSUM,     /**< nesouhlasí checksum */
        WAV_ANALYZER_ERROR_DECODE_INCOMPLETE,   /**< neúplný blok dat (předčasný konec signálu) */
        WAV_ANALYZER_ERROR_BUFFER_OVERFLOW,     /**< přetečení výstupního bufferu */
    } en_WAV_ANALYZER_ERROR;


    /** @brief Režim detekce pulzů ve WAV signálu. */
    typedef enum en_WAV_PULSE_MODE {
        WAV_PULSE_MODE_ZERO_CROSSING = 0,   /**< zero-crossing - prechod přes nulovou úroveň */
        WAV_PULSE_MODE_SCHMITT_TRIGGER,     /**< Schmitt trigger - hysterezní přepínání */
    } en_WAV_PULSE_MODE;


    /** @brief Polarita vstupního signálu. */
    typedef enum en_WAV_SIGNAL_POLARITY {
        WAV_SIGNAL_POLARITY_NORMAL = 0,     /**< normální polarita */
        WAV_SIGNAL_POLARITY_INVERTED,       /**< invertovaná polarita */
    } en_WAV_SIGNAL_POLARITY;


    /** @brief Výběr kanálu ze stereo WAV souboru. */
    typedef enum en_WAV_CHANNEL_SELECT {
        WAV_CHANNEL_LEFT = 0,   /**< levý kanál (index 0) */
        WAV_CHANNEL_RIGHT,      /**< pravý kanál (index 1) */
        WAV_CHANNEL_AUTO,       /**< automatický výběr (kanál s vyšší amplitudou) */
    } en_WAV_CHANNEL_SELECT;


    /**
     * @brief Rozpoznaný kazetový formát.
     *
     * Zahrnuje jak rychlostní třídu (z leader tónu),
     * tak konkrétní formát (ze signatury v hlavičce).
     */
    typedef enum en_WAV_TAPE_FORMAT {
        WAV_TAPE_FORMAT_UNKNOWN = 0,    /**< nerozpoznaný formát */
        WAV_TAPE_FORMAT_NORMAL,         /**< NORMAL FM 1200 Bd (MZ-700/800) */
        WAV_TAPE_FORMAT_MZ80B,          /**< NORMAL FM 1800 Bd (MZ-80B/MZ-2000) */
        WAV_TAPE_FORMAT_SINCLAIR,       /**< SINCLAIR FM 1400 Bd (MZ varianta) */
        WAV_TAPE_FORMAT_CPM_CMT,        /**< CPM-CMT: NORMAL FM 2400 Bd, ftype=$22 (SOKODI CMT.COM, RadSoft) */
        WAV_TAPE_FORMAT_CPM_TAPE,       /**< CPM-TAPE: vlastni format Pezik/MarVan (LSB first, bez stop bitu) */
        WAV_TAPE_FORMAT_TURBO,          /**< TURBO FM (předzavaděč TurboCopy, signatura "[turbo]") */
        WAV_TAPE_FORMAT_FASTIPL,        /**< FASTIPL ($BB prefix, V02/V07 loader) */
        WAV_TAPE_FORMAT_BSD,            /**< BSD chunkovany format (typ=0x04) */
        WAV_TAPE_FORMAT_FSK,            /**< FSK - frekvenční klíčování */
        WAV_TAPE_FORMAT_SLOW,           /**< SLOW - kvaternální kódování */
        WAV_TAPE_FORMAT_DIRECT,         /**< DIRECT - přímý bitový zápis */
        WAV_TAPE_FORMAT_ZX_SPECTRUM,    /**< ZX Spectrum nativní formát */
        WAV_TAPE_FORMAT_COUNT           /**< počet platných hodnot (sentinel) */
    } en_WAV_TAPE_FORMAT;


    /**
     * @brief Rychlostní třída detekovaná z leader tónu.
     *
     * Určena z průměrné půl-periody leader tónu (v us).
     */
    typedef enum en_WAV_SPEED_CLASS {
        WAV_SPEED_CLASS_UNKNOWN = 0,    /**< nerozpoznaná rychlost */
        WAV_SPEED_CLASS_DIRECT,         /**< < 70 us - DIRECT */
        WAV_SPEED_CLASS_FAST,           /**< 70-155 us - CPM, FSK (vyšší rychlosti) */
        WAV_SPEED_CLASS_MEDIUM,         /**< 155-220 us - SINCLAIR, MZ-80B 1800 Bd */
        WAV_SPEED_CLASS_NORMAL,         /**< 220-500 us - NORMAL 1200 Bd, TURBO, FASTIPL, BSD */
        WAV_SPEED_CLASS_ZX,             /**< 500-1100 us - ZX Spectrum nativní formát (i pomalý pilot Intercopy SINCLAIR 900 Bd) */
        WAV_SPEED_CLASS_ERROR,          /**< > 1100 us - poškozený signál */
    } en_WAV_SPEED_CLASS;


    /**
     * @brief Jeden pulz (půl-perioda) extrahovaný ze signálu.
     *
     * Pulz je definován délkou trvání v mikrosekundách a úrovní signálu.
     */
    typedef struct st_WAV_PULSE {
        double duration_us;     /**< délka pulzu v mikrosekundách */
        uint32_t duration_samples; /**< délka pulzu ve vzorcích (surová hodnota) */
        int level;              /**< úroveň signálu: 1 = HIGH, 0 = LOW */
    } st_WAV_PULSE;


    /**
     * @brief Sekvence pulzů extrahovaných ze signálu.
     *
     * Pole pulzů s metadaty o vzorkovací frekvenci.
     * Vlastní alokovanou paměť pro pole pulzů.
     *
     * @par Invarianty:
     * - count <= capacity
     * - pokud count > 0, pulses != NULL
     * - sample_rate > 0
     */
    typedef struct st_WAV_PULSE_SEQUENCE {
        st_WAV_PULSE *pulses;       /**< pole pulzů (alokováno na heapu) */
        uint32_t count;             /**< počet platných pulzů */
        uint32_t capacity;          /**< alokovaná kapacita pole */
        uint32_t sample_rate;       /**< vzorkovací frekvence zdrojového WAV (Hz) */
    } st_WAV_PULSE_SEQUENCE;


    /**
     * @brief Informace o nalezeném leader tónu.
     *
     * Leader tón je dlouhá sekvence pulzů konstantní délky,
     * která předchází datovému bloku na pásce.
     */
    typedef struct st_WAV_LEADER_INFO {
        uint32_t start_index;       /**< index prvního pulzu v sekvenci */
        uint32_t pulse_count;       /**< počet pulzů v leader tónu */
        double avg_period_us;       /**< průměrná délka půl-periody (us) */
        double min_period_us;       /**< minimální délka pulzu (us) */
        double max_period_us;       /**< maximální délka pulzu (us) */
        double stddev_us;           /**< směrodatná odchylka délek pulzů (us) */
    } st_WAV_LEADER_INFO;


    /**
     * @brief Seznam nalezených leader tónů.
     *
     * WAV soubor může obsahovat více leader tónů (hlavička + tělo,
     * případně více souborů na jedné pásce).
     *
     * @par Invarianty:
     * - count <= capacity
     * - pokud count > 0, leaders != NULL
     */
    typedef struct st_WAV_LEADER_LIST {
        st_WAV_LEADER_INFO *leaders;    /**< pole leader tónů (alokováno na heapu) */
        uint32_t count;                 /**< počet nalezených leader tónů */
        uint32_t capacity;              /**< alokovaná kapacita pole */
    } st_WAV_LEADER_LIST;


    /**
     * @brief Stav CRC verifikace dekódovaného bloku.
     */
    typedef enum en_WAV_CRC_STATUS {
        WAV_CRC_OK = 0,            /**< checksum souhlasí */
        WAV_CRC_ERROR,             /**< checksum nesouhlasí */
        WAV_CRC_NOT_AVAILABLE,     /**< checksum nebyl k dispozici */
    } en_WAV_CRC_STATUS;


    /**
     * @brief Výsledek dekódování jednoho bloku dat z pásky.
     *
     * Obsahuje dekódovaná data, metadata o formátu a stavu CRC.
     * Vlastní alokovanou paměť pro data.
     */
    typedef struct st_WAV_DECODE_RESULT {
        en_WAV_TAPE_FORMAT format;      /**< detekovaný formát */
        uint8_t *data;                  /**< dekódovaná data (alokována na heapu) */
        uint32_t data_size;             /**< velikost dekódovaných dat v bajtech */
        en_WAV_CRC_STATUS crc_status;   /**< stav CRC verifikace */
        uint16_t crc_computed;          /**< vypočtený checksum */
        uint16_t crc_stored;            /**< checksum přečtený z pásky */
        uint32_t pulse_start;           /**< index prvního pulzu bloku v sekvenci */
        uint32_t pulse_end;             /**< index posledního pulzu bloku v sekvenci */
        int is_header;                  /**< 1 = hlavička (128B), 0 = tělo souboru */
        int copy2_used;                 /**< 1 = data pochází z Copy2 (druhé kopie), 0 = z první kopie */
    } st_WAV_DECODE_RESULT;


    /**
     * @brief Seznam dekódovaných bloků.
     *
     * @par Invarianty:
     * - count <= capacity
     * - pokud count > 0, results != NULL
     */
    typedef struct st_WAV_DECODE_RESULT_LIST {
        st_WAV_DECODE_RESULT *results;  /**< pole výsledků (alokováno na heapu) */
        uint32_t count;                 /**< počet dekódovaných bloků */
        uint32_t capacity;              /**< alokovaná kapacita pole */
    } st_WAV_DECODE_RESULT_LIST;


    /**
     * @brief Statistiky pulzů extrahovaných ze signálu.
     *
     * Jednoprůchodové výpočty průměru, směrodatné odchylky,
     * minima a maxima délek pulzů v mikrosekundách.
     */
    typedef struct st_WAV_PULSE_STATS {
        uint32_t count;      /**< počet pulzů */
        double avg_us;       /**< průměrná délka pulzu (us) */
        double stddev_us;    /**< směrodatná odchylka (us) */
        double min_us;       /**< minimální délka pulzu (us) */
        double max_us;       /**< maximální délka pulzu (us) */
    } st_WAV_PULSE_STATS;


    /**
     * @brief Stav obnovy dat u částečně dekódovaného souboru.
     *
     * Bitové příznaky - mohou se kombinovat pomocí OR.
     * Hodnota 0 (WAV_RECOVERY_NONE) znamená kompletní soubor bez problémů.
     */
    typedef enum en_WAV_RECOVERY_STATUS {
        WAV_RECOVERY_NONE           = 0x00, /**< kompletní soubor, žádná obnova */
        WAV_RECOVERY_BSD_INCOMPLETE = 0x01, /**< BSD soubor bez ukončovacího chunku (ID=0xFFFF) */
        WAV_RECOVERY_PARTIAL_BODY   = 0x02, /**< tělo přečtené jen zčásti (konec signálu nebo konec dat před fsize), zbytek 00h */
        WAV_RECOVERY_HEADER_ONLY    = 0x04, /**< hlavička bez těla (tapemark těla nenalezen nebo tělo bez dat), tělo 00h */
        WAV_RECOVERY_FSIZE_REPAIRED = 0x08, /**< fsize hlavičky s chybou CRC opraveno podle délky těla na pásce */
        WAV_RECOVERY_COPY_MERGED    = 0x10, /**< tělo složené ze dvou poškozených kopií (součet souhlasí) */
        WAV_RECOVERY_BSD_GAPS       = 0x20, /**< BSD: chybějící kusy uprostřed souboru, doplněné nulami */
    } en_WAV_RECOVERY_STATUS;


    /**
     * @brief Volba a výsledek obnovy částečného těla pro dekodéry bez kopií.
     *
     * Volající nastaví enable, ostatní pole vyplní dekodér. Předání NULL znamená bez obnovy.
     */
    typedef struct st_WAV_BODY_RECOVERY {
        int enable;              /**< 1 = ponechat neúplné tělo, zbytek do velikosti doplnit 00h */
        uint32_t status;         /**< bitové OR en_WAV_RECOVERY_STATUS */
        uint32_t read_bytes;     /**< bajtů těla skutečně přečtených z pásky */
        uint32_t expected_bytes; /**< očekávaná velikost těla */
    } st_WAV_BODY_RECOVERY;


    /**
     * @brief Formát pro uložení neidentifikovaných bloků.
     *
     * Určuje, v jakém TZX blokovém typu se uloží surová data
     * z oblastí signálu, které nebyly rozpoznány jako žádný
     * známý kazetový formát.
     */
    typedef enum en_WAV_RAW_FORMAT {
        WAV_RAW_FORMAT_DIRECT = 0,  /**< TZX Direct Recording (blok 0x15) */
    } en_WAV_RAW_FORMAT;


    /**
     * @brief Neidentifikovaný blok signálu pro uložení jako TZX Direct Recording.
     *
     * Obsahuje surová bitová data z oblasti pulzů, která nebyla
     * rozpoznána jako žádný známý kazetový formát. Data jsou
     * zabalena MSb first dle specifikace TZX bloku 0x15.
     *
     * @par Ownership:
     * Pole data je vlastněno touto strukturou. Volající uvolní
     * přes wav_analyzer_raw_block_destroy().
     */
    typedef struct st_WAV_ANALYZER_RAW_BLOCK {
        uint32_t pulse_start;       /**< index prvního pulzu v sekvenci */
        uint32_t pulse_end;         /**< index za posledním pulzem */
        double start_time_sec;      /**< čas začátku v sekundách */
        double duration_sec;        /**< délka v sekundách */
        uint32_t sample_rate;       /**< vzorkovací frekvence zdrojového WAV (Hz) */
        uint8_t *data;              /**< bitová data pro TZX blok 0x15 (alokována na heapu) */
        uint32_t data_size;         /**< velikost dat v bajtech */
        uint8_t used_bits_last;     /**< použité bity v posledním bajtu (1-8) */
    } st_WAV_ANALYZER_RAW_BLOCK;


    /** @brief Výchozí tolerance pro detekci leader tónu (20 %).
     *
     * Nahrávky z magnetofonu mají jitter na délce pulzů (asymetrický
     * duty cycle, rychlostní kolísání), velikost [neověřeno].
     * Tolerance 20 % spolehlivě detekuje leadery i v degradovaných
     * signálech a nezpůsobuje problémy u syntetických signálů
     * (které mají téměř nulový jitter).
     */
#define WAV_ANALYZER_DEFAULT_TOLERANCE      0.20

    /** @brief Minimální povolená tolerance (2 %). */
#define WAV_ANALYZER_MIN_TOLERANCE          0.02

    /** @brief Maximální povolená tolerance (35 %). */
#define WAV_ANALYZER_MAX_TOLERANCE          0.35

    /** @brief Výchozí minimální počet pulzů pro detekci leader tónu.
     *
     * Hodnota 500 zajistí detekci i rychlých signálů (3200+ Bd)
     * s vysokým kvantizačním jitterem. LGAP má typicky 22000+
     * půl-period, ale při vysokém jitteru se leader detektor
     * resetuje častěji a potřebuje kratší konzistentní běhy.
     * Hodnota 500 je dostatečně vysoká pro odfiltrování náhodných
     * shod v datech nebo audio prefixech.
     */
#define WAV_ANALYZER_DEFAULT_MIN_LEADER_PULSES  500

    /** @brief Výchozí horní práh Schmitt triggeru. */
#define WAV_ANALYZER_DEFAULT_SCHMITT_HIGH   0.1

    /** @brief Výchozí dolní práh Schmitt triggeru. */
#define WAV_ANALYZER_DEFAULT_SCHMITT_LOW    (-0.1)

    /** @brief Výchozí šířka binu histogramu v mikrosekundách. */
#define WAV_ANALYZER_DEFAULT_HISTOGRAM_BIN_WIDTH    5.0

    /** @brief Maximální velikost histogramu (počet binů). */
#define WAV_ANALYZER_MAX_HISTOGRAM_BINS     1000

    /** @brief Koeficient pro vypocet FM bit-prahu: leader_avg * THRESHOLD_FACTOR.
     *
     * Prah musi lezet bezpecne mezi SHORT a LONG pul-periodami.
     * Standard FM ma SHORT:LONG pomer ~1:2. Vyssi faktor posouva prah
     * blize k LONG, cimz se vyhneme misklasifikaci
     * u asymetrickych pulsetu (napr. CMT.COM kde SHORT-HIGH ~304 us
     * a LONG ~407 us - faktor 1.5 dava 306 us, coz je prilis blizko
     * SHORT-HIGH a zpusobuje chyby pri detekci tapemarku).
     * Faktor 1.6 je kompromis: bezpecne nad SHORT-HIGH pro CMT.COM
     * (207*1.6=331 > 304) a pod LONG-HIGH pro CPM 2x (147*1.6=235 < 249).
     */
#define WAV_ANALYZER_FM_THRESHOLD_FACTOR    1.6


    /**
     * @brief Konfigurace analyzéru WAV signálu.
     *
     * Obsahuje všechny nastavitelné parametry pro řízení analýzy.
     * Výchozí hodnoty se inicializují přes wav_analyzer_config_default().
     */
    typedef struct st_WAV_ANALYZER_CONFIG {
        en_WAV_PULSE_MODE pulse_mode;           /**< režim detekce pulzů */
        double tolerance;                       /**< tolerance pro detekci leaderu (0.02-0.35) */
        double schmitt_high;                    /**< horní práh Schmitt triggeru */
        double schmitt_low;                     /**< dolní práh Schmitt triggeru */
        uint32_t min_leader_pulses;             /**< minimální počet pulzů pro leader */
        en_WAV_CHANNEL_SELECT channel;          /**< výběr kanálu ze sterea */
        en_WAV_SIGNAL_POLARITY polarity;        /**< polarita signálu */
        int enable_dc_offset;                   /**< 1 = zapnout DC offset korekci */
        int enable_highpass;                    /**< 1 = zapnout vysokofrekvenční filtr */
        int enable_normalize;                   /**< 1 = zapnout normalizaci amplitudy */
        double highpass_alpha;                  /**< koeficient HP filtru (0.995-0.999) */
        double histogram_bin_width;             /**< šířka binu histogramu (us) */
        int verbose;                            /**< 1 = podrobný výstup na stderr */
        int keep_unknown;                       /**< 1 = uložit neidentifikované bloky jako Direct Recording */
        int pass_count;                         /**< počet průchodů (výchozí 1, zatím není použit) */
        en_WAV_RAW_FORMAT raw_format;           /**< formát pro neidentifikované bloky */
        int recover_bsd;                        /**< 1 = uložit částečná BSD data (chybějící terminátor) */
        int recover_body;                       /**< 1 = ponechat neúplné tělo záznamu FM (zbytek 00h) */
        int recover_header;                     /**< 1 = ponechat hlavičku záznamu FM s platným CRC, jejíž tělo chybí (tělo 00h) */
    } st_WAV_ANALYZER_CONFIG;


    /**
     * @brief Vrátí textový popis chybového kódu wav analyzéru.
     *
     * Vždy vrací platný řetězec (nikdy NULL). Pro neznámé kódy
     * vrací "unknown error".
     *
     * @param err Chybový kód.
     * @return Textový popis chyby (anglicky).
     */
    extern const char* wav_analyzer_error_string ( en_WAV_ANALYZER_ERROR err );


    /**
     * @brief Vrátí textový název kazetového formátu.
     *
     * @param format Kazetový formát.
     * @return Textový název formátu (anglicky).
     */
    extern const char* wav_tape_format_name ( en_WAV_TAPE_FORMAT format );


    /**
     * @brief Vrátí textový popis stavu obnovy dat.
     *
     * Vždy vrací platný řetězec (nikdy NULL). Pro WAV_RECOVERY_NONE
     * vrací "complete". Pro kombinace příznaků vrací popis
     * prvního nastaveného příznaku.
     *
     * @param status Bitové OR z en_WAV_RECOVERY_STATUS.
     * @return Textový popis stavu obnovy (anglicky).
     */
    extern const char* wav_recovery_status_string ( uint32_t status );


    /**
     * @brief Inicializuje konfiguraci na výchozí hodnoty.
     *
     * Nastaví všechny parametry na rozumné výchozí hodnoty
     * vhodné pro čisté signály z emulátoru.
     *
     * @param config Ukazatel na konfiguraci k inicializaci. Nesmí být NULL.
     */
    extern void wav_analyzer_config_default ( st_WAV_ANALYZER_CONFIG *config );


    /**
     * @brief Vypočítá statistiky ze sekvence pulzů.
     *
     * Jednoprůchodový výpočet průměru, směrodatné odchylky (Welfordova metoda),
     * minima a maxima délek pulzů. Pro prázdnou sekvenci nastaví
     * všechny hodnoty na 0.
     *
     * @param seq Sekvence pulzů k analyzování. Nesmí být NULL.
     * @param[out] out_stats Výstupní statistiky. Nesmí být NULL.
     *
     * @pre seq != NULL, out_stats != NULL
     * @post out_stats je naplněn platnými hodnotami.
     */
    extern void wav_pulse_stats_compute ( const st_WAV_PULSE_SEQUENCE *seq,
                                          st_WAV_PULSE_STATS *out_stats );


    /**
     * @brief Uvolní neidentifikovaný raw blok včetně alokovaných dat.
     *
     * Bezpečné volání s NULL (no-op).
     *
     * @param block Ukazatel na blok k uvolnění (může být NULL).
     */
    extern void wav_analyzer_raw_block_destroy ( st_WAV_ANALYZER_RAW_BLOCK *block );


    /**
     * @brief Uvolní sekvenci pulzů včetně alokovaného pole.
     *
     * Bezpečné volání s NULL (no-op).
     *
     * @param seq Ukazatel na sekvenci k uvolnění (může být NULL).
     */
    extern void wav_pulse_sequence_destroy ( st_WAV_PULSE_SEQUENCE *seq );


    /**
     * @brief Uvolní seznam leader tónů včetně alokovaného pole.
     *
     * Bezpečné volání s NULL (no-op).
     *
     * @param list Ukazatel na seznam k uvolnění (může být NULL).
     */
    extern void wav_leader_list_destroy ( st_WAV_LEADER_LIST *list );


    /**
     * @brief Uvolní seznam dekódovaných výsledků včetně dat.
     *
     * Bezpečné volání s NULL (no-op). Uvolní také data
     * jednotlivých výsledků.
     *
     * @param list Ukazatel na seznam k uvolnění (může být NULL).
     */
    extern void wav_decode_result_list_destroy ( st_WAV_DECODE_RESULT_LIST *list );


#ifdef __cplusplus
}
#endif

#endif /* WAV_ANALYZER_TYPES_H */
