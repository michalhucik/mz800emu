/**
 * @file   replay_media.c
 * @brief  Média v záznamu vstupů (viz replay_media.h).
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "replay_media.h"
#include "mzarch/mzarch_config.h"
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/ide8/ide8.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_block.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "device_request.h"
#include "recent_media/recent_media.h"
#include <json-glib/json-glib.h>


/** @brief Délka jména slotu včetně nuly (`fdc1.3`, `ide8.1`, `qd`). */
#define REPLAY_SLOT_MAX 16


/* ---- stav přehrání (emu vlákno) ---- */
static bool s_prepared = false; /**< replay_media_prepare() změnil média, restore je čeká. */
static char *s_tmpdir = NULL;   /**< Dočasný adresář kopií IDE8 (g_dir_make_tmp), NULL = nevytvořen. */

/** @brief Původní stav mechaniky FDC před přehráním. */
typedef struct st_REPLAY_MEDIA_FDC_ORIG
{
    bool changed;   /**< Přehrání mechaniku změnilo. */
    bool mounted;   /**< Před přehráním byl nasazený obraz. */
    char *filename; /**< Jeho cesta (g_free). */
} st_REPLAY_MEDIA_FDC_ORIG;

static st_REPLAY_MEDIA_FDC_ORIG s_orig_fdc[FDC_INSTANCE_COUNT][FDC_NUM_DRIVES];
static bool s_qd_changed = false;                   /**< Přehrání změnilo Quick Disk. */
static bool s_ide8_changed[IDE8_DRIVES_COUNT];      /**< Přehrání změnilo disk IDE8. */
static bool s_ide8_connected[IDE8_DRIVES_COUNT];    /**< Disk byl před přehráním připojený. */

/**
 * @brief Režim a rychlost jednoho bloku uživatelovy pásky v indexu (uschované po dobu přehrání).
 *
 * Obraz pásky ani soubor je nenesou celé (blok s rychlostí změněnou
 * v indexu se v souboru přestaví až při otevření nebo uložení, blok
 * DEFAULT se z obrazu vrátí jako FIXED), proto se uschovají zvlášť.
 */
typedef struct st_REPLAY_MEDIA_CMT_BLOCK
{
    en_CMTEXT_BLOCK_TYPE bltype;    /**< typ bloku (kontrola, že se vrací táž páska) */
    en_CMTEXT_BLOCK_SPEED blspeed;  /**< režim rychlosti bloku */
    en_CMTSPEED speed;              /**< rychlost bloku */
    bool has_pulses;                /**< pulses platí (blok MZF s vlastními pulzy) */
    st_MZTAPE_PULSES_LENGTH pulses; /**< vlastní pulzy bloku */
} st_REPLAY_MEDIA_CMT_BLOCK;

/** @brief Původní stav kazety před přehráním (nastavení uživatele a vložená páska). */
typedef struct st_REPLAY_MEDIA_CMT_ORIG
{
    bool changed;                    /**< Přehrání změnilo kazetu. */
    en_CMTSPEED speed;               /**< Výchozí rychlost. */
    st_MZTAPE_PULSES_LENGTH pulses;  /**< Výchozí vlastní pulzy. */
    en_CMT_STREAM_POLARITY polarity; /**< Polarita. */
    unsigned rec_autostop_s;         /**< Automatické ukončení záznamu (INI [CMT] rec_autostop_s). */
    char *tape;                      /**< Vložená páska (cesta, g_free), NULL = žádná. */
    uint8_t *image;                  /**< Obraz pásky Modified nebo bez cesty (g_free), NULL = páska ze souboru. */
    size_t image_size;               /**< Délka obrazu. */
    uint8_t *wav_blocks;             /**< Příznaky původu WAV bloků obrazu (cmttmz_get_wav_blocks(), g_free), NULL = žádný. */
    uint32_t wav_count;              /**< Počet položek wav_blocks. */
    uint8_t *unsaved;                /**< Příznaky neuloženo bloků obrazu (cmttmz_get_unsaved_blocks(), g_free), NULL = žádný blok neuložený. */
    uint32_t unsaved_count;          /**< Počet položek unsaved. */
    st_REPLAY_MEDIA_CMT_BLOCK *blocks; /**< Režim a rychlost bloků z indexu (g_free), NULL = bez pásky nebo bez indexu. */
    uint32_t block_count;            /**< Počet položek blocks (= počet bloků pásky). */
    int modified;                    /**< Modified pásky z obrazu. */
} st_REPLAY_MEDIA_CMT_ORIG;

static void replay_media_cmt_blocks_take(st_CMTEXT_CONTAINER *c);
static bool replay_media_cmt_blocks_restore(st_CMTEXT_CONTAINER *c);

static st_REPLAY_MEDIA_CMT_ORIG s_orig_cmt;

/** @brief Velikost textu varování návratu pásky (replay_media_get_cmt_warning(), včetně nuly). */
#define REPLAY_MEDIA_CMT_WARNING_SIZE 160

/** @brief Poslední varování návratu pásky po přehrání (anglicky, "" = žádné); zapisuje emu vlákno pod device_state_lock(). */
static char s_cmt_warning[REPLAY_MEDIA_CMT_WARNING_SIZE];
/** @brief Čítač varování návratu pásky (zvedne se s každým novým varováním). */
static unsigned s_cmt_warning_serial = 0;


/**
 * @brief Zapíše varování návratu pásky uživatele po přehrání (okno ho ohlásí jednou).
 * @param text Věta (anglicky).
 * @pre Vlákno emulátoru pod device_state_lock().
 * @post s_cmt_warning = @p text, s_cmt_warning_serial zvednutý; řádek na stderr.
 */
static void replay_media_cmt_warn(const char *text)
{
    g_strlcpy(s_cmt_warning, text, sizeof(s_cmt_warning));
    s_cmt_warning_serial++;
    fprintf(stderr, "[REPLAY] %s\n", text);
}


const char *replay_media_get_cmt_warning(unsigned *serial)
{
    if (serial) *serial = s_cmt_warning_serial;
    return s_cmt_warning;
}

/**
 * @brief Páska vložená přehráním: cesta kopie v dočasném adresáři a cesta původního souboru při záznamu.
 *
 * Obě g_free; NULL = přehrání pásku nevložilo. Nastaví
 * replay_media_prepare_cmt_unlocked(), vynuluje
 * replay_media_restore_cmt_unlocked(). Čte replay_media_cmt_snapshot_path().
 */
static char *s_cmt_copy_path = NULL;
static char *s_cmt_copy_name = NULL; /**< viz s_cmt_copy_path */

/**
 * @brief Páry cest (dočasná, původní) pásek uložených nahráváním při přehrání.
 *
 * Sudé položky dočasné cesty, liché původní cesty ze záznamu. Plní
 * replay_media_cmt_map_path(), čte replay_media_cmt_snapshot_path(),
 * vyprázdní replay_media_restore_cmt_unlocked(). NULL = prázdné.
 */
static GPtrArray *s_cmt_maps = NULL;

