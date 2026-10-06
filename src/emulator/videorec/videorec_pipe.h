/**
 * @file   videorec_pipe.h
 * @brief  Párování hotových snímků obrazu se zvukem v emulačním čase pro video záznam.
 *
 * Vstupem jsou hotové snímky (pixely + GDG takt konce snímku) a změny zvukových
 * kanálů s absolutním časem. Snímek se předá sinku až ve chvíli, kdy "horizont"
 * (čas, do kterého jsou všechny zvukové události doručeny) dosáhne jeho konce.
 * Tehdy se pro něj vyrenderuje přesně `samples_per_frame` zvukových vzorků
 * (videorec_audio_render()) a trojice (pixely, zvuk, index) se předá sinku.
 *
 * @par Časový model
 * Snímek `k` (od 1) po `origin` končí v taktu `origin + k * ticks_per_frame`.
 * Počet vzorků na snímek je `spf = rate * ticks_per_frame / clk_hz` (celočíselně
 * dolů); musí vycházet beze zbytku, jinak by se zvuk a obraz rozjížděly.
 *
 * @par Pauza nahrávání
 * Snímek s `pixels == NULL` se do sinku nepředá, ale jeho zvuk se spotřebuje
 * (časová osa zvuku zůstane souvislá) a index zapsaných snímků se nezvýší.
 *
 * @par Zachycení stavu zvuku (plynulý retake)
 * videorec_pipe_request_capture() si vyžádá stav audio rendereru na konci
 * posledního přijatého snímku. Pokud snímek ještě čeká na zvuk, stav se
 * zachytí až při jeho vyrenderování; callback pak dostane stav přesně v tomto
 * bodě časové osy. Rebase nevyřízené požadavky zahodí (jejich snímky už
 * nebudou vyrenderované).
 *
 * @par Vlákna
 * Instance se používá jen z emulačního vlákna, nebo z UI vlákna v době, kdy je
 * emulace pozastavena, a to pod zámkem volajícího (lepidla). Modul nemá vlastní zámek.
 * Sink je volán synchronně ve vlákně volajícího.
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_PIPE_H
#define VIDEOREC_PIPE_H

#include <stddef.h>
#include <stdint.h>

#include "videorec_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximální počet čekajících snímků (pokryje událost trvající až 40 snímků, např. při 4000 %). */
#define VIDEOREC_PIPE_MAX_PENDING 64

/**
 * @brief Hotový snímek předávaný sinku.
 *
 * Ownership: `pixels` (`width * height` bajtů, hodnoty 0..15) i `audio`
 * (`audio_frames * 2` hodnot int16, prokládané L,R) jsou alokované přes `g_malloc`
 * a jejich vlastnictví přechází na sink, který je musí uvolnit přes `g_free`.
 */
typedef struct st_VIDEOREC_PIPE_FRAME {
    uint8_t *pixels;     /**< Pixely snímku (4bitové indexy, maskované `& 0x0F`). */
    int16_t *audio;      /**< Stereo zvuk snímku, prokládané L,R. */
    size_t audio_frames; /**< Počet zvukových vzorků (framů) ve `audio`. */
    uint64_t index;      /**< Index snímku mezi zapsanými snímky (od nastaveného počátku). */
} st_VIDEOREC_PIPE_FRAME;

/**
 * @brief Callback přijímající hotové snímky.
 *
 * @param frame Snímek; sink přebírá vlastnictví `frame->pixels` a `frame->audio`
 *              (struktura samotná je platná jen po dobu volání).
 * @param user  Ukazatel předaný ve videorec_pipe_init().
 */
typedef void (*videorec_pipe_sink_cb)(st_VIDEOREC_PIPE_FRAME *frame, void *user);

/** @brief Maximální počet nevyřízených požadavků na zachycení stavu zvuku (nejstarší se při přetečení zahodí). */
#define VIDEOREC_PIPE_MAX_CAPTURES 16

