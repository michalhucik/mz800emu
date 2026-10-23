/**
 * @file   machine_hash.h
 * @brief  Obsahový otisk celého stroje po částech (SHA-256).
 *
 * Odpovídá na otázku "je stav stroje stejný jako tehdy?" a slouží jako
 * podklad testu úplnosti revize stavu (state_rev.h): co změní otisk, musí
 * změnit i revizi. Na rozdíl od revize vidí jen obsah, ne historii
 * (změna a návrat zpět = stejný otisk).
 *
 * | Část (en_MACHINE_HASH_PART) | Obsah | Chybí, když |
 * |-----------------------------|-------|-------------|
 * | `cpu`     | registry a skrytý stav Z80 (state_hash_part()) | CPU ještě neexistuje |
 * | `ram`     | vnitřní RAM 64 KB (state_hash_part()) | - |
 * | `memext`  | RAM MemExt 512 KB (state_hash_part()) | MemExt odpojený |
 * | `vram`    | VRAM platformy (state_hash_part()) | - |
 * | `pcg`     | PCG (state_hash_part()) | mimo MZ-1500 |
 * | `periph`  | čipy a řadiče, stejný seznam komponent snapshotu jako hash `periph` záznamu vstupů (replay_periph_components()) | chyba handleru snapshotu |
 * | `memmap`  | mapa paměti `g_memory.map`, SW1 (MZ-800, MZ-1500), druhý PSG (MZ-800), konfigurace a mapa MemExt | - |
 * | `rom`     | obsah `g_memory.ROM` (vidí i patch CMT hacku) | - |
 * | `media`   | identita vložených médií (cesty, ochrana proti zápisu, připojení) a počítadlo zápisů `g_media_writes`; s MACHINE_HASH_FLAG_INCLUDE_MEDIA i obsah obrazů disket, Quick Disku, IDE8, souboru pásky a FLASH MemExt | - |
 * | `inputs`  | výsledná matice klávesnice (`keyboard_matrix & vkbd_matrix`), typ a stav joysticků, běžící autotype | - |
 * | `ramdisk` | stav RAM disků (STD a Pezik); s MACHINE_HASH_FLAG_INCLUDE_MEDIA i jejich obsah | - |
 * | `cmt`     | stav kazety: transport, poloha (blok, čas), nastavení, rychlosti bloků, CMT hack | - |
 *
 * Mimo otisk zůstává: obraz (framebuffer, výstup), zvuk, plotter MZ-1P16,
 * živý stav hostitelského joysticku (čte se v okamžiku IN), obsah adresáře
 * Unicard a Quick Disku ve virtuálním režimu (jen cesta).
 *
 * Celkový otisk je SHA-256 řádků `jméno:hex\n` vyžádaných částí v pořadí
 * výčtu; chybějící část má místo hexu `-`. Porovnávat lze jen otisky se
 * stejnou maskou částí a stejnými příznaky.
 *
 * Vlákna: funkce čtou stav emulátoru bez zámku, volat jen z emu vlákna
 * (nebo když emu vlákno stojí v pauze).
 */

#ifndef MACHINE_HASH_H
#define MACHINE_HASH_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "state_hash.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Části otisku stroje (pořadí = pořadí v celkovém otisku a v odpovědi MCP).
     */
    typedef enum en_MACHINE_HASH_PART
    {
        MACHINE_HASH_CPU = 0, /**< Registry a skrytý stav Z80. */
        MACHINE_HASH_RAM,     /**< Vnitřní RAM. */
        MACHINE_HASH_MEMEXT,  /**< RAM MemExt (jen připojený). */
        MACHINE_HASH_VRAM,    /**< VRAM. */
        MACHINE_HASH_PCG,     /**< PCG (jen MZ-1500). */
        MACHINE_HASH_PERIPH,  /**< Čipy a řadiče (komponenty snapshotu). */
        MACHINE_HASH_MEMMAP,  /**< Mapování paměti a HW volby, které ho určují. */
        MACHINE_HASH_ROM,     /**< Obsah ROM. */
        MACHINE_HASH_MEDIA,   /**< Identita (volitelně obsah) vložených médií. */
        MACHINE_HASH_INPUTS,  /**< Klávesnice, joysticky, autotype. */
        MACHINE_HASH_RAMDISK, /**< Stav (volitelně obsah) RAM disků. */
        MACHINE_HASH_CMT,     /**< Stav kazety. */
        MACHINE_HASH_PART_COUNT /**< Počet částí (není to část). */
    } en_MACHINE_HASH_PART;

    /** @brief Maska všech částí (bit = 1 << en_MACHINE_HASH_PART). */