/** @brief Jméno slotu mechaniky FDC. */
static void replay_media_fdc_slot(char *out, int fdc, int drive)
{
    g_snprintf(out, REPLAY_SLOT_MAX, "fdc%d.%d", fdc, drive);
}


/** @brief Jméno slotu disku IDE8. */
static void replay_media_ide8_slot(char *out, int drive)
{
    g_snprintf(out, REPLAY_SLOT_MAX, "ide8.%d", drive);
}


/**
 * @brief Přidá obraz do záznamu: do limitu vložený, nad ním jen hash.
 *
 * @param budget Zbývající limit vložených dat; sníží se o vložené médium.
 * @post Data zůstávají volajícímu.
 */
static void replay_media_add(st_REPLAY_DATA *d, const char *slot, const char *name, const uint8_t *data, size_t size,
                             bool readonly, uint64_t *budget)
{
    if ((uint64_t)size <= *budget)
    {
        replay_data_add_media(d, slot, name, data, size, NULL, readonly);
        *budget -= size;
        return;
    };
    char sha[REPLAY_HASH_HEX_LEN + 1];
    replay_sha256_hex(data, size, sha);
    replay_data_add_media(d, slot, name, NULL, size, sha, readonly);
    char *w = g_strdup_printf("image in slot %s (%s) exceeds the replay size limit and is not stored; "
                              "playback needs the same image mounted", slot, name ? name : "?");
    replay_data_add_warning(d, w);
    g_free(w);
}


/** @brief Callback ide8_drive_read_image(): přidá úsek do GByteArray. */
static void replay_media_chunk_to_array(const uint8_t *data, size_t len, void *user)
{
    g_byte_array_append((GByteArray *)user, data, (guint)len);
}


/** @brief Callback ide8_drive_read_image(): přidá úsek do SHA-256. */
static void replay_media_chunk_to_hash(const uint8_t *data, size_t len, void *user)
{
    g_checksum_update((GChecksum *)user, data, (gssize)len);
}


/** @brief Kontext kopie IDE8 do souboru s výpočtem hashe. */
typedef struct st_REPLAY_MEDIA_COPY
{
    FILE *f;        /**< Cílový soubor. */
    GChecksum *sha; /**< Hash obsahu. */
    bool ok;        /**< Zápis bez chyby. */
} st_REPLAY_MEDIA_COPY;


/** @brief Callback ide8_drive_read_image(): zapíše úsek do souboru a hashe. */
static void replay_media_chunk_to_file(const uint8_t *data, size_t len, void *user)
{
    st_REPLAY_MEDIA_COPY *c = (st_REPLAY_MEDIA_COPY *)user;
    g_checksum_update(c->sha, data, (gssize)len);
    if (c->ok && fwrite(data, 1, len, c->f) != len) c->ok = false;
}


/** @brief Účinný limit vložených médií záznamu (REPLAY_MEDIA_EMBED_LIMIT, v testech menší). */
static uint64_t s_embed_limit = REPLAY_MEDIA_EMBED_LIMIT;


#ifdef MZTEST_HEADLESS
void replay_media_test_set_embed_limit(uint64_t limit)
{
    s_embed_limit = limit ? limit : REPLAY_MEDIA_EMBED_LIMIT;
}
#endif


/**
 * @brief Odečte médium od zbývajícího limitu jako replay_media_add() (vložené do limitu, nad ním jen hash).
 * @param size Velikost obrazu. @param[in,out] budget Zbývající limit.
 */
static void replay_media_budget_take(size_t size, uint64_t *budget)
{
    if ((uint64_t)size <= *budget) *budget -= size;
}


const char *replay_media_check_cmt_fits(char *errbuf, size_t errsize)
{
    if (!CMT_TEST_FILLED) return NULL;
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    bool image = cmttmz_is_modified() || !c || !cmtext_container_get_filepath(c)[0];
    if (!image) return NULL;

    /* zbývající limit jako v replay_media_capture(): média před kazetou */
    uint64_t budget = s_embed_limit;
#if CFG_HWEXT_HAVE_FDC
    for (int f = 0; f < FDC_INSTANCE_COUNT; f++)
    {
        for (int n = 0; n < FDC_NUM_DRIVES; n++)
        {
            size_t size = 0;
            uint8_t *data = fdc_drive_copy_image(&g_fdc[f], (unsigned)n, &size);
            if (!data) continue;
            replay_media_budget_take(size, &budget);
            g_free(data);
        };
    };
#endif
#if CFG_HWEXT_HAVE_QDISK
    if (!(QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_VIRTUAL))
    {
        size_t size = 0;
        uint8_t *data = qdisk_copy_image(&size);
        if (data)
        {
            replay_media_budget_take(size, &budget);
            g_free(data);
        };
    };
#endif
    uint8_t *img = NULL;
    size_t len = 0;
    if (EXIT_SUCCESS != cmttmz_save_image(&img, &len)) return NULL; /* capture přidá varování */
    g_free(img);
    if ((uint64_t)len <= budget) return NULL;
    g_snprintf(errbuf, errsize, "Tape with unsaved changes is too large for an input recording; save the tape first");
    return errbuf;
}


