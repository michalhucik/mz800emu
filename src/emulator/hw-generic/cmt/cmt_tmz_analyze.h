/**
 * @file   cmt_tmz_analyze.h
 * @brief  Analýza surového bloku pásky (0x18 CSW) na soubory a uplatnění výsledku.
 *
 * Surový blok pásky (nahrávka, kterou záznam nedekódoval, nebo blok
 * z importu WAV) se rozebere analyzátorem TapeMZ na pojmenované soubory.
 * Postup má tři kroky s různými podmínkami vláken:
 *
 * 1. cmttmz_analyze_copy_pulses() - kopie pulzů bloku 0x18 z vložené pásky
 *    (vlákno emulátoru pod device_state_lock()); kopie si pamatuje čítač
 *    výměn pásky a číslo bloku, aby šlo výsledek později ověřit.
 * 2. cmttmz_analyze_pulses() - čistá funkce nad kopií pulzů:
 *    wav_analyzer_analyze_pulses() a z výsledku řádky v pořadí polohy bez
 *    překryvů (soubor nebo neznámý úsek; neznámý úsek kratší než
 *    CMTTMZ_REC_MIN_EDGES pulzů se vynechá jako u záznamu). Stav pásky ani
 *    jiný globální stav emulátoru nečte a nemění; smí běžet v jiném vlákně
 *    (ověřeno čtením zdrojů knihoven, viz devdoc/cmt.md). Histogram délek
 *    pulzů (cmttmz_analysis_histogram()) je stejně čistý.
 * 3. cmttmz_analysis_apply() - stavba bloků a změna pásky (vlákno emulátoru
 *    pod device_state_lock()): soubory se přidají za surový blok, nebo se
 *    surový blok nahradí posloupností bloků souborů a surových zbytků:
 *    každý pulz řádku, který zůstává surový, je dál na pásce v surovém
 *    bloku, v původním pořadí vůči blokům souborů (zachová se obsah
 *    signálu, ne časová osa).
 *
 * Přidat jako blok jde jen soubor formátu NORMAL s bezchybnou hlavičkou
 * i tělem, nebo soubor opravený volbou opravy; stavba je stejná
 * jako u záznamu (cmttmz_build_mzf_block(), profil MZ800EMU-CMT, výchozí
 * rychlost decku). Plán částí a přijetí souboru sdílí analýza se záznamem
 * (cmttmz_rec_plan(), cmttmz_rec_file_accepted() v cmt_tmz_record.h).
 *
 * Texty (chyby, důvody, jména formátů) jsou anglicky a nelokalizované;
 * okno je překládá samo (důvod nese i výčet en_CMTTMZ_ANALYZE_REASON).
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

#ifndef CMT_TMZ_ANALYZE_H
#define CMT_TMZ_ANALYZE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/wav_analyzer/wav_analyzer_types.h"


    /**
     * @brief Nejvýš tolik pulzů započte cmttmz_analysis_histogram() (rovnoměrně vybraných z rozsahu).
     */
#define CMTTMZ_ANALYZE_HIST_MAX_PULSES 200000u

    /**
     * @brief Nejmenší tolerance detekce leaderu, kterou analýza použije (0,02, knihovna wav_analyzer).
     *
     * Analýza menší hodnotu ořízne; MCP ji odmítne jako neplatný parametr.
     */
#define CMTTMZ_ANALYZE_TOLERANCE_MIN WAV_ANALYZER_MIN_TOLERANCE

    /** @brief Největší tolerance detekce leaderu (0,35, knihovna wav_analyzer); viz CMTTMZ_ANALYZE_TOLERANCE_MIN. */
#define CMTTMZ_ANALYZE_TOLERANCE_MAX WAV_ANALYZER_MAX_TOLERANCE

    /** @brief Velikost textu chyby analýzy (cmttmz_analysis_error()) včetně ukončovací nuly. */
#define CMTTMZ_ANALYZE_ERROR_SIZE 256u

    /** @brief Velikost jména souboru v UTF-8 v řádku analýzy (včetně ukončovací nuly). */
#define CMTTMZ_ANALYZE_NAME_SIZE MZF_FNAME_UTF8_BUF_SIZE

    /**
     * @brief Nejmenší pauza v ms za posledním novým blokem souboru při Replace, následuje-li na pásce další blok.
     *
     * Soubor nikdy bez mezery před dalším blokem;
     * stejná hodnota jako pauza švu úprav pásky (CMTTMZ_APPEND_GAP_MS
     * v cmt_tmz.c) a pauza mezi bloky záznamu.
     */
