/**
 * @file   videorec_audio.h
 * @brief  Renderer audio událostí na stereo PCM v emulačním čase pro video záznam.
 *
 * Vstupem jsou změny hodnot zvukového logu (0 až 31, viz audio.h
 * AUDIO_MAXVAL_PER_CHANNEL) jednotlivých zvukových kanálů s absolutním
 * časem v GDG taktech. Výstupem jsou stereo vzorky int16 (prokládané L, R).
 *
 * @par Rozložení kanálů do L/R
 * Každý kanál má masku stran (VIDEOREC_AUDIO_ROUTE_L / _R). Po
 * videorec_audio_init() jdou všechny kanály do obou stran (L == R).
 * videorec_audio_set_stereo() nastaví rozložení SDL cesty (iface_audio.c):
 * - stereo (iface_audio_mix_channels_stereo(), MZ-1500 a MZ-800 s druhým
 *   PSG): kanál 0 (CTC0) do obou stran, kanály 1-4 (PSG0) do L,
 *   kanály 5-8 (PSG1) do R;
 * - mono (iface_audio_mix_channels_with_gain()): kanály 0-4 do obou stran,
 *   kanály 5-8 (PSG1) se nemixují (SDL cesta je v mono režimu ignoruje).
 * Filtry běží pro všechny kanály vždy (stav nezávisí na rozložení), ořez
 * mixu se dělá pro každou stranu zvlášť.
 *
 * @par Časový model
 * Vzorek `n` pokrývá takty `[origin + n*clk/rate, origin + (n+1)*clk/rate)`
 * (celočíselné dělení se zaokrouhlením dolů, počítané z `n`, takže chyba se
 * nekumuluje). Vstupem filtrů je průměr úrovně kanálu přes tento interval
 * (box filtr), tj. `level[ch][hodnota]` vážené délkou, po kterou kanál hodnotu
 * držel.
 *
 * @par Řetězec SDL cesty (režim `sdl_chain`)
 * Nahrávka má znít jako výstup emulátoru, proto renderer kopíruje filtry SDL
 * cesty (src/iface/iface_audio_resampler.c, mix v src/iface/iface_audio.c):
 * - kanál 0 (CTC0): střídavá vazba (horní propust 1. řádu, časová konstanta
 *   VIDEOREC_AUDIO_CTC0_AC_TAU_MS) -> 1-pólový low-pass (`alpha` 0,4,
 *   iface_audio_lowpass_filter()) -> anti-glitch (0,2, iface_audio_anti_glitch_filter());
 * - kanály 1.. (PSG): parkování -> IIR `y += (x - y) / 6`;
 * - mix: součet kanálů každé strany (úrovně už obsahují gain a průměr přes
 *   4 kanály PSG, viz iface_audio_build_videorec_levels()) jako
 *   iface_audio_mix_channels_with_gain() / iface_audio_mix_channels_stereo();
 * - výstupní propust (volba, videorec_audio_set_output_filter()): místo
 *   low-passu, anti-glitch a IIR jednotlivých kanálů projde součet kanálů
 *   (CTC0 po střídavé vazbě, PSG po parkování) dolní propustí 1. řádu
 *   s časovou konstantou VIDEOREC_AUDIO_OUT_LP_TAU_US. Obě varianty se počítají
 *   stále, přepnutí je plynulý přechod VIDEOREC_AUDIO_OUT_XFADE_MS;
 * - střídavá vazba výstupu (videorec_audio_set_output_coupling(), MZ-800
 *   a MZ-1500): horní propust 1. řádu VIDEOREC_AUDIO_OUT_AC_TAU_MS na součtu
 *   kanálů každé strany (kondenzátor v sérii s výstupem čipu PSG); kanály PSG
 *   se pak neparkují;
 * - dorovnání hlasitosti a měkký omezovač (videorec_audio_output_stage()):
 *   výsledek se násobí VIDEOREC_AUDIO_OUT_MAKEUP_ON_DB nebo _OFF_DB podle
 *   stavu výstupní propusti a projde omezovačem videorec_audio_soft_limit()
 *   (pod kolenem VIDEOREC_AUDIO_OUT_KNEE přesně lineární, nad ním stlačení
 *   k 1,0 bez tvrdého ořezu).
 *
 * Střídavá vazba CTC0: na skutečném MZ-800 je CTC0 do výstupu vázaný
 * střídavě (změřeno osciloskopem 2026-11-18: obdélník souměrný kolem klidu,
 * stálá úroveň se vrací ke klidu s časovou konstantou 0,8 až 0,9 s). Stav je
 * střední hodnota kanálu `hp_mean` v jednotkách 0..1 (podíl času v jedničce);
 * výstup je `x - hp_mean * unit`. Nahrazuje dřívější parkování CTC0.
 *
 * Parkování (jen PSG): hodnota kanálu na konci vzorku, která je nenulová a beze změny
 * déle než 45 ms, se během cca 20 ms ve 20 krocích stáhne k 0 (SDL cesta tak
 * odstraňuje trvalou stejnosměrnou úroveň). Koeficienty jsou v SDL cestě
 * definované pro 44 100 Hz; při této frekvenci renderer používá přesně stejné
 * hodnoty, při jiné (48 000 Hz) přepočítané se stejnou zlomovou frekvencí
 * (`a = 1 - (1 - a44)^(44100/rate)`) a stejnými dobami parkování v ms
 * (videorec_audio_coef_init()).
 *
 * Proti SDL cestě se liší jen vstupní převzorkování: SDL bere bodové vzorky
 * (PSG) a u CTC0 podíl nenulových vzorků 553,8 kHz proudu dělený
 * `(délka bloku + 1)`; renderer průměruje přesně přes interval vzorku
 * a zesílení CTC0 dorovnává konstantou videorec_audio_sdl_ctc0_gain()
 * (zapečenou do tabulky úrovní). Paritu ověřuje
 * tests/videorec/test_videorec_audio_sdl_parity.c.
 *
 * Trvalou úroveň (CTC0 držený na 1), která by zabrala rozsah, odstraňuje
 * u CTC0 střídavá vazba a u PSG parkování, stejně jako v SDL cestě.
 *
 * @par Surový režim (bez `sdl_chain`)
 * Součet box průměrů kanálů bez filtrů (testy časování a box filtru).
 *
 * @par Výstup
 * Výsledek se převede na int16 (x 32767); v režimu `sdl_chain` je díky
 * omezovači vždy v [-1, 1], v surovém režimu se na tento rozsah ořízne.
 *
 * @par Vlákna
 * Jedna instance smí být používána jen z jednoho vlákna, nebo musí volající
 * všechna volání serializovat vnějším zámkem. Modul nemá žádný vlastní zámek.
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_AUDIO_H
#define VIDEOREC_AUDIO_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximální počet kanálů, které renderer zpracuje (nadbytečné se při init ořežou). */
#define VIDEOREC_AUDIO_MAX_CHANNELS 9