const char *replay_media_capture(st_REPLAY_DATA *d, char *errbuf, size_t errsize)
{
    uint64_t budget = s_embed_limit;
    char slot[REPLAY_SLOT_MAX];
    d->media_known = true;

#if CFG_HWEXT_HAVE_FDC
    for (int f = 0; f < FDC_INSTANCE_COUNT; f++)
    {
        for (int n = 0; n < FDC_NUM_DRIVES; n++)
        {
            size_t size = 0;
            uint8_t *data = fdc_drive_copy_image(&g_fdc[f], (unsigned)n, &size);
            if (!data) continue;
            replay_media_fdc_slot(slot, f, n);
            replay_media_add(d, slot, g_fdc[f].drive[n].filename, data, size, g_fdc[f].drive[n].readonly != 0, &budget);
            g_free(data);
        };
    };
#endif

#if CFG_HWEXT_HAVE_QDISK
    if (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_VIRTUAL)
    {
        replay_data_add_warning(d, "the Quick Disk in virtual (directory) mode is not part of the replay; "
                                   "play it back with the same directory contents");
    }
    else
    {
        size_t size = 0;
        uint8_t *data = qdisk_copy_image(&size);
        if (data)
        {
            replay_media_add(d, "qd", g_qdisk.filename, data, size, g_qdisk.readonly != 0, &budget);
            g_free(data);
        };
    };
#endif

    /* Kazeta: páska (soubor) + stav a nastavení v manifestu. Páska se
     * změnami neuloženými do souboru (Modified) nebo bez cesty se uloží
     * jako obraz z paměti (TMZ, cmttmz_save_image()), jméno je cesta pásky
     * ("" bez cesty); manifest pak nese "image" true. */
    if (CMT_TEST_FILLED && (cmttmz_is_modified() || !cmtext_get_container(g_cmt.ext)
                            || !cmtext_container_get_filepath(cmtext_get_container(g_cmt.ext))[0]))
    {
        st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
        const char *path = c ? cmtext_container_get_filepath(c) : "";
        uint8_t *img = NULL;
        size_t len = 0;
        if (EXIT_SUCCESS == cmttmz_save_image(&img, &len))
        {
            if ((uint64_t)len > budget)
            {
                /* jen hash by nestačil: přehrání bere pásku nad limitem ze
                 * souboru na původní cestě, který neuložené změny nemá */
                g_free(img);
                g_snprintf(errbuf, errsize, "Tape with unsaved changes is too large for an input recording; save the tape first");
                return errbuf;
            };
            replay_media_add(d, "cmt", path, img, len, true, &budget);
        }
        else
        {
            replay_data_add_warning(d, "cannot build the tape image; the tape is not part of the replay");
        };
        g_free(img);
    }
    else if (CMT_TEST_FILLED)
    {
        st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
        const char *path = c ? cmtext_container_get_filepath(c) : NULL;
        gchar *data = NULL;
        gsize len = 0;
        if (path && g_file_get_contents(path, &data, &len, NULL))
        {
            replay_media_add(d, "cmt", path, (const uint8_t *)data, len, true, &budget);
        }
        else
        {
            replay_data_add_warning(d, "cannot read the tape file; the tape is not part of the replay");
        };
        g_free(data);
    };
    g_free(d->cmt_json);
    d->cmt_json = replay_media_cmt_state_json();

#if CFG_HWEXT_HAVE_IDE8
    for (int n = 0; n < IDE8_DRIVES_COUNT; n++)
    {
        uint64_t size = ide8_drive_image_size((en_IDE8_DRIVE)n);
        if (size == 0) continue;
        replay_media_ide8_slot(slot, n);
        const char *name = g_ide8.drive[n].filepath;
        if (size <= budget)
        {
            GByteArray *a = g_byte_array_sized_new((guint)size);
            if (ide8_drive_read_image((en_IDE8_DRIVE)n, replay_media_chunk_to_array, a) == EXIT_SUCCESS)
            {
                replay_data_add_media(d, slot, name, a->data, a->len, NULL, false);
                budget -= a->len;
            }
            else
            {
                replay_data_add_warning(d, "cannot read an IDE8 disk image; it is not part of the replay");
            };
            g_byte_array_unref(a);
            continue;
        };
        GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
        if (ide8_drive_read_image((en_IDE8_DRIVE)n, replay_media_chunk_to_hash, c) == EXIT_SUCCESS)
        {
            replay_data_add_media(d, slot, name, NULL, size, g_checksum_get_string(c), false);
            char *w = g_strdup_printf("image in slot %s (%s) exceeds the replay size limit and is not stored; "
                                      "playback needs the same image mounted", slot, name ? name : "?");
            replay_data_add_warning(d, w);
            g_free(w);
        }
        else
        {
            replay_data_add_warning(d, "cannot read an IDE8 disk image; it is not part of the replay");
        };
        g_checksum_free(c);
    };
#endif
    return NULL;
}


/* ---- kazeta: stav v manifestu (`cmt`) ---- */

/** @brief Zapíše délky pulzů (sekundy) jako pole [long.high, long.low, short.high, short.low]. */
static void replay_media_json_pulses(JsonBuilder *b, const st_MZTAPE_PULSES_LENGTH *p)
{
    json_builder_begin_array(b);
    json_builder_add_double_value(b, p->long_pulse.high);
    json_builder_add_double_value(b, p->long_pulse.low);
    json_builder_add_double_value(b, p->short_pulse.high);
    json_builder_add_double_value(b, p->short_pulse.low);
    json_builder_end_array(b);
}


/** @brief Načte délky pulzů z pole (viz replay_media_json_pulses()). @return true = platné. */
static bool replay_media_pulses_from_json(JsonNode *n, st_MZTAPE_PULSES_LENGTH *p)
{
    if (!n || !JSON_NODE_HOLDS_ARRAY(n)) return false;
    JsonArray *a = json_node_get_array(n);
    if (json_array_get_length(a) != 4) return false;
    double v[4];
    for (guint i = 0; i < 4; i++)
    {
        JsonNode *e = json_array_get_element(a, i);
        if (!JSON_NODE_HOLDS_VALUE(e)) return false;
        v[i] = json_node_get_double(e);
        if (!(v[i] > 0)) return false;
    };
    /* Přesně jako mztape_pulses_set_us(): total = high + low. */
    p->long_pulse.high = v[0];
    p->long_pulse.low = v[1];
    p->long_pulse.total = v[0] + v[1];
    p->short_pulse.high = v[2];
    p->short_pulse.low = v[3];
    p->short_pulse.total = v[2] + v[3];
    return true;
}


/** @brief Celé číslo členu objektu, nebo @p def. */
static gint64 replay_media_json_int(JsonObject *o, const char *name, gint64 def)
{
    JsonNode *n = json_object_get_member(o, name);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return def;
    return json_node_get_int(n);
}


char *replay_media_cmt_state_json(void)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "hack");
    json_builder_add_boolean_value(b, CMTHACK_TEST_IS_INSTALLED ? TRUE : FALSE);
    /* nastavení Direct Save: s hackem rozhoduje (spolu s transportem, který
     * záznam reprodukuje) o patchi zápisu v ROM i o tom, co udělá ROM SAVE */
    json_builder_set_member_name(b, "direct_save");
    json_builder_add_boolean_value(b, g_cmthack.direct_save ? TRUE : FALSE);
    json_builder_set_member_name(b, "speed");
    json_builder_add_int_value(b, (gint64)g_cmt.mz_cmtspeed);
    json_builder_set_member_name(b, "pulses");
    replay_media_json_pulses(b, &g_cmt.mz_custom_pulses);
    /* automatické ukončení záznamu mění průběh nahrávání (STOP), proto
     * patří do záznamu vstupů jako rychlost a polarita */
    json_builder_set_member_name(b, "rec_autostop_s");
    json_builder_add_int_value(b, (gint64)g_cmt.rec_autostop_s);
    json_builder_set_member_name(b, "polarity");
    json_builder_add_int_value(b, (gint64)g_cmt.polarity);
    json_builder_set_member_name(b, "block");
    json_builder_add_int_value(b, (CMT_TEST_FILLED && g_cmt.ext->block) ? cmtext_block_get_block_id(g_cmt.ext->block) : -1);
    /* páska v záznamu je obraz z paměti (replay_media_capture()); Modified
     * patří ke stavu pásky (přehrání ho obnoví, otisk stroje ho vidí) */
    json_builder_set_member_name(b, "modified");
    json_builder_add_boolean_value(b, cmttmz_is_modified() ? TRUE : FALSE);
    /* obraz = páska Modified nebo bez cesty; cesta původní (při přehrání
     * zpět namapovaná z dočasné kopie), jinak by otisk stroje přehrání
     * u pásky bez cesty viděl cestu dočasného souboru */
    json_builder_set_member_name(b, "image");
    st_CMTEXT_CONTAINER *ic = CMT_TEST_FILLED ? cmtext_get_container(g_cmt.ext) : NULL;
    const char *ip = ic ? replay_media_cmt_snapshot_path(cmtext_container_get_filepath(ic)) : "";
    json_builder_add_boolean_value(b, (CMT_TEST_FILLED && (cmttmz_is_modified() || !ip || !ip[0])) ? TRUE : FALSE);


    st_CMTEXT_CONTAINER *c = CMT_TEST_FILLED ? cmtext_get_container(g_cmt.ext) : NULL;
    if (c && cmtext_container_get_type(c) == CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE)
    {
        json_builder_set_member_name(b, "blocks");
        json_builder_begin_array(b);
        int count = cmtext_container_get_count_blocks(c);
        for (int i = 0; i < count; i++)
        {
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "blspeed");
            json_builder_add_int_value(b, (gint64)cmtext_container_get_block_speed(c, i));
            en_CMTSPEED sp = cmtext_container_get_block_cmt_speed(c, i);
            json_builder_set_member_name(b, "speed");
            json_builder_add_int_value(b, (gint64)sp);
            /* blok 0x18 z importu WAV: obraz pásky typ bloku nenese (přepínač
             * polarity na něj působí); jen u takového bloku, starší záznam
             * bez klíče = blok TZX */
            if (cmtext_container_get_block_type(c, i) == CMTEXT_BLOCK_TYPE_WAV)
            {
                json_builder_set_member_name(b, "wav");
                json_builder_add_boolean_value(b, TRUE);
            };
            st_MZTAPE_PULSES_LENGTH p;
            if (cmtext_container_get_block_pulses(c, i, &p) == EXIT_SUCCESS)
            {
                json_builder_set_member_name(b, "pulses");
                replay_media_json_pulses(b, &p);
            };
            json_builder_end_object(b);
        };
        json_builder_end_array(b);
    };
    json_builder_end_object(b);

    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    char *text = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_unref(root);
    g_object_unref(b);
    return text;
}


