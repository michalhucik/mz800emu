/**
 * @file   videorec_audio.h
 * @brief  Renderer audio událostí na stereo PCM v emulačním čase pro video záznam.
 *
 * Vstupem jsou změny 4bitových hodnot jednotlivých zvukových kanálů s absolutním
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
 * - kanál 0 (CTC0): parkování -> 1-pólový low-pass (`alpha` 0,4,
 *   iface_audio_lowpass_filter()) -> anti-glitch (0,2, iface_audio_anti_glitch_filter());
 * - kanály 1.. (PSG): parkování -> IIR `y += (x - y) / 6`;
 * - mix: součet kanálů každé strany (úrovně už obsahují gain a průměr přes
 *   4 kanály PSG, viz iface_audio_build_videorec_levels()), ořez shora na 1,0
 *   jako iface_audio_mix_channels_with_gain() / iface_audio_mix_channels_stereo().
 *
 * Parkování: hodnota kanálu na konci vzorku, která je nenulová a beze změny
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
 * Dřívější DC blok (horní propust cca 4 Hz) je nahrazený parkováním: obojí
 * brání tomu, aby trvalá úroveň (CTC0 držený na 1) zabrala rozsah a přehlušila
 * ostatní kanály, ale jen parkování dává stejný průběh jako výstup emulátoru
 * (unipolární signál, ticho = 0).
 *
 * @par Surový režim (bez `sdl_chain`)
 * Součet box průměrů kanálů bez filtrů (testy časování a box filtru).
 *
 * @par Výstup
 * Výsledek se ořízne na [-1, 1] a převede na int16 (x 32767).
 *
 * @par Vlákna
 * Jedna instance smí být používána jen z jednoho vlákna, nebo musí volající
 * všechna volání serializovat vnějším zámkem. Modul nemá žádný vlastní zámek.
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_AUDIO_H
#define VIDEOREC_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximální počet kanálů, které renderer zpracuje (nadbytečné se při init ořežou). */
#define VIDEOREC_AUDIO_MAX_CHANNELS 9

/** @brief Počet úrovní na kanál (4bitová hodnota 0..15). */
#define VIDEOREC_AUDIO_LEVELS 16

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

/**
 * @brief Jedna změna hodnoty kanálu ve frontě.
 *
 * Invariant: `value` je v rozsahu 0..15.
 */