/**
 * @brief Callback se zachyceným stavem audio rendereru.
 *
 * @param tag   Značka z videorec_pipe_request_capture().
 * @param state Stav rendereru na konci snímku, pro který byl požadavek podán
 *              (platný jen po dobu volání).
 * @param user  Ukazatel předaný ve videorec_pipe_set_capture_cb().
 */
typedef void (*videorec_pipe_capture_cb)(uint64_t tag, const st_VIDEOREC_AUDIO_STATE *state, void *user);

/** @brief Nevyřízený požadavek na zachycení stavu zvuku. */
typedef struct st_VIDEOREC_PIPE_CAPTURE {
    uint64_t end; /**< Takt konce snímku, po jehož vyrenderování se stav zachytí. */
    uint64_t tag; /**< Značka volajícího. */
} st_VIDEOREC_PIPE_CAPTURE;

/** @brief Výsledek videorec_pipe_frame(). */
typedef enum {
    VIDEOREC_PIPE_OK = 0,                 /**< Snímek přijat. */
    VIDEOREC_PIPE_ERR_DISCONTINUITY = -1, /**< Konec snímku neodpovídá očekávanému taktu; volající má udělat rebase. */
    VIDEOREC_PIPE_ERR_FULL = -2           /**< Fronta čekajících snímků je plná (snímek nepřijat). */
} en_VIDEOREC_PIPE_RESULT;

/** @brief Čekající snímek: pixely (NULL = pauza) a takt konce. */
typedef struct st_VIDEOREC_PIPE_PENDING {
    uint8_t *pixels; /**< Kopie pixelů vlastněná pipe (NULL = snímek se nezapisuje). */
    uint64_t end;    /**< Takt konce snímku. */
} st_VIDEOREC_PIPE_PENDING;

/**
 * @brief Stav párování.
 *
 * Invarianty: `count <= VIDEOREC_PIPE_MAX_PENDING`; čekající snímky v kruhové
 * frontě `[head, head+count)` mají rostoucí `end` po `tpf`; poslední čekající
 * končí v `next_frame_end - tpf`; `ncapture <= VIDEOREC_PIPE_MAX_CAPTURES`
 * a každý nevyřízený požadavek míří na `end` některého čekajícího snímku.
 * Ownership: pipe vlastní pixely čekajících snímků a audio renderer; uvolní
 * je videorec_pipe_free().
 */
typedef struct st_VIDEOREC_PIPE {
    unsigned width, height;                                      /**< Rozměry snímku v pixelech. */
    uint64_t tpf;                                                /**< GDG takty na snímek. */
    unsigned spf;                                                /**< Zvukové vzorky na snímek. */
    st_VIDEOREC_AUDIO audio;                                     /**< Renderer zvuku. */
    st_VIDEOREC_PIPE_PENDING pending[VIDEOREC_PIPE_MAX_PENDING]; /**< Kruhová fronta čekajících snímků. */
    unsigned head, count;                                        /**< Začátek a délka kruhové fronty. */
    uint64_t next_frame_end;                                     /**< Očekávaný konec dalšího snímku. */
    uint64_t next_index;                                         /**< Index dalšího zapsaného snímku. */
    videorec_pipe_sink_cb sink;                                  /**< Příjemce hotových snímků. */
    void *user;                                                  /**< Argument pro sink. */
    st_VIDEOREC_PIPE_CAPTURE capture[VIDEOREC_PIPE_MAX_CAPTURES]; /**< Nevyřízené požadavky na zachycení stavu zvuku (v pořadí podání). */
    unsigned ncapture;                                           /**< Počet nevyřízených požadavků. */
    videorec_pipe_capture_cb capture_cb;                         /**< Příjemce zachyceného stavu (NULL = žádný). */
    void *capture_user;                                          /**< Argument pro capture_cb. */
} st_VIDEOREC_PIPE;

