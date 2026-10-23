/**
 * @file   machine_hash.c
 * @brief  Obsahový otisk celého stroje po částech - viz machine_hash.h.
 *
 * Části, které nejsou blokem paměti, se hashují jako posloupnost
 * záznamů: celá čísla little endian v pevné šířce, řetězce s délkou
 * (uint32_t) před obsahem, takže dva různé stavy nedají stejný proud bajtů.
 */

#include <string.h>
#include <glib.h>

#include "machine_hash.h"
#include "state_hash.h"
#include "replay.h"
#include "replay_media.h"
#include "media_writes.h"
#include "mzarch/mzarch_config.h"
#include "mzarch/mzarch.h"
#include "snapshot/snapshot.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/ide8/ide8.h"
#include "hw-generic/unicard/unicard.h"
#include "hw-generic/ramdisk/ramdisk.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_container.h"
#if HAVE_JOY
#include "hw-generic/joy/joy.h"
#endif


/** @brief Velikost banky RAM disku [B] (stejná jako DEF_BANK_SIZE v ramdisk.c). */
#define MACHINE_HASH_RAMDISK_BANK 0x10000

/** @brief Počet bank RAM disku Pezik (ramdisk.c: 8 * DEF_BANK_SIZE). */
#define MACHINE_HASH_PEZIK_BANKS 8

/** @brief Nejvýš zachycený zbytek textu autotype [B] (delší se ořízne). */
#define MACHINE_HASH_AUTOTYPE_MAX 4096


/* ---- záznamy do hashe ---- */

/** @brief Přidá celé číslo jako 4 bajty little endian. */
static void mh_u32(GChecksum *cs, uint32_t v)
{
    guchar b[4] = { (guchar)v, (guchar)(v >> 8), (guchar)(v >> 16), (guchar)(v >> 24) };
    g_checksum_update(cs, b, sizeof(b));
}


/** @brief Přidá celé číslo jako 8 bajtů little endian. */
static void mh_u64(GChecksum *cs, uint64_t v)
{
    mh_u32(cs, (uint32_t)v);
    mh_u32(cs, (uint32_t)(v >> 32));
}


/** @brief Přidá řetězec s délkou (NULL = prázdný řetězec). */
static void mh_str(GChecksum *cs, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    mh_u32(cs, (uint32_t)len);
    if (len) g_checksum_update(cs, (const guchar *)s, (gssize)len);
}


/** @brief Přidá délku a hex SHA-256 obsahu bloku (obsah médií; NULL = prázdný). */
static void mh_content(GChecksum *cs, const void *data, size_t len)
{
    char hex[STATE_HASH_HEX_LEN + 1];
    state_hash_buffer(data, data ? len : 0, hex);
    mh_u64(cs, (uint64_t)(data ? len : 0));
    mh_str(cs, hex);
}


/** @brief Ukončí hash a zapíše hex do @p out_hex. */
static void mh_finish(GChecksum *cs, char *out_hex)
{
    g_strlcpy(out_hex, g_checksum_get_string(cs), STATE_HASH_HEX_LEN + 1);
    g_checksum_free(cs);
}


/* ---- jednotlivé části ---- */

/** @brief Část `memmap`: mapa paměti, SW1, druhý PSG, konfigurace a mapa MemExt. */
static void mh_memmap(GChecksum *cs)
{
    mh_u32(cs, g_memory.map);
#if MZARCH != 700
    mh_u32(cs, (uint32_t)g_mzarch_main.mode_sw);
#endif
#if MZARCH == 800
    mh_u32(cs, (uint32_t)g_mzarch_main.mz800_hwcompat_allow_psg1);
#endif
    mh_u32(cs, (uint32_t)g_memext.connection);
    mh_u32(cs, (uint32_t)g_memext.type);
    mh_u32(cs, (uint32_t)g_memext.init_mem);
    mh_u32(cs, (uint32_t)g_memext.init_luftner);
    mh_u32(cs, g_memext.addr_mask);
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++) mh_u32(cs, g_memext.map[i]);
}


#if CFG_HWEXT_HAVE_IDE8
/** @brief Callback ide8_drive_read_image(): přidá úsek do SHA-256. */
static void mh_ide8_chunk(const uint8_t *data, size_t len, void *user)
{
    g_checksum_update((GChecksum *)user, data, (gssize)len);
}
#endif


/**
 * @brief Část `media`: identita vložených médií, počítadlo zápisů,
 *        volitelně obsah.
 */