#define CMTTMZ_ANALYZE_FILE_GAP_MS 1000u


    /**
     * @brief Volby analýzy (dialog Analyze, MCP emu_cmt_tape_analyze).
     *
     * Volby oprav odpovídají polím konfigurace analyzátoru recover_header,
     * recover_body a recover_bsd (st_WAV_ANALYZER_CONFIG); ostatní volby
     * analyzátoru zůstávají výchozí (wav_analyzer_config_default(),
     * keep_unknown 0).
     */
    typedef struct st_CMTTMZ_ANALYZE_OPTS {
        double tolerance;  /**< tolerance detekce leaderu; analýza ji omezí na 0,02 až 0,35 (WAV_ANALYZER_MIN_TOLERANCE, _MAX_), NaN = 0,20 */
        int repair_header; /**< 1 = ponechat hlavičku s platným součtem bez těla (tělo 00h), recover_header */
        int repair_body;   /**< 1 = ponechat neúplné tělo (zbytek 00h) a složit tělo ze dvou poškozených kopií, recover_body */
        int repair_bsd;    /**< 1 = ponechat neúplný soubor BSD, recover_bsd (BSD jako blok přidat nejde) */
    } st_CMTTMZ_ANALYZE_OPTS;


    /**
     * @brief Nastaví výchozí volby: tolerance 0,20, všechny opravy vypnuté.
     * @param opts Volby (NULL se ignoruje).
     */
    extern void cmttmz_analyze_opts_default ( st_CMTTMZ_ANALYZE_OPTS *opts );


    /**
     * @brief Kopie pulzů jednoho bloku 0x18 vložené pásky (vstup analýzy a histogramu).
     *
     * Vzniká jen cmttmz_analyze_copy_pulses(), uvolňuje se
     * cmttmz_pulses_free(). Po vzniku se nemění (čtou ji analýza, histogram
     * a uplatnění, i z různých vláken současně).
     *
     * @invariant count == 0 <=> lengths == NULL.
     * @invariant lengths[i] >= 1 (pulz CSW nemá délku 0).
     * @invariant total_us = round ( součet lengths * 1e6 / sample_rate ).
     * @invariant Úroveň pulzu i je first_level ^ (i & 1); u bloku 0x18 je
     *            first_level 0 (první pulz CSW je LOW, tzx.h).
     *
     * Vlastnictví: struktura i pole lengths patří kopii.
     */
    typedef struct st_CMTTMZ_PULSES {
        uint32_t sample_rate;  /**< vzorkovací frekvence bloku v Hz */
        uint32_t count;        /**< počet pulzů */
        uint32_t *lengths;     /**< délky pulzů ve vzorcích při sample_rate (count položek) */
        int first_level;       /**< úroveň prvního pulzu (0 = LOW) */
        uint16_t pause_ms;     /**< pauza bloku za pulzy v ms (při Replace ji dostane poslední nový blok) */
        unsigned tape_serial;  /**< čítač výměn pásky při kopii (cmttmz_get_tape_serial()) */
        int block_id;          /**< číslo bloku na pásce při kopii */
        int unsaved;           /**< příznak neuloženo bloku při kopii (cmttmz_block_is_unsaved()) */
        int wav_type;          /**< 1 = blok má v indexu typ WAV (z importu WAV; na takový blok působí přepínač polarity, analýza ho neuplatňuje) */
        uint64_t total_us;     /**< délka pulzů v µs (bez pauzy) */
        uint32_t block_length; /**< délka dat bloku na pásce při kopii (st_TZX_BLOCK::length) */
        uint32_t block_hash;   /**< otisk dat bloku při kopii (FNV-1a 32 bitů); uplatnění jím ověří, že blok je týž */
    } st_CMTTMZ_PULSES;


    /**
     * @brief Zkopíruje pulzy bloku 0x18 vložené pásky.
     *
     * Blok se rozbalí z dat RLE (tzx_parse_csw_recording(), komprese RLE);
     * data se ověří stejně jako při přehrání (délka pulzu 0, useknutá
     * 32bitová délka, data kratší nebo delší než počet pulzů = chyba); počet
     * pulzů z hlavičky větší než délka dat (pulz RLE má aspoň 1 bajt) je
     * chyba ještě před alokací. Blok s 0 pulzy dá kopii s count 0 (analýza
     * pak nemá řádky). Kopie si uloží délku a otisk dat bloku.
     *
     * Chyby (text v cmttmz_get_edit_error(), anglicky): "No tape inserted",
     * "Block N does not exist", "Block N is not a raw signal block (CSW
     * recording)" (jiný typ bloku, také 0x15), "Block N uses Z-RLE compression,
     * which is not supported", "Block N contains invalid signal data",
     * "Not enough memory".
     *
     * @param block_id Číslo bloku.
     * @return Nová kopie (uvolnit cmttmz_pulses_free()); NULL při chybě.
     * @pre Vlákno emulátoru pod device_state_lock() (páska se nesmí měnit).
     * @post Páska se nemění; při úspěchu se text chyby úpravy nemění.
     */
    extern st_CMTTMZ_PULSES* cmttmz_analyze_copy_pulses ( int block_id );

    /**
     * @brief Uvolní kopii pulzů (NULL je přípustné).
     * @param p Kopie.
     * @pre Žádné jiné vlákno kopii nečte (pracovní vlákno analýzy doběhlo).
     */
    extern void cmttmz_pulses_free ( st_CMTTMZ_PULSES *p );


    /** @brief Druh řádku analýzy. */
    typedef enum en_CMTTMZ_ANALYZE_ITEM {
        CMTTMZ_ANALYZE_FILE = 0, /**< soubor, který analyzátor našel (libovolného formátu, i s chybou) */
        CMTTMZ_ANALYZE_UNKNOWN,  /**< úsek signálu bez nalezeného souboru (šum, ticho, nerozpoznaný záznam) */
    } en_CMTTMZ_ANALYZE_ITEM;


    /** @brief Co se s řádkem stane při uplatnění. */
    typedef enum en_CMTTMZ_ANALYZE_RESULT {
        CMTTMZ_ANALYZE_ADDABLE = 0,       /**< bezchybný soubor formátu, který jde sestavit jako blok (NORMAL, CPM-CMT, TURBO, FASTIPL, BSD, CPM-TAPE): jde přidat (Will be added) */
        CMTTMZ_ANALYZE_ADDABLE_REPAIRED,  /**< takový soubor opravený zapnutou volbou opravy: jde přidat (Will be added (repaired)) */
        CMTTMZ_ANALYZE_STAYS_RAW,         /**< takový soubor s chybou součtu nebo neúplný, nebo neznámý úsek: zůstává v surovém signálu (Stays raw) */
        CMTTMZ_ANALYZE_CANNOT_ADD,        /**< soubor formátu, který jako blok sestavit nejde (MZ-80B, SINCLAIR, FSK, SLOW, DIRECT, ZX), nebo soubor, který profil zapisovače odmítl: zůstává surový (Cannot add) */
    } en_CMTTMZ_ANALYZE_RESULT;


    /** @brief Stav kontrolního součtu části souboru. */
    typedef enum en_CMTTMZ_ANALYZE_CHECKSUM {
        CMTTMZ_ANALYZE_CHECKSUM_OK = 0,        /**< součet souhlasí (z první nebo druhé kopie) */
        CMTTMZ_ANALYZE_CHECKSUM_ERROR,         /**< součet nesouhlasí (Checksum error) */
        CMTTMZ_ANALYZE_CHECKSUM_NOT_AVAILABLE, /**< součet není k dispozici (část chybí, formát ho nemá, neznámý úsek) */
    } en_CMTTMZ_ANALYZE_CHECKSUM;


    /** @brief Důvod výsledku řádku (pro tooltip; text cmttmz_analyze_reason_text()). */
    typedef enum en_CMTTMZ_ANALYZE_REASON {
        CMTTMZ_ANALYZE_REASON_NONE = 0,        /**< ADDABLE: žádný */
        CMTTMZ_ANALYZE_REASON_REPAIRED,        /**< ADDABLE_REPAIRED: data doplněná opravou (recovery_status) */
        CMTTMZ_ANALYZE_REASON_NO_FILE,         /**< UNKNOWN: v úseku není soubor */
        CMTTMZ_ANALYZE_REASON_FORMAT,          /**< CANNOT_ADD: formát nejde sestavit jako blok (format_name) */
        CMTTMZ_ANALYZE_REASON_PROFILE,         /**< CANNOT_ADD: soubor nejde sestavit profilem zapisovače (např. tělo delší, než formát unese) */
        CMTTMZ_ANALYZE_REASON_NO_MZF,          /**< CANNOT_ADD: záznam bez dat Sharp MZF (ZX) */
        CMTTMZ_ANALYZE_REASON_HEADER_CHECKSUM, /**< STAYS_RAW: chyba součtu hlavičky */
        CMTTMZ_ANALYZE_REASON_BODY_CHECKSUM,   /**< STAYS_RAW: chyba součtu těla (nebo součet těla chybí) */
        CMTTMZ_ANALYZE_REASON_REPAIR_DISABLED, /**< STAYS_RAW: data opravená způsobem, pro který není zapnutá volba (recovery_status) */
        CMTTMZ_ANALYZE_REASON_INCOMPLETE,      /**< STAYS_RAW: tělo kratší než fsize nebo neplatný rozsah pulzů */
    } en_CMTTMZ_ANALYZE_REASON;


    /**
     * @brief Jeden řádek analýzy (soubor nebo neznámý úsek), jen data pro zobrazení a uplatnění.
     *
     * Řádky analýzy jdou v pořadí polohy bez překryvů a pokrývají blok
     * s výjimkou neznámých úseků kratších než CMTTMZ_REC_MIN_EDGES pulzů
     * (ticho, zákmit mezi soubory), které se jako u záznamu vynechají:
     * end_pulse řádku <= first_pulse dalšího a každá mezera mezi řádky (i
     * před prvním a za posledním řádkem) má méně než CMTTMZ_REC_MIN_EDGES
     * pulzů. start_us a length_us jsou zaokrouhlené součty pulzů od začátku
     * bloku. Řádek souboru (každý výsledek analyzátoru s platným rozsahem,
     * i s chybou, jiného formátu nebo ZX / SINCLAIR) se nevynechá nikdy, ať
     * je jakkoli krátký. Řádky souborů jdou v pořadí začátku
     * (cmttmz_rec_file_start()); soubor, který začíná před koncem
     * předchozího, má rozsah od konce předchozího (překrytý kus patří
     * dřívějšímu řádku - zůstane-li surový, je v surovém bloku jednou,
     * přidá-li se oba soubory, nezůstane z překryvu nic), soubor celý uvnitř
     * dřívějšího má prázdný rozsah.
     *
     * @invariant kind == CMTTMZ_ANALYZE_UNKNOWN => result == STAYS_RAW,
     *            reason == NO_FILE, oba součty NOT_AVAILABLE, size 0, jméno "".
     * @invariant result ADDABLE* => kind FILE, formát NORMAL, CPM-CMT, TURBO,
     *            FASTIPL, BSD nebo CPM-TAPE a writer_profile != "".
     * @invariant Neznámý řádek: first_pulse < end_pulse a aspoň
     *            CMTTMZ_REC_MIN_EDGES pulzů - výjimka: řádek přes celý blok
     *            při selhání analyzátoru (cmttmz_analysis_error() neprázdné).
     * @invariant Řádek souboru: first_pulse <= end_pulse; rovnost jen u souboru
     *            celého uvnitř dřívějšího.
     *
     * Vlastnictví: řádek patří analýze; format_name a recovery_text jsou
     * statické řetězce knihovny (platí trvale).
     */
    typedef struct st_CMTTMZ_ANALYZE_ROW {
        en_CMTTMZ_ANALYZE_ITEM kind;               /**< soubor / neznámý úsek */
        char name[CMTTMZ_ANALYZE_NAME_SIZE];       /**< jméno souboru v UTF-8 pro zobrazení (Sharp ASCII EU, sharpmz_str_to_utf8()), "" bez jména */
        uint8_t name_sharp[MZF_FNAME_FULL_LENGTH]; /**< jméno v Sharp MZ ASCII (bajty hlavičky před 0Dh) */
        uint8_t name_sharp_len;                    /**< počet platných bajtů name_sharp */
        uint8_t ftype;                             /**< typ souboru z hlavičky MZF (0 bez hlavičky) */
        const char *format_name;                   /**< jméno formátu wav_tape_format_name() ("UNKNOWN" u neznámého úseku) */
        uint64_t start_us;                         /**< začátek úseku v µs od začátku bloku (zaokrouhlený součet pulzů před ním) */
        uint64_t length_us;                        /**< délka úseku v µs */
        uint32_t size;                             /**< velikost dat v bajtech: fsize MZF, délka TAP u ZX, 0 u neznámého úseku */
        en_CMTTMZ_ANALYZE_CHECKSUM header_checksum; /**< součet hlavičky */
        en_CMTTMZ_ANALYZE_CHECKSUM body_checksum;  /**< součet těla */
        en_CMTTMZ_ANALYZE_RESULT result;           /**< co se s řádkem stane */
        en_CMTTMZ_ANALYZE_REASON reason;           /**< proč (tooltip) */
        uint32_t first_pulse;                      /**< první pulz úseku (index v kopii pulzů) */
        uint32_t end_pulse;                        /**< pulz za úsekem */
        double short_pulse_us;                     /**< naměřený krátký pulz v µs (0 = neznámo) */
        double long_pulse_us;                      /**< naměřený dlouhý pulz v µs (0 = neznámo) */
        uint32_t recovery_status;                  /**< stav opravy: bitové OR en_WAV_RECOVERY_STATUS (0 = úplný soubor) */
        const char *recovery_text;                 /**< popis stavu opravy wav_recovery_status_string() ("complete" bez opravy) */
        const char *writer_profile;                /**< čím se sestaví blok řádku, který jde přidat: ID profilu zapisovače z registru TapeMZ
                                                        (např. "TC-1.21", "IC-10.2-FASTIPL", "BASIC-1Z016", "MZ800EMU-CMT"), nebo "MZ Turbo Data"
                                                        u bloku 0x41 (TURBO a FASTIPL bez rozpoznaného zapisovače); "" u řádku, který přidat nejde.
                                                        Statický řetězec. */
    } st_CMTTMZ_ANALYZE_ROW;


    /**
     * @brief Výsledek analýzy (neprůhledný): řádky, opravená data souborů, vazba na kopii pulzů.
     *
     * Vzniká cmttmz_analyze_pulses(), uvolňuje se cmttmz_analysis_free().
     * Po vzniku se nemění.
     */
    typedef struct st_CMTTMZ_ANALYSIS st_CMTTMZ_ANALYSIS;


    /**
     * @brief Rozebere kopii pulzů na řádky (soubory a neznámé úseky).
     *
     * Pulzy se předají wav_analyzer_analyze_pulses() se sample_rate bloku
     * (duration_samples = délka, duration_us = délka * 1e6 / frekvence,
     * úroveň first_level ^ (i & 1)); konfigurace wav_analyzer_config_default()
     * s tolerancí a volbami oprav z @p opts (keep_unknown 0). Indexy pulzů
     * výsledku odpovídají kopii 1:1 (pulz CSW nemá délku 0).
     *
     * Řádky: všechny soubory analyzátoru s platným rozsahem v pořadí začátku
     * (cmttmz_rec_file_start(): header_leader_pulse, u ZX / SINCLAIR
     * leader.start_index) s rozsahem [začátek, consumed_until_pulse); soubor
     * začínající před koncem předchozího začíná koncem předchozího (viz
     * st_CMTTMZ_ANALYZE_ROW), a neznámé úseky mezi nimi a kolem nich s aspoň CMTTMZ_REC_MIN_EDGES
     * pulzy (plán cmttmz_rec_plan() v režimu všech souborů se stejnou mezí
     * jako záznam; kratší úsek řádek nedá). Výsledek souboru:
     * - formát, který jako blok sestavit nejde (MZ-80B, SINCLAIR, FSK, SLOW,
     *   DIRECT, ZX Spectrum), nebo bez dat MZF: CANNOT_ADD,
     * - NORMAL přijatý záznamem (cmttmz_rec_file_accepted(): součty OK, bez
     *   opravy, úplné tělo, platný rozsah): ADDABLE,
     * - CPM-CMT, TURBO, FASTIPL, BSD a CPM-TAPE s platným součtem hlavičky,
     *   bez chyby součtu těla (BSD: žádný kus bez platné kopie), bez opravy,
     *   s úplným tělem a platným rozsahem: ADDABLE,
     * - s opravou (recovery_status != 0), kterou dovolují zapnuté volby
     *   (repair_body: PARTIAL_BODY, COPY_MERGED; repair_header: HEADER_ONLY;
     *   repair_bsd u BSD: BSD_INCOMPLETE, BSD_GAPS),
     *   s platným součtem hlavičky, tělem délky fsize a platným rozsahem:
     *   ADDABLE_REPAIRED,
     * - řádek ADDABLE* jiného formátu než NORMAL, jehož blok profil
     *   zapisovače nesestaví (zkušební stavba; např. tělo CP/M Pezik nad
     *   16 KB, BSD nad 254 kusů): CANNOT_ADD s důvodem PROFILE,
     * - jinak STAYS_RAW s důvodem (součet hlavičky, oprava bez volby, součet
     *   těla, neúplný soubor; v tomto pořadí).
     *
     * Kopie bez pulzů dá analýzu bez řádků s chybou "The block contains no
     * signal". Selhání analyzátoru (alokace) dá jeden neznámý řádek přes
     * celý blok a chybu "Analysis failed: <důvod>". Úspěšná analýza bez
     * souboru a bez úseku od CMTTMZ_REC_MIN_EDGES pulzů dá žádný řádek
     * (bez chyby).
     *
     * Vlákna: čistá funkce - čte jen @p p a @p opts, stav pásky ani jiný
     * globální stav emulátoru nečte ani nemění, výsledek alokuje sama. Smí
     * běžet v jiném vlákně než emulátor (podmínka ověřená čtením zdrojů
     * knihoven): analyzátor nemá měnitelný statický stav
     * a nepoužívá paměťový ovladač generic_driver. Zkušební stavba bloků
     * jiných formátů než NORMAL volá registr profilů TapeMZ a knihovnu tzx
     * (výchozí alokátor a error callback, které emulátor nikdy nemění;
     * stav knihoven se nemění); varování píše na stderr (zápis FILE je
     * zamčený, řádky z různých vláken se mohou jen proložit).
     *
     * @param p Kopie pulzů (cmttmz_analyze_copy_pulses()); během volání se
     *        nesmí uvolnit.
     * @param opts Volby (NULL = cmttmz_analyze_opts_default()).
     * @return Nová analýza (uvolnit cmttmz_analysis_free()); NULL jen při NULL
     *         @p p nebo selhání alokace samotné analýzy.
     * @post @p p se nemění; analýza nese číslo bloku, čítač výměn pásky
     *       a počet pulzů kopie (ověří je cmttmz_analysis_apply()).
     */
    extern st_CMTTMZ_ANALYSIS* cmttmz_analyze_pulses ( const struct st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYZE_OPTS *opts );

    /** @brief Uvolní analýzu (NULL je přípustné). @param a Analýza. */
    extern void cmttmz_analysis_free ( st_CMTTMZ_ANALYSIS *a );

    /**
     * @brief Počet řádků analýzy.
     *
     * Řádky pokrývají blok kromě neznámých úseků kratších než
     * CMTTMZ_REC_MIN_EDGES pulzů (viz st_CMTTMZ_ANALYZE_ROW).
     *
     * @param a Analýza (NULL = 0). @return Počet řádků.
     */
    extern uint32_t cmttmz_analysis_row_count ( const st_CMTTMZ_ANALYSIS *a );

    /**
     * @brief Řádek analýzy.
     *
     * Řádky jdou v pořadí polohy; mezi dvěma řádky (i před prvním a za
     * posledním) může chybět neznámý úsek kratší než CMTTMZ_REC_MIN_EDGES
     * pulzů, polohu řádku proto určují first_pulse a end_pulse, ne index.
     *
     * @param a Analýza. @param i Index řádku.
     * @return Řádek (platí do cmttmz_analysis_free()); NULL při NULL @p a nebo @p i mimo rozsah.
     */
    extern const st_CMTTMZ_ANALYZE_ROW* cmttmz_analysis_row ( const st_CMTTMZ_ANALYSIS *a, uint32_t i );

    /**
     * @brief Text chyby analýzy ("" = analýza proběhla).
     * @param a Analýza (NULL = "").
     * @return Text anglicky (platí do cmttmz_analysis_free()): "The block
     *         contains no signal", "Analysis failed: <důvod analyzátoru>".
     */
    extern const char* cmttmz_analysis_error ( const st_CMTTMZ_ANALYSIS *a );

    /**
     * @brief Text důvodu výsledku řádku (anglicky, nelokalizovaný, statický).
     * @param reason Důvod. @return Text ("" u CMTTMZ_ANALYZE_REASON_NONE a neznámé hodnoty).
     */
    extern const char* cmttmz_analyze_reason_text ( en_CMTTMZ_ANALYZE_REASON reason );

    /**
     * @brief Histogram délek pulzů rozsahu [first, end) kopie.
     *
     * Bin k počítá pulzy s délkou d (µs) v intervalu [k * bin_us,
     * (k + 1) * bin_us); delší pulzy padnou do posledního binu. Započte se
     * nejvýš CMTTMZ_ANALYZE_HIST_MAX_PULSES pulzů rovnoměrně vybraných
     * z rozsahu (pulz first + floor ( j * n / max ) pro j = 0 .. max - 1, n =
     * end - first); součet binů je tedy min ( n, CMTTMZ_ANALYZE_HIST_MAX_PULSES ).
     * Délka pulzu v µs je délka * 1e6 / sample_rate (jako vstup analýzy).
     * Funkci knihovny wav_histogram_build() nepoužívá: ta biny posouvá od
     * nejkratšího pulzu a omezuje na 1000 binů (mřížka by neodpovídala bin_us).
     *
     * Vlákna: čistá funkce jako cmttmz_analyze_pulses().
     *
     * @param p Kopie pulzů.
     * @param first První pulz rozsahu. @param end Pulz za rozsahem (first < end <= p->count).
     * @param bin_us Šířka binu v µs (> 0, konečná).
     * @param[out] bins Pole bin_count binů, vynuluje se (smí být NULL při bin_count 0 - pak jen max_us).
     * @param bin_count Počet binů.
     * @param[out] max_us Nejdelší pulz celého rozsahu [first, end) v µs,
     *        přesně (samostatný průchod všemi pulzy rozsahu, ne jen vzorkem);
     *        smí být NULL - pak se průchod pro maximum vůbec nedělá a funkce
     *        projde jen vzorek nejvýš CMTTMZ_ANALYZE_HIST_MAX_PULSES pulzů
     *        (dialog analýzy volá na UI vlákně jen tak).
     * @return EXIT_SUCCESS; EXIT_FAILURE při NULL @p p, neplatném rozsahu,
     *         neplatném bin_us nebo bins NULL s bin_count > 0 (výstupy pak
     *         vynulované, jsou-li zadané).
     */
    extern int cmttmz_analysis_histogram ( const struct st_CMTTMZ_PULSES *p, uint32_t first, uint32_t end, double bin_us,
                                           uint32_t *bins, uint32_t bin_count, double *max_us );

    /**
     * @brief Uplatní výsledek analýzy na vloženou pásku (všechno, nebo nic).
     *
     * Ověří, že páska je táž (cmttmz_get_tape_serial() == p->tape_serial)
     * a blok p->block_id je stále 0x18 se stejnou frekvencí, počtem pulzů,
     * délkou dat a otiskem dat (p->block_length, p->block_hash) a že analýza
     * vznikla z této kopie; jinak chyba a páska beze změny.
     * use[i] != 0 = přidat řádek i; řádek s výsledkem jiným než ADDABLE /
     * ADDABLE_REPAIRED je chyba. @p use NULL při nenulovém počtu řádků je
     * chyba. Žádný vybraný řádek = úspěch beze změny (*added 0).
     *
     * Bloky souborů se staví jako v záznamu: cmttmz_build_mzf_block()
     * s výchozí rychlostí decku (g_cmt.mz_cmtspeed, vlastní pulzy
     * g_cmt.mz_custom_pulses), záznam rychlosti DEFAULT, data MZF
     * z analýzy (u opraveného souboru doplněná 00h).
     *
     * @p replace == 0 (Add): bloky souborů se vloží za surový blok
     * (cmttmz_insert_blocks() na pozici block_id + 1); pauza jako u záznamu:
     * 1000 ms mezi dvěma vloženými bloky, poslední 0 (pravidlo švu mu dá
     * 1000 ms, následuje-li blok). Surový blok zůstává.
     *
     * @p replace == 1 (Replace): surový blok se nahradí (cmttmz_replace_block())
     * posloupností v pořadí řádků: vybraný řádek = blok 0x46, každý souvislý
     * běh nevybraných řádků = jeden blok 0x18 se stejnou frekvencí a beze
     * změny zkopírovanými pulzy (začíná-li běh pulzem HIGH, předřadí se pulz
     * LOW 1 vzorek, jinak by se úrovně obrátily). Surový blok běhu sahá od
     * first_pulse jeho prvního do end_pulse jeho posledního řádku, takže
     * krátký vynechaný úsek uvnitř běhu v něm zůstane; krátký úsek vedle
     * bloku souboru a na kraji bloku se zahodí jako u záznamu. Každý pulz
     * řádku, který zůstává surový (neznámý úsek, nevybraný soubor, soubor,
     * který nejde přidat), je tedy dál na pásce v surovém bloku, v původním
     * pořadí vůči blokům souborů. Časová osa se nedrží: pauza bloku souboru
     * je jako u záznamu (cmttmz_rec_mzf_pause(): 1000 ms, je-li další nový
     * blok také blok souboru, jinak 0). Poslední nový blok: surový blok nese
     * pauzu nahrazeného bloku (v hlavičce CSW); blok souboru pauzu
     * nahrazeného bloku, a následuje-li na pásce další blok, nejméně
     * CMTTMZ_ANALYZE_FILE_GAP_MS (1000 ms; soubor nikdy bez mezery před
     * dalším blokem). Páska je po nahrazení obvykle
     * kratší (blok 0x46 MZ800EMU-CMT hraje jednu kopii s kratším GAP, záznam
     * ROM dvě kopie s plným GAP). Běh nevybraných řádků s prázdným rozsahem
     * (jen soubor celý uvnitř dřívějšího) surový blok nedá. Surový zbytek,
     * který by s předřazeným pulzem přesáhl mez playeru
     * (TMZ_PLAYER_MAX_BLOCK_PULSES pulzů, nebo délku bloku podle
     * tzx_csw_recording_check()), je chyba a nahrazení se neprovede.
     *
     * Údržba pásky jako u ostatních úprav (cmttmz_insert_blocks()): nové bloky
     * neuložené, Modified 1, čítač výměn pásky vzroste, kurzor sleduje
     * otevřený blok (otevřený surový blok nahradí první nový blok). Bloky
     * 0x18 z Replace mají typ indexu jako původní blok (WAV / TZX).
     *
     * Varování (cmttmz_get_edit_warning(), úprava se provede): bloky souborů
     * vznikají pro výchozí takt playeru; hraje-li některý z nich jiným taktem
     * (blok 0x43 s jiným taktem před surovým blokem), věta
     * CMTTMZ_CLOCK_WARNING jako u importu (cmttmz_warn_new_blocks_clock()).
     * Replace ji může dát i pro dosavadní bloky
     * (cmttmz_edit_apply()); věta se pak neopakuje.
     *
     * Chyby (text v cmttmz_get_edit_error()): "No tape inserted", "The tape
     * changed since the analysis; analyze the block again", "Block N changed
     * since the analysis; analyze the block again", "Row N cannot be added",
     * "The analysis does not belong to these pulses", "Can't build a tape
     * block from row N", "The row selection is missing", "Block N cannot be
     * split: a raw part would be longer than a playable block", "Not enough
     * memory" a chyby úprav pásky (cmttmz_insert_blocks(), cmttmz_replace_block()).
     *
     * @param p Kopie pulzů, ze které analýza vznikla.
     * @param a Analýza (cmttmz_analyze_pulses() nad @p p).
     * @param use Pole cmttmz_analysis_row_count() příznaků (NULL jen při 0 řádcích, jinak chyba).
     * @param replace 0 = přidat za surový blok, 1 = nahradit surový blok.
     * @param[out] added Počet přidaných bloků souborů (smí být NULL); při chybě 0.
     * @return EXIT_SUCCESS; EXIT_FAILURE (páska beze změny, text chyby).
     * @post Po úspěchu s přidanými bloky (*added > 0) nese cmttmz_get_edit_warning() varování o taktu
     *       (viz výše), nebo ""; bez vybraného řádku se kanál nemění.
     * @pre Vlákno emulátoru pod device_state_lock(); transport ve STOP,
     *      neběží nahrávání ani záznam vstupů (ověřuje obal
     *      cmt_tape_analysis_apply(), cmt_tape_edit_allowed()).
     */
    extern int cmttmz_analysis_apply ( const struct st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYSIS *a, const uint8_t *use,
                                       int replace, uint32_t *added );

    /**
     * @brief Číslo prvního bloku souboru, který přidá úspěšné cmttmz_analysis_apply() s týmž výběrem.
     *
     * Add vkládá bloky souborů za surový blok (block_id + 1). Replace staví
     * posloupnost v pořadí řádků: před první vybraný řádek přijde surový
     * blok jen tehdy, když běh nevybraných řádků před ním má neprázdný
     * rozsah (stejné dělení na úseky jako cmttmz_analysis_apply()). Okno
     * a MCP z čísla vybírají první přidaný soubor.
     *
     * @param a Analýza. @param use Výběr (cmttmz_analysis_row_count() položek).
     * @param replace 0 = Add, 1 = Replace.
     * @return Číslo bloku po uplatnění; -1 bez vybraného řádku nebo s NULL.
     * @note Čistá funkce (čte jen @p a a @p use); platí pro pásku hned po
     *       úspěšném uplatnění téže analýzy s týmž výběrem.
     */
    extern int cmttmz_analysis_first_file_block ( const st_CMTTMZ_ANALYSIS *a, const uint8_t *use, int replace );


