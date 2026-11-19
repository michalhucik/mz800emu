/**
 * @file fdc_fileid.h
 * @brief Identita souboru obrazu diskety: pozná tentýž soubor přes různé cesty.
 *
 * Řadič podle ní hlídá, aby do jednoho souboru obrazu směla zapisovat
 * nejvýš jedna mechanika (fdc.h, oddíl o sdíleném obrazu). Identita se
 * zjišťuje jen při vložení obrazu, nikdy během emulace.
 *
 * Identita má dvě části:
 *  - klíč cesty: absolutní cesta bez `.` a `..` (g_canonicalize_filename()),
 *    na Windows navíc s lomítky `/` a bez ohledu na velikost písmen;
 *  - číslo souboru v souborovém systému (POSIX: zařízení a inode; Windows:
 *    sériové číslo svazku a index souboru), pokud ho systém vydá. To pokrývá
 *    pevné a symbolické odkazy, krátká jména 8.3 a různá písmena jednotek
 *    ukazující na tentýž svazek.
 *
 * Dvě identity se shodují, když se shoduje klíč cesty, nebo když obě mají
 * číslo souboru a to je stejné.
 *
 * Co pokryto není: soubor dosažitelný přes dvě síťové cesty, pokud pro ně
 * systém vydá různá čísla souboru nebo žádná [neověřeno, jak se chovají
 * jednotlivé síťové souborové systémy]; soubor, který jiný program za běhu
 * nahradí jiným souborem téhož jména (číslo souboru je z okamžiku vložení,
 * klíč cesty se ale shoduje dál); souborové systémy, kde 64bitový index
 * souboru není jednoznačný (shoda navíc jen znamená zbytečnou ochranu).
 *
 * License: GPLv3.
 */

#ifndef FDC_FILEID_H
#define FDC_FILEID_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

    /**
     * @brief Identita souboru obrazu diskety.
     *
     * Invarianty: prázdná identita má path_key == NULL a has_id == 0 a
     * neshoduje se s žádnou jinou (ani s jinou prázdnou). Při has_id == 0
     * jsou volume a file nulové.
     *
     * Ownership: path_key vlastní struktura, uvolní ho fdc_fileid_clear().
     * Strukturu smí vynulovat memset (prázdná identita).
     */
    typedef struct st_FDC_FILEID
    {
        char *path_key;  /**< klíč cesty (UTF-8), NULL = prázdná identita */
        int has_id;      /**< 1 = volume a file jsou platné */
        uint64_t volume; /**< zařízení (POSIX st_dev) nebo sériové číslo svazku (Windows) */
        uint64_t file;   /**< inode (POSIX) nebo index souboru (Windows) */
    } st_FDC_FILEID;

    /**
     * @brief Zjistí identitu souboru.
     *
     * Soubor nemusí existovat ani jít otevřít: pak má identita jen klíč
     * cesty. Volání sahá na souborový systém (jedno otevření nebo stat).
     *
     * @param id   Výstup; předchozí obsah se nepoužije ani neuvolní (volající
     *             předá vynulovanou nebo vyčištěnou strukturu).
     * @param path Cesta k souboru v kódování jmen souborů GLib (na Windows
     *             UTF-8); NULL nebo "" = prázdná identita.
     * @post id je platná identita; uvolnit přes fdc_fileid_clear().
     * @note Bez zámků; volající zajišťuje, že s id nepracuje jiné vlákno.
     */
    extern void fdc_fileid_init(st_FDC_FILEID *id, const char *path);

    /**
     * @brief Uvolní identitu a nastaví ji na prázdnou.
     * @param id Identita (NULL = no-op); opakované volání je bezpečné.
     */
    extern void fdc_fileid_clear(st_FDC_FILEID *id);

    /**
     * @brief Test, zda dvě identity označují tentýž soubor.
     * @param a První identita (NULL = prázdná).
     * @param b Druhá identita (NULL = prázdná).
     * @return true = shodný klíč cesty, nebo obě mají číslo souboru a to
     *         je stejné; false jinak (i když je některá prázdná).
     */
    extern bool fdc_fileid_same(const st_FDC_FILEID *a, const st_FDC_FILEID *b);

#ifdef __cplusplus
}
#endif

#endif /* FDC_FILEID_H */
