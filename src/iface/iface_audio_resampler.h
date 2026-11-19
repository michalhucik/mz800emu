#ifndef IFACE_AUDIO_RESAMPLER_H
#define IFACE_AUDIO_RESAMPLER_H

#include <stdint.h>
#include "emulator/audio.h"

#ifdef __cplusplus
extern "C"
{
#endif

    uint8_t *iface_audio_resampler_process_audio_log(const st_AUDIO_SOURCE_LOG *source, uint64_t log_count_samples, size_t output_samples_count);
    /**
     * @brief Vytvoří zvukový proud CTC0 ve float ze vstupního proudu hodnot zvukového logu.
     *
     * Vstup nese hodnoty kanálu CTC0: 0, AUDIO_MAXVAL_PER_CHANNEL (váha 1)
     * a AUDIO_LEVEL_CTC0_DIV2 (režim 3 s děličem 2, váha AUDIO_CTC0_DIV2_FACTOR).
     * Vzorek je součet vah vstupních vzorků bloku dělený `(délka bloku + 1)`,
     * pak střídavá vazba (horní propust 1. řádu, AUDIO_CTC0_AC_TAU_MS; výstup je
     * souměrný kolem nuly a stálá úroveň doznívá), jednopólový low-pass
     * a anti-glitch filtr - sdílená funkce videorec_audio_chain_ctc0(). Stav
     * filtrů drží modul mezi voláními.
     *
     * @param input_samples        Vstupní proud (IFACE_AUDIO_CTC5253_SAMPLE_RATE).
     * @param input_samples_count  Počet vstupních vzorků; nenulový.
     * @param output_samples_count Počet výstupních vzorků; nenulový.
     * @param raw_out              Výstup po střídavé vazbě před low-passem
     *                             (`output_samples_count` vzorků, vstup výstupní
     *                             propusti), nebo NULL.
     * @return Alokovaný buffer s výstupem (uvolní volající g_free()), NULL při nedostatku paměti.
     * @pre Volá jen vlákno zvukového výstupu (statický stav bez zámku).
     */
    float *iface_audio_resampler_output_stream_ctc0(const uint8_t *input_samples, size_t input_samples_count, size_t output_samples_count, float *raw_out);

    /**
     * @brief Nastaví stav střídavé vazby CTC0 SDL cesty.
     *
     * Volá se, když zpracovávaný zvukový log nenavazuje na předchozí
     * (start, pauza, MAX SPEED, headless bez odběru): stav se převezme
     * z emulátoru (st_AUDIO_LOG::ctc0_ac_mean), který ho vede v emulovaném čase.
     *
     * @param mean01 Střední hodnota CTC0 0..1; mimo rozsah se ořízne.
     * @pre Stejné vlákno jako iface_audio_resampler_output_stream_ctc0().
     */
    void iface_audio_resampler_ctc0_ac_seed(double mean01);
    float *iface_audio_resampler_output_stream_psg(int channel, const uint8_t *input_samples, size_t input_samples_count, size_t output_samples_count, float *SN76489_volume_value, size_t parked_samples, size_t parked_fade);

    /**
     * @brief Vytvoří zvukový proud kanálu PSG ve float přímo ze zvukového logu.
     *
     * Bodové vzorky hodnoty kanálu -> tabulka výchylek -> parkování stálé
     * nenulové hodnoty (jen bez AUDIO_HAVE_OUTPUT_COUPLING; jinak stálou
     * úroveň odstraní střídavá vazba výstupu v mixu) -> IIR `y += (x - y) / 6`.
     * Kanál beze změny v celém logu vrací výchylku poslední hodnoty bez
     * parkování a bez IIR.
     *
     * @param channel              Index kanálu (1 až AUDIO_SRC_CHANNELS_COUNT - 1).
     * @param source               Log kanálu.
     * @param log_count_samples    Délka logu v ticích GDG.
     * @param SN76489_volume_value Tabulka výchylek pro hodnoty logu 0 až 31.
     * @param output_samples_count Počet výstupních vzorků; nenulový.
     * @param parked_samples       Počet vzorků stálé nenulové hodnoty před parkováním.
     * @param parked_fade          Délka útlumu zaparkované hodnoty ve vzorcích.
     * @param raw_out              Výstup po parkování před IIR (`output_samples_count`
     *                             vzorků, vstup výstupní propusti), nebo NULL.
     * @return Alokovaný buffer s výstupem (uvolní volající g_free()), NULL při nedostatku paměti.
     * @pre Volá jen vlákno zvukového výstupu (statický stav bez zámku).
     */
    float *iface_audio_resampler_process_psg_audio_log(int channel, const st_AUDIO_SOURCE_LOG *source, uint64_t log_count_samples, float *SN76489_volume_value, size_t output_samples_count, size_t parked_samples, size_t parked_fade, float *raw_out);
#ifdef __cplusplus
}
#endif
#endif // IFACE_AUDIO_RESAMPLER_H