/**
 * @brief Příznaky původu WAV bloků z pole "blocks" stavu kazety v manifestu.
 *
 * Položka s "wav": true = blok 0x18 z importu WAV (cmttmz_get_wav_blocks()
 * při záznamu). Položka bez klíče (i celý starší záznam) = bez příznaku.
 * Manifest je nedůvěryhodný vstup: příznak mění jen typ bloku 0x18.
 *
 * @param bl Uzel "blocks" (NULL nebo jiný než pole = žádný příznak).
 * @param[out] count Počet položek výsledku.
 * @return Pole příznaků (g_malloc0, uvolní volající g_free()), NULL bez
 *         bloku s příznakem (*count 0).
 */
static uint8_t *replay_media_wav_blocks_from_json(JsonNode *bl, uint32_t *count)
{
    *count = 0;
    if (!bl || !JSON_NODE_HOLDS_ARRAY(bl)) return NULL;
    JsonArray *a = json_node_get_array(bl);
    guint n = json_array_get_length(a);
    uint8_t *flags = NULL;
    for (guint i = 0; i < n; i++)
    {
        JsonNode *e = json_array_get_element(a, i);
        if (!e || !JSON_NODE_HOLDS_OBJECT(e)) continue;
        JsonNode *w = json_object_get_member(json_node_get_object(e), "wav");
        if (!w || !JSON_NODE_HOLDS_VALUE(w) || json_node_get_value_type(w) != G_TYPE_BOOLEAN || !json_node_get_boolean(w)) continue;
        if (!flags) flags = (uint8_t *)g_malloc0(n);
        flags[i] = 1;
    };
    if (flags) *count = (uint32_t)n;
    return flags;
}


/** @brief Nastaví text chyby do @p errbuf a vrátí ho. */
static const char *replay_media_fail(char *errbuf, size_t errsize, const char *fmt, const char *slot)
{
    g_snprintf(errbuf, errsize, fmt, slot);
    return errbuf;
}


char *replay_media_tmp_file(const char *name)
{
    if (!s_tmpdir) s_tmpdir = g_dir_make_tmp("mz800emu-replay-XXXXXX", NULL);
    if (!s_tmpdir || !name) return NULL;
    return g_build_filename(s_tmpdir, name, NULL);
}


/** @brief Cesta dočasného souboru obrazu slotu (g_free). */
static char *replay_media_tmp_path(const char *slot)
{
    char *name = g_strdup_printf("%s.img", slot);
    char *p = replay_media_tmp_file(name);
    g_free(name);
    return p;
}


static void replay_media_restore_cmt_unlocked(void);


/**
 * @brief Tělo replay_media_prepare_cmt() (volající drží device_state_lock()).
 * @return NULL = v pořádku, jinak text chyby v @p errbuf.
 */
