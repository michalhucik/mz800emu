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
     * Páska s neuloženými změnami (Modified) nebo bez cesty se ukládá jako
     * obraz z paměti; nad zbývajícím limitem by záznam nesl jen hash
     * a přehrání by ji nemělo odkud vzít (hash souboru na původní cestě
     * se s obrazem neshoduje). Taková páska záznam odmítne: chyba "Tape
     * with unsaved changes is too large for an input recording; save the
     * tape first".
     *
     * @param d Nový záznam.
     * @param errbuf Buffer pro text chyby. @param errsize Velikost bufferu.
     * @return NULL = v pořádku; jinak text chyby v @p errbuf (záznam pak
     *         nezačne, @p d uvolní volající).
     * @pre Emu vlákno, emulace stojí (start záznamu).
     */
    const char *replay_media_capture(st_REPLAY_DATA *d, char *errbuf, size_t errsize);

    /**
     * @brief Předběžná kontrola před startem záznamu: vejde se páska s neuloženými změnami do limitu?
     *
     * Stejné pravidlo jako replay_media_capture() (zbytek limitu
     * REPLAY_MEDIA_EMBED_LIMIT po obrazech disket a Quick Disku), ale bez
     * změny čehokoli: záznam vstupů ji volá dřív, než uloží a znovu načte
     * startovní snapshot, takže odmítnutý start stav stroje nemění.
     *
     * @param errbuf Buffer pro text chyby. @param errsize Velikost bufferu.
     * @return NULL = vejde se (nebo páska jde ze souboru, nebo žádná); jinak
     *         "Tape with unsaved changes is too large for an input
     *         recording; save the tape first" v @p errbuf.
     * @pre Emu vlákno, emulace stojí.
     */
    const char *replay_media_check_cmt_fits(char *errbuf, size_t errsize);

#ifdef MZTEST_HEADLESS
    /**
     * @brief Jen pro testy: limit vložených médií místo REPLAY_MEDIA_EMBED_LIMIT.
     * @param limit Limit v bajtech (0 = výchozí).
     */
    void replay_media_test_set_embed_limit(uint64_t limit);
