/**
 * @file   cmt_tmz_record.h
 * @brief  Záznam Virtual CMT: buffer hran PC1 8255 a jeho dekódování na bloky pásky.
 *
 * Při RECORD zapisuje emulovaný program na pásku přes PC1 8255. Tento
 * modul sbírá hrany toho signálu s časem v taktech GDG (GDGCLK_BASE
 * sestavení) a drží je jako posloupnost délek úseků stálé úrovně. Po
 * STOP z bufferu cmttmz_rec_decode() sestaví bloky pásky (0x46 a 0x18);
 * modul sám pásku nemění, bloky připojí volající. Buffer vypisuje jen
 * selhání alokace (stderr, anglicky); dosažení meze nehlásí, to dělá
 * volající.
 *
 * Model záznamu:
 * - cmttmz_rec_begin() vyprázdní buffer a zapamatuje úroveň PC1 na začátku
 *   (pio8255_pc1_get()); jiný globální stav emulátoru modul nečte,
 * - cmttmz_rec_edge() dostává čas hrany od začátku záznamu bez doby
 *   stojícího motoru (jak ho počítá cmt_write_data()) a úroveň PC1 po hraně,
 * - úsek je doba mezi dvěma po sobě jdoucími přijatými hranami; doba od
 *   begin do první hrany ani doba za poslední hranou úsekem nejsou (žádná
 *   hrana = žádný signál). Tři hrany tedy dají dva úseky,
 * - úroveň úseku i je first_level, když je i sudé, jinak opačná úroveň
 *   (úrovně se střídají; i úsek nulové délky má svou úroveň).
 *
 * Úsek delší než UINT32_MAX taktů (MZ-800 asi 242 s) se rozdělí stejně
 * jako v cmt_vstream_add_value_long(): úsek UINT32_MAX taktů, úsek délky 0
 * opačné úrovně a pokračování téže úrovně (opakovaně). Úseky nulové délky
 * proto vznikají jen tímto dělením; součet délek i střídání úrovní zůstává
 * přesné. Zvoleno místo 64bitových délek, aby ticks zůstalo uint32_t
 * (převod na pulzy WAV a CSW pracuje s 32bitovými délkami).
 *
 * Meze (vstup z emulovaného programu je nedůvěryhodný): nejvýš
 * CMTTMZ_REC_MAX_EDGES položek v ticks (včetně položek z dělení) a čas hrany
 * nejvýš CMTTMZ_REC_MAX_SECONDS * GDGCLK_BASE taktů od začátku záznamu.
 * Hrana, která by mez překročila, se zahodí a od té chvíle
 * cmttmz_rec_limit_reached() vrací 1 a všechny další hrany se zahazují
 * (buffer už neroste). Stejně se chová selhání alokace. Ukončení záznamu
 * a varování pro uživatele řeší volající.
 *
 * Paměť: ticks roste po blocích s geometrickým růstem (první blok
 * CMTTMZ_REC_INITIAL_CAPACITY položek, pak dvojnásobek, nejvýš mez), tedy
 * ne realloc na každou hranu; při mezi 16 000 000 položek nejvýš 64 MB.
 *
 * Vlákna: stav je globální v modulu a nechráněný; všechny funkce volá
 * jen vlákno emulátoru (cmt_write_data(), transport CMT).
 *
 * Dekódování (cmttmz_rec_decode()) je čistá funkce nad obsahem bufferu:
 * záznam převede na pulzy, předá je wav_analyzer_analyze_pulses()
 * a z výsledku sestaví bloky pásky (0x46 za každý bezchybně dekódovaný
 * soubor ROM, 0x18 CSW za nedekódované úseky). Globální stav nečte, pásku
 * ani transport nemění; bloky vrací volajícímu. Přijetí souboru, plán
 * částí a stavba bloku 0x46 jsou veřejné (cmttmz_rec_file_accepted(),
 * cmttmz_rec_plan(), cmttmz_rec_build_mzf()): sdílí je analýza surového
 * bloku pásky (cmt_tmz_analyze.h), aby záznam a analýza rozhodovaly stejně.
 *
 * Úroveň signálu: úroveň v bufferu i úroveň pulzu v sestaveném bloku je
 * úroveň PC1, kterou program zapsal. Při přehrání bloku dostane
 * g_cmt.output úroveň bloku a PC5 ji čte beze změny (cmt_read_data(),
 * pio8255.c; obrácení přepínačem polarity platí jen pro bloky typu WAV, viz
 * cmttmz_block_wrap()). Program tedy po přehrání čte stejnou polaritu, jakou
 * zapsal; ROM zapisuje bit začínající HIGH a čte PC5 = 1 jako bit 1 (báze
 * hw/14-cmt.md "Okamžik čtení bitu"), stejně jako u bloků MZF z mztape.
 * Ověřeno skutečnou ROM všech tří počítačů při výchozí polaritě
 * (tests/mcp/test_cmt_record.py, fáze E: SAVE a LOAD rutinami ROM): hlavička z \@WHEAD uložená jako
 * blok 0x18 se přehraje a \@RHEAD ji přečte bez chyby součtu.
 * Vztah úrovně PC1 k magnetizaci skutečné pásky emulátor nemodeluje.
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

#ifndef CMT_TMZ_RECORD_H
#define CMT_TMZ_RECORD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/tzx/tzx.h"
#include "libs/mzf/mzf.h"
#include "libs/wav_analyzer/wav_analyzer.h"
#include "cmt_tmz_import.h"


    /**
     * @brief Mez počtu položek bufferu (úseků včetně úseků nulové délky z dělení).
     *
     * Shodná s mezí bloku v knihovně TapeMZ. Při 4 B na položku nejvýš 64 MB.
     */