typedef struct st_VIDEOREC_AUDIO_EVENT {
    uint64_t ticks; /**< Absolutní čas změny v GDG taktech. */
    uint8_t value;  /**< Nová hodnota kanálu (0..15). */
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
 * z SDL cesty (0,4f, 0,2f, 6,0f, 1980, 880).
 */
typedef struct st_VIDEOREC_AUDIO_COEF {
    unsigned park_samples; /**< Počet vzorků konstantní nenulové hodnoty před parkováním (`(rate/1000) * 45`). */
    unsigned park_fade;    /**< Délka útlumu zaparkované hodnoty ve vzorcích (`(rate/1000) * 20`). */
    float ctc_lp_alpha;    /**< CTC0 low-pass: `y = alpha*x + (1-alpha)*y`. */
    float ctc_ag_factor;   /**< CTC0 anti-glitch: `y += f*(x - y)`. */
    float psg_iir_div;     /**< PSG IIR: `y += (x - y) / div`. */
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
 * U CTC0 je `lp` výstup low-passu a `ag` výstup anti-glitch filtru; u PSG je
 * `lp` stav IIR a `ag` se nepoužívá (zůstává 0).
 */
typedef struct st_VIDEOREC_AUDIO_CHSTATE {
    st_VIDEOREC_AUDIO_PARK park; /**< Parkování. */
    float lp;                    /**< CTC0: low-pass; PSG: IIR. */
    float ag;                    /**< CTC0: anti-glitch; PSG: nepoužito. */
} st_VIDEOREC_AUDIO_CHSTATE;

/**
 * @brief Úplný stav rendereru v okamžiku mezi vzorky (pro plynulý retake).
 *
 * Obsahuje vše, na čem závisí další výstup kromě času a čekajících událostí:
 * hodnoty kanálů a stavy filtrů. Kopírovatelná hodnotou (bez ukazatelů).
 */
typedef struct st_VIDEOREC_AUDIO_STATE {
    uint8_t value[VIDEOREC_AUDIO_MAX_CHANNELS];                /**< Hodnoty kanálů (0..15). */
    st_VIDEOREC_AUDIO_CHSTATE ch[VIDEOREC_AUDIO_MAX_CHANNELS]; /**< Stavy filtrů kanálů. */
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
 * - `route[c]` je kombinace VIDEOREC_AUDIO_ROUTE_L / _R (0 = kanál se nemixuje).
 *
 * Ownership: struktura vlastní pole událostí ve frontách; uvolňuje je
 * videorec_audio_free().
 */
typedef struct st_VIDEOREC_AUDIO {
    uint64_t clk_hz;                                                 /**< Frekvence GDG taktů [Hz]. */
    unsigned rate;                                                   /**< Výstupní vzorkovací frekvence [Hz]. */
    unsigned channels;                                               /**< Počet aktivních kanálů. */
    float level[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS]; /**< Úroveň (amplituda) kanálu pro hodnotu 0..15. */
    uint8_t value[VIDEOREC_AUDIO_MAX_CHANNELS];                      /**< Aktuální hodnota kanálu (viz invarianty). */
    st_VIDEOREC_AUDIO_QUEUE q[VIDEOREC_AUDIO_MAX_CHANNELS];          /**< Fronty čekajících změn po kanálech. */
    uint64_t origin;                                                 /**< Čas (takty) začátku vzorku 0. */
    uint64_t n;                                                      /**< Počet vyrenderovaných vzorků od `origin`. */
    bool sdl_chain;                                                  /**< Zapnutý řetězec SDL cesty (kanál 0 = CTC0, ostatní PSG). */
    st_VIDEOREC_AUDIO_COEF coef;                                     /**< Koeficienty řetězce pro `rate`. */
    st_VIDEOREC_AUDIO_CHSTATE flt[VIDEOREC_AUDIO_MAX_CHANNELS];      /**< Stavy filtrů kanálů. */
    uint8_t route[VIDEOREC_AUDIO_MAX_CHANNELS];                      /**< Strany, do kterých se kanál mixuje (VIDEOREC_AUDIO_ROUTE_*). */
    bool stereo;                                                     /**< Nastavené stereo rozložení SDL cesty (videorec_audio_set_stereo()). */
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
 * @brief Jeden vzorek řetězce CTC0: parkování -> low-pass -> anti-glitch.
 *
 * Operace i pořadí ve float jsou shodné s iface_audio_resampler.c:229-283.
 *
 * @param s         Stav kanálu (mění se).
 * @param c         Koeficienty.
 * @param x         Vstup (průměrná úroveň kanálu ve vzorku).
 * @param end_value Hodnota kanálu na konci vzorku (pro parkování).
 * @return Výstup kanálu.
 */
float videorec_audio_chain_ctc0(st_VIDEOREC_AUDIO_CHSTATE *s, const st_VIDEOREC_AUDIO_COEF *c, float x, uint8_t end_value);

/**
 * @brief Jeden vzorek řetězce PSG: parkování -> IIR `y += (x - y) / div`.
 *
 * Operace i pořadí ve float jsou shodné s iface_audio_resampler.c:342-387.
 *
 * @param s         Stav kanálu (mění se).
 * @param c         Koeficienty.
 * @param x         Vstup (průměrná úroveň kanálu ve vzorku).
 * @param end_value Hodnota kanálu na konci vzorku (pro parkování).
 * @return Výstup kanálu.
 */
float videorec_audio_chain_psg(st_VIDEOREC_AUDIO_CHSTATE *s, const st_VIDEOREC_AUDIO_COEF *c, float x, uint8_t end_value);

/**
 * @brief Inicializuje renderer.
 *
 * @param a            Instance (obsah se přepíše; nemusí být předem inicializována).
 * @param clk_hz       Frekvence taktů časových značek [Hz]; nenulová.
 * @param rate         Výstupní vzorkovací frekvence [Hz]; nenulová.
 * @param channels     Počet kanálů; hodnota nad VIDEOREC_AUDIO_MAX_CHANNELS se ořízne.
 * @param level        Tabulka úrovní `level[kanál][hodnota]`, aspoň `channels` řádků;
 *                     zkopíruje se (po návratu ji volající může uvolnit).
 * @param values       Počáteční hodnoty kanálů (`channels` prvků, bity 0..3), nebo NULL = samé 0.
 * @param origin_ticks Čas začátku vzorku 0 v taktech.
 * @param sdl_chain    true = řetězec filtrů SDL cesty (kanál 0 = CTC0, ostatní PSG),
 *                     false = surový součet box průměrů.
 *
 * @pre `a`, `level` nejsou NULL; `clk_hz` a `rate` nenulové.
 * @post Renderer je prázdný, `n == 0`, stavy filtrů nulové (jako SDL cesta po
 *       startu emulátoru), všechny kanály se mixují do obou stran
 *       (`stereo == false`, viz videorec_audio_set_stereo()), nevlastní
 *       žádnou alokovanou paměť.
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
 * @post Fronty prázdné, `n == 0`, `value[c] = values[c] & 0x0F`, `flt` beze změny.
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
 * @post `value[c] = st->value[c] & 0x0F`, `flt[c] = st->ch[c]` pro c < `channels`.
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
 * @param value Nová hodnota; použijí se jen bity 0..3.
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
