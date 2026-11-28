#ifndef IFACE_AUDIO_RESAMPLER_H
#define IFACE_AUDIO_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "emulator/audio.h"
#include "videorec/videorec_audio.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Kapacita přenosu buněk v st_IFACE_AUDIO_CTC0_RESAMPLER.
     *
     * Horní mez IFACE_AUDIO_CTC0_CARRY_MAX všech platforem (MZ-800 a MZ-700
     * PAL 13, MZ-1500 a MZ-700 NTSC 11), aby šel stav převodu použít
     * s parametry libovolné normy v jednom sestavení (testy). Platformní mez
     * hlídá statický assert v iface_audio_resampler.c.
     */
#define IFACE_AUDIO_CTC0_CARRY_LIMIT 16u

    /**
     * @brief Stav převodu buněk CTC0 na vzorky SDL cesty (iface_audio_resampler_ctc0_run()).
     *
     * Nulová struktura je platný počáteční stav (koeficienty se spočtou při
     * prvním převodu, filtry a přenos jsou prázdné). Mimo emulaci: snapshot,
     * replay ani otisk stroje stav neobsahují.
     *
     * Invariant: `carry_count <= IFACE_AUDIO_CTC0_CARRY_LIMIT`; platí prvních
     * `carry_count` prvků `carry`, prvek `carry_count - k` je buňka -k dalšího
     * snímku.
     *
     * Ownership: struktura nic nevlastní; kopírovatelná hodnotou.
     */
    typedef struct st_IFACE_AUDIO_CTC0_RESAMPLER
    {
        st_VIDEOREC_AUDIO_CHSTATE chain;              /**< Střídavá vazba, low-pass a anti-glitch (videorec_audio_chain_ctc0()). */
        st_VIDEOREC_AUDIO_COEF coef;                  /**< Koeficienty řetězce pro IFACE_AUDIO_SAMPLE_RATE. */
        bool coef_ready;                              /**< `coef` je spočtené. */
        uint8_t carry[IFACE_AUDIO_CTC0_CARRY_LIMIT];  /**< Buňky z konce předchozího snímku pro první vzorek dalšího bloku. */
        size_t carry_count;                           /**< Počet platných přenesených buněk. */
    } st_IFACE_AUDIO_CTC0_RESAMPLER;

    /**
     * @brief Převod jednoho bloku buněk CTC0 na vzorky s explicitními konstantami normy.
     *
     * Výpočet iface_audio_resampler_output_stream_ctc0() nad zadaným stavem
     * a jednotkami buňky: platformní verze ho volá se statickým stavem
     * a konstantami sestavení, testy s parametry PAL i NTSC.
     *
     * Vzorek i je vážený součet buněk přes `[edge_pos[i], edge_pos[i + 1])`.
     * Buňky mezi krajními mají všechny váhu `cell_units`, takže se jen
     * spočítají (běžné a AUDIO_LEVEL_CTC0_DIV2 zvlášť); jednotlivě se váží
     * jen krajní buňky. Buňka hrany se určuje dělením jen tehdy, když se
     * délka intervalu liší od předchozí (začátek bloku, ořízlá hrana);
     * jinak se k buňce a zbytku přičte podíl a zbytek délky. Vzorek, jehož
     * interval začíná v přenesených buňkách (záporné indexy), se počítá
     * obecnou smyčkou přes buňky. Celočíselné součty i následné výrazy
     * v plovoucí čárce jsou shodné s dřívějším vážením každé buňky zvlášť,
     * výstup je tedy bitově stejný (ověřuje test audio_ctc0_resample_equiv).
     *
     * @param rs           Stav převodu (nesmí být NULL); mění se filtry a přenos.
     * @param cells        Buňky snímku, `cells_count` bajtů.
     * @param cells_count  Počet buněk snímku, > 0.
     * @param edge_pos     `n + 1` poloh hranic v jednotkách `1 / cell_units` buňky
     *                     (iface_audio_block_timing_for()); mimo meze se ořízne.
     * @param n            Počet výstupních vzorků, > 0.
     * @param contiguous   Log navazuje na předchozí volání se stejným `rs`.
     * @param cell_units   Jednotek polohy na buňku (`rate * frame_ticks` normy), > 0.
     * @param carry_max    Nejvyšší počet přenášených buněk normy (IFACE_AUDIO_CTC0_CARRY_MAX), 1 až IFACE_AUDIO_CTC0_CARRY_LIMIT.
     * @param out          Výstup, `n` vzorků.
     * @param raw_out      Výstup po střídavé vazbě před low-passem (`n` vzorků), nebo NULL.
     * @pre Součty vah přes interval vzorku (`(carry_max + 2) * cell_units`) se vejdou do int64.
     * @post Přenesené buňky v `rs` odpovídají konci tohoto snímku.
     * @note Stav `rs` smí používat jen jedno vlákno najednou.
     */
    void iface_audio_resampler_ctc0_run(st_IFACE_AUDIO_CTC0_RESAMPLER *rs, const uint8_t *cells, size_t cells_count,
                                        const int64_t *edge_pos, size_t n, bool contiguous, int64_t cell_units,
                                        size_t carry_max, float *out, float *raw_out);

    /**
     * @brief Nastaví střední hodnotu střídavé vazby ve stavu převodu CTC0.
     *
     * @param rs     Stav převodu (nesmí být NULL).
     * @param mean01 Střední hodnota CTC0 0..1; mimo rozsah se ořízne.
     */
    void iface_audio_resampler_ctc0_state_seed(st_IFACE_AUDIO_CTC0_RESAMPLER *rs, double mean01);

    /**
     * @brief Rozdělí snímek zvukového logu na buňky mezilehlého proudu CTC0 (bodové vzorky).
     *
     * Buňka j má hodnotu kanálu v taktu `offset + floor(j * span / cells_count)`
     * logu (událost začínající o takt později se už počítá, jako dosud).
     * Při 100 % je `span` délka snímku a buňka trvá přesně
     * 2 * GDGCLK_CTC0_DIVIDER taktů; při vlastní rychlosti se snímek
     * rozdělí na stejný počet buněk (změna výšky tónu podle rychlosti).
     * Počítá se celočíselně bez dělení ve smyčce. Kanál beze změny v celém
     * logu vyplní buňky poslední hodnotou.
     *
     * @param source      Log kanálu CTC0.
     * @param offset      Začátek snímku v logu [takty od first_timestamp].
     * @param span        Délka snímku v logu [takty].
     * @param cells       Výstup, `cells_count` bajtů.
     * @param cells_count Počet buněk na snímek (IFACE_AUDIO_CTC0_SAMPLES_PER_FRAME), > 0.
     * @note Čistá funkce nad vstupem; libovolné vlákno.
     */
    void iface_audio_resampler_ctc0_cells(const st_AUDIO_SOURCE_LOG *source, uint64_t offset, uint64_t span, uint8_t *cells, size_t cells_count);

    /**
     * @brief Vytvoří zvukový proud CTC0 ve float z buněk snímku a hranic vzorků na souvislé ose.
     *
     * Buňky nesou hodnoty kanálu CTC0: 0, AUDIO_MAXVAL_PER_CHANNEL (váha 1)
     * a AUDIO_LEVEL_CTC0_DIV2 (režim 3 s děličem 2, váha AUDIO_CTC0_DIV2_FACTOR).
     * Vzorek i je vážený součet vah buněk přes přesný interval
     * `[edge_pos[i], edge_pos[i + 1])` (polohy iface_audio_block_cell_pos(),
     * buňka b pokrývá `[b * d, (b + 1) * d)`, d = IFACE_AUDIO_SAMPLE_RATE *
     * VIDEO_SCREEN_TICKS): vnitřní buňky celé, krajní podílem překryvu,
     * celočíselně. Součet se dělí délkou intervalu v buňkách plus jedna
     * (zesílení iface_audio_ctc0_sdl_gain(), u každého vzorku stejné). Hrana
     * vzorku uvnitř buňky se tak neposouvá na hranici buňky a vzorek
     * odpovídá skutečnému intervalu (bez čar kolem nosné ze vzoru 12/13
     * buněk na vzorek). Pak střídavá vazba (horní propust 1. řádu,
     * AUDIO_CTC0_AC_TAU_MS; výstup je souměrný kolem nuly a stálá úroveň
     * doznívá), jednopólový low-pass a anti-glitch filtr - sdílená funkce
     * videorec_audio_chain_ctc0(). Stav filtrů drží modul mezi voláními.
     * Výpočet provádí iface_audio_resampler_ctc0_run() se statickým stavem
     * modulu a konstantami platformy (vnitřní buňky intervalu se jen
     * počítají, krajní se váží).
     *
     * Hranice leží na souvislé časové ose (iface_audio_block_timing()), takže
     * první vzorek bloku průměruje i buňky z konce předchozího snímku
     * (záporné indexy). Modul je přenáší: buňky od buňky poslední hrany
     * bloku (`[floor(edge_pos[n] / d), cells_count)`, včetně buňky, kterou
     * poslední vzorek započítal jen částí) si uloží a další blok je použije
     * jako buňky -k až -1. Nenavazuje-li log na předchozí
     * (`contiguous == false`: start, pauza, MAX SPEED, skok času) nebo
     * neodpovídá-li počet přenesených buněk první hranici, nahradí je
     * hodnotou první buňky snímku (časová osa se tím založí znovu, bez
     * čtení mimo pole).
     *
     * @param cells                Buňky snímku (iface_audio_resampler_ctc0_cells()).
     * @param cells_count          Počet buněk, > 0.
     * @param edge_pos             `output_samples_count + 1` rostoucích poloh hranic
     *                             (iface_audio_block_timing()); buňka první v
     *                             `[-IFACE_AUDIO_CTC0_CARRY_MAX, 0]`, poslední hrana v
     *                             `(0, cells_count * d]`; mimo meze se ořízne.
     * @param output_samples_count Počet výstupních vzorků; nenulový.
     * @param contiguous           Log navazuje na log předchozího volání.
     * @param raw_out              Výstup po střídavé vazbě před low-passem
     *                             (`output_samples_count` vzorků, vstup výstupní
     *                             propusti), nebo NULL.
     * @return Alokovaný buffer s výstupem (uvolní volající g_free()), NULL při nedostatku paměti.
     * @pre Volá jen vlákno zvukového výstupu (statický stav bez zámku).
     * @post Přenesené buňky odpovídají konci tohoto snímku.
     */
    float *iface_audio_resampler_output_stream_ctc0(const uint8_t *cells, size_t cells_count, const int64_t *edge_pos,
                                                    size_t output_samples_count, bool contiguous, float *raw_out);

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
     * @brief Rozpracovaný interval vzorku PSG na hranici bloku (konec snímku), po kanálech.
     *
     * První vzorek bloku začíná až o necelý vzorek před začátkem snímku
     * (souvislá časová osa); ten kus leží v logu předchozího snímku. Převod
     * si proto na konci bloku uloží integrál výchylky přes zbytek snímku za
     * poslední hranicí vzorku a jeho délku, a první vzorek dalšího bloku ho
     * přičte. Nulová struktura je platný počáteční stav (`valid` false).
     * Mimo emulaci: snapshot, replay ani otisk stroje ho neobsahují.
     *
     * Invariant: `valid` => `len` je délka zbytku předchozího snímku v taktech,
     * `sum` integrál výchylky přes jeho prvních `len - pending` taktů; zbylých
     * `pending` taktů (za posledním krokem PSG předchozího logu) doplní další
     * log, který tímto krokem začíná.
     */
    typedef struct st_IFACE_AUDIO_PSG_CARRY
    {
        double sum;      /**< Integrál výchylky přes známou část zbytku [výchylka * takty]. */
        int64_t len;     /**< Délka celého zbytku [takty], >= 0. */
        int64_t pending; /**< Konec zbytku za posledním krokem PSG, který doplní další log [takty], 0 až `len`. */
        bool valid;      /**< Platí pro navazující log (jinak se založí znovu). */
    } st_IFACE_AUDIO_PSG_CARRY;

    /**
     * @brief Průměrná výchylka kanálu PSG přes přesný interval každého vzorku bloku.
     *
     * Vzorek i pokrývá takty `[ticks[i], ticks[i + 1])` od začátku snímku
     * (iface_audio_block_ticks(); snímek začíná v taktu `frame_offset` logu
     * a trvá `frame_span` taktů). Výsledek je integrál výchylky
     * `volume[hodnota]` přes interval děleno jeho délkou, v plovoucí čárce
     * s dvojitou přesností jako renderer videorec_audio_render() (stejné
     * hranice vzorků, celé takty). Úsek logu j platí na taktech `[S_j, S_j +
     * count_ticks)` (S_j = součet předchozích délek), poslední úsek trvá dál.
     * Kus prvního vzorku před začátkem snímku dodá `carry` z předchozího
     * bloku; nenavazuje-li log (`contiguous` false nebo `carry` neplatné),
     * nahradí se výchylkou na začátku snímku. Na konci se do `carry` uloží
     * zbytek snímku za poslední hranicí. Interval celý v jednom úseku logu dá
     * přesně `volume[hodnota]` (bez dělení). Log beze změny (`samples_count`
     * 0, typicky ztlumený kanál) jde zkrácenou cestou bez smyčky přes úseky
     * logu, bitově se stejným výsledkem (test audio_psg_average_equiv proti
     * dřívější implementaci).
     *
     * Log nese skutečné hodnoty PSG jen do posledního kroku PSG snímku
     * (`valid_end`, st_AUDIO_LOG::last_psg_timestamp); zbytek snímku (nejvýš
     * PSG_DIVIDER - 1 taktů) je v něm jen držená hodnota, skutečná začíná až
     * dalším krokem v dalším logu. Část přenášeného zbytku za `valid_end` se
     * proto nezapočítá hned, ale z dalšího logu (jeho takty `[frame_offset -
     * pending, frame_offset)`). Vzorek tohoto bloku, jehož interval do
     * drženého úseku zasahuje (jen poslední, je-li přenášený zbytek kratší
     * než držený úsek), ho počítá drženou hodnotou (data ještě nejsou).
     *
     * Známé omezení (změřeno, potvrzeno modelem bez emulátoru): u velmi
     * vysokých tónů to podle fáze mřížky bloků proti krokům PSG vnáší do
     * basového pásma falešné složky. Výkon pásma 20 až 1000 Hz v sdl.wav nad
     * reference.wav přes 8 fází: MZ-800 dělič 2 +14 až +28 dB (nejsilnější
     * čára -50 dBc), 8 +2 až +17 dB, 10 0 až +18 dB, 20 0 až +3 dB; MZ-1500
     * dělič 2 +12 až +15 dB, 10 0 až +5 dB, 20 0 až +3 dB; od děliče 28 na
     * obou nejvýš asi 2 dB (podle metody a fáze). V modelu stejná chyba zmizí, když má poslední vzorek
     * bloku skutečné hodnoty. Oprava by musela poslední vzorek bloku odložit do
     * dalšího bloku (až je znám skutečný konec snímku), tedy posunout výstup
     * PSG proti CTC0 a proti rendereru o vzorek, nebo celou SDL cestu o vzorek
     * (přenos buněk CTC0 a časová osa bloků); neprovedeno.
     *
     * @param source       Log kanálu (samples_count 0 = stálá last_value).
     * @param frame_offset Začátek snímku v logu [takty od first_timestamp].
     * @param frame_span   Délka snímku v logu [takty], >= 1.
     * @param valid_end    Konec skutečných hodnot v logu [takty od first_timestamp]
     *                     (poslední krok PSG); UINT64_MAX = celý log platí.
     * @param ticks        `n + 1` neklesajících hranic [takty od začátku snímku]; `ticks[0] <= 0`,
     *                     `ticks[n] <= frame_span` (mimo meze se ořízne).
     * @param n            Počet vzorků, > 0.
     * @param contiguous   Log navazuje na log předchozího volání se stejným `carry`.
     * @param volume       Výchylky pro hodnoty logu 0 až 31.
     * @param carry        Stav na hranici bloku (čte se a přepíše).
     * @param avg          Výstup: `n` průměrných výchylek.
     * @param end_value    Výstup: hodnota logu na konci intervalu každého vzorku (`n` bajtů), nebo NULL.
     * @note Bez statického stavu; volající drží `carry` po kanálech.
     */
    void iface_audio_resampler_psg_average(const st_AUDIO_SOURCE_LOG *source, uint64_t frame_offset, uint64_t frame_span,
                                           uint64_t valid_end, const int64_t *ticks, size_t n, bool contiguous,
                                           const float *volume, st_IFACE_AUDIO_PSG_CARRY *carry, float *avg,
                                           uint8_t *end_value);

    /**
     * @brief Vytvoří zvukový proud kanálu PSG ve float přímo ze zvukového logu.
     *
     * Průměrná výchylka kanálu přes přesný interval vzorku na souvislé časové
     * ose (iface_audio_resampler_psg_average(); dřív bodový vzorek na konci
     * intervalu, při kterém se harmonické obdélníku nad 22 kHz překládaly do
     * slyšitelného pásma) -> parkování stálé nenulové hodnoty (podle hodnoty
     * na konci intervalu; jen bez AUDIO_HAVE_OUTPUT_COUPLING, jinak stálou
     * úroveň odstraní střídavá vazba výstupu v mixu) -> IIR `y += (x - y) / 6`.
     * Kanál beze změny v celém logu (`samples_count` 0) jde jako dosud bez
     * parkování a bez IIR: průměr se píše rovnou do výstupu a `raw_out`.
     * Stav na hranici bloku drží modul po kanálech.
     *
     * @param channel              Index kanálu (1 až AUDIO_SRC_CHANNELS_COUNT - 1).
     * @param source               Log kanálu.
     * @param frame_offset         Začátek snímku v logu [takty od first_timestamp].
     * @param frame_span           Délka snímku v logu [takty], >= 1.
     * @param valid_end            Konec skutečných hodnot PSG v logu [takty od first_timestamp]
     *                             (last_psg_timestamp - first_timestamp).
     * @param ticks                `output_samples_count + 1` hranic vzorků [takty od začátku snímku]
     *                             (iface_audio_block_ticks()).
     * @param contiguous           Log navazuje na log předchozího volání.
     * @param SN76489_volume_value Tabulka výchylek pro hodnoty logu 0 až 31.
     * @param output_samples_count Počet výstupních vzorků; nenulový, nejvýš IFACE_AUDIO_BLOCK_SAMPLES_MAX.
     * @param parked_samples       Počet vzorků stálé nenulové hodnoty před parkováním.
     * @param parked_fade          Délka útlumu zaparkované hodnoty ve vzorcích.
     * @param raw_out              Výstup po parkování před IIR (`output_samples_count`
     *                             vzorků, vstup výstupní propusti), nebo NULL.
     * @return Alokovaný buffer s výstupem (uvolní volající g_free()), NULL při nedostatku paměti.
     * @pre Volá jen vlákno zvukového výstupu (statický stav bez zámku).
     */
    float *iface_audio_resampler_process_psg_audio_log(int channel, const st_AUDIO_SOURCE_LOG *source,
                                                       uint64_t frame_offset, uint64_t frame_span, uint64_t valid_end,
                                                       const int64_t *ticks, bool contiguous, float *SN76489_volume_value,
                                                       size_t output_samples_count, size_t parked_samples,
                                                       size_t parked_fade, float *raw_out);
#ifdef __cplusplus
}
#endif
#endif // IFACE_AUDIO_RESAMPLER_H
