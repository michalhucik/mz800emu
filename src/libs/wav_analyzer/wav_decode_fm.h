/**
 * @file   wav_decode_fm.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.5.0
 * @brief  Vrstva 4a analyzéru - FM dekodér pro NORMAL/SINCLAIR/CPM/MZ-80B.
 *
 * Dekóduje data z FM modulovaného signálu (obdélníkové pulzy).
 * Všechny čtyři formáty sdílí stejnou modulaci - liší se jen časováním.
 *
 * Struktura záznamu na pásce:
 * - GAP: sekvence SHORT pulzů (leader tón)
 * - Tapemark: N_LONG long pulzů + N_SHORT short pulzů
 *   - Hlavička: 40 LONG + 40 SHORT
 *   - Tělo: 20 LONG + 20 SHORT
 * - 1 LONG pulz (sync/start)
 * - Data: bajty odesílány MSB first, každý bajt:
 *   - 8 datových bitů (1=LONG, 0=SHORT)
 *   - 1 stop bit (LONG)
 * - CRC: 2 bajty (16-bit popcount, big-endian)
 *
 * Bitový práh: leader_avg * 1.6 (WAV_ANALYZER_FM_THRESHOLD_FACTOR; wavdec.c měl 1.5)
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


#ifndef WAV_DECODE_FM_H
#define WAV_DECODE_FM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/mzf/mzf.h"

#include "wav_analyzer_types.h"


    /** @brief Typ tapemarku (určuje počet LONG a SHORT pulzů). */
    typedef enum en_WAV_TAPEMARK_TYPE {
        WAV_TAPEMARK_LONG = 0,  /**< dlouhý tapemark: 40 LONG + 40 SHORT (hlavička) */
        WAV_TAPEMARK_SHORT,     /**< krátký tapemark: 20 LONG + 20 SHORT (tělo) */
    } en_WAV_TAPEMARK_TYPE;


    /**
     * @brief Kontext FM dekodéru.
     *
     * Udržuje stav dekódování: aktuální pozici v sekvenci pulzů,
     * kalibrační práh pro rozlišení SHORT/LONG a CRC akumulátor.
     *
     * @par Invarianty:
     * - threshold_us > 0 (po kalibraci)
     * - pos < pulse_count
     */
    typedef struct st_WAV_FM_DECODER {
        const st_WAV_PULSE_SEQUENCE *seq;   /**< zdrojová sekvence pulzů */
        uint32_t pos;                       /**< aktuální pozice čtení v sekvenci */
        double threshold_us;                /**< práh SHORT/LONG (us) */
        uint16_t crc_accumulator;           /**< akumulátor checksumu (popcount) */
    } st_WAV_FM_DECODER;


    /**
     * @brief Volby obnovy pro wav_decode_fm_decode_mzf_ex().
     *
     * Nulová struktura (nebo NULL) znamená chování bez obnovy: soubor, jehož tělo
     * nejde přečíst, se zahodí.
     */
    typedef struct st_WAV_FM_DECODE_OPTIONS {
        int recover_body;      /**< 1 = ponechat neúplné tělo, zbytek do fsize doplnit 00h */
        int recover_header;    /**< 1 = ponechat hlavičku s platným CRC, jejíž tělo chybí (tělo 00h) */
    } st_WAV_FM_DECODE_OPTIONS;


    /**
     * @brief Informace o průběhu dekódování, i když dekódování selhalo.
     *
     * Vyplní ji wav_decode_fm_decode_mzf_ex(). Slouží k hlášení uživateli
     * a k vyplnění údajů o obnově ve výsledku souboru.
     */
    typedef struct st_WAV_FM_DECODE_INFO {
        uint32_t recovery_status;          /**< bitové OR en_WAV_RECOVERY_STATUS */
        uint32_t recovered_bytes;          /**< bajtů těla skutečně přečtených z pásky */
        uint32_t expected_bytes;           /**< fsize (po případné opravě); platné při header_decoded */
        int header_decoded;                /**< 1 = 128 B hlavičky přečteno (i když funkce dál selhala) */
        int header_crc_ok;                 /**< 1 = hlavička (1. kopie nebo kopie) má platný CRC; platné při header_decoded */
        en_WAV_ANALYZER_ERROR body_error;  /**< proč selhalo čtení hlavičky nebo těla (WAV_ANALYZER_OK = neselhalo) */
        uint8_t header_raw[MZF_HEADER_SIZE]; /**< hlavička jak byla na pásce; platné při header_decoded */
        uint32_t header_copy_diff;         /**< kolik bajtů hlavičky se liší od kopie, ačkoli obě mají platný součet (0 = shoda nebo kopie není) */
        uint32_t body_copy_diff;           /**< totéž pro tělo */
        uint32_t merged_bytes;             /**< při sloučení kopií: kolik bajtů těla pochází z 2. kopie */
    } st_WAV_FM_DECODE_INFO;


    /**
     * @brief Inicializuje FM dekodér.
     *
     * Nastaví počáteční pozici a kalibrační práh vypočtený
     * z průměrné délky leader pulzů.
     *
     * @param dec Kontext dekodéru k inicializaci. Nesmí být NULL.
     * @param seq Zdrojová sekvence pulzů. Nesmí být NULL.
     * @param start_pos Počáteční pozice čtení (typicky konec leaderu).
     * @param leader_avg_us Průměrná délka leader pulzu (us).
     *
     * @post dec je připraven k dekódování.
     *       dec->threshold_us = leader_avg_us * WAV_ANALYZER_FM_THRESHOLD_FACTOR
     */
    extern void wav_decode_fm_init (
        st_WAV_FM_DECODER *dec,
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t start_pos,
        double leader_avg_us
    );


    /**
     * @brief Detekuje tapemark v sekvenci pulzů od aktuální pozice.
     *
     * Tapemark se skládá z:
     * - N LONG pulzů (>= min_long_count)
     * - N SHORT pulzů (>= min_short_count)
     *
     * Pro hlavičku: 40 LONG + 40 SHORT (minimum 36 + 30)
     * Pro tělo: 20 LONG + 20 SHORT (minimum 18 + 18)
     *
     * Po úspěšné detekci posune pozici za tapemark.
     *
     * @param dec Kontext dekodéru. Nesmí být NULL.
     * @param type Typ tapemarku (LONG = hlavička, SHORT = tělo).
     * @return WAV_ANALYZER_OK při nalezení,
     *         WAV_ANALYZER_ERROR_DECODE_TAPEMARK při nenalezení.
     *
     * @post dec->pos je za posledním pulzem tapemarku.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_find_tapemark (
        st_WAV_FM_DECODER *dec,
        en_WAV_TAPEMARK_TYPE type
    );


    /**
     * @brief Najde tapemark podle celých period a přeskočí synchronizaci.
     *
     * Od dec->pos postupuje po dvojicích půlvln a průměr dvojice porovnává
     * s dec->threshold_us: hledá aspoň min_long dlouhých period a hned za
     * nimi aspoň min_short krátkých. Za krátkými přeskočí nejvýš 2 dlouhé
     * periody (synchronizace 2L). Na rozdíl od wav_decode_fm_find_tapemark()
     * nevadí asymetrické půlvlny, které se po kvantizaci překrývají
     * (Intercopy 2800/3200 Bd).
     *
     * @param dec Kontext dekodéru (pos = začátek hledání, threshold_us = práh
     *        průměru půlvlny). Nesmí být NULL.
     * @param min_long Minimální počet dlouhých period tapemarku.
     * @param min_short Minimální počet krátkých period tapemarku.
     * @param max_long Nejvyšší počet dlouhých period tapemarku (0 = bez meze).
     *        Delší běh (např. LTM 40 cizí hlavičky místo STM 20) je chyba,
     *        hledání nepokračuje.
     * @param max_skip Nejvýš tolik period smí ležet mezi dec->pos a začátkem
     *        tapemarku (0 = bez meze). Váže tapemark na konec leaderu.
     * @return WAV_ANALYZER_OK při nalezení,
     *         WAV_ANALYZER_ERROR_DECODE_TAPEMARK při nenalezení (do konce
     *         sekvence nebo do max_skip) a při běhu delším než max_long.
     *
     * @post Při úspěchu dec->pos ukazuje na první půlvlnu dat (za synchronizací),
     *       ve stejné fázi dvojic jako vstupní dec->pos.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_find_tapemark_period (
        st_WAV_FM_DECODER *dec,
        uint32_t min_long,
        uint32_t min_short,
        uint32_t max_long,
        uint32_t max_skip
    );


    /**
     * @brief Přečte jeden bit z FM signálu.
     *
     * Přečte jeden pulz (půl-periodu) a porovná s prahem.
     * Pulz >= threshold = LONG = bit 1.
     * Pulz < threshold = SHORT = bit 0.
     *
     * Aktualizuje CRC akumulátor (popcount).
     *
     * @param dec Kontext dekodéru. Nesmí být NULL.
     * @param[out] bit Výstupní hodnota bitu (0 nebo 1). Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu,
     *         WAV_ANALYZER_ERROR_DECODE_INCOMPLETE na konci sekvence.
     *
     * @post dec->pos je posunutá o 1 pulz.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_read_bit (
        st_WAV_FM_DECODER *dec,
        int *bit
    );


    /**
     * @brief Přečte jeden bajt z FM signálu.
     *
     * Struktura bajtu na pásce:
     * - 8 datových bitů (MSB first)
     * - 1 stop bit (LONG) - ověřen, ale ignorován při chybě
     *
     * @param dec Kontext dekodéru. Nesmí být NULL.
     * @param[out] byte Výstupní hodnota bajtu. Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @post dec->pos je za stop bitem.
     * @post dec->crc_accumulator je aktualizován.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_read_byte (
        st_WAV_FM_DECODER *dec,
        uint8_t *byte
    );


    /**
     * @brief Přečte blok bajtů z FM signálu.
     *
     * @param dec Kontext dekodéru. Nesmí být NULL.
     * @param[out] data Výstupní buffer pro data. Nesmí být NULL.
     * @param size Počet bajtů k přečtení.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @post dec->crc_accumulator obsahuje kumulativní popcount.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_read_block (
        st_WAV_FM_DECODER *dec,
        uint8_t *data,
        uint32_t size
    );

    /**
     * @brief Přečte blok bajtů a vrátí, kolik jich stihl přečíst.
     *
     * Čtení selže jen tehdy, když dojdou půlvlny (konec signálu).
     *
     * @param dec Kontext dekodéru. Nesmí být NULL.
     * @param[out] data Výstupní buffer pro data. Nesmí být NULL.
     * @param size Počet bajtů k přečtení.
     * @param[out] out_read Počet celých přečtených bajtů, i při chybě. Může být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @post dec->crc_accumulator obsahuje kumulativní popcount přečtených bitů.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_read_block_n (
        st_WAV_FM_DECODER *dec,
        uint8_t *data,
        uint32_t size,
        uint32_t *out_read
    );


    /**
     * @brief Přečte a ověří 16-bit popcount checksum.
     *
     * Přečte 2 bajty checksumu z pásky (big-endian) a porovná
     * s akumulovaným popcountem.
     *
     * @param dec Kontext dekodéru. Nesmí být NULL.
     * @param[out] crc_status Výsledek verifikace. Nesmí být NULL.
     * @param[out] crc_stored Přečtený checksum z pásky. Může být NULL.
     * @param[out] crc_computed Vypočtený checksum. Může být NULL.
     * @return WAV_ANALYZER_OK při úspěchu čtení (i při CRC chybě),
     *         WAV_ANALYZER_ERROR_DECODE_INCOMPLETE při chybě čtení.
     *
     * @post dec->crc_accumulator je resetován na 0.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_verify_checksum (
        st_WAV_FM_DECODER *dec,
        en_WAV_CRC_STATUS *crc_status,
        uint16_t *crc_stored,
        uint16_t *crc_computed
    );


    /**
     * @brief Najde a přečte 2. kopii sekce (Copy2) za oddělovačem SHORT.
     *
     * ROM (a BASIC 1Z-016 u kusů BSD) zapisuje kopii hned za
     * 256 SHORT, bez tónu a tapemarku. Hledá se v okně od začátku 1. kopie
     * (2x nominální délka sekce + rezerva) běh SHORT délky oddělovače, za
     * ním start bit a blok s checksumem. Kandidát s chybou CRC se přeskočí
     * a zkusí se další běh v okně (šum v 1. kopii umí vytvořit falešný
     * dlouhý běh SHORT). Běh SHORT delší než oddělovač (tón další části)
     * hledání ukončí.
     *
     * @param seq Sekvence pulzů.
     * @param section_start První datová půlvlna 1. kopie sekce (za start bitem).
     * @param threshold_us Práh SHORT/LONG v mikrosekundách.
     * @param expected_size Velikost dat sekce v bajtech (bez checksumu).
     * @param[out] out_data Data 2. kopie s CRC OK (alokována, uvolní volající),
     *             jinak NULL.
     * @param[out] out_crc_status WAV_CRC_OK (kopie s platným checksumem),
     *             WAV_CRC_ERROR (kopie strukturálně nalezena, ale checksum
     *             nesedí; data se nevracejí), WAV_CRC_NOT_AVAILABLE (nenalezena).
     * @param[out] out_crc_stored Uložený checksum. Může být NULL.
     * @param[out] out_crc_computed Vypočtený checksum. Může být NULL.
     * @param[out] out_end_pos Pozice za checksumem 2. kopie (i při WAV_CRC_ERROR,
     *             aby se oddělovač nehlásil jako leader). Může být NULL.
     * @return WAV_ANALYZER_OK, když byla kopie nalezena (CRC viz out_crc_status);
     *         WAV_ANALYZER_ERROR_NO_LEADER, když v okně není žádný oddělovač
     *         s čitelnou kopií; WAV_ANALYZER_ERROR_ALLOC; WAV_ANALYZER_ERROR_INVALID_PARAM.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_try_copy2 (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t section_start,
        double threshold_us,
        uint32_t expected_size,
        uint8_t **out_data,
        en_WAV_CRC_STATUS *out_crc_status,
        uint16_t *out_crc_stored,
        uint16_t *out_crc_computed,
        uint32_t *out_end_pos
    );

    /**
     * @brief Přečte tělo a jeho součet; s obnovou ponechá neúplná data.
     *
     * S rec->enable vrátí OK i pro tělo, ze kterého se přečetl aspoň 1 bajt: signál skončil
     * uvnitř těla nebo součtu, nebo tělo přerušil běh krátkých pulzů, který v datech být
     * nemůže (přemazáno dalším záznamem). Zbytek do size je pak 00h a crc_status WAV_CRC_ERROR.
     *
     * @param dec Dekodér na první datové půlvlně. Nesmí být NULL.
     * @param[out] data Výstup (size B). Nesmí být NULL.
     * @param size Velikost těla.
     * @param[in,out] rec Volba a výsledek obnovy. Může být NULL.
     * @param[out] crc_status Stav součtu.
     * @param[out] crc_stored Součet z pásky (0 u neúplného těla).
     * @param[out] crc_computed Vypočtený součet (0 u neúplného těla).
     * @param[out] out_end Pozice za koncem přečtených dat (při přerušení začátek běhu).
     * @return WAV_ANALYZER_OK; chybový kód čtení, když se tělo ponechat nedá.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_read_body (
        st_WAV_FM_DECODER *dec,
        uint8_t *data,
        uint32_t size,
        st_WAV_BODY_RECOVERY *rec,
        en_WAV_CRC_STATUS *crc_status,
        uint16_t *crc_stored,
        uint16_t *crc_computed,
        uint32_t *out_end
    );

    /**
     * @brief Najde začátek druhé kopie sekce (bez čtení dat).
     *
     * Hledá od section_start první běh SHORT o délce oddělovače kopie, ve stejném okně
     * jako wav_decode_fm_try_copy2(). Běh delší než oddělovač je tón další části
     * a hledání končí. Na rozdíl od wav_decode_fm_try_copy2() najde i kopii, která
     * se nedá přečíst celá.
     *
     * @param seq Sekvence pulzů.
     * @param section_start První datová půlvlna 1. kopie.
     * @param threshold_us Práh SHORT/LONG v mikrosekundách.
     * @param expected_size Velikost dat sekce v bajtech.
     * @param[out] out_start První datová půlvlna 2. kopie (za start bitem).
     * @return 1 = oddělovač nalezen, 0 = nenalezen.
     */
    extern int wav_decode_fm_find_copy_start (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t section_start,
        double threshold_us,
        uint32_t expected_size,
        uint32_t *out_start
    );

    /**
     * @brief Složí sekci ze dvou poškozených kopií.
     *
     * Každou kopii čte z obou konců podle start bitů (velikost je známá, konec dat určí
     * běh SHORT, mezera v signálu nebo konec sekvence), takže po ztrátě nebo přidání
     * půlvlny zůstanou platné úseky před i za poškozením. Bajt platný jen v jedné kopii se
     * vezme z ní. U bajtů platných v obou a rozdílných ustoupí kopie s pochybným bitem
     * (blízko prahu) kopii čisté; zbylé rozdíly (nejvýš 16) se zkouší proti součtu z pásky.
     *
     * Součet ROM je jen počet jedniček. Výsledek se proto přijme jedině tehdy, když každý
     * bajt je aspoň v jedné kopii platný, souhlasí se součtem z pásky a vyhovuje
     * právě jedna kombinace.
     *
     * @param dec Dekodér (práh, sekvence; nemění se). Nesmí být NULL.
     * @param from1 První datová půlvlna 1. kopie.
     * @param from2 První datová půlvlna 2. kopie.
     * @param size Velikost dat sekce (bez součtu), aspoň 1.
     * @param[out] out Složená data (size B); změní se jen při úspěchu.
     * @param[out] out_from_copy Kolik bajtů se od 1. kopie liší nebo v ní bylo neplatných. Může být NULL.
     * @return 1 = přijato, 0 = nejde složit jednoznačně nebo chyba alokace.
     */
    extern int wav_decode_fm_merge_copies (
        const st_WAV_FM_DECODER *dec,
        uint32_t from1,
        uint32_t from2,
        uint32_t size,
        uint8_t *out,
        uint32_t *out_from_copy
    );

    /**
     * @brief Dekóduje kompletní MZF soubor z FM signálu.
     *
     * Provede kompletní dekódování jednoho MZF souboru:
     * 1. Detekce tapemarku hlavičky (LTM); po půlvlnách, při neúspěchu
     *    po celých periodách (wav_decode_fm_find_tapemark_period(), nesouměrné
     *    půlvlny), stejně u STM v kroku 6 (nejvýš 30 dlouhých period)
     * 2. Přečtení 1 sync bitu (LONG)
     * 3. Dekódování 128B hlavičky
     * 4. Verifikace CRC hlavičky
     * 5. Nalezení dalšího leaderu (krátký GAP)
     * 6. Detekce tapemarku těla (STM)
     * 7. Přečtení 1 sync bitu (LONG)
     * 8. Dekódování těla (velikost z fsize v hlavičce)
     * 9. Verifikace CRC těla
     *
     * Za hlavičkou i tělem hledá druhou kopii (Copy2, ROM ji zapisuje hned
     * za 256 SHORT, bez tónu a tapemarku). Při chybě CRC 1. kopie a CRC OK
     * 2. kopie nahradí data a nastaví copy2_used. Kandidát bez platného CRC
     * data nikdy nenahradí.
     *
     * @param seq Sekvence pulzů. Nesmí být NULL.
     * @param leader Informace o leader tónu hlavičky. Nesmí být NULL.
     * @param[out] out_mzf Výstupní MZF struktura (volající uvolní přes mzf_free).
     *             Nesmí být NULL.
     * @param[out] out_header_result Výsledek dekódování hlavičky. Může být NULL.
     * @param[out] out_body_result Výsledek dekódování těla. Může být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @post Při úspěchu *out_mzf ukazuje na dekódovaný MZF soubor.
     * @post pulse_end hlavičky i těla je konec 2. kopie, když se strukturně
     *       našla (i s chybou CRC), jinak konec 1. kopie. Orchestrátor podle
     *       něj pokračuje (consumed_until, hledání části TURBO), takže se
     *       2. kopie nehlásí jako další leader.
     * @post Při fsize 0 se tělo nečte ani neověřuje: out_body_result má
     *       crc_status WAV_CRC_NOT_AVAILABLE, ostatní pole nezměněná.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_decode_mzf (
        const st_WAV_PULSE_SEQUENCE *seq,
        const st_WAV_LEADER_INFO *leader,
        st_MZF **out_mzf,
        st_WAV_DECODE_RESULT *out_header_result,
        st_WAV_DECODE_RESULT *out_body_result
    );

    /**
     * @brief Odhadne počet datových bajtů části FM z délky signálu.
     *
     * Od from počítá půlvlny do prvního běhu aspoň 32 SHORT půlvln (oddělovač kopie
     * 256 S, tón; v datech je nejdelší běh SHORT 16 půlvln, protože start bit každého
     * bajtu je LONG, kód ROM @WBYTE), do mezery v signálu, nebo do konce sekvence.
     * Od první datové půlvlny zabere část (n + 2) x 18 půlvln včetně koncového L,
     * proto n = (půlvlny + 1) / 18 - 2 (zaokrouhlení kvůli půlvlně splynulé s tichem).
     *
     * @param dec Dekodér (práh a sekvence). Nesmí být NULL.
     * @param from První datová půlvlna (za start bitem prvního bajtu).
     * @param[out] out_bytes Odhad počtu datových bajtů. Nesmí být NULL.
     * @param[out] out_frame_ok 1 = počet půlvln sedí na rámec bajtů (+- 1 půlvlna). Nesmí být NULL.
     * @return WAV_ANALYZER_OK; WAV_ANALYZER_ERROR_DECODE_DATA, když je signálu méně než
     *         na 1 bajt a checksum; WAV_ANALYZER_ERROR_INVALID_PARAM.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_estimate_body_bytes (
        const st_WAV_FM_DECODER *dec,
        uint32_t from,
        uint32_t *out_bytes,
        int *out_frame_ok
    );

    /**
     * @brief Dekóduje MZF soubor z FM signálu s volbami obnovy.
     *
     * Totéž jako wav_decode_fm_decode_mzf(), navíc umí ponechat poškozený
     * soubor podle voleb a popíše průběh v out_info.
     *
     * @param seq Sekvence pulzů. Nesmí být NULL.
     * @param leader Informace o leader tónu hlavičky. Nesmí být NULL.
     * @param opts Volby obnovy. NULL = bez obnovy.
     * @param[out] out_mzf Výstupní MZF struktura (volající uvolní přes mzf_free).
     *             Nesmí být NULL.
     * @param[out] out_header_result Výsledek dekódování hlavičky. Může být NULL.
     * @param[out] out_body_result Výsledek dekódování těla. Může být NULL.
     * @param[out] out_info Informace o průběhu (vyplní se i při chybě). Může být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @post Stejné jako u wav_decode_fm_decode_mzf().
     * @post S recover_body vrátí OK i pro tělo, ze kterého se přečetl aspoň 1 bajt:
     *       když signál skončí uvnitř těla nebo checksumu, nebo když tělo s chybou
     *       CRC přeruší běh SHORT, který v datech nemůže být (přemazáno dalším
     *       záznamem). Tělo má fsize bajtů (přečtené + 00h), CRC těla WAV_CRC_ERROR,
     *       recovery_status WAV_RECOVERY_PARTIAL_BODY, pulse_end je konec přečtených dat.
     * @post S recover_header vrátí OK pro hlavičku s platným CRC (a fsize > 0), jejíž tělo
     *       chybí (tapemark těla nenalezen, tapemark je LTM dalšího záznamu, nebo za
     *       tapemarkem není ani bajt dat).
     *       Tělo má fsize bajtů 00h, CRC těla WAV_CRC_NOT_AVAILABLE, recovery_status
     *       WAV_RECOVERY_HEADER_ONLY. Hlavička s chybou CRC bez těla se neponechá.
     * @post S recover_body se tělo s chybou CRC v obou kopiích složí, když to
     *       wav_decode_fm_merge_copies() dovolí: CRC těla WAV_CRC_OK,
     *       recovery_status WAV_RECOVERY_COPY_MERGED.
     * @post Neúplná hlavička (méně než 128 B) se neponechá nikdy.
     * @post Mají-li obě kopie platný součet a liší se nejvýš v polovině bajtů, je počet
     *       rozdílů v header_copy_diff / body_copy_diff a na stderr jde varování. Data se
     *       nemění. Kandidát bez platného součtu nebo s větším rozdílem se nebere jako kopie.
     * @post Bez ohledu na volby: má-li hlavička chybu CRC bez platné kopie a fsize
     *       nesplňuje checksum těla, ale délka těla odhadnutá ze signálu ano, použije
     *       se odhad (header.fsize, recovery_status WAV_RECOVERY_FSIZE_REPAIRED).
     *       CRC hlavičky zůstává WAV_CRC_ERROR.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fm_decode_mzf_ex (
        const st_WAV_PULSE_SEQUENCE *seq,
        const st_WAV_LEADER_INFO *leader,
        const st_WAV_FM_DECODE_OPTIONS *opts,
        st_MZF **out_mzf,
        st_WAV_DECODE_RESULT *out_header_result,
        st_WAV_DECODE_RESULT *out_body_result,
        st_WAV_FM_DECODE_INFO *out_info
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_DECODE_FM_H */