#define CMTTMZ_REC_MAX_EDGES 16000000u

    /**
     * @brief Mez délky záznamu v sekundách (čas hrany od začátku záznamu).
     *
     * Shodná s mezí bloku v knihovně TapeMZ; v taktech se násobí GDGCLK_BASE
     * sestavení (MZ-800 17 721 600 Hz, MZ-700 a MZ-1500 14 336 640 Hz).
     */
#define CMTTMZ_REC_MAX_SECONDS 14400u

    /**
     * @brief Počet položek prvního bloku bufferu (pak se kapacita zdvojnásobuje).
     */
#define CMTTMZ_REC_INITIAL_CAPACITY 65536u


    /**
     * @brief Obsah bufferu hran (pohled pro dekódování záznamu).
     *
     * Invarianty: count == 0 právě když ticks může být NULL; total_ticks je
     * součet ticks[0 .. count-1]; úroveň ticks[i] je first_level ^ (i & 1);
     * ticks[i] == 0 jen u úseku vloženého dělením dlouhého úseku (pak
     * 0 < i < count - 1 a ticks[i-1] == UINT32_MAX).
     *
     * Vlastnictví: struktura i pole ticks patří modulu. Ukazatel z
     * cmttmz_rec_get() i pole ticks platí do dalšího volání cmttmz_rec_edge(),
     * cmttmz_rec_begin() nebo cmttmz_rec_discard().
     */
    typedef struct st_CMTTMZ_REC {
        uint32_t *ticks; /**< délky úseků stálé úrovně v taktech GDGCLK_BASE */
        uint32_t count; /**< počet platných položek v ticks */
        int first_level; /**< úroveň PC1 (0 / 1) úseku ticks[0]; bez úseku úroveň PC1 po poslední přijaté hraně, bez hrany úroveň z begin */
        uint64_t total_ticks; /**< součet délek všech úseků (čas od první do poslední přijaté hrany) */
    } st_CMTTMZ_REC;


    /**
     * @brief Zahájí nový záznam: vyprázdní buffer a zapamatuje úroveň PC1.
     *
     * Uvolní dosavadní buffer (jako cmttmz_rec_discard()), vynuluje příznak
     * meze a nastaví first_level na pio8255_pc1_get() & 1. Hrana se stejnou
     * úrovní, jako má PC1 na začátku, se pak ignoruje.
     *
     * @pre Vlákno emulátoru.
     * @post Záznam je aktivní, count == 0, total_ticks == 0,
     *       cmttmz_rec_limit_reached() == 0.
     */
    void cmttmz_rec_begin ( void );

    /**
     * @brief Přidá hranu signálu PC1.
     *
     * Pravidla (v tomto pořadí):
     * - bez aktivního záznamu (před begin, po discard) nebo po dosažení meze
     *   se hrana zahodí,
     * - úroveň level_after & 1 shodná s úrovní po poslední přijaté hraně
     *   (u první hrany s úrovní z begin) se ignoruje,
     * - čas hrany nejvýš roven času poslední přijaté hrany se odmítne
     *   (úsek délky 0 nevznikne, úroveň se nemění); u první hrany se přijme
     *   každý čas včetně 0,
     * - čas nad CMTTMZ_REC_MAX_SECONDS * GDGCLK_BASE, úsek, pro který není
     *   v bufferu místo pod CMTTMZ_REC_MAX_EDGES (s položkami z dělení), nebo
     *   selhání alokace: hrana se zahodí a nastaví se příznak meze,
     * - jinak první hrana jen zahájí úsek (zapamatuje čas a úroveň), každá
     *   další uzavře úsek délky play_ticks - čas předchozí hrany.
     *
     * @param play_ticks  Čas hrany v taktech GDGCLK_BASE od začátku záznamu
     *                    bez doby stojícího motoru (jako v cmt_write_data()).
     * @param level_after Úroveň PC1 po hraně (bere se bit 0).
     * @pre Vlákno emulátoru.
     * @post Při přijetí hrany count vzroste nejvýš o 1 + 2 * (délka / UINT32_MAX)
     *       a total_ticks o délku úseku; jinak se buffer nemění.
     */
    void cmttmz_rec_edge ( uint64_t play_ticks, int level_after );

    /**
     * @brief Byla dosažena mez záznamu (počet položek, délka, alokace)?
     *
     * @return 1 po dosažení meze (další hrany se zahazují), jinak 0.
     *         Vynuluje ho jen cmttmz_rec_begin() nebo cmttmz_rec_discard().
     * @pre Vlákno emulátoru.
     */
    int cmttmz_rec_limit_reached ( void );

    /**
     * @brief Ověří mez délky záznamu i bez hrany (motor běží, program nezapisuje).
     *
     * cmttmz_rec_edge() hlídá mez času jen při příchodu hrany; s běžícím
     * motorem a bez hran by záznam neskončil nikdy. Volá se jednou za snímek
     * (cmt_screen_done_period(), stejně jako automatické ukončení): čas nad
     * CMTTMZ_REC_MAX_SECONDS * GDGCLK_BASE taktů (v testech mez ze setteru)
     * nastaví příznak meze, záznam pak skončí jako po mezi bufferu.
     *
     * @param play_ticks Aktuální čas od začátku záznamu bez doby stojícího
     *        motoru (takty GDGCLK_BASE, stejná základna jako u hran).
     * @return 1 = mez dosažena (i dřív, cmttmz_rec_limit_reached()); 0 jinak
     *         nebo bez aktivního záznamu.
     * @pre Vlákno emulátoru.
     * @post Při překročení času cmttmz_rec_limit_reached() == 1.
     */
    int cmttmz_rec_check_time ( uint64_t play_ticks );

    /**
     * @brief Běží záznam (cmttmz_rec_begin() bez následného cmttmz_rec_discard())?
     *
     * cmt_sanitize_state() podle něj pozná stav RECORD obnovený snapshotem
     * bez bufferu hran (buffer ve snapshotu není).
     *
     * @return 1 = aktivní, 0 = ne.
     * @pre Vlákno emulátoru.
     */
    int cmttmz_rec_is_active ( void );

    /**
     * @brief Vrátí obsah bufferu.
     *
     * @return Ukazatel na stav modulu (nikdy NULL); platnost viz st_CMTTMZ_REC.
     *         Po discard a před begin je count == 0 a ticks == NULL.
     * @pre Vlákno emulátoru.
     */
    const st_CMTTMZ_REC* cmttmz_rec_get ( void );

    /**
     * @brief Zahodí záznam: uvolní buffer a ukončí aktivní záznam.
     *
     * Další hrany se ignorují až do cmttmz_rec_begin(). Lze volat opakovaně
     * i bez předchozího begin.
     *
     * @pre Vlákno emulátoru.
     * @post count == 0, ticks == NULL, total_ticks == 0, příznak meze 0.
     */
    void cmttmz_rec_discard ( void );

    /**
     * @brief Čas poslední přijaté hrany (od začátku záznamu bez doby stojícího motoru).
     * @param[out] play_ticks Čas v taktech GDGCLK_BASE (smí být NULL).
     * @return 1 = záznam běží a má aspoň jednu přijatou hranu, jinak 0 (*play_ticks pak 0).
     * @pre Vlákno emulátoru.
     */
    int cmttmz_rec_last_edge ( uint64_t *play_ticks );

    /**
     * @brief Kopie probíhajícího záznamu (checkpoint záznamu vstupů v paměti).
     *
     * Neprůhledný typ: celý stav bufferu (úseky, úrovně, čas poslední hrany,
     * příznak meze). Vlastník ho uvolní cmttmz_rec_copy_free().
     */
    typedef struct st_CMTTMZ_REC_COPY st_CMTTMZ_REC_COPY;

    /**
     * @brief Zkopíruje probíhající záznam, nepřesáhne-li buffer @p max_bytes.
     *
     * @param max_bytes Nejvyšší velikost kopie úseků v bajtech (count * 4).
     * @param[out] too_big 1 = záznam běží, ale je větší než mez (smí být NULL).
     * @return Kopie; NULL bez aktivního záznamu, nad mezí nebo při selhání alokace.
     * @pre Vlákno emulátoru.
     */
    st_CMTTMZ_REC_COPY* cmttmz_rec_copy_take ( size_t max_bytes, int *too_big );

    /**
     * @brief Nahradí stav záznamu kopií (dosavadní buffer uvolní); záznam je pak aktivní.
     * @param copy Kopie (zůstává volajícímu).
     * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace (záznam pak neaktivní).
     * @pre Vlákno emulátoru.
     */
    int cmttmz_rec_copy_install ( const st_CMTTMZ_REC_COPY *copy );

    /**
     * @brief Velikost kopie úseků v bajtech.
     * @param copy Kopie (NULL = 0). @return count * sizeof ( uint32_t ).
     */
    size_t cmttmz_rec_copy_bytes ( const st_CMTTMZ_REC_COPY *copy );

    /** @brief Uvolní kopii záznamu (NULL je přípustné). @param copy Kopie. */
    void cmttmz_rec_copy_free ( st_CMTTMZ_REC_COPY *copy );


    /**
     * @brief Nejmenší počet pulzů, ze kterého vzniká blok pásky.
     *
     * Počítají se pulzy po sloučení úseků rozdělených kvůli délce (úsek
     * UINT32_MAX, úsek 0, pokračování = jeden pulz). Záznam s menším počtem
     * pulzů (pár hran, zákmit) nedá žádný blok; stejná mez platí pro
     * nedekódovaný úsek před, mezi a za přijatými soubory: kratší úsek
     * (ticho, pár hran) se zahodí.
     */