/**
 * @brief Počet úrovní na kanál (5bitová hodnota 0..31).
 *
 * Zvukový log používá hodnoty 0 až 31: 0 až 15 plná výchylka stupně
 * hlasitosti, 16 až 30 poloviční výchylka stupně `hodnota - 15` (tón PSG
 * s děličem 001h), 31 výchylka 0,64 stupně 15 (jen kanál CTC0 v režimu 3
 * s děličem 2, AUDIO_LEVEL_CTC0_DIV2 v audio.h).
 */
#define VIDEOREC_AUDIO_LEVELS 32

/** @brief Maska platných bitů hodnoty kanálu (VIDEOREC_AUDIO_LEVELS - 1). */
#define VIDEOREC_AUDIO_VALUE_MASK 0x1Fu

/** @brief Počet kanálů jednoho PSG (PSG_CHANNELS_COUNT); určuje rozložení kanálů do L/R. */
#define VIDEOREC_AUDIO_PSG_CHANNELS 4u

/** @brief Maska strany: kanál se mixuje do levého výstupu. */
#define VIDEOREC_AUDIO_ROUTE_L 0x01u
/** @brief Maska strany: kanál se mixuje do pravého výstupu. */
#define VIDEOREC_AUDIO_ROUTE_R 0x02u
/** @brief Maska strany: kanál se mixuje do obou výstupů. */
#define VIDEOREC_AUDIO_ROUTE_LR (VIDEOREC_AUDIO_ROUTE_L | VIDEOREC_AUDIO_ROUTE_R)

/** @brief Vzorkovací frekvence, pro kterou jsou definované koeficienty SDL cesty [Hz] (IFACE_AUDIO_SAMPLE_RATE). */
#define VIDEOREC_AUDIO_SDL_RATE 44100u

/** @brief Časová konstanta střídavé vazby CTC0 [ms] (AUDIO_CTC0_AC_TAU_MS v audio.h; změřeno 0,8 až 0,9 s). */
#define VIDEOREC_AUDIO_CTC0_AC_TAU_MS 850u

/** @brief Časová konstanta výstupní dolní propusti [us] (AUDIO_OUT_LP_TAU_US v audio.h; změřeno 460 us, zlom 346 Hz). */
#define VIDEOREC_AUDIO_OUT_LP_TAU_US 460u

/** @brief Časová konstanta střídavé vazby celého výstupu [ms] (AUDIO_OUT_AC_TAU_MS v audio.h). */
#define VIDEOREC_AUDIO_OUT_AC_TAU_MS 570u

/** @brief Délka plynulého přechodu při přepnutí výstupní propusti [ms]. */
#define VIDEOREC_AUDIO_OUT_XFADE_MS 10u

/**
 * @brief Dorovnání hlasitosti výstupu se zapnutou výstupní propustí [dB] (AUDIO_OUT_MAKEUP_FILTER_ON_DB v audio.h).
 *
 * Volba emulátoru, ne změřená hodnota; zdůvodnění viz audio.h.
 */
#define VIDEOREC_AUDIO_OUT_MAKEUP_ON_DB 8.0

/** @brief Dorovnání hlasitosti výstupu s vypnutou výstupní propustí a na strojích bez ní [dB] (AUDIO_OUT_MAKEUP_FILTER_OFF_DB v audio.h). */
#define VIDEOREC_AUDIO_OUT_MAKEUP_OFF_DB 4.0

/**
 * @brief Koleno měkkého omezovače výstupu (AUDIO_OUT_LIMIT_KNEE v audio.h).
 *
 * Výchylka (v rozsahu 0 až 1), do které je výstup přesně lineární.
 * Invariant: 0 < koleno < 1.
 */
#define VIDEOREC_AUDIO_OUT_KNEE 0.7f

/** @brief Hodnota kanálu CTC0 ve stavu "jednička" (AUDIO_MAXVAL_PER_CHANNEL); její úroveň je jednotka střídavé vazby. */
#define VIDEOREC_AUDIO_CTC0_ON_VALUE 15u

/**
 * @brief Jedna změna hodnoty kanálu ve frontě.
 *
 * Invariant: `value` je v rozsahu 0..31.
 */
