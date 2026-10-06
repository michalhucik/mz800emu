/**
 * @file   avi_writer.h
 * @brief  Jednoduchý zapisovač RIFF AVI 1.0 (video ZMBV + nekomprimované PCM audio).
 *
 * Soubor obsahuje právě jeden video stream (FourCC "ZMBV", chunky "00dc") a jeden
 * audio stream (PCM 16 bit, chunky "01wb"). Chunky se zapisují prokládaně po
 * snímcích (video chunk, pak audio chunk téhož snímku) do LIST "movi"; index
 * "idx1" se zapíše při avi_writer_close(), kdy se také doplní velikosti
 * a počty v hlavičkách.
 *
 * Velikost souboru je shora omezena na AVI_WRITER_MAX_BYTES (pod limitem
 * 32bitového `long` na Windows a pod limitem RIFF 4 GiB), případně na menší
 * st_AVI_WRITER_PARAMS::max_bytes. Po dosažení limitu
 * vrací avi_writer_write_frame() hodnotu AVI_WRITER_ERR_FULL a volající má soubor
 * zavřít a pokračovat dalším partem.
 *
 * Předpoklad: hostitel je little-endian (x86-64, ARM64 macOS); audio vzorky
 * se zapisují přes fwrite přímo.
 *
 * Instance není thread-safe; používá ji jen jedno (writer) vlákno.
 *
 * @par Licence: GPLv3
 */

#ifndef AVI_WRITER_H
#define AVI_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximální velikost jednoho AVI souboru v bajtech (1,75 GiB). Zahrnuje i index
 * "idx1", který se zapisuje při zavření souboru.
 */
#define AVI_WRITER_MAX_BYTES 0x70000000u

/**
 * Parametry zapisovaného souboru. Všechny členy kromě `max_bytes` musí být
 * nenulové. Hodnoty se po avi_writer_open() nemění. Při inicializaci
 * pozičním initializerem bez posledního členu je `max_bytes` 0 = výchozí limit.
 */
typedef struct {
    unsigned width;           /**< šířka videa v pixelech (musí se vejít do 16 bitů) */
    unsigned height;          /**< výška videa v pixelech (musí se vejít do 16 bitů) */
    unsigned fps_num;         /**< čitatel snímkové frekvence */
    unsigned fps_den;         /**< jmenovatel snímkové frekvence */
    unsigned audio_rate;      /**< vzorkovací frekvence audia v Hz */
    unsigned audio_channels;  /**< počet audio kanálů (vzorek = 16 bit se znaménkem) */
    uint32_t max_bytes;       /**< limit velikosti souboru v bajtech; 0 nebo hodnota nad
                                   AVI_WRITER_MAX_BYTES = AVI_WRITER_MAX_BYTES (menší hodnota
                                   slouží testům k vyvolání AVI_WRITER_ERR_FULL) */
} st_AVI_WRITER_PARAMS;

/** Výsledky operací zapisovače. */
typedef enum {
    AVI_WRITER_OK = 0,        /**< operace uspěla */
    AVI_WRITER_ERR_IO = -1,   /**< chyba I/O nebo paměti; soubor může být nedokončený */
    AVI_WRITER_ERR_FULL = -2, /**< snímek by překročil AVI_WRITER_MAX_BYTES; nic se nezapsalo */
    AVI_WRITER_ERR_ARG = -3   /**< neplatný argument (NULL ukazatel apod.) */
} en_AVI_WRITER_RESULT;

/** Neprůhledný stav zapisovače. Vlastník je volající, uvolnění přes avi_writer_close(). */
typedef struct st_AVI_WRITER st_AVI_WRITER;

/**
 * @brief Vytvoří (přepíše) AVI soubor a zapíše hlavičky.
 *
 * @param path  Cesta k souboru v UTF-8 (otevírá se "w+b", existující obsah se
 *              zahodí). Na Windows se převede na UTF-16 a otevře přes
 *              `_wfopen` (funguje s diakritikou nezávisle na kódové stránce
 *              a locale procesu); není-li cesta platné UTF-8, použije se
 *              `fopen` s cestou beze změny.
 * @param p     Parametry souboru; hodnoty se zkopírují.
 * @return Nový zapisovač, nebo NULL při neplatných parametrech (NULL, nulový člen),
 *         selhání otevření/zápisu hlaviček či nedostatku paměti. Při NULL
 *         nezůstává alokovaný žádný zdroj.
 *
 * @par Ownership
 * Vlastnictví vráceného ukazatele přechází na volajícího; musí ho uvolnit
 * přes avi_writer_close().
 */