#define CMTTMZ_REC_MIN_EDGES 64u

    /** @brief Velikost textu v st_CMTTMZ_REC_REPORT (včetně ukončovací nuly). */
#define CMTTMZ_REC_REPORT_TEXT_SIZE 256u


    /**
     * @brief Druh bloku vytvořeného dekódováním záznamu.
     */
    typedef enum en_CMTTMZ_REC_BLOCK_KIND {
        CMTTMZ_REC_BLOCK_MZF = 0, /**< blok 0x46 MZ800EMU-CMT sestavený výchozí rychlostí (záznam rychlosti DEFAULT) */
        CMTTMZ_REC_BLOCK_RAW,     /**< blok 0x18 CSW s hranami záznamu (rychlost nejde měnit) */
    } en_CMTTMZ_REC_BLOCK_KIND;


    /**
     * @brief Popis bloku vytvořeného dekódováním (paralelně s polem bloků).
     *
     * Pravidlo pauzy je jen v cmttmz_rec_decode(): blok 0x46 má
     * opts->pause_ms, je-li další část téhož záznamu také 0x46, jinak 0;
     * blok 0x18 má vždy 0. Volající hodnotu přenese do vedlejšího pole
     * rychlostí (st_CMTTMZ_BLOCK_SPEED::pause_ms), aby přestavba bloku jinou
     * rychlostí pauzu zachovala.
     */
    typedef struct st_CMTTMZ_REC_BLOCK_INFO {
        en_CMTTMZ_REC_BLOCK_KIND kind; /**< druh bloku */
        uint16_t pause_ms;             /**< pauza za blokem v ms, se kterou je blok sestaven */
    } st_CMTTMZ_REC_BLOCK_INFO;


    /**
     * @brief Výsledek dekódování záznamu pro uživatele.
     *
     * Invariant: mzf_blocks + raw_blocks == počet vrácených bloků; text je
     * vždy ukončený nulou (oříznutý na CMTTMZ_REC_REPORT_TEXT_SIZE - 1 znaků).
     */
    typedef struct st_CMTTMZ_REC_REPORT {
        uint32_t mzf_blocks;  /**< počet bloků 0x46 (dekódované soubory) */
        uint32_t raw_blocks;  /**< počet bloků 0x18 (nedekódované úseky) */
        char text[CMTTMZ_REC_REPORT_TEXT_SIZE]; /**< shrnutí anglicky, nelokalizované (např. Recorded "NAME", 1024 B) */
    } st_CMTTMZ_REC_REPORT;


    /**
     * @brief Dekóduje záznam na bloky pásky.
     *
     * Postup:
     * 1. Méně než CMTTMZ_REC_MIN_EDGES pulzů (po sloučení rozdělených úseků)
     *    = žádný blok (EXIT_SUCCESS, *count_out == 0).
     * 2. Úseky se převedou na pulzy wav_analyzer: úsek rozdělený kvůli délce
     *    nad UINT32_MAX taktů (s úsekem délky 0) se sloučí zpět do jednoho
     *    pulzu, takže posloupnost nemá nulové pulzy a indexy pulzů ve výsledku
     *    analýzy odpovídají pulzům 1:1. sample_rate = GDGCLK_BASE,
     *    duration_samples = takty (nad UINT32_MAX nasycené), duration_us =
     *    takty * 1e6 / GDGCLK_BASE, úroveň PC1.
     * 3. wav_analyzer_analyze_pulses() s wav_analyzer_config_default() beze
     *    změny (keep_unknown 0, obnova recover_* vypnutá). Když analýza
     *    selže (alokace, chyba analyzátoru), záznam se neztratí: celý se
     *    uloží jako jeden blok 0x18 a text reportu to řekne.
     * 4. Soubor se přijme, jen když format == WAV_TAPE_FORMAT_NORMAL,
     *    header_crc == body_crc == WAV_CRC_OK (z první nebo druhé kopie),
     *    recovery_status == WAV_RECOVERY_NONE a mzf != NULL. Z něj vznikne
     *    blok 0x46 cmttmz_build_mzf_block() rychlostí opts->default_speed
     *    (opts->custom_pulses) a pauzou opts->pause_ms, je-li další část
     *    záznamu také blok 0x46, jinak 0 (před blokem 0x18 a na konci).
     *    Ostatní soubory (jiný formát, chyba kontrolního součtu, neúplné tělo)
     *    se nepřijmou a jejich pulzy patří do nedekódovaného úseku.
     * 5. Nedekódované úseky jsou pulzy před prvním přijatým souborem, mezi
     *    přijatými soubory (od consumed_until_pulse jednoho do
     *    header_leader_pulse dalšího) a za posledním. Úsek s aspoň
     *    CMTTMZ_REC_MIN_EDGES pulzy dá blok 0x18 (bez přijatého souboru je to
     *    celý záznam), kratší úsek se zahodí.
     * 6. Blok 0x18: frekvence GDGCLK_BASE / 2, pauza 0; konce pulzů se
     *    převádějí kumulativně (zaokrouhlením součtu taktů, ne každého pulzu
     *    zvlášť), pulz má nejméně 1 vzorek. Úsek začínající HIGH dostane
     *    předřazený pulz LOW délky 1 vzorek (první pulz CSW je LOW). Pulz delší
     *    než UINT32_MAX vzorků (ticho nad asi 8 minut) se zkrátí na
     *    UINT32_MAX vzorků. Blok má nejvýš TMZ_PLAYER_MAX_BLOCK_PULSES pulzů
     *    a nejvýš CMT_VSTREAM_MAX_BLOCK_SECONDS * frekvence vzorků; co by mez
     *    přesáhlo (jen při záznamu na mezi bufferu), se z konce úseku vynechá.
     * Bloky jsou v pořadí, v jakém leží v záznamu.
     *
     * @param rec Záznam (cmttmz_rec_get()); nesmí být NULL. Funkce ho jen čte.
     * @param opts Rychlost bloků 0x46 a pauza (NULL = cmttmz_import_opts_init():
     *        1:1, 1000 ms); ostatní pole se nepoužívají.
     * @param[out] blocks_out Nové pole *count_out bloků (st_TZX_BLOCK po
     *        hodnotě, data vlastní volající); při *count_out == 0 NULL. Bloky
     *        se předají tzx_file_append_block() (data převezme soubor) a pole
     *        se uvolní free(), nebo se vše uvolní cmttmz_rec_blocks_free().
     * @param[out] info_out Nové pole *count_out popisů bloků (druh a pauza,
     *        uvolnit free()); při *count_out == 0 NULL. Smí být NULL, pak se
     *        nevrací.
     * @param[out] count_out Počet bloků.
     * @param[out] rep Report (smí být NULL); vynuluje se na začátku.
     * @return EXIT_SUCCESS i pro "nic k uložení"; EXIT_FAILURE při NULL
     *         parametru, selhání alokace nebo stavby bloku (pak *blocks_out
     *         a *info_out jsou NULL, *count_out 0, rep->text popisuje chybu).
     *         Selhání samotné analýzy chybou není (viz krok 3).
     * @pre Žádné souběžné volání nad týmž rec (rec patří vláknu emulátoru).
     * @post Záznam ani globální stav emulátoru se nemění.
     */
    int cmttmz_rec_decode ( const st_CMTTMZ_REC *rec, const st_CMTTMZ_IMPORT_OPTS *opts, st_TZX_BLOCK **blocks_out,
                            st_CMTTMZ_REC_BLOCK_INFO **info_out, uint32_t *count_out, st_CMTTMZ_REC_REPORT *rep );

    /*
     * Sdílené části dekódování (záznam a analýza surového bloku,
     * cmt_tmz_analyze.c): přijetí souboru, plán částí a stavba bloku 0x46.
     * Chování záznamu (cmttmz_rec_decode()) určují právě tyto funkce;
     * analýza je volá s jinými parametry plánu.
     */

    /**
     * @brief Část posloupnosti pulzů, ze které vznikne jeden blok nebo řádek analýzy (cmttmz_rec_plan()).
     *
     * @invariant first_pulse <= end_pulse <= počet pulzů posloupnosti;
     *            rovnost jen u části MZF v režimu všech souborů (soubor celý
     *            uvnitř dřívějšího), jinak first_pulse < end_pulse.
     * @invariant Části plánu jdou za sebou bez překryvu (end_pulse jedné
     *            <= first_pulse další).
     * @invariant U MZF je file index souboru ve výsledku analýzy; v režimu
     *            záznamu jsou first_pulse / end_pulse jeho header_leader_pulse /
     *            consumed_until_pulse, v režimu všech souborů začátek
     *            cmttmz_rec_file_start() nebo konec předchozí části (překryv)
     *            a consumed_until_pulse (nejméně first_pulse); u RAW má úsek
     *            aspoň min_gap pulzů plánu a file je 0.
     */
    typedef struct st_CMTTMZ_REC_PART {
        en_CMTTMZ_REC_BLOCK_KIND kind; /**< MZF = soubor analyzátoru, RAW = úsek bez souboru */
        uint32_t first_pulse;          /**< první pulz části */
        uint32_t end_pulse;            /**< pulz za částí */
        uint32_t file;                 /**< MZF: index souboru ve výsledku analýzy; RAW: 0 */
    } st_CMTTMZ_REC_PART;

    /**
     * @brief Má soubor analyzátoru platný rozsah pulzů (header_leader_pulse < consumed_until_pulse <= npulses)?
     * @param f Soubor. @param npulses Počet pulzů posloupnosti.
     * @return 1 = ano, 0 = ne.
     */
    int cmttmz_rec_file_range_valid ( const st_WAV_ANALYZER_FILE_RESULT *f, uint32_t npulses );

    /**
     * @brief Začátek souboru analyzátoru na pásce (pulz) jako řazení knihovny (file_position()).
     *
     * header_leader_pulse; výsledky formátu ZX_SPECTRUM a SINCLAIR ho
     * nenastavují (zůstává 0), u nich při 0 leader.start_index. U ostatních
     * formátů platí header_leader_pulse i jako 0 (soubor na začátku
     * posloupnosti; leader TURBO je datový leader za preloaderem).
     *
     * @param f Soubor. @return Index prvního pulzu souboru.
     */
    uint32_t cmttmz_rec_file_start ( const st_WAV_ANALYZER_FILE_RESULT *f );

    /**
     * @brief Má soubor analyzátoru data MZF s tělem aspoň fsize bajtů?
     * @param f Soubor. @return 1 = ano (také fsize 0), 0 = bez MZF nebo s kratším tělem.
     */
    int cmttmz_rec_file_body_complete ( const st_WAV_ANALYZER_FILE_RESULT *f );

    /**
     * @brief Je soubor z analýzy bezchybný záznam ROM, ze kterého záznam staví blok 0x46?
     *
     * Podmínky (cmttmz_rec_decode() krok 4): format == WAV_TAPE_FORMAT_NORMAL,
     * header_crc == body_crc == WAV_CRC_OK, recovery_status ==
     * WAV_RECOVERY_NONE, mzf != NULL, úplné tělo
     * (cmttmz_rec_file_body_complete()), platný rozsah
     * (cmttmz_rec_file_range_valid()). Analýza surového bloku ho bere jako
     * výsledek ADDABLE.
     *
     * @param f Soubor. @param npulses Počet pulzů posloupnosti.
     * @return 1 = přijmout, 0 = patří do nedekódovaného úseku.
     */
    int cmttmz_rec_file_accepted ( const st_WAV_ANALYZER_FILE_RESULT *f, uint32_t npulses );

    /**
     * @brief Sestaví plán částí z výsledku analýzy.
     *
     * Režim záznamu (@p all_files 0): přijaté soubory v pořadí podle
     * header_leader_pulse, soubor začínající před koncem předchozího se
     * přeskočí. Režim všech souborů (@p all_files 1): každý soubor s platným
     * rozsahem od cmttmz_rec_file_start() (i ZX / SINCLAIR bez
     * header_leader_pulse) v pořadí začátku, žádný se nevynechá; soubor
     * začínající před koncem předchozího dostane rozsah od konce předchozího
     * (překryv patří dřívějšímu), soubor celý uvnitř dřívějšího prázdný
     * rozsah. Úseky před, mezi a za soubory s aspoň @p min_gap pulzy dají
     * část RAW, kratší úseky se do plánu nedostanou. Bez souboru je jedinou
     * částí celá posloupnost (má-li aspoň @p min_gap pulzů).
     *
     * Záznam: @p all_files 0 (jen přijaté soubory, cmttmz_rec_file_accepted())
     * a @p min_gap CMTTMZ_REC_MIN_EDGES. Analýza surového bloku: @p all_files 1
     * (každý soubor s platným rozsahem, i s chybou nebo jiného formátu)
     * a @p min_gap CMTTMZ_REC_MIN_EDGES - části pokrývají posloupnost kromě
     * úseků bez souboru kratších než mez (stejně jako u záznamu).
     *
     * @param res Výsledek analýzy. @param npulses Počet pulzů posloupnosti.
     * @param all_files 0 = jen přijaté soubory, 1 = všechny soubory s platným rozsahem.
     * @param min_gap Nejmenší počet pulzů části RAW (0 se bere jako 1).
     * @param[out] parts Pole nejméně 2 * res->file_count + 1 částí.
     * @return Počet částí.
     */
    uint32_t cmttmz_rec_plan ( const st_WAV_ANALYZER_RESULT *res, uint32_t npulses, int all_files, uint32_t min_gap,
                               st_CMTTMZ_REC_PART *parts );

    /**
     * @brief Pauza za blokem 0x46 záznamu: opts->pause_ms, následuje-li další blok 0x46, jinak 0.
     * @param opts Volby (NULL = 0). @param next_is_mzf 1 = další blok posloupnosti je také 0x46.
     * @return Pauza v ms.
     */
    uint16_t cmttmz_rec_mzf_pause ( const st_CMTTMZ_IMPORT_OPTS *opts, int next_is_mzf );

    /**
     * @brief Sestaví z MZF analyzátoru blok 0x46 jako záznam (cmttmz_build_mzf_block()).
     *
     * Profil MZ800EMU-CMT, rychlost opts->default_speed (vlastní pulzy
     * opts->custom_pulses), tělo délky fsize.
     *
     * @param mzf MZF (hlavička v pořadí hostitele, tělo aspoň fsize bajtů).
     * @param opts Rychlost (default_speed, custom_pulses). @param pause_ms Pauza za blokem.
     * @param[out] block Blok po hodnotě (data alokovaná knihovnou, vlastní volající, free()).
     * @return EXIT_SUCCESS; EXIT_FAILURE při NULL parametru nebo chybě stavby (*block beze změny).
     */
    int cmttmz_rec_build_mzf ( const st_MZF *mzf, const st_CMTTMZ_IMPORT_OPTS *opts, uint16_t pause_ms, st_TZX_BLOCK *block );

    /**
     * @brief Uvolní pole bloků z cmttmz_rec_decode() včetně dat bloků.
     *
     * @param blocks Pole (smí být NULL). Data bloků, která už převzal soubor,
     *        musí mít volající nastavená na NULL.
     * @param count Počet bloků v poli.
     */
    void cmttmz_rec_blocks_free ( st_TZX_BLOCK *blocks, uint32_t count );