typedef struct st_VIDEOREC_AUDIO_EVENT {
    uint64_t ticks; /**< Absolutní čas změny v GDG taktech. */
    uint8_t value;  /**< Nová hodnota kanálu (0..31). */
} st_VIDEOREC_AUDIO_EVENT;

/**
 * @brief Fronta událostí jednoho kanálu (dynamické pole s oknem [head, head+count)).
 *
 * Invarianty: `head + count <= cap`; události v okně mají neklesající `ticks`
 * (zajišťuje volající, viz videorec_audio_event()). Pole `ev` vlastní fronta
 * (uvolní se ve videorec_audio_free()).
 */
typedef struct st_VIDEOREC_AUDIO_QUEUE {
    st_VIDEOREC_AUDIO_EVENT *ev; /**< Pole událostí (NULL, dokud se nepřidá první). */
    size_t head;                 /**< Index první nezpracované události. */
    size_t count;                /**< Počet nezpracovaných událostí. */
    size_t cap;                  /**< Kapacita pole `ev`. */
} st_VIDEOREC_AUDIO_QUEUE;

/**
 * @brief Koeficienty řetězce SDL cesty pro danou vzorkovací frekvenci.
 *
 * Invariant: při `rate == VIDEOREC_AUDIO_SDL_RATE` jsou hodnoty přesně ty
 * z SDL cesty (0,4f, 0,2f, 6,0f, 1980, 880; `ctc_ac_k`, `out_lp_k`
 * a `out_xfade_step` stejným výrazem jako v iface_audio). Dorovnání
 * hlasitosti (`out_makeup_off`, `out_makeup_on`) a koleno omezovače
 * (`out_knee`) na frekvenci nezávisí; po videorec_audio_coef_init() mají
 * hodnoty emulátoru, jiné nastaví jen videorec_audio_coef_set_output_level().
 */
typedef struct st_VIDEOREC_AUDIO_COEF {
    unsigned park_samples; /**< Počet vzorků konstantní nenulové hodnoty před parkováním (`(rate/1000) * 45`). */
    unsigned park_fade;    /**< Délka útlumu zaparkované hodnoty ve vzorcích (`(rate/1000) * 20`). */
    float ctc_lp_alpha;    /**< CTC0 low-pass: `y = alpha*x + (1-alpha)*y`. */
    float ctc_ag_factor;   /**< CTC0 anti-glitch: `y += f*(x - y)`. */
    float psg_iir_div;     /**< PSG IIR: `y += (x - y) / div`. */
    double ctc_ac_k;       /**< Střídavá vazba CTC0: `mean += (x01 - mean) * k`, `k = 1 - exp(-1000 / (tau_ms * rate))`. */
    double out_lp_k;       /**< Výstupní propust: `y += (x - y) * k`, `k = 1 - exp(-1e6 / (tau_us * rate))`. */
    float out_xfade_step;  /**< Krok váhy výstupní propusti na vzorek (`1000 / (xfade_ms * rate)`). */
    double out_ac_k;       /**< Střídavá vazba výstupu: `mean += (x - mean) * k`; 0 = vazba vypnutá (výchozí). */
    bool psg_park;         /**< Parkovat stálou úroveň kanálů PSG (výchozí true; false s vazbou výstupu). */
    float out_makeup_off;  /**< Násobitel výstupu při váze propusti 0 (`10^(VIDEOREC_AUDIO_OUT_MAKEUP_OFF_DB / 20)`). */
    float out_makeup_on;   /**< Násobitel výstupu při váze propusti 1 (`10^(VIDEOREC_AUDIO_OUT_MAKEUP_ON_DB / 20)`). */
    float out_knee;        /**< Koleno měkkého omezovače výstupu (VIDEOREC_AUDIO_OUT_KNEE); 0 < koleno <= 1, 1 = tvrdý ořez na [-1, 1]. */
} st_VIDEOREC_AUDIO_COEF;

/**
 * @brief Stav parkování jednoho kanálu (kopie `static` proměnných SDL cesty).
 *
 * Význam polí odpovídá `parking_previous_resampled`, `parking_counter`,
 * `parking_gain` a `parking_gain_counter` v iface_audio_resampler.c:230-233.
 */
typedef struct st_VIDEOREC_AUDIO_PARK {
    uint8_t prev;     /**< Hodnota kanálu na konci předchozího vzorku. */
    uint32_t counter; /**< Počet vzorků, po které je nenulová hodnota beze změny (nasycuje se na park_samples). */
    float gain;       /**< Aktuální zesílení během útlumu (1,0 -> 0). */
    int gain_counter; /**< Vzorky do dalšího kroku útlumu. */
} st_VIDEOREC_AUDIO_PARK;

/**
 * @brief Stav filtrů jednoho kanálu.
 *
 * U CTC0 je `hp_mean` stav střídavé vazby, `lp` výstup low-passu a `ag` výstup
 * anti-glitch filtru (`park` se nepoužívá); u PSG je `lp` stav IIR, `park`
 * parkování a `ag` ani `hp_mean` se nepoužívají (zůstávají 0).
 *
 * Invariant: `hp_mean` je v rozsahu 0..1.
 */
typedef struct st_VIDEOREC_AUDIO_CHSTATE {
    st_VIDEOREC_AUDIO_PARK park; /**< PSG: parkování; CTC0: nepoužito. */
    float lp;                    /**< CTC0: low-pass; PSG: IIR. */
    float ag;                    /**< CTC0: anti-glitch; PSG: nepoužito. */
    double hp_mean;              /**< CTC0: střední hodnota kanálu (0..1) pro střídavou vazbu; PSG: nepoužito. */
} st_VIDEOREC_AUDIO_CHSTATE;