/**
 * @brief Inicializuje pipe (audio renderer s řetězcem filtrů SDL cesty, viz videorec_audio.h).
 *
 * @param p               Instance (obsah se přepíše).
 * @param width           Šířka snímku [px].
 * @param height          Výška snímku [px].
 * @param ticks_per_frame GDG takty na snímek; nenulové.
 * @param clk_hz          Frekvence taktů [Hz]; nenulová.
 * @param rate            Vzorkovací frekvence zvuku [Hz]; nenulová.
 * @param channels        Počet zvukových kanálů (viz videorec_audio_init()).
 * @param level           Tabulka úrovní kanálů (zkopíruje se).
 * @param values          Počáteční hodnoty kanálů, nebo NULL = samé 0.
 * @param origin_ticks    Takt začátku prvního snímku.
 * @param sink            Příjemce snímků; nesmí být NULL.
 * @param user            Argument předaný sinku.
 *
 * @post Fronta prázdná, `next_index == 0`, `next_frame_end == origin_ticks + ticks_per_frame`,
 *       bez callbacku zachycení stavu a bez požadavků.
 * @note Selhat nemůže.
 */
void videorec_pipe_init(st_VIDEOREC_PIPE *p, unsigned width, unsigned height, uint64_t ticks_per_frame, uint64_t clk_hz,
                        unsigned rate, unsigned channels, const float level[][VIDEOREC_AUDIO_LEVELS],
                        const uint8_t *values, uint64_t origin_ticks, videorec_pipe_sink_cb sink, void *user);

/**
 * @brief Uvolní čekající snímky a audio renderer (bez volání sinku).
 *
 * @param p Instance po videorec_pipe_init(). Opakované volání je bezpečné.
 */
void videorec_pipe_free(st_VIDEOREC_PIPE *p);

/**
 * @brief Přidá hotový snímek do fronty čekajících.
 *
 * @param p               Instance.
 * @param pixels_or_null  `width * height` bajtů (zkopírují se s maskou `& 0x0F`),
 *                        nebo NULL = snímek se nezapíše (pauza), zvuk se spotřebuje.
 * @param frame_end_ticks Takt konce snímku; musí být rovný očekávanému (`origin + k * tpf`).
 *
 * @return VIDEOREC_PIPE_OK; VIDEOREC_PIPE_ERR_DISCONTINUITY při nesouladu taktu
 *         (stav se nezmění); VIDEOREC_PIPE_ERR_FULL při plné frontě (stav se nezmění).
 *
 * @note Snímek se sinku předá až v videorec_pipe_horizon() / videorec_pipe_flush().
 */
en_VIDEOREC_PIPE_RESULT videorec_pipe_frame(st_VIDEOREC_PIPE *p, const uint8_t *pixels_or_null, uint64_t frame_end_ticks);

/**
 * @brief Předá pipe změnu hodnoty zvukového kanálu.
 *
 * @param p     Instance.
 * @param ch    Kanál.
 * @param value Nová hodnota (bity 0..3).
 * @param ticks Absolutní čas změny; musí být neklesající po kanálech (viz videorec_audio_event()).
 *
 * @note Chybu (neplatný kanál, alokace) tiše ignoruje.
 */
void videorec_pipe_audio_event(st_VIDEOREC_PIPE *p, unsigned ch, uint8_t value, uint64_t ticks);

/**
 * @brief Oznámí, že všechny zvukové události do `horizon_ticks` jsou doručeny, a vydá hotové snímky.
 *
 * Pro každý čekající snímek s `end <= horizon_ticks` (v pořadí) vyrenderuje zvuk
 * a předá snímek sinku (nebo ho zahodí, je-li pauzový).
 *
 * @param p             Instance.
 * @param horizon_ticks Horizont v taktech.
 */
void videorec_pipe_horizon(st_VIDEOREC_PIPE *p, uint64_t horizon_ticks);

/**
 * @brief Vyrenderuje a vydá všechny čekající snímky bez ohledu na horizont.
 *
 * Zvuk, který ještě nedorazil, drží poslední známé hodnoty kanálů.
 *
 * @param p Instance.
 * @post `count == 0`.
 */
void videorec_pipe_flush(st_VIDEOREC_PIPE *p);

