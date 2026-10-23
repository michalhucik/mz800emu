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
#include "device_request.h"
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

/** @brief Původní stav kazety před přehráním (nastavení uživatele a vložená páska). */
typedef struct st_REPLAY_MEDIA_CMT_ORIG
{
    bool changed;                    /**< Přehrání změnilo kazetu. */
    en_CMTSPEED speed;               /**< Výchozí rychlost. */
    st_MZTAPE_PULSES_LENGTH pulses;  /**< Výchozí vlastní pulzy. */
    en_CMT_STREAM_POLARITY polarity; /**< Polarita. */
    char *tape;                      /**< Vložená páska (cesta, g_free), NULL = žádná. */
} st_REPLAY_MEDIA_CMT_ORIG;

static st_REPLAY_MEDIA_CMT_ORIG s_orig_cmt;

static char *replay_media_cmt_state_json(void);


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


void replay_media_capture(st_REPLAY_DATA *d)
{
    uint64_t budget = REPLAY_MEDIA_EMBED_LIMIT;
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

    /* Kazeta: páska (soubor) + stav a nastavení v manifestu. */
    if (CMT_TEST_FILLED)
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


/**
 * @brief Stav kazety pro manifest: CMT hack, nastavení, blok a rychlosti bloků.
 * @return JSON objekt jako text (g_free).
 */
static char *replay_media_cmt_state_json(void)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "hack");
    json_builder_add_boolean_value(b, CMTHACK_TEST_IS_INSTALLED ? TRUE : FALSE);
    json_builder_set_member_name(b, "speed");
    json_builder_add_int_value(b, (gint64)g_cmt.mz_cmtspeed);
    json_builder_set_member_name(b, "pulses");
    replay_media_json_pulses(b, &g_cmt.mz_custom_pulses);
    json_builder_set_member_name(b, "polarity");
    json_builder_add_int_value(b, (gint64)g_cmt.polarity);
    json_builder_set_member_name(b, "block");
    json_builder_add_int_value(b, (CMT_TEST_FILLED && g_cmt.ext->block) ? cmtext_block_get_block_id(g_cmt.ext->block) : -1);

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


/**
 * @brief Kazeta pro přehrání: CMT hack, nastavení, páska a blok ze záznamu.
 * @return NULL = v pořádku, jinak text chyby v @p errbuf.
 */
static const char *replay_media_prepare_cmt(const st_REPLAY_DATA *d, char *errbuf, size_t errsize)
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

    s_orig_cmt.changed = true;
    s_orig_cmt.speed = g_cmt.mz_cmtspeed;
    s_orig_cmt.pulses = g_cmt.mz_custom_pulses;
    s_orig_cmt.polarity = g_cmt.polarity;
    g_free(s_orig_cmt.tape);
    s_orig_cmt.tape = NULL;
    if (CMT_TEST_FILLED)
    {
        st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
        const char *path = c ? cmtext_container_get_filepath(c) : NULL;
        if (path && path[0]) s_orig_cmt.tape = g_strdup(path);
    };
    cmt_eject();

    /* Nastavení záznamu před vložením pásky (blok se vygeneruje s nimi). */
    st_MZTAPE_PULSES_LENGTH p;
    if (replay_media_pulses_from_json(json_object_get_member(o, "pulses"), &p)) g_cmt.mz_custom_pulses = p;
    g_cmt.mz_cmtspeed = (en_CMTSPEED)replay_media_json_int(o, "speed", (gint64)g_cmt.mz_cmtspeed);
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
            /* Přípona určuje formát pásky - jméno souboru jako při záznamu. */
            char *base = g_path_get_basename(m->name ? m->name : "tape.mzf");
            char *tmp = replay_media_tmp_file(base);
            g_free(base);
            if (!tmp || !g_file_set_contents(tmp, (const gchar *)data, (gssize)m->size, NULL)
                || cmt_open_file_by_extension(tmp) != EXIT_SUCCESS)
            {
                err = replay_media_fail(errbuf, errsize, "slot %s: cannot insert the tape of the recording", "cmt");
            }
            else
            {
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
                /* Blok ze záznamu (znovu otevřený = vygenerovaný s nastavením výše). */
                int block = (int)replay_media_json_int(o, "block", -1);
                if (block >= 0 && c && c->cb_open_block && c->cb_open_block(block) != EXIT_SUCCESS)
                {
                    err = replay_media_fail(errbuf, errsize, "slot %s: cannot open the tape block of the recording", "cmt");
                };
            };
            g_free(tmp);
        };
        g_free(current);
    };
    json_node_unref(root);
    return err;
}


/** @brief Vrátí nastavení kazety a původní pásku (od začátku). */
static void replay_media_restore_cmt(void)
{
    if (!s_orig_cmt.changed) return;
    s_orig_cmt.changed = false;
    cmt_eject();
    g_cmt.mz_custom_pulses = s_orig_cmt.pulses;
    g_cmt.mz_cmtspeed = s_orig_cmt.speed;
    cmt_rear_dip_switch_cmt_inverted_polarity((unsigned)s_orig_cmt.polarity);
    if (s_orig_cmt.tape) cmt_open_file_by_extension(s_orig_cmt.tape);
    g_free(s_orig_cmt.tape);
    s_orig_cmt.tape = NULL;
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
            if (!CMT_TEST_FILLED || !g_cmt.ext->container || !g_cmt.ext->container->cb_open_block) return false;
            return g_cmt.ext->container->cb_open_block(arg) == EXIT_SUCCESS;
        case REPLAY_CMT_SELECT: return cmt_tape_select_block(arg, 0) == EXIT_SUCCESS;
        case REPLAY_CMT_SELECT_FORCED: return cmt_tape_select_block(arg, 1) == EXIT_SUCCESS;
        case REPLAY_CMT_NEXT: return cmt_tape_step_block(1) == EXIT_SUCCESS;
        case REPLAY_CMT_PREV: return cmt_tape_step_block(0) == EXIT_SUCCESS;
        case REPLAY_CMT_RECORD: return path && cmt_record_to_file(path) == EXIT_SUCCESS;
        default: break;
    };
    return false;
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
    for (int f = 0; f < FDC_INSTANCE_COUNT; f++)
    {
        for (int n = 0; n < FDC_NUM_DRIVES; n++)
        {
            st_REPLAY_MEDIA_FDC_ORIG *o = &s_orig_fdc[f][n];
            if (!o->changed) continue;
            o->changed = false;
            if (o->mounted && o->filename) fdc_mount_dskfile(&g_fdc[f], (unsigned)n, o->filename);
            else fdc_umount_keep_cfg(&g_fdc[f], (unsigned)n);
            g_free(o->filename);
            o->filename = NULL;
        };
    };
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