/**
 * @brief Úplný stav rendereru v okamžiku mezi vzorky (pro plynulý retake).
 *
 * Obsahuje vše, na čem závisí další výstup kromě času a čekajících událostí:
 * hodnoty kanálů, stavy filtrů kanálů a stav výstupní propusti. Kopírovatelná
 * hodnotou (bez ukazatelů).
 */
typedef struct st_VIDEOREC_AUDIO_STATE {
    uint8_t value[VIDEOREC_AUDIO_MAX_CHANNELS];                /**< Hodnoty kanálů (0..31). */
    st_VIDEOREC_AUDIO_CHSTATE ch[VIDEOREC_AUDIO_MAX_CHANNELS]; /**< Stavy filtrů kanálů. */
    double out_lp[2];                                          /**< Stav výstupní propusti L, R. */
    double out_ac[2];                                          /**< Stav střídavé vazby výstupu L, R (střední hodnota součtu kanálů). */
    float out_gain;                                            /**< Váha výstupní propusti v mixu (0..1). */
    bool out_filter;                                           /**< Cílový stav výstupní propusti. */
} st_VIDEOREC_AUDIO_STATE;

/**
 * @brief Stav rendereru (veřejná struktura kvůli vložení do pipeline záznamu).
 *
 * Členy by měl měnit jen modul pomocí funkcí videorec_audio_*(); veřejná je jen
 * kvůli statické alokaci uvnitř jiných struktur.
 *
 * Invarianty:
 * - `channels <= VIDEOREC_AUDIO_MAX_CHANNELS`,
 * - fronty `q[c]` mají neklesající čas událostí,
 * - `n` je počet vzorků vyrenderovaných od `origin`,
 * - `value[c]` je hodnota kanálu na začátku dosud nevyrenderovaného vzorku `n`,
 * - `flt` je stav filtrů po posledním vyrenderovaném vzorku (bez `sdl_chain` se nemění),
 * - `route[c]` je kombinace VIDEOREC_AUDIO_ROUTE_L / _R (0 = kanál se nemixuje),
 * - `out_gain` je v 0..1 a blíží se k `out_filter ? 1 : 0` krokem
 *   `coef.out_xfade_step` na vzorek; `out_lp` běží stále (i při `out_gain == 0`).
 *
 * Ownership: struktura vlastní pole událostí ve frontách; uvolňuje je
 * videorec_audio_free().
 */
typedef struct st_VIDEOREC_AUDIO {
    uint64_t clk_hz;                                                 /**< Frekvence GDG taktů [Hz]. */
    unsigned rate;                                                   /**< Výstupní vzorkovací frekvence [Hz]. */
    unsigned channels;                                               /**< Počet aktivních kanálů. */
    float level[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS]; /**< Úroveň (amplituda) kanálu pro hodnotu 0..31. */
    uint8_t value[VIDEOREC_AUDIO_MAX_CHANNELS];                      /**< Aktuální hodnota kanálu (viz invarianty). */
    st_VIDEOREC_AUDIO_QUEUE q[VIDEOREC_AUDIO_MAX_CHANNELS];          /**< Fronty čekajících změn po kanálech. */
    uint64_t origin;                                                 /**< Čas (takty) začátku vzorku 0. */
    uint64_t n;                                                      /**< Počet vyrenderovaných vzorků od `origin`. */
    bool sdl_chain;                                                  /**< Zapnutý řetězec SDL cesty (kanál 0 = CTC0, ostatní PSG). */
    st_VIDEOREC_AUDIO_COEF coef;                                     /**< Koeficienty řetězce pro `rate`. */
    st_VIDEOREC_AUDIO_CHSTATE flt[VIDEOREC_AUDIO_MAX_CHANNELS];      /**< Stavy filtrů kanálů. */
    uint8_t route[VIDEOREC_AUDIO_MAX_CHANNELS];                      /**< Strany, do kterých se kanál mixuje (VIDEOREC_AUDIO_ROUTE_*). */
    bool stereo;                                                     /**< Nastavené stereo rozložení SDL cesty (videorec_audio_set_stereo()). */
    double out_lp[2];                                                /**< Stav výstupní propusti L, R (jen `sdl_chain`). */
    double out_ac[2];                                                /**< Stav střídavé vazby výstupu L, R: střední hodnota součtu kanálů (jen `sdl_chain`). */
    float out_gain;                                                  /**< Váha výstupní propusti v mixu (0 = dřívější filtry kanálů, 1 = výstupní propust). */
    bool out_filter;                                                 /**< Cílový stav výstupní propusti (videorec_audio_set_output_filter()). */
} st_VIDEOREC_AUDIO;

/**
 * @brief Spočítá koeficienty řetězce SDL cesty pro vzorkovací frekvenci.
 *
 * Při `rate == VIDEOREC_AUDIO_SDL_RATE` přesně konstanty SDL cesty
 * (iface_audio_resampler.c:82 `alpha = 0.4f`, :160 `smooth_factor = 0.2f`,
 * :384 `iir_x = 6.0f`; iface_audio.c:224, :226 doby parkování). Jinak
 * `a = 1 - (1 - a44)^(44100/rate)` (stejná zlomová frekvence jednopólového
 * filtru), `div = 1 / a`, doby parkování `(rate/1000) * 45` a `* 20` vzorků.
 *
 * @param c    Výstup.
 * @param rate Vzorkovací frekvence [Hz]; nenulová.
 */