static const char *replay_media_prepare_cmt_unlocked(const st_REPLAY_DATA *d, char *errbuf, size_t errsize)
{
    JsonNode *root = d->cmt_json ? json_from_string(d->cmt_json, NULL) : NULL;
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
    {
        if (root) json_node_unref(root);
        return NULL; /* starší záznam bez stavu kazety: kazetu nechat */
    };
    JsonObject *o = json_node_get_object(root);
    const char *err = NULL;

    /* CMT hack je volba INI a mění ROM - jiný stav by přehrání rozjel. */
    JsonNode *hack = json_object_get_member(o, "hack");
    if (hack && JSON_NODE_HOLDS_VALUE(hack) && json_node_get_value_type(hack) == G_TYPE_BOOLEAN)
    {
        bool rec_hack = json_node_get_boolean(hack);
        if (rec_hack != (CMTHACK_TEST_IS_INSTALLED ? true : false))
        {
            g_snprintf(errbuf, errsize, "the CMT hack was %s when recording, here it is %s; change it in the Virtual CMT window",
                       rec_hack ? "on" : "off", CMTHACK_TEST_IS_INSTALLED ? "on" : "off");
            json_node_unref(root);
            return errbuf;
        };
    };
    /* Nastavení Direct Save totéž, jen se zapnutým hackem (bez hacku patch
     * zápisu v ROM není a nastavení nic nemění; hack sám se kontroluje
     * výše). Starší záznam bez klíče vznikl bez této funkce = vypnuto. */
    if (CMTHACK_TEST_IS_INSTALLED)
    {
        JsonNode *dsave = json_object_get_member(o, "direct_save");
        bool rec_save = false;
        if (dsave && JSON_NODE_HOLDS_VALUE(dsave) && json_node_get_value_type(dsave) == G_TYPE_BOOLEAN)
            rec_save = json_node_get_boolean(dsave);
        bool cur_save = g_cmthack.direct_save ? true : false;
        if (rec_save != cur_save)
        {
            g_snprintf(errbuf, errsize, "Direct Save of the CMT hack was %s when recording, here it is %s; change it in the CMT menu",
                       rec_save ? "on" : "off", cur_save ? "on" : "off");
            json_node_unref(root);
            return errbuf;
        };
    };

    s_orig_cmt.changed = true;
    s_orig_cmt.speed = g_cmt.mz_cmtspeed;
    s_orig_cmt.pulses = g_cmt.mz_custom_pulses;
    s_orig_cmt.polarity = g_cmt.polarity;
    s_orig_cmt.rec_autostop_s = g_cmt.rec_autostop_s;
    g_free(s_orig_cmt.tape);
    s_orig_cmt.tape = NULL;
    g_free(s_orig_cmt.image);
    s_orig_cmt.image = NULL;
    s_orig_cmt.image_size = 0;
    g_free(s_orig_cmt.wav_blocks);
    s_orig_cmt.wav_blocks = NULL;
    s_orig_cmt.wav_count = 0;
    g_free(s_orig_cmt.unsaved);
    s_orig_cmt.unsaved = NULL;
    s_orig_cmt.unsaved_count = 0;
    if (CMT_TEST_FILLED)
    {
        /* páska s neuloženými změnami (nebo bez cesty) by se po přehrání ze
         * souboru vrátila bez nich: uschovat obraz z paměti */
        st_CMTEXT_CONTAINER *oc = cmtext_get_container(g_cmt.ext);
        const char *op = oc ? cmtext_container_get_filepath(oc) : "";
        if (cmttmz_is_modified() || !op[0])
        {
            s_orig_cmt.modified = cmttmz_is_modified();
            if (EXIT_SUCCESS != cmttmz_save_image(&s_orig_cmt.image, &s_orig_cmt.image_size)) s_orig_cmt.image = NULL;
            s_orig_cmt.wav_blocks = cmttmz_get_wav_blocks(&s_orig_cmt.wav_count);
            /* obraz příznaky neuloženo nenese (importované bloky, jejichž data jinde nejsou) */
            s_orig_cmt.unsaved = cmttmz_get_unsaved_blocks(&s_orig_cmt.unsaved_count);
        };
    };
    g_free(s_orig_cmt.blocks);
    s_orig_cmt.blocks = NULL;
    s_orig_cmt.block_count = 0;
    s_orig_cmt.modified = 0;
    if (CMT_TEST_FILLED)
    {
        st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
        const char *path = c ? cmtext_container_get_filepath(c) : NULL;
        if (path && path[0]) s_orig_cmt.tape = g_strdup(path);
        /* Modified a rychlosti bloků z indexu vždy (i u pásky ze souboru) */
        s_orig_cmt.modified = cmttmz_is_modified();
        replay_media_cmt_blocks_take(c);
    };
    cmt_eject();

    /* Nastavení záznamu před vložením pásky (blok se vygeneruje s nimi). */
    st_MZTAPE_PULSES_LENGTH p;
    if (replay_media_pulses_from_json(json_object_get_member(o, "pulses"), &p)) g_cmt.mz_custom_pulses = p;
    g_cmt.mz_cmtspeed = (en_CMTSPEED)replay_media_json_int(o, "speed", (gint64)g_cmt.mz_cmtspeed);
    /* starší záznam bez klíče: automatické ukončení tehdy neexistovalo (0) */
    gint64 autostop = replay_media_json_int(o, "rec_autostop_s", 0);
    g_cmt.rec_autostop_s = (autostop >= 0 && autostop <= (gint64)CMT_REC_AUTOSTOP_MAX_S) ? (unsigned)autostop : 0u;
    cmt_rear_dip_switch_cmt_inverted_polarity((unsigned)replay_media_json_int(o, "polarity", (gint64)g_cmt.polarity));

    const st_REPLAY_MEDIA *m = replay_data_find_media(d, "cmt");
    if (m)
    {
        uint8_t *current = NULL;
        const uint8_t *data = m->data;
        if (!data && s_orig_cmt.tape)
        {
            /* Páska nad limitem: musí to být ta vložená (hash). */
            gchar *buf = NULL;
            gsize len = 0;
            char sha[REPLAY_HASH_HEX_LEN + 1];
            if (g_file_get_contents(s_orig_cmt.tape, &buf, &len, NULL)) replay_sha256_hex((const uint8_t *)buf, len, sha);
            if (buf && len == m->size && strcmp(sha, m->sha256) == 0) current = (uint8_t *)buf;
            else g_free(buf);
            data = current;
        };
        if (!data)
        {
            err = replay_media_fail(errbuf, errsize, "slot %s: the inserted tape differs from the recording "
                                                     "(the tape is not stored in the replay)", "cmt");
        }
        else
        {
            /* Přípona určuje formát pásky - jméno souboru jako při záznamu;
             * obraz z paměti je vždy TMZ (jméno "tape.tmz"). */
            JsonNode *img_node = json_object_get_member(o, "image");
            bool image = img_node && JSON_NODE_HOLDS_VALUE(img_node) && json_node_get_boolean(img_node);
            char *base = image ? g_strdup("tape.tmz") : g_path_get_basename(m->name ? m->name : "tape.mzf");
            char *tmp = replay_media_tmp_file(base);
            g_free(base);
            /* obraz z paměti se vkládá jako obraz (i páska bez bloků, kterou
             * import souboru odmítá: nová prázdná páska na startu záznamu);
             * bloky 0x18 z importu WAV podle "wav" v poli blocks */
            uint32_t wav_count = 0;
            uint8_t *wav = image ? replay_media_wav_blocks_from_json(json_object_get_member(o, "blocks"), &wav_count) : NULL;
            /* páska záznamu (dočasný soubor) do historie vložených médií nepatří */
            recent_media_suspend();
            const bool tape_failed = !tmp || !g_file_set_contents(tmp, (const gchar *)data, (gssize)m->size, NULL)
                || (image ? cmt_open_image(data, m->size, tmp, wav, wav_count) : cmt_open_file_by_extension(tmp)) != EXIT_SUCCESS;
            recent_media_resume();
            if (tape_failed)
            {
                err = replay_media_fail(errbuf, errsize, "slot %s: cannot insert the tape of the recording", "cmt");
            }
            else
            {
                g_free(s_cmt_copy_path);
                g_free(s_cmt_copy_name);
                s_cmt_copy_path = g_strdup(tmp);
                s_cmt_copy_name = (m->name && m->name[0]) ? g_strdup(m->name) : NULL;
                st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
                JsonNode *bl = json_object_get_member(o, "blocks");
                if (c && bl && JSON_NODE_HOLDS_ARRAY(bl) && cmtext_container_get_type(c) == CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE)
                {
                    JsonArray *a = json_node_get_array(bl);
                    int count = cmtext_container_get_count_blocks(c);
                    for (int i = 0; i < count && (guint)i < json_array_get_length(a); i++)
                    {
                        JsonNode *e = json_array_get_element(a, (guint)i);
                        if (!e || !JSON_NODE_HOLDS_OBJECT(e)) continue;
                        JsonObject *bo = json_node_get_object(e);
                        if (replay_media_pulses_from_json(json_object_get_member(bo, "pulses"), &p))
                        {
                            cmtext_container_set_block_pulses(c, i, &p);
                        };
                        cmtext_container_set_block_speed(c, i, (en_CMTEXT_BLOCK_SPEED)replay_media_json_int(bo, "blspeed", 0));
                        cmtext_container_set_block_cmt_speed(c, i, (en_CMTSPEED)replay_media_json_int(bo, "speed", 0));
                    };
                };
                /* Modified jako při záznamu (kopie v dočasném souboru by ho nenesla) */
                JsonNode *mod_node = json_object_get_member(o, "modified");
                cmttmz_set_modified((mod_node && JSON_NODE_HOLDS_VALUE(mod_node) && json_node_get_boolean(mod_node)) ? 1 : 0);
                /* Blok ze záznamu (znovu otevřený = vygenerovaný s nastavením výše). */
                int block = (int)replay_media_json_int(o, "block", -1);
                if (block >= 0 && c && c->cb_open_block && c->cb_open_block(block) != EXIT_SUCCESS)
                {
                    err = replay_media_fail(errbuf, errsize, "slot %s: cannot open the tape block of the recording", "cmt");
                };
            };
            g_free(wav);
            g_free(tmp);
        };
        g_free(current);
    };
    json_node_unref(root);
    return err;
}


