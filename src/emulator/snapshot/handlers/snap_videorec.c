/**
 * @file snap_videorec.c
 * @brief Snapshot handler: informace o video záznamu pro retake a šev.
 *
 * Ukládá do snapshotu, ke které session nahrávání a ke kterému snímku
 * nahrávky snapshot patří, a po úspěšném nahrání snapshotu o tom informuje
 * lepidlo video záznamu (videorec_on_snapshot_loaded()), které podle toho
 * udělá retake (zahodí snímky po bodu snapshotu) nebo šev.
 *
 * Formát entry `videorec/state.bin` (32 bajtů, little-endian):
 *
 * | Offset | Typ      | Význam                                               |
 * |--------|----------|------------------------------------------------------|
 * | 0      | char[4]  | magic "VREC"                                         |
 * | 4      | uint32   | verze formátu = 2                                    |
 * | 8      | uint64   | session_id (nenulové ID session nahrávání)           |
 * | 16     | uint64   | frame = počet snímků nahrávky v okamžiku uložení     |
 * | 24     | uint64   | take_id = ID větve časové osy (viz st_VIDEOREC_SNAPINFO); 0 = bod mimo linii |
 *
 * take_id = 0 (VIDEOREC_TAKE_ID_NONE) zapisuje snapshot uložený v době, kdy
 * čekalo zpracování předchozího loadu (videorec_get_snapinfo()): bod nepatří
 * žádné větvi, load vede na šev. Formát ani verze se kvůli tomu nemění
 * (nula v poli take_id je jen hodnota, kterou žádná větev nemá), takže dříve
 * uložené entry verze 2 se čtou beze změny a starší emulátor nulu také
 * vyhodnotí jako šev.
 *
 * Entry se zapisuje jen během nahrávání; snapshot bez ní je "cizí"
 * (při nahrání během nahrávání vede na šev). Komponenta je volitelná.
 *
 * Verze 1 (24 bajtů, bez take_id) vznikala jen ve vývojové větvi před
 * vydáním. Bez ID větve nejde poznat, zda snapshot nepatří opuštěné větvi
 * nahrávky, proto se verze 1 (i jakákoli jiná neznámá verze nebo velikost)
 * čte jako neplatná entry = cizí snapshot -> šev. Bezpečné: v nejhorším
 * vznikne šev tam, kde by šel retake.
 *
 * Pořadí: registruje se s prioritou SNAPSHOT_PRIORITY_DEVICE za komponentou
 * "audio" (snapshot_register_component() řadí stabilně - v rámci stejné
 * priority podle pořadí registrace), takže při loadu je audio log už
 * resetovaný na obnovený čas GDG. Lepidlo se ale informuje až ze
 * snap_videorec_after_load(), po načtení všech komponent.
 *
 * @par Licence: GPLv3
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "videorec/videorec.h"

/** Jméno entry v archivu. */
#define SNAP_VIDEOREC_ENTRY "videorec/state.bin"
/** Velikost entry v bajtech (verze 2). */
#define SNAP_VIDEOREC_SIZE 32
/** Verze formátu entry. */
#define SNAP_VIDEOREC_VERSION 2u

/** Informace přečtená při posledním loadu (platná při s_has_loaded). */
static st_VIDEOREC_SNAPINFO s_loaded;
/** Poslední load obsahoval platnou entry videorec/state.bin. */
static bool s_has_loaded = false;

/** @brief Zapíše uint32 little-endian. @param p Cíl (4 B). @param v Hodnota. */
static void wr32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/** @brief Zapíše uint64 little-endian. @param p Cíl (8 B). @param v Hodnota. */
static void wr64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/** @brief Přečte uint32 little-endian. @param p Zdroj (4 B). @return Hodnota. */
static uint32_t rd32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/** @brief Přečte uint64 little-endian. @param p Zdroj (8 B). @return Hodnota. */
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/**
 * @brief Save callback: během nahrávání zapíše videorec/state.bin, jinak nic.
 *
 * Snímek se bere z videorec_get_snapinfo(): počet snímků nahrávky včetně
 * snímků, které ještě čekají na zvuk (ve snapshotu jsou už minulostí).
 *
 * @param ctx Kontext ukládání.
 * @return SNAPSHOT_OK, nebo chyba zápisu entry.
 * @par Vlákna Volající vlákno ukládání snapshotu (emulace v pauze nebo v safe-pointu);
 *      bere zámek lepidla videorec.
 */
static en_SNAPSHOT_RESULT snap_videorec_save(st_SNAPSHOT_CONTEXT *ctx)
{
    st_VIDEOREC_SNAPINFO info;
    if (!videorec_get_snapinfo(&info)) return SNAPSHOT_OK;

    uint8_t buf[SNAP_VIDEOREC_SIZE];
    memcpy(buf, "VREC", 4);
    wr32(buf + 4, SNAP_VIDEOREC_VERSION);
    wr64(buf + 8, info.session_id);
    wr64(buf + 16, info.frame);
    wr64(buf + 24, info.take_id);
    return snapshot_io_write_bin(ctx->io, SNAP_VIDEOREC_ENTRY, buf, sizeof(buf));
}

/**
 * @brief Load callback: jen přečte videorec/state.bin (pokud existuje), stav nemění.
 *
 * Neplatná entry (chyba čtení, jiná velikost než SNAP_VIDEOREC_SIZE, špatná
 * magic nebo verze - včetně nevydané verze 1) se ohlásí na stderr a snapshot
 * se pak chová jako cizí (šev).
 *
 * @param ctx Kontext nahrávání.
 * @return Vždy SNAPSHOT_OK (komponenta nesmí shodit nahrání snapshotu).
 * @post s_has_loaded == true právě když entry existuje a je platná.
 */
static en_SNAPSHOT_RESULT snap_videorec_load(st_SNAPSHOT_CONTEXT *ctx)
{
    s_has_loaded = false;
    memset(&s_loaded, 0, sizeof(s_loaded));
    if (!snapshot_io_entry_exists(ctx->io, SNAP_VIDEOREC_ENTRY)) return SNAPSHOT_OK;

    /* Čte se s libovolnou velikostí (snapshot_io_read_bin_into by jinou velikost
     * ohlásil jako poškozený archiv), velikost a verze se ověří zde. */
    uint8_t *buf = NULL;
    size_t size = 0;
    if (snapshot_io_read_bin(ctx->io, SNAP_VIDEOREC_ENTRY, &buf, &size) != SNAPSHOT_OK || size != SNAP_VIDEOREC_SIZE ||
        memcmp(buf, "VREC", 4) != 0 || rd32(buf + 4) != SNAP_VIDEOREC_VERSION) {
        fprintf(stderr, "Snapshot: ignoring invalid or unsupported %s (recording seam instead of retake)\n",
                SNAP_VIDEOREC_ENTRY);
        g_free(buf);
        return SNAPSHOT_OK;
    }
    s_loaded.session_id = rd64(buf + 8);
    s_loaded.frame = rd64(buf + 16);
    s_loaded.take_id = rd64(buf + 24);
    g_free(buf);
    s_has_loaded = true;
    return SNAPSHOT_OK;
}

void snap_videorec_register(void)
{
    snapshot_register_component("videorec", SNAPSHOT_PRIORITY_DEVICE, snap_videorec_save, snap_videorec_load, true);
}

void snap_videorec_after_load(void)
{
    videorec_on_snapshot_loaded(s_has_loaded ? &s_loaded : NULL);
    s_has_loaded = false;
}