void videorec_audio_coef_init(st_VIDEOREC_AUDIO_COEF *c, unsigned rate);

/**
 * @brief Střední zesílení převzorkování CTC0 v SDL cestě.
 *
 * SDL cesta (iface_audio_resampler_output_stream_ctc0(), iface_audio_resampler.c:203-227)
 * počítá vzorek jako `počet nenulových / (délka bloku + 1)` z proudu
 * `ctc_samples` bodových vzorků na snímek do `out_samples` výstupních. Pro
 * trvale nenulový vstup to dává `délka / (délka + 1)` (cca 0,926 u MZ-800);
 * funkce vrací průměr přes snímek, kterým renderer dorovnává hlasitost CTC0.
 *
 * @param ctc_samples Vzorky CTC0 proudu na snímek (IFACE_AUDIO_CTC5253_SAMPLE_RATE / fps); nenulové.
 * @param out_samples Výstupní vzorky na snímek (44100 / fps); nenulové, `<= ctc_samples`.
 * @return Střední zesílení v (0, 1).
 */
float videorec_audio_sdl_ctc0_gain(size_t ctc_samples, size_t out_samples);

/**
 * @brief Jeden vzorek řetězce CTC0: střídavá vazba -> low-pass -> anti-glitch.
 *
 * Střídavá vazba: `x01 = x / unit` (podíl času v jedničce, 0..1),
 * `hp_mean += (x01 - hp_mean) * ctc_ac_k`, `ac = x - hp_mean * unit`
 * (počítáno v double, výsledek float). Při `unit <= 0` (nulová hlasitost)
 * se stav nemění a `ac = x`. Stejnou funkci volá SDL cesta
 * (iface_audio_resampler_output_stream_ctc0()), kde `unit` je zesílení
 * převzorkování daného vzorku `délka / (délka + 1)`; renderer dává úroveň
 * hodnoty VIDEOREC_AUDIO_CTC0_ON_VALUE.
 *
 * @param s    Stav kanálu (mění se).
 * @param c    Koeficienty.
 * @param x    Vstup (průměrná úroveň kanálu ve vzorku, 0..unit).
 * @param unit Úroveň kanálu trvale v jedničce.
 * @param raw  Výstup po střídavé vazbě před low-passem (vstup výstupní propusti), nebo NULL.
 * @return Výstup kanálu po low-passu a anti-glitch filtru.
 */
float videorec_audio_chain_ctc0(st_VIDEOREC_AUDIO_CHSTATE *s, const st_VIDEOREC_AUDIO_COEF *c, float x, float unit, float *raw);

/**
 * @brief Jeden vzorek řetězce PSG: parkování -> IIR `y += (x - y) / div`.
 *
 * Operace i pořadí ve float jsou shodné s iface_audio_resampler_process_psg_audio_log().
 *
 * Parkování se dělá jen při `c->psg_park`; se střídavou vazbou výstupu
 * (videorec_audio_set_output_coupling()) odpadá.
 *
 * @param s         Stav kanálu (mění se).
 * @param c         Koeficienty.
 * @param x         Vstup (průměrná úroveň kanálu ve vzorku).
 * @param end_value Hodnota kanálu na konci vzorku (pro parkování).
 * @param raw       Výstup po parkování před IIR (vstup výstupní propusti), nebo NULL.
 * @return Výstup kanálu po IIR.
 */
float videorec_audio_chain_psg(st_VIDEOREC_AUDIO_CHSTATE *s, const st_VIDEOREC_AUDIO_COEF *c, float x, uint8_t end_value, float *raw);

/**
 * @brief Měkký omezovač špiček: pod kolenem přesně lineární, nad ním plynulé stlačení k 1,0.
 *
 * Pro `|x| <= knee` vrací přesně `x` (stejný bitový obraz). Pro větší
 * výchylku `sign(x) * (knee + (1 - knee) * tanh((|x| - knee) / (1 - knee)))`:
 * v koleni navazuje hodnotou i směrnicí (1), je rostoucí a pro libovolně
 * velký vstup zůstává v [-1, 1] (k 1,0 se blíží, tvrdě neořezává). Nemá
 * stav, takže nepumpuje a nezávisí na předchozích vzorcích; cenou je
 * zkreslení špiček nad kolenem (liché harmonické jako u přebuzeného
 * zesilovače).
 *
 * @param x    Vstupní vzorek (libovolná hodnota včetně nekonečna).
 * @param knee Koleno, 0 < knee <= 1; při 1 je funkce tvrdý ořez na [-1, 1].
 * @return Vzorek v [-1, 1]; NaN se vrací beze změny.
 * @note Čistá funkce; lze volat z libovolného vlákna.
 */
static inline float videorec_audio_soft_limit_knee(float x, float knee)
{
    float a = fabsf(x);
    if (!(a > knee)) return x;
    float span = 1.0f - knee;
    return copysignf(knee + span * tanhf((a - knee) / span), x);
}

/**
 * @brief Měkký omezovač výstupu s kolenem VIDEOREC_AUDIO_OUT_KNEE (viz videorec_audio_soft_limit_knee()).
 *
 * Používá ho výstupní stupeň (videorec_audio_output_stage()) a hlavní
 * hlasitost poslechu nad 100 % (iface_audio_apply_master()).
 *
 * @param x Vstupní vzorek.
 * @return Vzorek v [-1, 1], pro `|x| <= VIDEOREC_AUDIO_OUT_KNEE` přesně `x`.
 */