/**
 * @brief Kazeta pro přehrání: CMT hack, nastavení, páska a blok ze záznamu.
 *
 * Vysouvá a vkládá pásku (uvolňuje kontejner a g_cmt.last_filename), proto
 * pod device_state_lock(): okna kazety a menu je v UI vlákně čtou pod týmž
 * zámkem (stejně jako replay_media_cmt_execute()).
 *
 * @return NULL = v pořádku, jinak text chyby v @p errbuf.
 * @pre Vlákno emulátoru, volající zámek nedrží.
 */
static const char *replay_media_prepare_cmt(const st_REPLAY_DATA *d, char *errbuf, size_t errsize)
{
    device_state_lock();
    const char *err = replay_media_prepare_cmt_unlocked(d, errbuf, errsize);
    device_state_unlock();
    return err;
}


/**
 * @brief Vrátí nastavení kazety a původní pásku (od začátku).
 *
 * Pod device_state_lock() ze stejného důvodu jako replay_media_prepare_cmt().
 *
 * @pre Vlákno emulátoru, volající zámek nedrží.
 */
static void replay_media_restore_cmt(void)
{
    device_state_lock();
    replay_media_restore_cmt_unlocked();
    device_state_unlock();
}


/**
 * @brief Uschová režim, rychlost a pulzy všech bloků vložené pásky z indexu (s_orig_cmt.blocks).
 * @param c Kontejner vložené pásky (smí být NULL; jen SIMPLE_TAPE má index).
 * @pre Vlákno emulátoru pod device_state_lock().
 */
static void replay_media_cmt_blocks_take(st_CMTEXT_CONTAINER *c)
{
    if (!c || cmtext_container_get_type(c) != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE) return;
    int n = cmtext_container_get_count_blocks(c);
    if (n <= 0) return;
    s_orig_cmt.blocks = g_new0(st_REPLAY_MEDIA_CMT_BLOCK, n);
    s_orig_cmt.block_count = (uint32_t)n;
    for (int i = 0; i < n; i++)
    {
        st_REPLAY_MEDIA_CMT_BLOCK *b = &s_orig_cmt.blocks[i];
        b->bltype = cmtext_container_get_block_type(c, i);
        b->blspeed = cmtext_container_get_block_speed(c, i);
        b->speed = cmtext_container_get_block_cmt_speed(c, i);
        b->has_pulses = (EXIT_SUCCESS == cmtext_container_get_block_pulses(c, i, &b->pulses));
    };
}


/**
 * @brief Vrátí blokům vrácené pásky režim, rychlost a pulzy z doby před přehráním.
 *
 * Jen nad toutéž páskou: stejný počet bloků a u každého bloku stejný typ
 * Jinak (soubor pásky se během přehrání změnil na
 * disku) se rychlosti nezapíšou - setter kontejneru by je zapsal i na blok
 * jiného typu - a okno dostane varování (replay_media_get_cmt_warning()).
 * Settery kontejneru jako u pásky ze záznamu (replay_media_prepare()),
 * Modified nemění (nastaví ho volající). Otevřený blok se otevře znovu, aby
 * hrál vrácenou rychlostí (nepodaří-li se, varování); délky bloků pro okno
 * se zneplatní.
 *
 * @param c Kontejner vrácené pásky.
 * @return true = rychlosti vrácené; false = jiná páska nebo bez indexu (nic nezapsáno).
 * @pre Vlákno emulátoru pod device_state_lock(), transport ve STOP.
 */
static bool replay_media_cmt_blocks_restore(st_CMTEXT_CONTAINER *c)
{
    if (!c || cmtext_container_get_type(c) != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE) return false;
    bool same = ((uint32_t)cmtext_container_get_count_blocks(c) == s_orig_cmt.block_count);
    for (uint32_t i = 0; same && i < s_orig_cmt.block_count; i++)
        same = (cmtext_container_get_block_type(c, (int)i) == s_orig_cmt.blocks[i].bltype);
    if (!same)
    {
        replay_media_cmt_warn("The tape file changed during the input replay playback; block speeds were not restored.");
        return false;
    };
    for (uint32_t i = 0; i < s_orig_cmt.block_count; i++)
    {
        const st_REPLAY_MEDIA_CMT_BLOCK *b = &s_orig_cmt.blocks[i];
        if (b->has_pulses) cmtext_container_set_block_pulses(c, (int)i, &b->pulses);
        cmtext_container_set_block_speed(c, (int)i, b->blspeed);
        cmtext_container_set_block_cmt_speed(c, (int)i, b->speed);
    };
    int open = cmttmz_get_open_block_id();
    if (open >= 0 && c->cb_open_block && (EXIT_SUCCESS != c->cb_open_block(open)))
        replay_media_cmt_warn("The current tape block could not be reopened at its restored speed.");
    cmttmz_note_change();
    cmttmz_lengths_invalidate();
    return true;
}


/** @brief Tělo replay_media_restore_cmt() (volající drží device_state_lock()). */
static void replay_media_restore_cmt_unlocked(void)
{
    g_free(s_cmt_copy_path);
    g_free(s_cmt_copy_name);
    s_cmt_copy_path = NULL;
    s_cmt_copy_name = NULL;
    if (s_cmt_maps) g_ptr_array_free(s_cmt_maps, TRUE);
    s_cmt_maps = NULL;
    if (!s_orig_cmt.changed) return;
    s_orig_cmt.changed = false;
    cmt_eject();
    g_cmt.mz_custom_pulses = s_orig_cmt.pulses;
    g_cmt.mz_cmtspeed = s_orig_cmt.speed;
    cmt_rear_dip_switch_cmt_inverted_polarity((unsigned)s_orig_cmt.polarity);
    g_cmt.rec_autostop_s = s_orig_cmt.rec_autostop_s;
    bool from_image = false;
    if (s_orig_cmt.image)
    {
        from_image = (EXIT_SUCCESS == cmt_open_image(s_orig_cmt.image, s_orig_cmt.image_size, s_orig_cmt.tape ? s_orig_cmt.tape : "",
                                                     s_orig_cmt.wav_blocks, s_orig_cmt.wav_count));
    }
    else if (s_orig_cmt.tape)
    {
        /* návrat pásky uživatele není nové vložení: historie vložených médií se nemění */
        recent_media_suspend();
        cmt_open_file_by_extension(s_orig_cmt.tape);
        recent_media_resume();
    };
    /* rychlosti bloků (jen nad toutéž páskou, jinak varování) */
    if (CMT_TEST_FILLED && s_orig_cmt.blocks) (void)replay_media_cmt_blocks_restore(cmtext_get_container(g_cmt.ext));
    /* Modified a příznaky neuloženo jako před přehráním, jednou až po rychlostech
     * (settery rychlosti Modified nastavují; obraz s cestou má příznaky všechny 0) */
    if (CMT_TEST_FILLED && (from_image || s_orig_cmt.blocks))
    {
        cmttmz_set_modified(s_orig_cmt.modified);
        for (uint32_t i = 0; s_orig_cmt.modified && s_orig_cmt.unsaved && i < s_orig_cmt.unsaved_count; i++)
            (void)cmttmz_set_block_unsaved((int)i, s_orig_cmt.unsaved[i]);
    };
    g_free(s_orig_cmt.blocks);
    s_orig_cmt.blocks = NULL;
    s_orig_cmt.block_count = 0;
    g_free(s_orig_cmt.tape);
    s_orig_cmt.tape = NULL;
    g_free(s_orig_cmt.image);
    s_orig_cmt.image = NULL;
    s_orig_cmt.image_size = 0;
    g_free(s_orig_cmt.wav_blocks);
    s_orig_cmt.wav_blocks = NULL;
    s_orig_cmt.wav_count = 0;
    g_free(s_orig_cmt.unsaved);
    s_orig_cmt.unsaved = NULL;
    s_orig_cmt.unsaved_count = 0;
}