#ifdef MZTEST_HEADLESS

    struct st_WAV_ANALYZER_RESULT; /* libs/wav_analyzer/wav_analyzer.h */

    /**
     * @brief Jen pro testy: analýza kopie z daného výsledku analyzátoru místo spuštění analyzátoru.
     *
     * Řádky staví stejná cesta jako cmttmz_analyze_pulses() (plán, výsledky,
     * časy); test tak může ověřit výsledky, které se z vyrobeného signálu
     * získávají těžko (ZX bez header_leader_pulse, překryv souborů).
     *
     * @param p Kopie pulzů (count > 0). @param opts Volby (NULL = výchozí).
     * @param res Výsledek (files alokované malloc(), MZF mzf_free()-kompatibilní);
     *        funkce ho vždy uvolní wav_analyzer_result_destroy().
     * @return Analýza, nebo NULL při NULL parametru, prázdné kopii nebo selhání alokace.
     * @pre Vlákno testu.
     */
    extern st_CMTTMZ_ANALYSIS* cmttmz_analysis_test_from_result ( const struct st_CMTTMZ_PULSES *p,
                                                                   const st_CMTTMZ_ANALYZE_OPTS *opts,
                                                                   struct st_WAV_ANALYZER_RESULT *res );

    /**
     * @brief Jen pro testy: mez pulzů surového zbytku při Replace místo TMZ_PLAYER_MAX_BLOCK_PULSES.
     * @param max_pulses Mez (0 = výchozí TMZ_PLAYER_MAX_BLOCK_PULSES).
     * @pre Vlákno testu.
     */
    extern void cmttmz_analysis_test_set_max_pulses ( uint32_t max_pulses );

#endif


#ifdef __cplusplus
}
#endif

#endif /* CMT_TMZ_ANALYZE_H */