static inline float videorec_audio_soft_limit(float x)
{
    return videorec_audio_soft_limit_knee(x, VIDEOREC_AUDIO_OUT_KNEE);
}

/**
 * @brief Jeden vzorek výstupního stupně jedné strany: střídavá vazba, výstupní propust, plynulý přechod, dorovnání hlasitosti a měkký omezovač.
 *
 * Střídavá vazba výstupu (horní propust 1. řádu, VIDEOREC_AUDIO_OUT_AC_TAU_MS):
 * `*ac += (raw - *ac) * out_ac_k` a střední hodnota `*ac` se odečte od obou
 * součtů (jejich stejnosměrná složka je stejná, filtry kanálů mají
 * stejnosměrné zesílení 1). Při `out_ac_k == 0` a `*ac == 0` se nic nemění.
 * `raw` (součet kanálů před jejich filtry) projde dolní propustí
 * `*lp += (raw - *lp) * out_lp_k`, která běží vždy. Smíchaný vzorek je
 * `m = legacy + gain * (lp - legacy)` (`legacy` = součet kanálů po jejich
 * filtrech; při `gain == 0` přesně `legacy`).
 *
 * Dorovnání hlasitosti: `m` se násobí
 * `out_makeup_off + gain * (out_makeup_on - out_makeup_off)`, tedy při
 * přepnutí propusti přechází násobitel plynule spolu s vahou propusti.
 * Výsledek projde měkkým omezovačem videorec_audio_soft_limit_knee() s kolenem
 * `out_knee`: pod kolenem (VIDEOREC_AUDIO_OUT_KNEE) je výstup přesně lineární (poměry mezi zdroji se
 * nemění), nad ním se špičky stlačí k 1,0. Dřívější tvrdý ořez součtu
 * `legacy` shora na 1,0 omezovač nahrazuje. Stejnou funkci volá mix SDL
 * cesty (iface_audio.c).
 *
 * @param c      Koeficienty.
 * @param ac     Stav střídavé vazby strany (mění se).
 * @param lp     Stav výstupní propusti strany (mění se).
 * @param legacy Součet kanálů po filtrech kanálů.
 * @param raw    Součet kanálů před filtry kanálů.
 * @param gain   Váha výstupní propusti 0..1.
 * @return Vzorek strany v rozsahu [-1, 1].
 */
float videorec_audio_output_stage(const st_VIDEOREC_AUDIO_COEF *c, double *ac, double *lp, float legacy, float raw, float gain);

/**
 * @brief Přepíše dorovnání hlasitosti a koleno omezovače výstupního stupně.
 *
 * Emulátor ji nevolá: videorec_audio_coef_init() nastaví hodnoty
 * VIDEOREC_AUDIO_OUT_MAKEUP_OFF_DB, _ON_DB a VIDEOREC_AUDIO_OUT_KNEE. Slouží
 * testům tvaru filtrů a nástrojům, které chtějí výstup bez dorovnání:
 * `(1, 1, 1)` dává výstupní stupeň bez zesílení s tvrdým ořezem na [-1, 1].
 *
 * @param c          Koeficienty po videorec_audio_coef_init().
 * @param makeup_off Násobitel výstupu při váze propusti 0 (větší než 0).
 * @param makeup_on  Násobitel výstupu při váze propusti 1 (větší než 0).
 * @param knee       Koleno omezovače, 0 < knee <= 1.
 */
void videorec_audio_coef_set_output_level(st_VIDEOREC_AUDIO_COEF *c, float makeup_off, float makeup_on, float knee);

/**
 * @brief Zapne nebo vypne v koeficientech střídavou vazbu výstupu.
 *
 * Zapnuto: `out_ac_k = 1 - exp(-1000 / (VIDEOREC_AUDIO_OUT_AC_TAU_MS * rate))`
 * a parkování kanálů PSG se vypne (`psg_park = false`) - stálou úroveň
 * odstraní vazba, jako kondenzátor v sérii s výstupem skutečného MZ-800.
 * Vypnuto: `out_ac_k = 0`, `psg_park = true` (stav po videorec_audio_coef_init()).
 *
 * @param c    Koeficienty po videorec_audio_coef_init().
 * @param rate Vzorkovací frekvence [Hz]; nenulová.
 * @param on   true = vazba výstupu (MZ-800, MZ-1500), false = bez ní (MZ-700).
 */
void videorec_audio_coef_set_output_coupling(st_VIDEOREC_AUDIO_COEF *c, unsigned rate, bool on);

/**
 * @brief Posune váhu výstupní propusti o jeden vzorek směrem k cíli.
 *
 * @param c    Koeficienty (`out_xfade_step`).
 * @param gain Dosavadní váha 0..1.
 * @param on   Cílový stav (true = 1, false = 0).
 * @return Nová váha v 0..1.
 */
float videorec_audio_output_gain_step(const st_VIDEOREC_AUDIO_COEF *c, float gain, bool on);

