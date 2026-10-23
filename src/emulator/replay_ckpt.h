/**
 * @file   replay_ckpt.h
 * @brief  Kruhový buffer checkpointů záznamu a přehrání vstupů (rewind, seek).
 *
 * Checkpoint = snapshot stroje v bodě hranice snímku (snapshot_save_to_buffer())
 * + stav záznamu nebo přehrávání v tom bodě (délky seznamů, kurzory, poslední
 * vstupy) + počítadlo zápisů na média (media_writes.h). Obsah médií checkpoint
 * nenese: použít ho jde jen tehdy, když se od něj na médium nic nezapsalo
 * (replay_ckpt_ring_find()).
 *
 * Modul jen spravuje data (bez stavu emulátoru); používá ho replay.c v emu
 * vlákně.
 */

#ifndef REPLAY_CKPT_H
#define REPLAY_CKPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <glib.h>

#include "replay_format.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief Výchozí interval checkpointů ve snímcích (5 s při 50 Hz). */
#define REPLAY_CKPT_DEFAULT_INTERVAL 250

    /** @brief Výchozí počet uchovaných checkpointů (5 min při výchozím intervalu). */
#define REPLAY_CKPT_DEFAULT_COUNT 60

    /** @brief Největší počet uchovaných checkpointů. */
