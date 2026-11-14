/**
 * @file   wav_preprocess.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.0
 * @brief  Vrstva 0 analyzéru - předspracování WAV signálu.
 *
 * Poskytuje funkce pro přípravu signálu před extrakcí pulzů:
 *   - DC offset korekce (odečtení průměru)
 *   - vysokofrekvenční filtr (1-pólový IIR, alpha 0.997: asi 21 Hz při 44,1 kHz)
 *   - peak normalizace na rozsah [-1.0, 1.0]
 *   - výběr kanálu ze sterea
 *
 * Vstupem je WAV soubor přes generic_driver + wav knihovnu.
 * Výstupem je float buffer připravený pro pulse extraction.
 *
 * Pro čisté signály z emulátoru nejsou preprocessing kroky potřeba.
 * Pro reálné nahrávky z pásky jsou kritické.
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


#ifndef WAV_PREPROCESS_H
#define WAV_PREPROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/wav/wav.h"

#include "wav_analyzer_types.h"


    /**
     * @brief Načte vzorky z WAV souboru do float bufferu.
     *
     * Přečte vzorky z vybraného kanálu a uloží je jako normalizované
     * hodnoty (převod wav_decode_sample() knihovny wav: PCM 8 až 64 bit
     * v rozsahu [-1.0, 1.0], IEEE float 32/64 beze změny).
     *
     * @param h Otevřený handler s WAV daty. Nesmí být NULL.
     * @param sh Parsovaná WAV hlavička. Nesmí být NULL.
     * @param channel Index kanálu (0 = levý, 1 = pravý).
     *                Musí být < sh->channels.
     * @param[out] out_buffer Výstupní ukazatel na alokovaný buffer.
     *             Volající musí uvolnit přes free(). Nesmí být NULL.
     * @param[out] out_count Počet vzorků v bufferu. Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @pre h musí být otevřený s platným WAV obsahem.
     * @pre sh musí být výsledkem wav_simple_header_new_from_handler().
     * @post Při úspěchu *out_buffer ukazuje na nově alokovaný buffer
     *       o *out_count prvcích. Volající vlastní paměť.
     */
    extern en_WAV_ANALYZER_ERROR wav_preprocess_load_samples (
        st_HANDLER *h,
        const st_WAV_SIMPLE_HEADER *sh,
        uint16_t channel,
        double **out_buffer,
        uint32_t *out_count
    );


    /**
     * @brief Provede DC offset korekci - odečte průměr signálu.
     *
     * Spočítá aritmetický průměr všech vzorků a odečte jej od každého
     * vzorku. Odstraňuje stejnosměrnou složku signálu.
     *
     * @param buffer Pole vzorků k úpravě (in-place). Nesmí být NULL.
     * @param count Počet vzorků. Musí být > 0.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @pre buffer obsahuje platná float data.
     * @post Průměr vzorků v bufferu je přibližně 0.
     */
    extern en_WAV_ANALYZER_ERROR wav_preprocess_dc_offset (
        double *buffer,
        uint32_t count
    );


    /**
     * @brief Aplikuje 1-pólový IIR high-pass filtr.
     *
     * Odstraňuje nízkofrekvenční šum (50/60 Hz hum, mechanické vibrace).
     * Implementace: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
     *
     * @param buffer Pole vzorků k filtraci (in-place). Nesmí být NULL.
     * @param count Počet vzorků. Musí být > 0.
     * @param alpha Koeficient filtru (typicky 0.995-0.999).
     *              Vyšší hodnota = nižší mezní frekvence.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @pre 0.0 < alpha < 1.0
     * @post buffer obsahuje filtrovaný signál.
     */
    extern en_WAV_ANALYZER_ERROR wav_preprocess_highpass (
        double *buffer,
        uint32_t count,
        double alpha
    );


    /**
     * @brief Provede peak normalizaci na rozsah [-1.0, 1.0].
     *
     * Najde maximální absolutní hodnotu v bufferu a vydělí
     * všechny vzorky touto hodnotou. Pokud je signál nulový
     * (max == 0), nedělá nic.
     *
     * @param buffer Pole vzorků k normalizaci (in-place). Nesmí být NULL.
     * @param count Počet vzorků. Musí být > 0.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @pre buffer obsahuje platná float data.
     * @post Maximální absolutní hodnota v bufferu je <= 1.0.
     */
    extern en_WAV_ANALYZER_ERROR wav_preprocess_normalize (
        double *buffer,
        uint32_t count
    );


    /**
     * @brief Vybere nejlepší kanál ze stereo WAV souboru.
     *
     * Při automatickém výběru (WAV_CHANNEL_AUTO) porovná RMS
     * obou kanálů a vrátí index kanálu s vyšší energií.
     *
     * @param h Otevřený handler s WAV daty. Nesmí být NULL.
     * @param sh Parsovaná WAV hlavička. Nesmí být NULL.
     * @param channel_select Preferovaný kanál (LEFT/RIGHT/AUTO).
     * @return Index kanálu (0 nebo 1). Pro mono vždy 0.
     */
    extern uint16_t wav_preprocess_select_channel (
        st_HANDLER *h,
        const st_WAV_SIMPLE_HEADER *sh,
        en_WAV_CHANNEL_SELECT channel_select
    );


    /**
     * @brief Spustí kompletní preprocessing pipeline.
     *
     * Provede všechny konfigurované kroky předspracování:
     * 1. Výběr kanálu
     * 2. Načtení vzorků do float bufferu
     * 3. DC offset korekce (pokud enable_dc_offset)
     * 4. High-pass filtr (pokud enable_highpass)
     * 5. Peak normalizace (pokud enable_normalize)
     *
     * @param h Otevřený handler s WAV daty. Nesmí být NULL.
     * @param sh Parsovaná WAV hlavička. Nesmí být NULL.
     * @param config Konfigurace analyzéru. Nesmí být NULL.
     * @param[out] out_buffer Výstupní buffer (volající vlastní paměť).
     * @param[out] out_count Počet vzorků ve výstupním bufferu.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @post Při úspěchu *out_buffer obsahuje předspracované vzorky.
     */
    extern en_WAV_ANALYZER_ERROR wav_preprocess_run (
        st_HANDLER *h,
        const st_WAV_SIMPLE_HEADER *sh,
        const st_WAV_ANALYZER_CONFIG *config,
        double **out_buffer,
        uint32_t *out_count
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_PREPROCESS_H */