/**
 * @brief Inicializuje renderer.
 *
 * @param a            Instance (obsah se přepíše; nemusí být předem inicializována).
 * @param clk_hz       Frekvence taktů časových značek [Hz]; nenulová.
 * @param rate         Výstupní vzorkovací frekvence [Hz]; nenulová.
 * @param channels     Počet kanálů; hodnota nad VIDEOREC_AUDIO_MAX_CHANNELS se ořízne.
 * @param level        Tabulka úrovní `level[kanál][hodnota]`, aspoň `channels` řádků;
 *                     zkopíruje se (po návratu ji volající může uvolnit).
 * @param values       Počáteční hodnoty kanálů (`channels` prvků, bity 0..4), nebo NULL = samé 0.
 * @param origin_ticks Čas začátku vzorku 0 v taktech.
 * @param sdl_chain    true = řetězec filtrů SDL cesty (kanál 0 = CTC0, ostatní PSG),
 *                     false = surový součet box průměrů.
 *
 * @pre `a`, `level` nejsou NULL; `clk_hz` a `rate` nenulové.
 * @post Renderer je prázdný, `n == 0`, stavy filtrů nulové (jako SDL cesta po
 *       startu emulátoru) kromě střídavé vazby CTC0, která začíná ustálená na
 *       počáteční hodnotě kanálu 0 (`hp_mean` = úroveň počáteční hodnoty dělená
 *       úrovní hodnoty VIDEOREC_AUDIO_CTC0_ON_VALUE, tedy 1, 0,64 u hodnoty 31
 *       nebo 0; přesný stav nastaví videorec_audio_set_ctc0_mean()); všechny kanály se
 *       mixují do obou stran (`stereo == false`, viz videorec_audio_set_stereo()),
 *       výstupní propust je vypnutá (videorec_audio_set_output_filter()),
 *       střídavá vazba výstupu je vypnutá a kanály PSG se parkují
 *       (videorec_audio_set_output_coupling()),
 *       nevlastní žádnou alokovanou paměť.
 * @note Selhat nemůže (nealokuje).
 */
void videorec_audio_init(st_VIDEOREC_AUDIO *a, uint64_t clk_hz, unsigned rate, unsigned channels,
                         const float level[][VIDEOREC_AUDIO_LEVELS], const uint8_t *values,
                         uint64_t origin_ticks, bool sdl_chain);

/**
 * @brief Nastaví rozložení kanálů do L/R podle SDL cesty (mono / stereo PSG).
 *
 * - `stereo == true`: kanál 0 (CTC0) do obou stran, kanály
 *   1..VIDEOREC_AUDIO_PSG_CHANNELS (PSG0) do L, další kanály (PSG1) do R
 *   (iface_audio_mix_channels_stereo());
 * - `stereo == false`: kanály 0..VIDEOREC_AUDIO_PSG_CHANNELS do obou stran,
 *   další kanály (PSG1) se nemixují (iface_audio_mix_channels_with_gain()).
 *
 * Platí od dalšího vyrenderovaného vzorku; stavy filtrů, fronty ani čas
 * se nemění (SDL cesta rozhoduje o mixu po blocích stejně bez vlivu na
 * filtry). Volá se typicky před každým renderem podle aktuálního stavu
 * emulátoru (MZ-800 může druhý PSG zapnout za běhu).
 *
 * @param a      Instance po videorec_audio_init().
 * @param stereo true = stereo rozložení, false = mono rozložení SDL cesty.
 * @post `a->stereo == stereo`, `route` podle pravidel výše.
 */
void videorec_audio_set_stereo(st_VIDEOREC_AUDIO *a, bool stereo);

/**
 * @brief Zapne nebo vypne výstupní dolní propust (věrný výstup MZ-800).
 *
 * Zapnuto: výstup je součet kanálů před jejich filtry (CTC0 po střídavé vazbě,
 * PSG po parkování) po dolní propusti 1. řádu s časovou konstantou
 * VIDEOREC_AUDIO_OUT_LP_TAU_US. Vypnuto: součet kanálů po jejich dřívějších
 * filtrech (low-pass a anti-glitch CTC0, IIR PSG). Úroveň nízkých kmitočtů je
 * v obou stavech stejná. Platí jen v režimu `sdl_chain`.
 *
 * @param a         Instance po videorec_audio_init().
 * @param on        Cílový stav.
 * @param immediate true = platí hned od dalšího vzorku (start nahrávky);
 *                  false = plynulý přechod VIDEOREC_AUDIO_OUT_XFADE_MS (přepnutí za běhu).
 * @post `a->out_filter == on`; při `immediate` navíc `a->out_gain` 1 nebo 0.
 */
void videorec_audio_set_output_filter(st_VIDEOREC_AUDIO *a, bool on, bool immediate);

/**
 * @brief Zapne nebo vypne střídavou vazbu celého výstupu (viz videorec_audio_coef_set_output_coupling()).
 *
 * Emulátor ji zapíná u počítačů, které ji mají (MZ-800, MZ-1500); po
 * videorec_audio_init() je vypnutá a kanály PSG se parkují. Platí jen v režimu
 * `sdl_chain`. Stav vazby (`out_ac`) se nemění; nastavuje ho
 * videorec_audio_set_output_mean().
 *
 * @param a  Instance po videorec_audio_init().
 * @param on true = vazba výstupu místo parkování PSG.
 */
void videorec_audio_set_output_coupling(st_VIDEOREC_AUDIO *a, bool on);

/**
 * @brief Nastaví stav střídavé vazby výstupu (střední hodnotu součtu kanálů každé strany).
 *
 * Emulátor vede střední hodnoty kanálů v emulovaném čase
 * (iface_audio_output_ac_means()); nahrávka tak začne bez doznívajícího
 * stejnosměrného posunu a shodně se SDL cestou.
 *
 * @param a Instance po videorec_audio_init().
 * @param l Střední hodnota levé strany v jednotkách úrovní (`level`).
 * @param r Střední hodnota pravé strany.
 */
void videorec_audio_set_output_mean(st_VIDEOREC_AUDIO *a, double l, double r);