#define REPLAY_CKPT_MAX_COUNT 1000

    /**
     * @brief Jeden checkpoint.
     *
     * @invariant `snap` != NULL a má `snap_size` bajtů (vlastní ho buffer).
     */
    typedef struct st_REPLAY_CKPT
    {
        uint32_t frame;          /**< Snímek od startu (bod B_f, po jeho krocích). */
        uint64_t ticks;          /**< gdg_get_total_ticks() v bodě checkpointu. */
        uint8_t *snap;           /**< Snapshot (.mzs v paměti, g_malloc). */
        size_t snap_size;        /**< Velikost snapshotu v bajtech. */
        uint32_t media_writes;   /**< g_media_writes v bodě checkpointu. */
        int tape_block;          /**< Aktuální blok vložené pásky, -1 = bez pásky (snapshot blok nenese). */

        /* Stav záznamu */
        guint events_len;        /**< Počet událostí záznamu. */
        guint hashes_len;        /**< Počet řádků hashů záznamu. */
        guint media_len;         /**< Počet médií záznamu (soubory CMT hacku přibývají). */
        int cmthack_count;       /**< Počet výběrů CMT hacku. */
        uint8_t last_kbd[REPLAY_KBD_COLS];  /**< Poslední zapsaná matice klávesnice. */
        uint8_t last_joy[REPLAY_JOY_PORTS]; /**< Poslední zapsaný stav joysticků. */

        /* Stav přehrávání */
        guint ev_cursor;         /**< Další událost k přehrání. */
        guint hash_cursor;       /**< Další řádek hashů k ověření. */
        guint cmthack_cursor;    /**< Další událost `cmthack`. */
        int record_count;        /**< Počet nahrávání na kazetu (jméno dočasného souboru). */
        uint8_t play_kbd[REPLAY_KBD_COLS];  /**< Aktuální matice přehrávání. */
        uint8_t play_joy[REPLAY_JOY_PORTS]; /**< Aktuální stav joysticků přehrávání. */
        bool deviated;           /**< Přehrávání bylo odchýlené. */
    } st_REPLAY_CKPT;

    /**
     * @brief Kruhový buffer checkpointů seřazených podle snímku (nejstarší první).
     *
     * @invariant `count` <= `capacity`; položky `0 .. count-1` (od nejstaršího)
     *            mají rostoucí `frame`; `bytes` = součet `snap_size`.
     */
    typedef struct st_REPLAY_CKPT_RING
    {
        st_REPLAY_CKPT *items;   /**< Pole `capacity` položek (g_new0). */
        guint capacity;          /**< Největší počet položek (0 = neinicializováno). */
        guint head;              /**< Index nejstarší položky v `items`. */
        guint count;             /**< Počet platných položek. */
        size_t bytes;            /**< Součet velikostí snapshotů. */
    } st_REPLAY_CKPT_RING;

    /**
     * @brief Výsledek hledání checkpointu.
     */
    typedef enum en_REPLAY_CKPT_FIND
    {
        REPLAY_CKPT_FOUND = 0,     /**< Nalezen použitelný checkpoint. */
        REPLAY_CKPT_NONE,          /**< Žádný checkpoint nemá snímek <= požadovaný. */
        REPLAY_CKPT_MEDIA_WRITTEN  /**< Nejbližší checkpoint (a tím i všechny starší) je za zápisem na médium. */
    } en_REPLAY_CKPT_FIND;

    /**
     * @brief Inicializace prázdného bufferu.
     * @param ring     Buffer (obsah se přepíše, předchozí data se neuvolní).
     * @param capacity Počet položek, ořízne se na 1 .. REPLAY_CKPT_MAX_COUNT.
     */
    void replay_ckpt_ring_init(st_REPLAY_CKPT_RING *ring, guint capacity);

    /**
     * @brief Uvolní všechny checkpointy a pole; buffer je pak neinicializovaný.
     * @param ring Buffer (neinicializovaný = nic).
     */
    void replay_ckpt_ring_free(st_REPLAY_CKPT_RING *ring);

    /**
     * @brief Odebere všechny checkpointy (kapacita zůstává).
     */
    void replay_ckpt_ring_clear(st_REPLAY_CKPT_RING *ring);

    /**
     * @brief Přidá checkpoint na konec; při plném bufferu uvolní nejstarší.
     *
     * @param ring Inicializovaný buffer.
     * @param c    Checkpoint; buffer převezme `c->snap` (volající ho neuvolňuje).
     * @pre `c->frame` je větší než snímek posledního checkpointu (jinak se
     *      checkpoint téhož nebo staršího snímku nejdřív odebere, aby pořadí
     *      zůstalo rostoucí).
     */
    void replay_ckpt_ring_push(st_REPLAY_CKPT_RING *ring, const st_REPLAY_CKPT *c);

    /**
     * @brief Checkpoint podle pořadí.
     * @param i 0 = nejstarší, `count - 1` = nejnovější.
     * @return Položka (vlastní ji buffer), NULL mimo rozsah.
     */
    const st_REPLAY_CKPT *replay_ckpt_ring_get(const st_REPLAY_CKPT_RING *ring, guint i);

    /**
     * @brief Najde nejnovější checkpoint se snímkem <= @p max_frame.
     *
     * @param ring         Buffer.
     * @param max_frame    Nejvyšší přípustný snímek.
     * @param media_writes Aktuální g_media_writes.
     * @param[out] out_index Pořadí nalezeného (nebo nejbližšího zablokovaného) checkpointu; smí být NULL.
     * @return REPLAY_CKPT_FOUND; REPLAY_CKPT_NONE, nebo REPLAY_CKPT_MEDIA_WRITTEN,
     *         když se od nejbližšího takového checkpointu zapsalo na médium.
     *         Počítadlo jen roste, takže starší checkpoint pak také nejde použít.
     */
    en_REPLAY_CKPT_FIND replay_ckpt_ring_find(const st_REPLAY_CKPT_RING *ring, uint32_t max_frame,
                                              uint32_t media_writes, guint *out_index);

    /**
     * @brief Vloží checkpoint podle snímku a novější checkpointy ponechá.
     *
     * Na rozdíl od replay_ckpt_ring_push() (záznam, rewind) nic novějšího
     * nemaže: použití při přehrávání bez odchýlení (deterministické), kdy
     * vzniká checkpoint snímku mezi uloženými (goto s checkpointem v cíli).
     * Checkpoint téhož snímku nahradí. Při plném bufferu uvolní nejstarší;
     * je-li nový starší než všechny uložené v plném bufferu, zahodí nový.
     *
     * @param ring Inicializovaný buffer.
     * @param c    Checkpoint; buffer převezme `c->snap` (i při zahození ho uvolní).
     * @return true = vložen, false = zahozen (plný buffer, nejstarší snímek).
     */
    bool replay_ckpt_ring_insert(st_REPLAY_CKPT_RING *ring, const st_REPLAY_CKPT *c);

    /**
     * @brief Je v bufferu checkpoint snímku @p frame?
     *
     * @param ring  Buffer.
     * @param frame Snímek.
     * @return true, když některý checkpoint má přesně snímek @p frame.
     */
    bool replay_ckpt_ring_has_frame(const st_REPLAY_CKPT_RING *ring, uint32_t frame);

    /**
     * @brief Uvolní checkpointy novější než pořadí @p i (po rewindu nebo seeku zpět).
     */
    void replay_ckpt_ring_truncate_after(st_REPLAY_CKPT_RING *ring, guint i);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_CKPT_H */
