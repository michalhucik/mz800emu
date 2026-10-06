/**
 * @file   zmbv_enc.h
 * @brief  Enkodér videokodeku ZMBV (Zip Motion Blocks Video) pro 8bpp paletový obraz.
 *
 * Vyrábí bitstream, který dekóduje ffmpeg (dekodér "zmbv") a tím i nástroje nad ním.
 * Podporuje jen 8bpp, bloky 16×16, zlib kompresi a nulové pohybové vektory
 * (delta snímek = XOR změněných bloků proti předchozímu snímku). Paleta je pevná
 * po celou dobu života enkodéru.
 *
 * Zlib stream je spojitý přes snímky a resetuje se na každém klíčovém snímku.
 * Dekodér proto musí začít klíčovým snímkem; po truncate souboru musí volající
 * vynutit klíčový snímek (force_keyframe).
 *
 * Reference formátu: https://wiki.multimedia.cx/index.php/DosBox_Capture_Codec
 *
 * @par Licence: GPLv3
 */

#ifndef ZMBV_ENC_H
#define ZMBV_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Šířka bloku v pixelech. */
#define ZMBV_BLOCK_W 16
/** Výška bloku v pixelech. */
#define ZMBV_BLOCK_H 16
/** Počet položek palety v klíčovém snímku (8bpp formát vždy nese 256 položek). */
#define ZMBV_PALETTE_ENTRIES 256

/** Neprůhledný stav enkodéru. Vlastník je volající, uvolnění přes zmbv_enc_free(). */
typedef struct st_ZMBV_ENC st_ZMBV_ENC;

/**
 * @brief Vytvoří enkodér pro snímky dané velikosti.
 *
 * @param width              Šířka snímku v pixelech (> 0).
 * @param height             Výška snímku v pixelech (> 0).
 * @param palette_rgb        Paleta jako 0x00RRGGBB, palette_count položek; zbytek do 256 je černý.
 * @param palette_count      Počet položek palety (1 až 256).
 * @param keyframe_interval  Max. počet delta snímků mezi klíčovými (0 = jen první snímek klíčový).
 * @return Nový enkodér, nebo NULL při chybných parametrech či nedostatku paměti.
 */
st_ZMBV_ENC *zmbv_enc_new(unsigned width, unsigned height, const uint32_t *palette_rgb,
                          unsigned palette_count, unsigned keyframe_interval);

/**
 * @brief Zakóduje jeden snímek.
 *
 * @param enc             Enkodér.
 * @param pixels          width*height bajtů, řádky shora dolů, hodnota = index palety.
 * @param force_keyframe  true = vynutit klíčový snímek (např. po truncate souboru).
 * @param[out] out        Ukazatel na zakódovaná data; buffer patří enkodéru a platí do dalšího volání.
 * @param[out] out_size   Velikost zakódovaných dat v bajtech.
 * @param[out] is_keyframe true pokud vznikl klíčový snímek (pro AVI index).
 * @return 0 při úspěchu, -1 při chybě zlib (enkodér je pak nepoužitelný, uvolni ho).
 *
 * @pre enc, pixels, out, out_size a is_keyframe jsou nenulové; pixels ukazuje na
 *      alespoň width*height bajtů (rozměr z zmbv_enc_new()).
 * @post Enkodér si pamatuje pixels jako referenci pro další delta snímek.
 */
int zmbv_enc_frame(st_ZMBV_ENC *enc, const uint8_t *pixels, bool force_keyframe,
                   const uint8_t **out, size_t *out_size, bool *is_keyframe);

/**
 * @brief Uvolní enkodér. Bezpečné volat s NULL.
 */
void zmbv_enc_free(st_ZMBV_ENC *enc);

#ifdef __cplusplus
}
#endif

#endif /* ZMBV_ENC_H */