#endif

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
     * @pre Emu vlákno, emulace stojí. Volající nedrží device_state_lock():
     *      kazetová část si zámek bere sama, disketová také (ta jen tehdy,
     *      když ho vlákno ještě nedrží).
     * @note Výměna disket a pásky běží pod device_state_lock(), protože
     *       uvolňuje paměť, kterou UI čte pod týmž zámkem.
     */
    const char *replay_media_prepare(const st_REPLAY_DATA *d, char *errbuf, size_t errsize);

    /**
     * @brief Vrátí obrazy podle INI a smaže dočasné kopie (konec přehrání).
     *
     * Bez předchozího úspěšného replay_media_prepare() nedělá nic.
     * Diskety se vracejí v pořadí FDC0 FD0..FD3, FDC1 FD0..FD3: měl-li
     * uživatel tentýž soubor ve více mechanikách, zápis dostane první
     * v pořadí a ostatní ochranu kvůli sdílení (fdc.h), i když před
     * přehráním zapisovala jiná. Známé omezení; data se neztratí, obrazy se
     * čtou znovu ze souboru.
     *
     * @pre Emu vlákno, emulace stojí nebo končí. Volající nedrží
     *      device_state_lock() (viz replay_media_prepare()).
     */
    void replay_media_restore(void);

    /**
     * @brief Poslední varování při návratu pásky uživatele po přehrání záznamu vstupů.
     *
     * Rychlosti bloků se vrací jen nad toutéž páskou (stejný počet a typy
     * bloků); změnil-li se soubor pásky během přehrání na disku, rychlosti
     * se nezapíšou a vznikne varování. Okno Virtual CMT
     * ho podle čítače ohlásí jednou (cmt_view_poll_replay_warning()).
     *
     * @param[out] serial Čítač varování (zvedne se s každým novým; smí být NULL).
     * @return Věta anglicky ("" = zatím žádné); platí do dalšího varování.
     * @pre Vlákno emulátoru, nebo UI vlákno pod device_state_lock() (zapisuje
     *      emu vlákno pod týmž zámkem).
     */
    const char *replay_media_get_cmt_warning(unsigned *serial);

    /**
     * @brief Provede operaci kazety (záznam, přehrání i mimo ně).
     *
     * @param op   Operace.
     * @param arg  Hodnota (pauza 0/1, číslo bloku).
     * @param path Cesta u REPLAY_CMT_RECORD (NULL = jen do pásky, cmt_record(); při přehrání dočasný WAV).
     * @return true = operace se provedla (u složených i "nic nedělat" je
     *         úspěch); false = selhala (bez pásky, neplatný blok, cesta).
     * @pre Emu vlákno; volající nedrží zámek stavu zařízení.
     * @note Operaci provede pod device_state_lock(): okna kazety čtou pásku
     *       pod tímtéž zámkem.
     */
    bool replay_media_cmt_execute(en_REPLAY_CMT_OP op, int32_t arg, const char *path);

    /**
     * @brief Platí podmínky operace kazety z okna (st_REPLAY_CMT_GUARD)?
     *
     * REPLAY_CMT_GUARD_TAPE: cmttmz_get_tape_serial() == tape_serial (bez
     * pásky čítač platí také, výměna ho mění vždy). REPLAY_CMT_GUARD_NOT_STOP:
     * transport není STOP. REPLAY_CMT_GUARD_RECORD: transport je RECORD
     * (Play okna při nahrávání čekajícím na motor posílá REPLAY_CMT_PAUSE 0;
     * přešel-li mezitím deck přes STOP do PLAY, cmt_pause(0) by rozjel
     * přehrávání čekající na program). REPLAY_CMT_GUARD_NOT_PLAYING_ARG:
     * neplatí, když páska hraje bez pauzy (PLAY, ne paused) a otevřený blok
     * je @p arg - přesně případ, kdy cmt_tape_select_block(arg, 1) pásku
     * pauzuje místo přehrání.
     *
     * @param guard Podmínky (NULL nebo flags 0 = vždy platí).
     * @param arg   Argument operace (číslo bloku).
     * @return true = provést; false = stav se od rozhodnutí okna změnil.
     * @pre Emu vlákno (čte g_cmt a index pásky, které mění jen ono).
     */
    bool replay_media_cmt_guard_holds(const st_REPLAY_CMT_GUARD *guard, int32_t arg);

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
     * @brief Zapamatuje dočasnou cestu souboru nahrávání při přehrání a jeho původní cestu.
     *
     * Nahrávání s cestou se při přehrání přesměruje do dočasného souboru
     * (replay.c); páska uložená do něj po STOP pak má dočasnou cestu.
     * replay_media_cmt_snapshot_path() místo ní vrací původní cestu, takže
     * snapshot i otisk stroje jsou stejné jako při záznamu. Páry platí do
     * konce přehrání (replay_media_restore()).
     *
     * @param tmp Dočasná cesta. @param orig Cesta ze záznamu.
     * @pre Vlákno emulátoru.
     */
    void replay_media_cmt_map_path(const char *tmp, const char *orig);

    /**
     * @brief Cesta pásky, kterou má uložit snapshot pořízený během přehrání záznamu.
     *
     * Přehrání vkládá pásku z kopie v dočasném adresáři (smaže se po konci
     * přehrání). Je-li @p path právě tato kopie, vrátí cestu původního
     * souboru při záznamu (st_REPLAY_MEDIA::name, slot `cmt`; "" u pásky,
     * která při záznamu cestu neměla), u pásky uložené nahráváním do
     * dočasného souboru původní cestu nahrávání
     * (replay_media_cmt_map_path()), jinak @p path beze změny. Snapshot
     * tak po konci přehrání pásku otevře z původního souboru (obsah se může
     * lišit od kopie; loader pak ohlásí neshodu jako u jiného souboru)
     * a otisk stroje (machine_hash.c) hashuje stejnou cestu jako při záznamu.
     *
     * @param path Cesta vložené pásky (g_cmt.last_filename), smí být NULL.
     * @return Cesta k uložení; platí do konce přehrání nebo do dalšího
     *         volání se změnou pásky (nevlastní ji volající).
     * @pre Emu vlákno.
     */
    const char *replay_media_cmt_snapshot_path(const char *path);

    /**
     * @brief Stav kazety jako JSON: CMT hack, nastavení Direct Save
     *        (`direct_save`), výchozí rychlost a pulzy,
     *        polarita, automatické ukončení záznamu (`rec_autostop_s`),
     *        přehrávaný blok, Modified (`modified`), páska jako
     *        obraz z paměti (`image`: Modified nebo bez cesty), rychlosti
     *        a pulzy bloků pásky.
     *
     * Starší záznamy `modified`, `image` a `rec_autostop_s` nemají:
     * přehrání je bere jako false (páska ze souboru, ne Modified) a 0
     * (bez automatického ukončení, jako tehdy). Záznam bez `direct_save`
     * vznikl bez Direct Save a bere se jako vypnuto; se zapnutým hackem se
     * přehrání odmítne, liší-li se od aktuálního nastavení (jako `hack`).
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
