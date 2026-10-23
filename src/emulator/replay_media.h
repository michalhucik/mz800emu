/**
 * @file   replay_media.h
 * @brief  Média v záznamu vstupů: obsah obrazů při startu záznamu
 *         a přehrání nad dočasnými kopiemi.
 *
 * Snapshot nese jen stav řadičů a cesty k obrazům, ne jejich obsah. Záznam
 * proto při startu uloží obsah všech nasazených obrazů (FDC, FDC1, QD,
 * IDE8) do `.mzr` a přehrání je nasadí z kopií:
 *
 * | Slot | Zařízení | Přehrání |
 * |------|----------|----------|
 * | `fdc0.N`, `fdc1.N` | mechanika N řadiče FDC / FDC1 | paměťová kopie, režim discard |
 * | `qd`   | Quick Disk (obraz, Unicard loader) | paměťová kopie, režim discard |
 * | `ide8.N` | disk IDE8 | dočasný soubor (zápisy jdou do něj) |
 * | `cmt`  | páska Virtual CMT | dočasný soubor + blok a nastavení z manifestu (`cmt`) |
 * | `cmthack.K` | soubor K-tého výběru v dialogu CMT hacku | dočasný soubor místo dialogu |
 *
 * Obraz nad limitem REPLAY_MEDIA_EMBED_LIMIT (součet za záznam) se do
 * záznamu nevloží, jen jeho SHA-256: přehrání pak vezme obraz nasazený
 * v emulátoru, ověří hash a pracuje nad jeho kopií. Originály se při
 * přehrání nikdy nemění a INI se nemění; po konci přehrání se vrátí obrazy
 * podle INI. U kazety se navíc dočasně převezme nastavení ze záznamu
 * (výchozí rychlost, vlastní pulzy, polarita, rychlosti bloků) a po konci
 * vrátí; původní páska se vloží znovu od začátku.
 *
 * Vlákna: všechny funkce volá emu vlákno v paused smyčce (start a konec
 * záznamu / přehrání) nebo při ukončení emulátoru.
 */

#ifndef REPLAY_MEDIA_H
#define REPLAY_MEDIA_H

#include <stddef.h>
#include <stdbool.h>

#include "replay_format.h"
#include "replay.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Uloží média nasazená při startu záznamu do @p d.
     *
     * Přidá médium za každý slot s obrazem (vložené do limitu, nad ním jen
     * hash s varováním) a varování pro obsah, který záznam nenese
     * (virtuální Quick Disk). Nastaví `d->media_known`.
     *
     * @param d Nový záznam.
     * @pre Emu vlákno, emulace stojí (start záznamu).
     */
    void replay_media_capture(st_REPLAY_DATA *d);

    /**
     * @brief Nasadí média záznamu před načtením startovního snapshotu.
     *
     * Sloty z manifestu dostanou kopie (vložené nebo ověřené hashem),
     * ostatní sloty se odpojí. Původní stav si zapamatuje pro
     * replay_media_restore(). Záznam bez seznamu médií (starší) média
     * nechá, jak jsou.
     *
     * @param d       Přehrávaný záznam.
     * @param errbuf  Buffer pro text chyby (anglicky).
     * @param errsize Velikost bufferu.
     * @return NULL = v pořádku; jinak text chyby (v @p errbuf), média jsou
     *         pak vrácená do původního stavu.
     * @pre Emu vlákno, emulace stojí.
     */
    const char *replay_media_prepare(const st_REPLAY_DATA *d, char *errbuf, size_t errsize);

    /**
     * @brief Vrátí obrazy podle INI a smaže dočasné kopie (konec přehrání).
     *
     * Bez předchozího úspěšného replay_media_prepare() nedělá nic.
     *
     * @pre Emu vlákno, emulace stojí nebo končí.
     */
    void replay_media_restore(void);

    /**
     * @brief Provede operaci kazety (záznam, přehrání i mimo ně).
     *
     * @param op   Operace.
     * @param arg  Hodnota (pauza 0/1, číslo bloku).
     * @param path Cesta u REPLAY_CMT_RECORD (při přehrání dočasná).
     * @return true = operace se provedla (u složených i "nic nedělat" je
     *         úspěch); false = selhala (bez pásky, neplatný blok, cesta).
     * @pre Emu vlákno; volající nedrží zámek stavu zařízení.
     * @note Operaci provede pod device_state_lock(): okna kazety čtou pásku
     *       pod tímtéž zámkem.
     */
    bool replay_media_cmt_execute(en_REPLAY_CMT_OP op, int32_t arg, const char *path);

    /**
     * @brief Cesta nového souboru v dočasném adresáři přehrání.
     *
     * Adresář se vytvoří při prvním použití a smaže v replay_media_restore().
     *
     * @param name Jméno souboru (bez adresáře; určuje příponu).
     * @return Cesta (g_free), nebo NULL (adresář nejde vytvořit).
     */
    char *replay_media_tmp_file(const char *name);

    /**
     * @brief Stav kazety jako JSON: CMT hack, výchozí rychlost a pulzy,
     *        polarita, přehrávaný blok, rychlosti a pulzy bloků pásky.
     *
     * Stejný text jako člen `cmt` manifestu záznamu; používá ho i otisk
     * stroje (machine_hash.h, část `cmt`).
     *
     * @return JSON objekt jako text (vlastní volající, g_free).
     * @pre Emu vlákno nebo stojící emu vlákno.
     */
    char *replay_media_cmt_state_json(void);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_MEDIA_H */