/** @brief Provede operaci kazety (tělo replay_media_cmt_execute(), bez zámku). */
static bool replay_media_cmt_execute_unlocked(en_REPLAY_CMT_OP op, int32_t arg, const char *path)
{
    switch (op)
    {
        case REPLAY_CMT_PLAY: cmt_play(); return true;
        case REPLAY_CMT_PLAY_PAUSED: cmt_play_paused(); return true;
        case REPLAY_CMT_STOP: cmt_stop(); return true;
        case REPLAY_CMT_STOP_BUTTON: cmt_stop_button(); return true;
        case REPLAY_CMT_PAUSE: cmt_pause(arg ? 1 : 0); return true;
        case REPLAY_CMT_EJECT: cmt_eject(); return true;
        case REPLAY_CMT_SEEK:
            /* stejná cesta jako živý MCP seek (dbgapi): blok od začátku, transport beze změny */
            return cmt_tape_seek_block(arg) == EXIT_SUCCESS;
        case REPLAY_CMT_SELECT: return cmt_tape_select_block(arg, 0) == EXIT_SUCCESS;
        case REPLAY_CMT_SELECT_FORCED: return cmt_tape_select_block(arg, 1) == EXIT_SUCCESS;
        case REPLAY_CMT_NEXT: return cmt_tape_step_block(1) == EXIT_SUCCESS;
        case REPLAY_CMT_PREV: return cmt_tape_step_block(0) == EXIT_SUCCESS;
        case REPLAY_CMT_RECORD:
            /* bez cesty jen do pásky (tlačítko RECORD, MCP bez path) */
            return (path ? cmt_record_to_file(path) : cmt_record()) == EXIT_SUCCESS;
        case REPLAY_CMT_NEW_TAPE: return cmt_new_tape() == EXIT_SUCCESS;
        default: break;
    };
    return false;
}


const char *replay_media_cmt_snapshot_path(const char *path)
{
    if (path && s_cmt_copy_path && (0 == strcmp(path, s_cmt_copy_path))) return s_cmt_copy_name ? s_cmt_copy_name : "";
    for (guint i = 0; path && s_cmt_maps && i + 1 < s_cmt_maps->len; i += 2)
    {
        if (0 == strcmp(path, (const char *)g_ptr_array_index(s_cmt_maps, i)))
            return (const char *)g_ptr_array_index(s_cmt_maps, i + 1);
    };
    return path;
}


void replay_media_cmt_map_path(const char *tmp, const char *orig)
{
    if (!tmp || !orig) return;
    if (!s_cmt_maps) s_cmt_maps = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(s_cmt_maps, g_strdup(tmp));
    g_ptr_array_add(s_cmt_maps, g_strdup(orig));
}


bool replay_media_cmt_guard_holds(const st_REPLAY_CMT_GUARD *guard, int32_t arg)
{
    if (!guard || !guard->flags) return true;
    if ((guard->flags & REPLAY_CMT_GUARD_TAPE) && (guard->tape_serial != cmttmz_get_tape_serial())) return false;
    if ((guard->flags & REPLAY_CMT_GUARD_NOT_STOP) && CMT_TEST_STOP) return false;
    if ((guard->flags & REPLAY_CMT_GUARD_RECORD) && !CMT_TEST_RECORD) return false;
    if ((guard->flags & REPLAY_CMT_GUARD_NOT_PLAYING_ARG) && CMT_TEST_FILLED && CMT_TEST_PLAY && !CMT_TEST_PAUSED
        && g_cmt.ext->block && (cmtext_block_get_block_id(g_cmt.ext->block) == arg))
        return false;
    return true;
}


bool replay_media_cmt_execute(en_REPLAY_CMT_OP op, int32_t arg, const char *path)
{
    /* Okna kazety čtou pásku pod zámkem stavu zařízení (vysunutí uvolňuje
     * kontejner); UI pod zámkem na emu vlákno nečeká. */
    device_state_lock();
    bool ok = replay_media_cmt_execute_unlocked(op, arg, path);
    device_state_unlock();
    return ok;
}