/**
 * @brief Nastaví stav střídavé vazby CTC0 (střední hodnotu kanálu 0).
 *
 * Emulátor vede střední hodnotu CTC0 průběžně v emulovaném čase
 * (audio_ctc0_ac_mean()); nahrávka, která začne uprostřed doznívání stálé
 * úrovně, tak naváže stejně jako SDL cesta.
 *
 * @param a      Instance po videorec_audio_init().
 * @param mean01 Střední hodnota 0..1 (podíl času v jedničce); mimo rozsah se ořízne.
 */
void videorec_audio_set_ctc0_mean(st_VIDEOREC_AUDIO *a, double mean01);

/**
 * @brief Je nastavené stereo rozložení (videorec_audio_set_stereo(a, true))?
 * @param a Instance.
 * @return true = stereo rozložení SDL cesty; false = mono rozložení nebo
 *         výchozí (všechny kanály do obou stran).
 */
bool videorec_audio_is_stereo(const st_VIDEOREC_AUDIO *a);

/**
 * @brief Uvolní paměť front událostí.
 *
 * @param a Instance po videorec_audio_init().
 *
 * @post Fronty jsou prázdné a bez alokace; instanci lze znovu použít přes
 *       videorec_audio_init() a opakované volání je bezpečné.
 */
void videorec_audio_free(st_VIDEOREC_AUDIO *a);

/**
 * @brief Znovu zahájí časovou osu (nový origin), zahodí čekající události.
 *
 * Použití např. po načtení snapshotu nebo přetočení času. Stavy filtrů se
 * nemění: výstup pokračuje z dosavadního stavu (jako SDL cesta, která o švu
 * neví). Pro navázání na jiný okamžik (retake) se po rebase obnoví uložený
 * stav přes videorec_audio_set_state().
 *
 * @param a            Instance.
 * @param origin_ticks Nový čas začátku vzorku 0 v taktech.
 * @param values       Hodnoty kanálů k tomuto okamžiku (`channels` prvků), nebo NULL = samé 0.
 *
 * @post Fronty prázdné, `n == 0`, `value[c] = values[c] & VIDEOREC_AUDIO_VALUE_MASK`, `flt`
 *       a stav výstupní propusti beze změny.
 */
void videorec_audio_rebase(st_VIDEOREC_AUDIO *a, uint64_t origin_ticks, const uint8_t *values);

/**
 * @brief Přečte stav rendereru (hodnoty kanálů a stavy filtrů) po posledním vyrenderovaném vzorku.
 *
 * Hodnoty kanálů jsou hodnoty na začátku dalšího vzorku (`value`), tj. po
 * všech už zpracovaných událostech.
 *
 * @param a   Instance.
 * @param out Výstup; kanály nad `channels` jsou nulové.
 */
void videorec_audio_get_state(const st_VIDEOREC_AUDIO *a, st_VIDEOREC_AUDIO_STATE *out);

/**
 * @brief Obnoví stav rendereru uložený videorec_audio_get_state().
 *
 * Typicky hned po videorec_audio_rebase() na čas, ve kterém byl stav uložen:
 * další vzorek pak naváže přesně jako nepřerušený běh. Fronty, čas a počet
 * vzorků se nemění.
 *
 * @param a  Instance (se stejným počtem kanálů jako při uložení).
 * @param st Uložený stav.
 * @post `value[c] = st->value[c] & VIDEOREC_AUDIO_VALUE_MASK`, `flt[c] = st->ch[c]` pro c < `channels`;
 *       `out_lp`, `out_gain` a `out_filter` podle `st`.
 */
void videorec_audio_set_state(st_VIDEOREC_AUDIO *a, const st_VIDEOREC_AUDIO_STATE *st);

/**
 * @brief Přidá změnu hodnoty kanálu.
 *
 * Události téhož kanálu musí přicházet s neklesajícím `ticks`. Událost s časem
 * před začátkem dosud nevyrenderovaného vzorku (např. před `origin`) se projeví
 * od začátku tohoto vzorku.
 *
 * @param a     Instance.
 * @param ch    Index kanálu (< `channels`).
 * @param value Nová hodnota; použijí se jen bity 0..4.
 * @param ticks Absolutní čas změny v taktech.
 *
 * @return 0 při úspěchu, -1 při neplatném kanálu nebo selhání alokace (instance zůstane konzistentní).
 */
int videorec_audio_event(st_VIDEOREC_AUDIO *a, unsigned ch, uint8_t value, uint64_t ticks);

/**
 * @brief Vyrenderuje všechny úplné vzorky, které končí nejpozději v `horizon_ticks`.
 *
 * @param a             Instance.
 * @param horizon_ticks Čas v taktech, do kterého jsou všechny události už doručeny
 *                      (vzorek, který by přesáhl horizont, se nerenderuje).
 * @param out_stereo    Výstup, prokládané L,R (rozložení kanálů viz videorec_audio_set_stereo()),
 *                      kapacita aspoň `2 * max_frames` hodnot int16.
 * @param max_frames    Maximální počet vzorků (framů) k vyrenderování.
 *
 * @return Počet vyrenderovaných vzorků (frameů), 0..max_frames.
 *
 * @post Zpracované události jsou z front odebrány, `n` se zvýšilo o návratovou hodnotu,
 *       stavy filtrů odpovídají poslednímu vyrenderovanému vzorku.
 *       Nevyrenderované vzorky (horizont/limit) zůstávají pro další volání.
 */
size_t videorec_audio_render(st_VIDEOREC_AUDIO *a, uint64_t horizon_ticks, int16_t *out_stereo, size_t max_frames);

#ifdef __cplusplus
}
#endif
#endif /* VIDEOREC_AUDIO_H */