st_AVI_WRITER *avi_writer_open(const char *path, const st_AVI_WRITER_PARAMS *p);

/**
 * @brief Zapíše jeden snímek: video chunk a navazující audio chunk.
 *
 * Lichá velikost video dat se doplní paddingovým bajtem (RIFF zarovnání).
 *
 * @param w             Zapisovač.
 * @param video         Zakódovaná video data snímku (nesmí být NULL).
 * @param video_size    Velikost video dat v bajtech.
 * @param keyframe      true = klíčový snímek (příznak v idx1).
 * @param audio         Prokládané 16bit vzorky; smí být NULL jen pokud audio_frames == 0.
 * @param audio_frames  Počet audio vzorků na kanál pro tento snímek.
 * @return AVI_WRITER_OK; AVI_WRITER_ERR_ARG (w nebo video je NULL);
 *         AVI_WRITER_ERR_FULL (snímek včetně rezervy pro index by přesáhl
 *         limit souboru (AVI_WRITER_MAX_BYTES, resp. max_bytes) - nic se nezapsalo, počet snímků se nezměnil,
 *         volající má soubor zavřít a pokračovat dalším partem);
 *         AVI_WRITER_ERR_IO (chyba zápisu nebo paměti - soubor může obsahovat
 *         část snímku, počet snímků se nezvýšil).
 *
 * @par Postconditions
 * Při AVI_WRITER_OK se avi_writer_frame_count() zvýší o 1.
 */
en_AVI_WRITER_RESULT avi_writer_write_frame(st_AVI_WRITER *w, const uint8_t *video, size_t video_size,
                                            bool keyframe, const int16_t *audio, size_t audio_frames);

/**
 * @brief Zahodí všechny snímky od indexu frame_count výše (ponechá 0 .. frame_count-1).
 *
 * Fyzicky zkrátí soubor a nastaví pozici zápisu za poslední ponechaný snímek,
 * zápis pak může pokračovat. Je-li frame_count >= aktuálnímu počtu snímků,
 * nedělá nic a vrací AVI_WRITER_OK.
 *
 * @param w            Zapisovač.
 * @param frame_count  Počet snímků, které se mají zachovat.
 * @return AVI_WRITER_OK; AVI_WRITER_ERR_ARG (w je NULL);
 *         AVI_WRITER_ERR_IO (zkrácení souboru selhalo, stav se nemění).
 *
 * @par Poznámka
 * Zlib stream ZMBV je spojitý přes snímky, proto musí volající po truncate
 * vynutit klíčový snímek.
 */
en_AVI_WRITER_RESULT avi_writer_truncate_to_frame(st_AVI_WRITER *w, uint64_t frame_count);

/**
 * @brief Vrátí počet dosud zapsaných (a nezahozených) snímků.
 *
 * @param w  Zapisovač; NULL je povoleno.
 * @return Počet snímků, 0 pro NULL.
 */
uint64_t avi_writer_frame_count(const st_AVI_WRITER *w);

/**
 * @brief Vrátí aktuální velikost souboru v bajtech (hlavičky + dosud zapsané chunky).
 *
 * Hodnota je pozice zápisu, která po avi_writer_open(), avi_writer_write_frame()
 * i avi_writer_truncate_to_frame() leží na konci souboru. Nezahrnuje index
 * "idx1" a doplnění hlaviček, které se zapíšou až při avi_writer_close();
 * výsledná velikost uzavřeného souboru je tedy větší.
 *
 * @param w  Zapisovač; NULL je povoleno.
 * @return Velikost v bajtech, 0 pro NULL (nebo když pozici nelze zjistit).
 * @par Vlákna Jen vlákno, které zapisovač používá (instance není thread-safe).
 */
uint64_t avi_writer_bytes(const st_AVI_WRITER *w);

/**
 * @brief Finalizuje soubor (idx1, velikosti, počty v hlavičkách), zavře ho a uvolní zapisovač.
 *
 * @param w  Zapisovač; po volání je ukazatel neplatný, i když funkce selže.
 * @return AVI_WRITER_OK; AVI_WRITER_ERR_ARG (w je NULL);
 *         AVI_WRITER_ERR_IO (chyba zápisu nebo zavření - soubor může být nedokončený).
 *
 * @par Ownership
 * Vždy uvolní `w` (a soubor zavře), bez ohledu na návratovou hodnotu.
 */
en_AVI_WRITER_RESULT avi_writer_close(st_AVI_WRITER *w);

#ifdef __cplusplus
}
#endif

#endif /* AVI_WRITER_H */