const char *replay_media_prepare(const st_REPLAY_DATA *d, char *errbuf, size_t errsize)
{
    if (!d->media_known) return NULL;
    char slot[REPLAY_SLOT_MAX];
    const char *err = NULL;
    s_prepared = true;

#if CFG_HWEXT_HAVE_FDC
    /* Výměna obrazů uvolňuje paměť mechanik (jméno, identita souboru), kterou
     * UI čte pod zámkem stavu zařízení; stejně jako u kazety
     * (replay_media_prepare_cmt()). Zámek není rekurzivní. */
    const bool fdc_took_lock = !device_state_lock_held();
    if (fdc_took_lock) device_state_lock();
    for (int f = 0; f < FDC_INSTANCE_COUNT && !err; f++)
    {
        for (int n = 0; n < FDC_NUM_DRIVES && !err; n++)
        {
            st_FDC *fdc = &g_fdc[f];
            st_REPLAY_MEDIA_FDC_ORIG *o = &s_orig_fdc[f][n];
            replay_media_fdc_slot(slot, f, n);
            const st_REPLAY_MEDIA *m = replay_data_find_media(d, slot);
            bool mounted = fdc_test_drive_id_mounted(fdc, (unsigned)n) != 0;
            if (!m && !mounted) continue;

            o->changed = true;
            o->mounted = mounted;
            g_free(o->filename);
            o->filename = mounted ? g_strdup(fdc->drive[n].filename) : NULL;

            if (!m)
            {
                fdc_umount_keep_cfg(fdc, (unsigned)n);
                continue;
            };
            const uint8_t *data = m->data;
            uint8_t *current = NULL;
            if (!data)
            {
                /* Obraz nad limitem: musí to být ten nasazený (hash). */
                size_t size = 0;
                current = fdc_drive_copy_image(fdc, (unsigned)n, &size);
                char sha[REPLAY_HASH_HEX_LEN + 1];
                if (current) replay_sha256_hex(current, size, sha);
                if (!current || size != m->size || strcmp(sha, m->sha256) != 0)
                {
                    g_free(current);
                    err = replay_media_fail(errbuf, errsize, "slot %s: the mounted disk image differs from the recording "
                                                             "(the image is not stored in the replay)", slot);
                    break;
                };
                data = current;
            };
            if (fdc_mount_dsk_buffer(fdc, (unsigned)n, data, (size_t)m->size, m->readonly ? 1 : 0, m->name) != EXIT_SUCCESS)
            {
                err = replay_media_fail(errbuf, errsize, "slot %s: cannot mount the disk image of the recording", slot);
            };
            g_free(current);
        };
    };
    if (fdc_took_lock) device_state_unlock();
#endif

#if CFG_HWEXT_HAVE_QDISK
    if (!err)
    {
        const st_REPLAY_MEDIA *m = replay_data_find_media(d, "qd");
        if (m)
        {
            s_qd_changed = true;
            const uint8_t *data = m->data;
            uint8_t *current = NULL;
            if (!data)
            {
                size_t size = 0;
                current = qdisk_copy_image(&size);
                char sha[REPLAY_HASH_HEX_LEN + 1];
                if (current) replay_sha256_hex(current, size, sha);
                if (!current || size != m->size || strcmp(sha, m->sha256) != 0)
                {
                    g_free(current);
                    err = replay_media_fail(errbuf, errsize, "slot %s: the mounted Quick Disk image differs from the "
                                                             "recording (the image is not stored in the replay)", "qd");
                };
                data = current;
            };
            if (!err && qdisk_mount_image_buffer(data, (uint32_t)m->size, m->readonly ? 1 : 0, m->name) != EXIT_SUCCESS)
            {
                err = replay_media_fail(errbuf, errsize, "slot %s: cannot mount the Quick Disk image of the recording", "qd");
            };
            g_free(current);
        }
        else if (g_qdisk.handler_valid)
        {
            s_qd_changed = true;
            qdisk_umount_keep_cfg();
        };
    };
#endif

    if (!err) err = replay_media_prepare_cmt(d, errbuf, errsize);

#if CFG_HWEXT_HAVE_IDE8
    for (int n = 0; n < IDE8_DRIVES_COUNT && !err; n++)
    {
        st_IDE8_DRIVE *drive = &g_ide8.drive[n];
        replay_media_ide8_slot(slot, n);
        const st_REPLAY_MEDIA *m = replay_data_find_media(d, slot);
        bool connected = (drive->connected == IDE8_STATE_CONNECTED);
        if (!m && !connected) continue;
        s_ide8_changed[n] = true;
        s_ide8_connected[n] = connected;
        if (!m)
        {
            ide8_drive_close_image(drive);
            continue;
        };

        char *tmp = replay_media_tmp_path(slot);
        FILE *f = tmp ? g_fopen(tmp, "wb") : NULL;
        if (!f)
        {
            g_free(tmp);
            err = replay_media_fail(errbuf, errsize, "slot %s: cannot create a temporary copy of the disk image", slot);
            break;
        };
        bool ok;
        if (m->data)
        {
            ok = (m->size == 0) || (fwrite(m->data, 1, (size_t)m->size, f) == (size_t)m->size);
        }
        else
        {
            /* Obraz nad limitem: kopie nasazeného obrazu, který musí mít hash záznamu. */
            st_REPLAY_MEDIA_COPY c = { f, g_checksum_new(G_CHECKSUM_SHA256), true };
            ok = connected && ide8_drive_read_image((en_IDE8_DRIVE)n, replay_media_chunk_to_file, &c) == EXIT_SUCCESS
                 && c.ok && strcmp(g_checksum_get_string(c.sha), m->sha256) == 0;
            g_checksum_free(c.sha);
            if (!ok)
            {
                fclose(f);
                g_remove(tmp);
                g_free(tmp);
                err = replay_media_fail(errbuf, errsize, "slot %s: the mounted disk image differs from the recording "
                                                         "(the image is not stored in the replay)", slot);
                break;
            };
        };
        if (fclose(f) != 0) ok = false;
        if (ok)
        {
            ide8_drive_close_image(drive);
            ok = ide8_drive_open_image(drive, tmp) == EXIT_SUCCESS;
        };
        g_free(tmp);
        if (!ok) err = replay_media_fail(errbuf, errsize, "slot %s: cannot mount the disk image of the recording", slot);
    };
#endif

    if (err)
    {
        fprintf(stderr, "[REPLAY] %s\n", err);
        replay_media_restore();
    };
    return err;
}


void replay_media_restore(void)
{
    if (!s_prepared) return;
    s_prepared = false;

#if CFG_HWEXT_HAVE_FDC
    /* Pod zámkem stavu zařízení ze stejného důvodu jako v replay_media_prepare(). */
    const bool fdc_took_lock = !device_state_lock_held();
    if (fdc_took_lock) device_state_lock();
    for (int f = 0; f < FDC_INSTANCE_COUNT; f++)
    {
        for (int n = 0; n < FDC_NUM_DRIVES; n++)
        {
            st_REPLAY_MEDIA_FDC_ORIG *o = &s_orig_fdc[f][n];
            if (!o->changed) continue;
            o->changed = false;
            if (o->mounted && o->filename)
            {
                /* návrat diskety uživatele není nové vložení: historie vložených médií se nemění */
                recent_media_suspend();
                fdc_mount_dskfile(&g_fdc[f], (unsigned)n, o->filename);
                recent_media_resume();
            }
            else fdc_umount_keep_cfg(&g_fdc[f], (unsigned)n);
            g_free(o->filename);
            o->filename = NULL;
        };
    };
    if (fdc_took_lock) device_state_unlock();
#endif

#if CFG_HWEXT_HAVE_QDISK
    if (s_qd_changed)
    {
        s_qd_changed = false;
        qdisk_umount_keep_cfg();
        qdisk_open();
    };
#endif

#if CFG_HWEXT_HAVE_IDE8
    for (int n = 0; n < IDE8_DRIVES_COUNT; n++)
    {
        if (!s_ide8_changed[n]) continue;
        s_ide8_changed[n] = false;
        st_IDE8_DRIVE *drive = &g_ide8.drive[n];
        ide8_drive_close_image(drive);
        if (s_ide8_connected[n]) ide8_drive_open_image(drive, drive->filepath);
    };
#endif

    replay_media_restore_cmt();

    if (s_tmpdir)
    {
        GDir *dir = g_dir_open(s_tmpdir, 0, NULL);
        if (dir)
        {
            const char *name;
            while ((name = g_dir_read_name(dir)) != NULL)
            {
                char *p = g_build_filename(s_tmpdir, name, NULL);
                g_remove(p);
                g_free(p);
            };
            g_dir_close(dir);
        };
        g_rmdir(s_tmpdir);
        g_free(s_tmpdir);
        s_tmpdir = NULL;
    };
}