static void mh_media(GChecksum *cs, bool content)
{
#if CFG_HWEXT_HAVE_FDC
    for (int f = 0; f < FDC_INSTANCE_COUNT; f++)
    {
        mh_u32(cs, (uint32_t)g_fdc[f].connected);
        for (int n = 0; n < FDC_NUM_DRIVES; n++)
        {
            const st_FDDrive *d = &g_fdc[f].drive[n];
            mh_u32(cs, d->mounted);
            mh_u32(cs, (uint32_t)d->readonly);
            mh_str(cs, d->mounted ? d->filename : "");
            if (content && d->mounted)
            {
                size_t size = 0;
                uint8_t *img = fdc_drive_copy_image(&g_fdc[f], (unsigned)n, &size);
                mh_content(cs, img, size);
                g_free(img);
            };
        };
    };
#endif

#if CFG_HWEXT_HAVE_QDISK
    mh_u32(cs, g_qdisk.connected);
    mh_u32(cs, g_qdisk.type);
    mh_u32(cs, (uint32_t)g_qdisk.readonly);
    mh_str(cs, g_qdisk.filename);
    if (content && QDISK_TEST_CONNECTED && !QDISK_TEST_TYPE_VIRTUAL)
    {
        size_t size = 0;
        uint8_t *img = qdisk_copy_image(&size);
        mh_content(cs, img, size);
        g_free(img);
    };
#endif

#if CFG_HWEXT_HAVE_IDE8
    for (int n = 0; n < IDE8_DRIVES_COUNT; n++)
    {
        mh_u32(cs, (uint32_t)g_ide8.drive[n].connected);
        mh_str(cs, g_ide8.drive[n].filepath);
        if (content && ide8_drive_image_size((en_IDE8_DRIVE)n) > 0)
        {
            GChecksum *img = g_checksum_new(G_CHECKSUM_SHA256);
            int rc = ide8_drive_read_image((en_IDE8_DRIVE)n, mh_ide8_chunk, img);
            mh_u32(cs, (uint32_t)rc);
            mh_str(cs, g_checksum_get_string(img));
            g_checksum_free(img);
        };
    };
#endif

    mh_u32(cs, (uint32_t)g_unicard.connected);
    mh_u32(cs, (uint32_t)g_unicard.readonly);
    mh_str(cs, g_unicard.work_dir);

    /* Páska: identita souboru (obsah volitelně ze souboru hostitele). */
    st_CMTEXT_CONTAINER *c = CMT_TEST_FILLED ? cmtext_get_container(g_cmt.ext) : NULL;
    const char *tape = c ? cmtext_container_get_filepath(c) : NULL;
    mh_u32(cs, CMT_TEST_FILLED ? 1u : 0u);
    mh_str(cs, tape);
    if (content && tape)
    {
        gchar *data = NULL;
        gsize len = 0;
        bool ok = g_file_get_contents(tape, &data, &len, NULL);
        mh_u32(cs, ok ? 1u : 0u);
        if (ok) mh_content(cs, data, len);
        g_free(data);
    };

    /* FLASH MemExt: Z80 ji jen čte (zápis jde do WOM), mění ji načtení souboru. */
    mh_str(cs, g_memext.flash_filepath);
    if (content && MEMEXT_TEST_CONNECTED) mh_content(cs, g_memext.FLASH, sizeof(g_memext.FLASH));

    /* Soubory RAM disků (zálohy, obrazy). */
#if CFG_HWEXT_HAVE_RAMDISK
    mh_str(cs, g_ramdisk.std.filepath);
    for (int i = 0; i < 2; i++) mh_str(cs, g_ramdisk.pezik[i].filepath);
#endif

    mh_u32(cs, g_media_writes);
}


/** @brief Část `inputs`: výsledná matice klávesnice, joysticky, autotype. */
static void mh_inputs(GChecksum *cs)
{
    for (int i = 0; i < (int)sizeof(g_pio8255.keyboard_matrix); i++)
    {
        mh_u32(cs, (uint32_t)(g_pio8255.keyboard_matrix[i] & g_pio8255.vkbd_matrix[i]));
    };
#if HAVE_JOY
    for (int d = 0; d < JOY_DEVID_COUNT; d++)
    {
        mh_u32(cs, (uint32_t)g_joy.dev[d].type);
        mh_u32(cs, g_joy.dev[d].state);
    };
#endif
    char *rest = g_malloc(MACHINE_HASH_AUTOTYPE_MAX);
    bool running = replay_autotype_peek_rest(rest, MACHINE_HASH_AUTOTYPE_MAX);
    mh_u32(cs, running ? 1u : 0u);
    mh_str(cs, rest);
    g_free(rest);
}