#ifdef MZTEST_HEADLESS

    /**
     * @brief Jen pro testy: nastaví meze záznamu místo výchozích.
     *
     * Existuje jen v sestavení testů (MZTEST_HEADLESS), produkční sestavení
     * meze měnit neumí. Platí od příští hrany a přežije begin i discard;
     * hodnota 0 vrací výchozí mez.
     *
     * @param max_edges Nejvýš položek bufferu (0 = CMTTMZ_REC_MAX_EDGES).
     * @param max_ticks Nejvyšší čas hrany v taktech (0 = CMTTMZ_REC_MAX_SECONDS * GDGCLK_BASE).
     * @pre Vlákno testu (žádné souběžné volání modulu).
     */
    void cmttmz_rec_test_set_limits ( uint32_t max_edges, uint64_t max_ticks );

    /**
     * @brief Jen pro testy: další volání cmttmz_rec_decode() se chovají, jako by analýza selhala.
     *
     * @param fail 1 = analýza "selže" (ověření zálohy celého záznamu jako
     *        bloku 0x18), 0 = normální chování.
     * @pre Vlákno testu.
     */
    void cmttmz_rec_test_fail_analysis ( int fail );

#endif


#ifdef __cplusplus
}
#endif

#endif /* CMT_TMZ_RECORD_H */