/**
 * @brief Vydá čekající snímky od nejstaršího, dokud index dalšího snímku nedosáhne `index`.
 *
 * Použití při retake: snímky před bodem snapshotu, které ještě čekají na zvuk,
 * jsou platná minulost a musí se zapsat dřív, než videorec_pipe_rebase() zahodí
 * zbytek fronty. Zvuk se renderuje jako u videorec_pipe_flush() (nedoručené
 * události drží poslední známé hodnoty). Pauzové snímky (bez pixelů) na cestě
 * se spotřebují bez zvýšení indexu.
 *
 * @param p     Instance.
 * @param index Cílový index dalšího snímku.
 *
 * @post `next_index >= index`, nebo je fronta prázdná (pak `next_index < index`
 *       znamená, že čekajících snímků bylo méně - volající to musí vyloučit).
 */
void videorec_pipe_flush_until_index(st_VIDEOREC_PIPE *p, uint64_t index);

/**
 * @brief Zahájí novou časovou osu: zahodí čekající snímky a zvukové události.
 *
 * Index zapsaných snímků se nemění (nastavuje se videorec_pipe_set_next_index()).
 *
 * @param p            Instance.
 * @param origin_ticks Nový takt začátku prvního snímku.
 * @param values       Hodnoty zvukových kanálů k tomuto okamžiku, nebo NULL = samé 0.
 *
 * Stav filtrů audio rendereru se nemění (viz videorec_audio_rebase()).
 *
 * @post Fronta prázdná, nevyřízené požadavky na zachycení zahozené,
 *       `next_frame_end == origin_ticks + tpf`.
 */
void videorec_pipe_rebase(st_VIDEOREC_PIPE *p, uint64_t origin_ticks, const uint8_t *values);

/**
 * @brief Nastaví příjemce zachyceného stavu zvuku.
 *
 * @param p    Instance.
 * @param cb   Callback (NULL = zachycené stavy se zahazují).
 * @param user Argument pro callback.
 */
void videorec_pipe_set_capture_cb(st_VIDEOREC_PIPE *p, videorec_pipe_capture_cb cb, void *user);

/**
 * @brief Vyžádá stav audio rendereru na konci posledního přijatého snímku.
 *
 * Bez čekajících snímků je renderer přesně na konci posledního přijatého
 * snímku (nebo v počátku osy) a callback se zavolá hned (synchronně).
 * Jinak se požadavek uloží a vyřídí po vyrenderování zvuku toho snímku
 * (videorec_pipe_horizon(), flush). Při plné tabulce požadavků se zahodí
 * nejstarší. Rebase nevyřízené požadavky zahodí - callback se pak nezavolá.
 *
 * @param p   Instance.
 * @param tag Značka předaná callbacku.
 *
 * @post Callback zavolán, nebo požadavek uložen (`ncapture` nejvýš VIDEOREC_PIPE_MAX_CAPTURES).
 */
void videorec_pipe_request_capture(st_VIDEOREC_PIPE *p, uint64_t tag);

/**
 * @brief Nastaví rozložení zvukových kanálů do L/R (viz videorec_audio_set_stereo()).
 *
 * Platí pro zvuk vyrenderovaný od dalšího volání videorec_pipe_horizon()
 * nebo flush; už předané snímky se nemění.
 *
 * @param p      Instance.
 * @param stereo true = stereo rozložení SDL cesty (L = CTC0 + PSG0, R = CTC0 + PSG1),
 *               false = mono rozložení (CTC0 + PSG0 do obou stran).
 */
void videorec_pipe_set_stereo(st_VIDEOREC_PIPE *p, bool stereo);

/**
 * @brief Nastaví index, který dostane další zapsaný snímek.
 *
 * @param p     Instance.
 * @param index Nový index.
 */
void videorec_pipe_set_next_index(st_VIDEOREC_PIPE *p, uint64_t index);

/**
 * @brief Vrátí index, který dostane další zapsaný snímek.
 *
 * @param p Instance.
 * @return Index dalšího snímku (= počet zapsaných od posledního nastavení).
 */
uint64_t videorec_pipe_next_index(const st_VIDEOREC_PIPE *p);

#ifdef __cplusplus
}
#endif
#endif /* VIDEOREC_PIPE_H */