#define MACHINE_HASH_ALL_PARTS ((uint32_t)((1u << MACHINE_HASH_PART_COUNT) - 1u))

    /**
     * @brief Příznak: do částí `media` a `ramdisk` zahrnout i obsah.
     *
     * Obsah obrazů disket, Quick Disku, IDE8, souboru pásky, FLASH MemExt
     * a RAM disků. Může být drahé (IDE8 se čte ze souboru celý).
     */
#define MACHINE_HASH_FLAG_INCLUDE_MEDIA 0x1u

    /**
     * @brief Výsledek machine_hash_compute().
     *
     * Invarianty: bit části v @c present je nastaven jen u vyžádané části,
     * která na platformě a v sestavě existuje; jen pro takovou platí
     * @c part. @c total pokrývá všechny vyžádané části.
     */
    typedef struct st_MACHINE_HASH
    {
        uint32_t requested; /**< Vyžádané části (maska). */
        uint32_t present;   /**< Části s vypočteným hashem (podmnožina @c requested). */
        char part[MACHINE_HASH_PART_COUNT][STATE_HASH_HEX_LEN + 1]; /**< Hex SHA-256 části (jen bity @c present, jinak ""). */
        char total[STATE_HASH_HEX_LEN + 1]; /**< Celkový otisk vyžádaných částí. */
    } st_MACHINE_HASH;

    /**
     * @brief Jméno části (`cpu`, `ram`, ..., `cmt`).
     *
     * Inline, aby jména šla použít bez jádra emulátoru (MCP dispatch a jeho
     * unit test linkují jen vybrané zdroje).
     *
     * @param part Část; mimo rozsah vrátí "unknown".
     * @return Statický řetězec, neuvolňovat.
     */
    static inline const char *machine_hash_part_name(en_MACHINE_HASH_PART part)
    {
        static const char *const names[MACHINE_HASH_PART_COUNT] = {
            "cpu", "ram", "memext", "vram", "pcg", "periph",
            "memmap", "rom", "media", "inputs", "ramdisk", "cmt",
        };
        if ((int)part < 0 || part >= MACHINE_HASH_PART_COUNT) return "unknown";
        return names[part];
    }

    /**
     * @brief Část podle jména.
     *
     * @param name Jméno (rozlišuje velikost písmen), NULL = neznámé.
     * @return Index části, nebo -1 pro neznámé jméno.
     */
    static inline int machine_hash_part_from_name(const char *name)
    {
        if (!name) return -1;
        for (int i = 0; i < MACHINE_HASH_PART_COUNT; i++)
        {
            if (strcmp(name, machine_hash_part_name((en_MACHINE_HASH_PART)i)) == 0) return i;
        };
        return -1;
    }

    /**
     * @brief Hash jedné části.
     *
     * @param part    Část podle tabulky v hlavičce souboru.
     * @param flags   MACHINE_HASH_FLAG_*.
     * @param[out] out_hex Buffer aspoň STATE_HASH_HEX_LEN + 1 znaků.
     * @return true = hash vypočten; false = část na platformě nebo
     *         v sestavě není (nebo selhal handler snapshotu u `periph`),
     *         @p out_hex se pak nemění.
     * @pre Emu vlákno nebo stojící emu vlákno.
     * @note Čtení obsahu médií (příznak) může číst soubory hostitele.
     */
    bool machine_hash_part(en_MACHINE_HASH_PART part, unsigned flags, char *out_hex);

    /**
     * @brief Otisk vybraných částí a celkový otisk.
     *
     * @param mask  Vyžádané části (bity 1 << en_MACHINE_HASH_PART); bity
     *              mimo MACHINE_HASH_ALL_PARTS se ignorují.
     * @param flags MACHINE_HASH_FLAG_*.
     * @param[out] out Výsledek (vždy celý přepsán).
     * @pre Emu vlákno nebo stojící emu vlákno; @p out není NULL.
     */
    void machine_hash_compute(uint32_t mask, unsigned flags, st_MACHINE_HASH *out);

#ifdef __cplusplus
}
#endif

#endif /* MACHINE_HASH_H */