/** @brief Část `ramdisk`: stav RAM disků, volitelně obsah. */
static void mh_ramdisk(GChecksum *cs, bool content)
{
#if CFG_HWEXT_HAVE_RAMDISK
    const st_RAMDISKSTD *s = &g_ramdisk.std;
    mh_u32(cs, s->connected);
    mh_u32(cs, (uint32_t)s->type);
    mh_u32(cs, (uint32_t)s->size);
    mh_u32(cs, s->offset);
    mh_u32(cs, s->bank);
    if (content && s->connected && s->memory)
    {
        mh_content(cs, s->memory, ((size_t)s->size + 1) * MACHINE_HASH_RAMDISK_BANK);
    };
    for (int i = 0; i < 2; i++)
    {
        const st_RAMDISKPEZIK *p = &g_ramdisk.pezik[i];
        mh_u32(cs, p->connected);
        mh_u32(cs, p->latch);
        mh_u32(cs, p->portmask);
        mh_u32(cs, p->backuped);
        if (content && p->connected && p->memory)
        {
            mh_content(cs, p->memory, (size_t)MACHINE_HASH_PEZIK_BANKS * MACHINE_HASH_RAMDISK_BANK);
        };
    };
#else
    (void)cs;
    (void)content;
#endif
}


/** @brief Část `cmt`: transport, poloha, nastavení a stav pásky (replay_media_cmt_state_json()). */
static void mh_cmt(GChecksum *cs)
{
    mh_u32(cs, CMT_TEST_FILLED ? 1u : 0u);
    mh_u32(cs, (uint32_t)g_cmt.state);
    mh_u32(cs, (uint32_t)g_cmt.paused);
    mh_u32(cs, (uint32_t)g_cmt.playsts);
    mh_u32(cs, (uint32_t)g_cmt.output);
    mh_u64(cs, g_cmt.start_time);
    mh_u64(cs, g_cmt.paused_time);
    mh_u32(cs, (uint32_t)g_cmt.recording_to_stream);
    mh_u64(cs, g_cmt.recording_last_event);
    char *json = replay_media_cmt_state_json();
    mh_str(cs, json);
    g_free(json);
}


bool machine_hash_part(en_MACHINE_HASH_PART part, unsigned flags, char *out_hex)
{
    const bool content = (flags & MACHINE_HASH_FLAG_INCLUDE_MEDIA) != 0;
    switch (part)
    {
        case MACHINE_HASH_CPU: return state_hash_part(STATE_HASH_CPU, out_hex);
        case MACHINE_HASH_RAM: return state_hash_part(STATE_HASH_RAM, out_hex);
        case MACHINE_HASH_MEMEXT: return state_hash_part(STATE_HASH_MEMEXT, out_hex);
        case MACHINE_HASH_VRAM: return state_hash_part(STATE_HASH_VRAM, out_hex);
        case MACHINE_HASH_PCG: return state_hash_part(STATE_HASH_PCG, out_hex);

        case MACHINE_HASH_PERIPH:
        {
            size_t count = 0;
            const char *const *names = replay_periph_components(&count);
            char hex[STATE_HASH_HEX_LEN + 1];
            if (snapshot_hash_components(names, count, hex, sizeof(hex)) != SNAPSHOT_OK) return false;
            g_strlcpy(out_hex, hex, STATE_HASH_HEX_LEN + 1);
            return true;
        };

        case MACHINE_HASH_ROM:
            state_hash_buffer(g_memory.ROM, sizeof(g_memory.ROM), out_hex);
            return true;

        case MACHINE_HASH_MEMMAP:
        case MACHINE_HASH_MEDIA:
        case MACHINE_HASH_INPUTS:
        case MACHINE_HASH_RAMDISK:
        case MACHINE_HASH_CMT:
        {
            GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
            if (part == MACHINE_HASH_MEMMAP) mh_memmap(cs);
            else if (part == MACHINE_HASH_MEDIA) mh_media(cs, content);
            else if (part == MACHINE_HASH_INPUTS) mh_inputs(cs);
            else if (part == MACHINE_HASH_RAMDISK) mh_ramdisk(cs, content);
            else mh_cmt(cs);
            mh_finish(cs, out_hex);
            return true;
        };

        default:
            break;
    };
    return false;
}


void machine_hash_compute(uint32_t mask, unsigned flags, st_MACHINE_HASH *out)
{
    memset(out, 0, sizeof(*out));
    out->requested = mask & MACHINE_HASH_ALL_PARTS;
    GChecksum *total = g_checksum_new(G_CHECKSUM_SHA256);
    for (int p = 0; p < MACHINE_HASH_PART_COUNT; p++)
    {
        if (!(out->requested & (1u << p))) continue;
        bool ok = machine_hash_part((en_MACHINE_HASH_PART)p, flags, out->part[p]);
        if (ok) out->present |= 1u << p;
        else out->part[p][0] = '\0';
        char *line = g_strdup_printf("%s:%s\n", machine_hash_part_name((en_MACHINE_HASH_PART)p),
                                     ok ? out->part[p] : "-");
        g_checksum_update(total, (const guchar *)line, (gssize)strlen(line));
        g_free(line);
    };
    mh_finish(total, out->total);
}
