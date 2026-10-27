/**
 * @file   replay_format.c
 * @brief  Formát záznamu vstupů `.mzr`: zápis a čtení (viz replay_format.h).
 *
 * JSON se píše a čte přes json-glib (devdoc/json-style.md), ZIP přes
 * snapshot_io. CSV hashů je jednoduchý text bez uvozovek (jen čísla
 * a hex řetězce).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "replay_format.h"
#include "snapshot/snapshot_io.h"

/** @brief Jméno souboru manifestu v archivu. */
#define REPLAY_ENTRY_MANIFEST "manifest.json"
/** @brief Jméno startovního snapshotu v archivu. */
#define REPLAY_ENTRY_START "start.mzs"
/** @brief Jméno souboru událostí v archivu. */
#define REPLAY_ENTRY_INPUTS "inputs.jsonl"
/** @brief Jméno souboru hashů v archivu. */
#define REPLAY_ENTRY_HASHES "hashes.csv"

/** @brief Předpona jmen entries vložených médií v archivu. */
#define REPLAY_ENTRY_MEDIA_PREFIX "media/"

/** @brief Komprese ZIP (snapshot uvnitř je už komprimovaný, text se zmenší). */
#define REPLAY_ZIP_COMPRESSION 6


const char *replay_ev_kind_name(en_REPLAY_EV_KIND kind)
{
    switch (kind)
    {
        case REPLAY_EV_KBD: return "kbd";
        case REPLAY_EV_JOY: return "joy";
        case REPLAY_EV_AUTOTYPE: return "autotype";
        case REPLAY_EV_RESET: return "reset";
        case REPLAY_EV_DBG: return "dbg";
        case REPLAY_EV_CMT: return "cmt";
        case REPLAY_EV_CMTHACK: return "cmthack";
        default: break;
    };
    return "unknown";
}


const char *replay_ev_at_name(en_REPLAY_EV_AT at)
{
    switch (at)
    {
        case REPLAY_AT_BOUNDARY: return "b";
        case REPLAY_AT_DRAIN: return "d";
        case REPLAY_AT_PAUSE: return "p";
        default: break;
    };
    return "?";
}


const char *replay_hash_part_name(en_REPLAY_HASH_PART part)
{
    switch (part)
    {
        case REPLAY_HASH_CPU: return "cpu";
        case REPLAY_HASH_RAM: return "ram";
        case REPLAY_HASH_MEMEXT: return "memext";
        case REPLAY_HASH_VRAM: return "vram";
        case REPLAY_HASH_PCG: return "pcg";
        case REPLAY_HASH_PERIPH: return "periph";
        default: break;
    };
    return "unknown";
}


/** @brief Uvolní řetězce jedné události (GArray clear func). */
static void replay_event_clear(gpointer p)
{
    st_REPLAY_EVENT *ev = (st_REPLAY_EVENT *)p;
    g_free(ev->text);
    g_free(ev->cmd);
    g_free(ev->args_json);
    ev->text = NULL;
    ev->cmd = NULL;
    ev->args_json = NULL;
}


/** @brief Uvolní médium (GPtrArray free func). */
static void replay_media_free(gpointer p)
{
    st_REPLAY_MEDIA *m = (st_REPLAY_MEDIA *)p;
    if (!m) return;
    g_free(m->slot);
    g_free(m->name);
    g_free(m->data);
    g_free(m);
}


st_REPLAY_DATA *replay_data_new(void)
{
    st_REPLAY_DATA *d = g_new0(st_REPLAY_DATA, 1);
    d->hash_interval = REPLAY_DEFAULT_HASH_INTERVAL;
    d->warnings = g_ptr_array_new_with_free_func(g_free);
    d->events = g_array_new(FALSE, TRUE, sizeof(st_REPLAY_EVENT));
    g_array_set_clear_func(d->events, replay_event_clear);
    d->hashes = g_array_new(FALSE, TRUE, sizeof(st_REPLAY_HASH_ROW));
    d->media = g_ptr_array_new_with_free_func(replay_media_free);
    for (int i = 0; i < REPLAY_JOY_PORTS; i++) d->joy_type[i] = REPLAY_JOY_TYPE_UNKNOWN;
    return d;
}


void replay_data_free(st_REPLAY_DATA *d)
{
    if (!d) return;
    g_free(d->emulator_version);
    g_free(d->platform);
    g_free(d->created);
    g_free(d->description);
    g_ptr_array_unref(d->warnings);
    g_free(d->start_snapshot);
    g_array_unref(d->events);
    g_array_unref(d->hashes);
    g_ptr_array_unref(d->media);
    g_free(d->rtc_base);
    g_free(d->cmt_json);
    g_free(d->devbus_conflict);
    g_free(d->devbus_bus_idle);
    g_free(d);
}


void replay_data_add_event(st_REPLAY_DATA *d, const st_REPLAY_EVENT *ev)
{
    st_REPLAY_EVENT copy = *ev;
    copy.text = g_strdup(ev->text);
    copy.cmd = g_strdup(ev->cmd);
    copy.args_json = g_strdup(ev->args_json);
    g_array_append_val(d->events, copy);
    if (ev->kind == REPLAY_EV_DBG) d->has_debugger_events = true;
}


void replay_data_add_hash(st_REPLAY_DATA *d, const st_REPLAY_HASH_ROW *row)
{
    g_array_append_val(d->hashes, *row);
}


void replay_data_truncate(st_REPLAY_DATA *d, guint events_len, guint hashes_len, guint media_len)
{
    if (events_len < d->events->len) g_array_remove_range(d->events, events_len, d->events->len - events_len);
    if (hashes_len < d->hashes->len) g_array_set_size(d->hashes, hashes_len);
    if (media_len < d->media->len) g_ptr_array_set_size(d->media, (gint)media_len);
    d->has_debugger_events = false;
    for (guint i = 0; i < d->events->len; i++)
    {
        if (g_array_index(d->events, st_REPLAY_EVENT, i).kind == REPLAY_EV_DBG) d->has_debugger_events = true;
    };
}


void replay_data_add_warning(st_REPLAY_DATA *d, const char *warning)
{
    if (!warning) return;
    for (guint i = 0; i < d->warnings->len; i++)
    {
        if (strcmp((const char *)g_ptr_array_index(d->warnings, i), warning) == 0) return;
    };
    g_ptr_array_add(d->warnings, g_strdup(warning));
}


void replay_sha256_hex(const uint8_t *data, size_t size, char *out)
{
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
    if (data && size) g_checksum_update(c, data, (gssize)size);
    g_strlcpy(out, g_checksum_get_string(c), REPLAY_HASH_HEX_LEN + 1);
    g_checksum_free(c);
}


/** @brief true = @p slot je platné jméno slotu (písmena, číslice, `.`, `_`, bez `..`). */
static bool replay_media_slot_valid(const char *slot)
{
    if (!slot || !slot[0] || strstr(slot, "..")) return false;
    for (const char *c = slot; *c; c++)
    {
        if (!g_ascii_isalnum(*c) && *c != '.' && *c != '_') return false;
    };
    return true;
}


/** @brief true = @p s je 64 hex znaků malými písmeny. */
static bool replay_sha_hex_valid(const char *s)
{
    if (!s || strlen(s) != REPLAY_HASH_HEX_LEN) return false;
    for (const char *c = s; *c; c++)
    {
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return false;
    };
    return true;
}


int replay_data_add_media(st_REPLAY_DATA *d, const char *slot, const char *name, const uint8_t *data,
                          uint64_t size, const char *sha256, bool readonly)
{
    if (!d || !replay_media_slot_valid(slot)) return -1;
    if (!data && !replay_sha_hex_valid(sha256)) return -1;
    st_REPLAY_MEDIA *m = g_new0(st_REPLAY_MEDIA, 1);
    m->slot = g_strdup(slot);
    m->name = g_strdup(name);
    m->size = size;
    m->readonly = readonly;
    if (data)
    {
        m->data = g_malloc(size ? (gsize)size : 1);
        if (size) memcpy(m->data, data, (size_t)size);
        replay_sha256_hex(data, (size_t)size, m->sha256);
    }
    else
    {
        g_strlcpy(m->sha256, sha256, sizeof(m->sha256));
    };
    d->media_known = true;
    for (guint i = 0; i < d->media->len; i++)
    {
        st_REPLAY_MEDIA *old = (st_REPLAY_MEDIA *)g_ptr_array_index(d->media, i);
        if (strcmp(old->slot, slot) == 0)
        {
            replay_media_free(old);
            g_ptr_array_index(d->media, i) = m;
            return (int)i;
        };
    };
    g_ptr_array_add(d->media, m);
    return (int)d->media->len - 1;
}


const st_REPLAY_MEDIA *replay_data_find_media(const st_REPLAY_DATA *d, const char *slot)
{
    if (!d || !slot) return NULL;
    for (guint i = 0; i < d->media->len; i++)
    {
        const st_REPLAY_MEDIA *m = (const st_REPLAY_MEDIA *)g_ptr_array_index(d->media, i);
        if (strcmp(m->slot, slot) == 0) return m;
    };
    return NULL;
}


uint64_t replay_data_embedded_size(const st_REPLAY_DATA *d)
{
    uint64_t total = 0;
    for (guint i = 0; d && i < d->media->len; i++)
    {
        const st_REPLAY_MEDIA *m = (const st_REPLAY_MEDIA *)g_ptr_array_index(d->media, i);
        if (m->data) total += m->size;
    };
    return total;
}


/** @brief JsonNode -> kompaktní text (g_free). */
static char *replay_json_node_to_string(JsonNode *node)
{
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, node);
    json_generator_set_pretty(gen, FALSE);
    char *s = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    return s;
}


/** @brief Zapíše jednu událost jako kompaktní JSON řádek do @p out (bez `\n`). */
static void replay_event_to_json(const st_REPLAY_EVENT *ev, GString *out)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "f");
    json_builder_add_int_value(b, ev->frame);
    json_builder_set_member_name(b, "at");
    json_builder_add_string_value(b, replay_ev_at_name(ev->at));
    json_builder_set_member_name(b, "k");
    json_builder_add_string_value(b, replay_ev_kind_name(ev->kind));

    switch (ev->kind)
    {
        case REPLAY_EV_KBD:
            json_builder_set_member_name(b, "m");
            json_builder_begin_array(b);
            for (int i = 0; i < REPLAY_KBD_COLS; i++)
            {
                json_builder_add_int_value(b, ev->kbd[i]);
            };
            json_builder_end_array(b);
            break;
        case REPLAY_EV_JOY:
            json_builder_set_member_name(b, "p");
            json_builder_add_int_value(b, ev->joy_port);
            json_builder_set_member_name(b, "s");
            json_builder_add_int_value(b, ev->joy_state);
            break;
        case REPLAY_EV_AUTOTYPE:
            json_builder_set_member_name(b, "text");
            json_builder_add_string_value(b, ev->text ? ev->text : "");
            json_builder_set_member_name(b, "kd_ms");
            json_builder_add_int_value(b, ev->kd_ms);
            json_builder_set_member_name(b, "ku_ms");
            json_builder_add_int_value(b, ev->ku_ms);
            break;
        case REPLAY_EV_DBG:
        {
            json_builder_set_member_name(b, "cmd");
            json_builder_add_string_value(b, ev->cmd ? ev->cmd : "");
            json_builder_set_member_name(b, "args");
            JsonNode *args = NULL;
            if (ev->args_json)
            {
                args = json_from_string(ev->args_json, NULL);
            };
            if (!args || !JSON_NODE_HOLDS_OBJECT(args))
            {
                if (args) json_node_unref(args);
                args = json_node_init_object(json_node_alloc(), json_object_new());
            };
            /* json_builder_add_value() převezme vlastnictví uzlu. */
            json_builder_add_value(b, args);
            break;
        };
        case REPLAY_EV_CMT:
            json_builder_set_member_name(b, "op");
            json_builder_add_string_value(b, ev->cmd ? ev->cmd : "");
            json_builder_set_member_name(b, "v");
            json_builder_add_int_value(b, ev->arg);
            if (ev->text)
            {
                json_builder_set_member_name(b, "path");
                json_builder_add_string_value(b, ev->text);
            };
            break;
        case REPLAY_EV_CMTHACK:
            json_builder_set_member_name(b, "media");
            json_builder_add_int_value(b, ev->arg);
            json_builder_set_member_name(b, "name");
            json_builder_add_string_value(b, ev->text ? ev->text : "");
            break;
        case REPLAY_EV_RESET:
        default:
            break;
    };

    json_builder_end_object(b);
    JsonNode *root = json_builder_get_root(b);
    char *s = replay_json_node_to_string(root);
    g_string_append(out, s);
    g_free(s);
    json_node_unref(root);
    g_object_unref(b);
}


char *replay_data_inputs_to_jsonl(const st_REPLAY_DATA *d)
{
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < d->events->len; i++)
    {
        replay_event_to_json(&g_array_index(d->events, st_REPLAY_EVENT, i), out);
        g_string_append_c(out, '\n');
    };
    g_string_append_printf(out, "{\"f\":%u,\"k\":\"end\"}\n", d->frames);
    return g_string_free(out, FALSE);
}


/** @brief Nastaví *err na nový text (printf), pokud err != NULL. */
static void replay_set_err(char **err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void replay_set_err(char **err, const char *fmt, ...)
{
    if (!err) return;
    va_list ap;
    va_start(ap, fmt);
    g_free(*err);
    *err = g_strdup_vprintf(fmt, ap);
    va_end(ap);
}


/**
 * @brief Celé nezáporné číslo členu objektu v rozsahu 0..max.
 * @return true = člen existuje, je celé číslo a vejde se.
 */
static bool replay_json_get_uint(JsonObject *o, const char *name, guint64 max, guint64 *out)
{
    JsonNode *n = json_object_get_member(o, name);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return false;
    gint64 v = json_node_get_int(n);
    if (v < 0 || (guint64)v > max) return false;
    *out = (guint64)v;
    return true;
}


/** @brief Celé číslo členu objektu (se znaménkem) v rozsahu int32. */
static bool replay_json_get_int32(JsonObject *o, const char *name, int32_t *out)
{
    JsonNode *n = json_object_get_member(o, name);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return false;
    gint64 v = json_node_get_int(n);
    if (v < G_MININT32 || v > G_MAXINT32) return false;
    *out = (int32_t)v;
    return true;
}


/** @brief Řetězcový člen objektu, NULL pokud chybí nebo není řetězec. */
static const char *replay_json_get_str(JsonObject *o, const char *name)
{
    JsonNode *n = json_object_get_member(o, name);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
    return json_node_get_string(n);
}


/**
 * @brief Rozebere jeden řádek události.
 *
 * @param o      Objekt řádku.
 * @param[out] ev Událost (řetězce nově alokované).
 * @param[out] is_end true = řádek `end` (pak je platné jen `ev->frame`).
 * @return NULL = v pořádku, jinak statický popis chyby anglicky.
 */
static const char *replay_event_from_json(JsonObject *o, st_REPLAY_EVENT *ev, bool *is_end)
{
    memset(ev, 0, sizeof(*ev));
    *is_end = false;

    guint64 f;
    if (!replay_json_get_uint(o, "f", G_MAXUINT32, &f)) return "missing or invalid \"f\"";
    ev->frame = (uint32_t)f;

    const char *k = replay_json_get_str(o, "k");
    if (!k) return "missing \"k\"";
    if (strcmp(k, "end") == 0)
    {
        *is_end = true;
        return NULL;
    };

    int kind = -1;
    for (int i = 0; i < REPLAY_EV_COUNT; i++)
    {
        if (strcmp(k, replay_ev_kind_name((en_REPLAY_EV_KIND)i)) == 0) kind = i;
    };
    if (kind < 0) return "unknown event kind";
    ev->kind = (en_REPLAY_EV_KIND)kind;

    const char *at = replay_json_get_str(o, "at");
    if (!at) return "missing \"at\"";
    int at_i = -1;
    for (int i = 0; i < REPLAY_AT_COUNT; i++)
    {
        if (strcmp(at, replay_ev_at_name((en_REPLAY_EV_AT)i)) == 0) at_i = i;
    };
    if (at_i < 0) return "unknown \"at\"";
    ev->at = (en_REPLAY_EV_AT)at_i;

    guint64 v;
    switch (ev->kind)
    {
        case REPLAY_EV_KBD:
        {
            JsonNode *m = json_object_get_member(o, "m");
            if (!m || !JSON_NODE_HOLDS_ARRAY(m)) return "missing \"m\"";
            JsonArray *a = json_node_get_array(m);
            if (json_array_get_length(a) != REPLAY_KBD_COLS) return "\"m\" must have 10 items";
            for (guint i = 0; i < REPLAY_KBD_COLS; i++)
            {
                JsonNode *n = json_array_get_element(a, i);
                if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return "\"m\" item is not a number";
                gint64 x = json_node_get_int(n);
                if (x < 0 || x > 255) return "\"m\" item out of range";
                ev->kbd[i] = (uint8_t)x;
            };
            break;
        };
        case REPLAY_EV_JOY:
            if (!replay_json_get_uint(o, "p", REPLAY_JOY_PORTS - 1, &v)) return "missing or invalid \"p\"";
            ev->joy_port = (uint8_t)v;
            if (!replay_json_get_uint(o, "s", 255, &v)) return "missing or invalid \"s\"";
            ev->joy_state = (uint8_t)v;
            break;
        case REPLAY_EV_AUTOTYPE:
        {
            const char *t = replay_json_get_str(o, "text");
            if (!t) return "missing \"text\"";
            if (!replay_json_get_uint(o, "kd_ms", G_MAXUINT32, &v)) return "missing or invalid \"kd_ms\"";
            ev->kd_ms = (uint32_t)v;
            if (!replay_json_get_uint(o, "ku_ms", G_MAXUINT32, &v)) return "missing or invalid \"ku_ms\"";
            ev->ku_ms = (uint32_t)v;
            ev->text = g_strdup(t);
            break;
        };
        case REPLAY_EV_DBG:
        {
            const char *c = replay_json_get_str(o, "cmd");
            if (!c || !c[0]) return "missing \"cmd\"";
            JsonNode *args = json_object_get_member(o, "args");
            if (!args || !JSON_NODE_HOLDS_OBJECT(args)) return "missing \"args\" object";
            ev->cmd = g_strdup(c);
            ev->args_json = replay_json_node_to_string(args);
            break;
        };
        case REPLAY_EV_CMT:
        {
            const char *op = replay_json_get_str(o, "op");
            if (!op || !op[0]) return "missing \"op\"";
            if (!replay_json_get_int32(o, "v", &ev->arg)) return "missing or invalid \"v\"";
            const char *path = replay_json_get_str(o, "path");
            ev->cmd = g_strdup(op);
            ev->text = g_strdup(path);
            break;
        };
        case REPLAY_EV_CMTHACK:
        {
            if (!replay_json_get_int32(o, "media", &ev->arg) || ev->arg < -1) return "missing or invalid \"media\"";
            const char *name = replay_json_get_str(o, "name");
            ev->text = g_strdup(name ? name : "");
            break;
        };
        case REPLAY_EV_RESET:
        default:
            break;
    };
    return NULL;
}


bool replay_data_inputs_from_jsonl(st_REPLAY_DATA *d, const char *text, char **err)
{
    g_array_set_size(d->events, 0);
    d->has_debugger_events = false;

    char **lines = g_strsplit(text ? text : "", "\n", -1);
    bool ok = true;
    bool have_end = false;
    uint32_t last_frame = 0;
    JsonParser *parser = json_parser_new();

    for (int i = 0; lines[i] != NULL && ok; i++)
    {
        char *line = g_strstrip(lines[i]);
        if (line[0] == '\0') continue;
        if (have_end)
        {
            replay_set_err(err, "inputs.jsonl line %d: event after \"end\"", i + 1);
            ok = false;
            break;
        };
        GError *jerr = NULL;
        if (!json_parser_load_from_data(parser, line, -1, &jerr))
        {
            replay_set_err(err, "inputs.jsonl line %d: %s", i + 1, jerr ? jerr->message : "invalid JSON");
            g_clear_error(&jerr);
            ok = false;
            break;
        };
        JsonNode *root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        {
            replay_set_err(err, "inputs.jsonl line %d: not a JSON object", i + 1);
            ok = false;
            break;
        };
        st_REPLAY_EVENT ev;
        bool is_end = false;
        const char *perr = replay_event_from_json(json_node_get_object(root), &ev, &is_end);
        if (perr)
        {
            replay_event_clear(&ev);
            replay_set_err(err, "inputs.jsonl line %d: %s", i + 1, perr);
            ok = false;
            break;
        };
        if (ev.frame < last_frame)
        {
            replay_event_clear(&ev);
            replay_set_err(err, "inputs.jsonl line %d: frame %u is before frame %u", i + 1, ev.frame, last_frame);
            ok = false;
            break;
        };
        last_frame = ev.frame;
        if (is_end)
        {
            have_end = true;
            d->frames = ev.frame;
            continue;
        };
        /* Řetězce už jsou nově alokované - vložit bez další kopie. */
        g_array_append_val(d->events, ev);
        if (ev.kind == REPLAY_EV_DBG) d->has_debugger_events = true;
    };

    if (ok && !have_end)
    {
        replay_set_err(err, "inputs.jsonl: missing \"end\" line");
        ok = false;
    };

    g_object_unref(parser);
    g_strfreev(lines);
    if (!ok)
    {
        g_array_set_size(d->events, 0);
        d->has_debugger_events = false;
    };
    return ok;
}


/** @brief Text `hashes.csv` (hlavička + řádky). */
static char *replay_hashes_to_csv(const st_REPLAY_DATA *d)
{
    GString *out = g_string_new("frame,ticks");
    for (int p = 0; p < REPLAY_HASH_PART_COUNT; p++)
    {
        g_string_append_printf(out, ",%s", replay_hash_part_name((en_REPLAY_HASH_PART)p));
    };
    g_string_append_c(out, '\n');
    for (guint i = 0; i < d->hashes->len; i++)
    {
        const st_REPLAY_HASH_ROW *r = &g_array_index(d->hashes, st_REPLAY_HASH_ROW, i);
        g_string_append_printf(out, "%u,%" G_GUINT64_FORMAT, r->frame, r->ticks);
        for (int p = 0; p < REPLAY_HASH_PART_COUNT; p++)
        {
            g_string_append_printf(out, ",%s", r->part[p]);
        };
        g_string_append_c(out, '\n');
    };
    return g_string_free(out, FALSE);
}


/** @brief true = @p s je prázdný nebo 64 hex znaků malými písmeny. */
static bool replay_hash_hex_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0) return true;
    if (n != REPLAY_HASH_HEX_LEN) return false;
    for (size_t i = 0; i < n; i++)
    {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    };
    return true;
}


/**
 * @brief Načte `hashes.csv` (nahradí řádky). @return true = v pořádku.
 *
 * Sloupce se párují podle hlavičky: první dva musí být `frame,ticks`,
 * neznámý sloupec je chyba, chybějící část (starší záznam bez `periph`)
 * zůstane v řádcích prázdná.
 */
static bool replay_hashes_from_csv(st_REPLAY_DATA *d, const char *text, char **err)
{
    g_array_set_size(d->hashes, 0);
    char **lines = g_strsplit(text ? text : "", "\n", -1);
    bool ok = true;
    bool header = true;
    int col_part[2 + REPLAY_HASH_PART_COUNT]; /* sloupec -> část (-1 = frame / ticks) */
    guint ncols = 0;
    for (int i = 0; lines[i] != NULL && ok; i++)
    {
        char *line = g_strstrip(lines[i]);
        if (line[0] == '\0') continue;
        if (header)
        {
            header = false;
            char **h = g_strsplit(line, ",", -1);
            ncols = g_strv_length(h);
            if (ncols < 2 || ncols > G_N_ELEMENTS(col_part) || strcmp(h[0], "frame") != 0 || strcmp(h[1], "ticks") != 0)
            {
                replay_set_err(err, "hashes.csv: invalid header");
                ok = false;
            };
            bool seen[REPLAY_HASH_PART_COUNT] = { false };
            for (guint c = 2; ok && c < ncols; c++)
            {
                col_part[c] = -1;
                for (int p = 0; p < REPLAY_HASH_PART_COUNT; p++)
                {
                    if (strcmp(h[c], replay_hash_part_name((en_REPLAY_HASH_PART)p)) == 0) col_part[c] = p;
                };
                if (col_part[c] < 0 || seen[col_part[c]])
                {
                    replay_set_err(err, "hashes.csv: unknown or repeated column '%s'", h[c]);
                    ok = false;
                }
                else
                {
                    seen[col_part[c]] = true;
                };
            };
            g_strfreev(h);
            continue;
        };
        char **cols = g_strsplit(line, ",", -1);
        if (g_strv_length(cols) != ncols)
        {
            replay_set_err(err, "hashes.csv line %d: expected %u columns", i + 1, ncols);
            ok = false;
        }
        else
        {
            st_REPLAY_HASH_ROW r;
            memset(&r, 0, sizeof(r));
            char *end = NULL;
            guint64 f = g_ascii_strtoull(cols[0], &end, 10);
            bool num_ok = (end && *end == '\0' && cols[0][0] != '\0' && f <= G_MAXUINT32);
            r.frame = (uint32_t)f;
            r.ticks = g_ascii_strtoull(cols[1], &end, 10);
            num_ok = num_ok && end && *end == '\0' && cols[1][0] != '\0';
            for (guint c = 2; c < ncols && num_ok; c++)
            {
                if (!replay_hash_hex_valid(cols[c])) num_ok = false;
                else g_strlcpy(r.part[col_part[c]], cols[c], sizeof(r.part[col_part[c]]));
            };
            if (!num_ok)
            {
                replay_set_err(err, "hashes.csv line %d: invalid value", i + 1);
                ok = false;
            }
            else if (d->hashes->len
                     && r.frame <= g_array_index(d->hashes, st_REPLAY_HASH_ROW, d->hashes->len - 1).frame)
            {
                replay_set_err(err, "hashes.csv line %d: frames must increase", i + 1);
                ok = false;
            }
            else
            {
                g_array_append_val(d->hashes, r);
            };
        };
        g_strfreev(cols);
    };
    g_strfreev(lines);
    if (!ok) g_array_set_size(d->hashes, 0);
    return ok;
}


/** @brief Text `manifest.json` (odsazený). */
static char *replay_manifest_to_json(const st_REPLAY_DATA *d)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "format");
    json_builder_add_string_value(b, REPLAY_FORMAT_ID);
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, REPLAY_FORMAT_VERSION);
    json_builder_set_member_name(b, "emulator_version");
    json_builder_add_string_value(b, d->emulator_version ? d->emulator_version : "");
    json_builder_set_member_name(b, "platform");
    json_builder_add_string_value(b, d->platform ? d->platform : "");
    json_builder_set_member_name(b, "screen_ticks");
    json_builder_add_int_value(b, d->screen_ticks);
    json_builder_set_member_name(b, "created");
    json_builder_add_string_value(b, d->created ? d->created : "");
    json_builder_set_member_name(b, "description");
    if (d->description) json_builder_add_string_value(b, d->description);
    else json_builder_add_null_value(b);
    json_builder_set_member_name(b, "frames");
    json_builder_add_int_value(b, d->frames);
    json_builder_set_member_name(b, "hash_interval");
    json_builder_add_int_value(b, d->hash_interval);
    json_builder_set_member_name(b, "rerecord_count");
    json_builder_add_int_value(b, d->rerecord_count);
    json_builder_set_member_name(b, "has_debugger_events");
    json_builder_add_boolean_value(b, d->has_debugger_events);
    json_builder_set_member_name(b, "final_hash");
    json_builder_add_boolean_value(b, d->final_hash);
    json_builder_set_member_name(b, "warnings");
    json_builder_begin_array(b);
    for (guint i = 0; i < d->warnings->len; i++)
    {
        json_builder_add_string_value(b, (const char *)g_ptr_array_index(d->warnings, i));
    };
    json_builder_end_array(b);

    json_builder_set_member_name(b, "rtc_base");
    if (d->rtc_base) json_builder_add_string_value(b, d->rtc_base);
    else json_builder_add_null_value(b);

    json_builder_set_member_name(b, "joystick_types");
    json_builder_begin_array(b);
    for (int i = 0; i < REPLAY_JOY_PORTS; i++) json_builder_add_int_value(b, d->joy_type[i]);
    json_builder_end_array(b);

    /* Model sběrnice devbus (konfigurace, ne stav stroje): jiná politika
     * nebo model nebuzené sběrnice při přehrání dá na sporném portu jinou
     * hodnotu; přehrání při neshodě varuje. Chybějící = neuloženo. */
    if (d->devbus_conflict)
    {
        json_builder_set_member_name(b, "devbus_conflict");
        json_builder_add_string_value(b, d->devbus_conflict);
    };
    if (d->devbus_bus_idle)
    {
        json_builder_set_member_name(b, "devbus_bus_idle");
        json_builder_add_string_value(b, d->devbus_bus_idle);
    };

    if (d->media_known)
    {
        json_builder_set_member_name(b, "media");
        json_builder_begin_array(b);
        for (guint i = 0; i < d->media->len; i++)
        {
            const st_REPLAY_MEDIA *m = (const st_REPLAY_MEDIA *)g_ptr_array_index(d->media, i);
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "slot");
            json_builder_add_string_value(b, m->slot);
            json_builder_set_member_name(b, "name");
            json_builder_add_string_value(b, m->name ? m->name : "");
            json_builder_set_member_name(b, "size");
            json_builder_add_int_value(b, (gint64)m->size);
            json_builder_set_member_name(b, "sha256");
            json_builder_add_string_value(b, m->sha256);
            json_builder_set_member_name(b, "readonly");
            json_builder_add_boolean_value(b, m->readonly);
            json_builder_set_member_name(b, "embedded");
            json_builder_add_boolean_value(b, m->data != NULL);
            json_builder_end_object(b);
        };
        json_builder_end_array(b);
    };

    json_builder_set_member_name(b, "cmt");
    JsonNode *cmt = d->cmt_json ? json_from_string(d->cmt_json, NULL) : NULL;
    if (cmt && JSON_NODE_HOLDS_OBJECT(cmt))
    {
        json_builder_add_value(b, cmt);
    }
    else
    {
        if (cmt) json_node_unref(cmt);
        json_builder_add_null_value(b);
    };
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_indent(gen, 2);
    char *s = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_unref(root);
    g_object_unref(b);
    return s;
}


/** @brief Načte manifest do @p d. @return true = v pořádku. */
static bool replay_manifest_from_json(st_REPLAY_DATA *d, const char *text, char **err)
{
    JsonParser *parser = json_parser_new();
    GError *jerr = NULL;
    bool ok = false;
    if (!json_parser_load_from_data(parser, text, -1, &jerr))
    {
        replay_set_err(err, "manifest.json: %s", jerr ? jerr->message : "invalid JSON");
        g_clear_error(&jerr);
        g_object_unref(parser);
        return false;
    };
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
    {
        replay_set_err(err, "manifest.json: not a JSON object");
        g_object_unref(parser);
        return false;
    };
    JsonObject *o = json_node_get_object(root);
    const char *fmt = replay_json_get_str(o, "format");
    guint64 v = 0;
    if (!fmt || strcmp(fmt, REPLAY_FORMAT_ID) != 0)
    {
        replay_set_err(err, "manifest.json: not a %s file", REPLAY_FORMAT_ID);
    }
    else if (!replay_json_get_uint(o, "version", G_MAXUINT32, &v) || v != REPLAY_FORMAT_VERSION)
    {
        replay_set_err(err, "manifest.json: unsupported version (expected %d)", REPLAY_FORMAT_VERSION);
    }
    else
    {
        ok = true;
        const char *s;
        if ((s = replay_json_get_str(o, "emulator_version"))) d->emulator_version = g_strdup(s);
        if ((s = replay_json_get_str(o, "platform"))) d->platform = g_strdup(s);
        if ((s = replay_json_get_str(o, "created"))) d->created = g_strdup(s);
        if ((s = replay_json_get_str(o, "description"))) d->description = g_strdup(s);
        if (replay_json_get_uint(o, "screen_ticks", G_MAXUINT32, &v)) d->screen_ticks = (uint32_t)v;
        if (replay_json_get_uint(o, "hash_interval", G_MAXUINT32, &v) && v > 0) d->hash_interval = (uint32_t)v;
        if (replay_json_get_uint(o, "rerecord_count", G_MAXUINT32, &v)) d->rerecord_count = (uint32_t)v;
        JsonNode *fh = json_object_get_member(o, "final_hash");
        if (fh && JSON_NODE_HOLDS_VALUE(fh) && json_node_get_value_type(fh) == G_TYPE_BOOLEAN) d->final_hash = json_node_get_boolean(fh);
        JsonNode *w = json_object_get_member(o, "warnings");
        if (w && JSON_NODE_HOLDS_ARRAY(w))
        {
            JsonArray *a = json_node_get_array(w);
            for (guint i = 0; i < json_array_get_length(a); i++)
            {
                JsonNode *n = json_array_get_element(a, i);
                if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
                {
                    replay_data_add_warning(d, json_node_get_string(n));
                };
            };
        };
        if ((s = replay_json_get_str(o, "rtc_base"))) d->rtc_base = g_strdup(s);
        if ((s = replay_json_get_str(o, "devbus_conflict"))) d->devbus_conflict = g_strdup(s);
        if ((s = replay_json_get_str(o, "devbus_bus_idle"))) d->devbus_bus_idle = g_strdup(s);
        JsonNode *jt = json_object_get_member(o, "joystick_types");
        if (jt && JSON_NODE_HOLDS_ARRAY(jt))
        {
            JsonArray *a = json_node_get_array(jt);
            for (guint i = 0; i < json_array_get_length(a) && i < REPLAY_JOY_PORTS; i++)
            {
                JsonNode *n = json_array_get_element(a, i);
                if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64)
                {
                    gint64 t = json_node_get_int(n);
                    d->joy_type[i] = (t >= REPLAY_JOY_TYPE_UNKNOWN && t <= 255) ? (int)t : REPLAY_JOY_TYPE_UNKNOWN;
                };
            };
        };
        JsonNode *cmt = json_object_get_member(o, "cmt");
        if (cmt && JSON_NODE_HOLDS_OBJECT(cmt)) d->cmt_json = replay_json_node_to_string(cmt);
        JsonNode *media = json_object_get_member(o, "media");
        if (media && JSON_NODE_HOLDS_ARRAY(media))
        {
            /* Obsah vložených médií se načte z archivu až v replay_data_read();
             * tady jen seznam (vložená média s prázdnými daty a příznakem). */
            d->media_known = true;
            JsonArray *a = json_node_get_array(media);
            for (guint i = 0; ok && i < json_array_get_length(a); i++)
            {
                JsonNode *n = json_array_get_element(a, i);
                JsonObject *mo = (n && JSON_NODE_HOLDS_OBJECT(n)) ? json_node_get_object(n) : NULL;
                const char *slot = mo ? replay_json_get_str(mo, "slot") : NULL;
                const char *sha = mo ? replay_json_get_str(mo, "sha256") : NULL;
                guint64 size = 0;
                if (!mo || !replay_media_slot_valid(slot) || !replay_sha_hex_valid(sha)
                    || !replay_json_get_uint(mo, "size", G_MAXINT64, &size) || replay_data_find_media(d, slot))
                {
                    replay_set_err(err, "manifest.json: invalid media item %u", i);
                    ok = false;
                    break;
                };
                JsonNode *ro = json_object_get_member(mo, "readonly");
                JsonNode *em = json_object_get_member(mo, "embedded");
                bool readonly = ro && JSON_NODE_HOLDS_VALUE(ro) && json_node_get_value_type(ro) == G_TYPE_BOOLEAN && json_node_get_boolean(ro);
                bool embedded = em && JSON_NODE_HOLDS_VALUE(em) && json_node_get_value_type(em) == G_TYPE_BOOLEAN && json_node_get_boolean(em);
                int idx = replay_data_add_media(d, slot, replay_json_get_str(mo, "name"), NULL, size, sha, readonly);
                /* Značka "vložené, data přijdou z archivu": size bajtů alokuje čtení. */
                st_REPLAY_MEDIA *m = (st_REPLAY_MEDIA *)g_ptr_array_index(d->media, idx);
                if (embedded) m->data = (uint8_t *)g_malloc0(1);
            };
        };
    };
    g_object_unref(parser);
    return ok;
}


bool replay_data_write(const st_REPLAY_DATA *d, const char *path, char **err)
{
    if (!d || !path || !d->start_snapshot || !d->start_snapshot_size)
    {
        replay_set_err(err, "replay has no start snapshot");
        return false;
    };

    char *tmp = g_strdup_printf("%s.tmp", path);
    snapshot_io_t *io = snapshot_io_open_write(tmp, REPLAY_ZIP_COMPRESSION);
    if (!io)
    {
        replay_set_err(err, "cannot create '%s'", tmp);
        g_free(tmp);
        return false;
    };

    char *manifest = replay_manifest_to_json(d);
    char *inputs = replay_data_inputs_to_jsonl(d);
    char *hashes = replay_hashes_to_csv(d);
    bool ok = (snapshot_io_write_bin(io, REPLAY_ENTRY_MANIFEST, (const uint8_t *)manifest, strlen(manifest)) == SNAPSHOT_OK)
              && (snapshot_io_write_bin(io, REPLAY_ENTRY_START, d->start_snapshot, d->start_snapshot_size) == SNAPSHOT_OK)
              && (snapshot_io_write_bin(io, REPLAY_ENTRY_INPUTS, (const uint8_t *)inputs, strlen(inputs)) == SNAPSHOT_OK)
              && (snapshot_io_write_bin(io, REPLAY_ENTRY_HASHES, (const uint8_t *)hashes, strlen(hashes)) == SNAPSHOT_OK);
    for (guint i = 0; ok && i < d->media->len; i++)
    {
        const st_REPLAY_MEDIA *m = (const st_REPLAY_MEDIA *)g_ptr_array_index(d->media, i);
        /* Prázdné médium nemá entry (ZIP čtení prázdné entry neumí). */
        if (!m->data || m->size == 0) continue;
        char *entry = g_strdup_printf(REPLAY_ENTRY_MEDIA_PREFIX "%s.bin", m->slot);
        ok = snapshot_io_write_bin(io, entry, m->data, (size_t)m->size) == SNAPSHOT_OK;
        g_free(entry);
    };
    snapshot_io_close(io);
    g_free(manifest);
    g_free(inputs);
    g_free(hashes);

    if (!ok)
    {
        g_remove(tmp);
        replay_set_err(err, "cannot write '%s'", tmp);
        g_free(tmp);
        return false;
    };

    /* Windows: cíl je nutné před přejmenováním smazat (jako snapshot_save). */
    g_remove(path);
    if (g_rename(tmp, path) != 0)
    {
        g_remove(tmp);
        replay_set_err(err, "cannot rename '%s' to '%s'", tmp, path);
        g_free(tmp);
        return false;
    };
    g_free(tmp);
    return true;
}


/** @brief Přečte textový soubor z archivu jako řetězec s nulou (g_free). */
static char *replay_read_text(snapshot_io_t *io, const char *name)
{
    uint8_t *data = NULL;
    size_t size = 0;
    if (snapshot_io_read_bin(io, name, &data, &size) != SNAPSHOT_OK) return NULL;
    char *s = g_malloc(size + 1);
    if (size) memcpy(s, data, size);
    s[size] = '\0';
    g_free(data);
    return s;
}


st_REPLAY_DATA *replay_data_read(const char *path, char **err)
{
    if (!path)
    {
        replay_set_err(err, "no file name");
        return NULL;
    };
    snapshot_io_t *io = snapshot_io_open_read(path);
    if (!io)
    {
        replay_set_err(err, "cannot open '%s' (not a replay file?)", path);
        return NULL;
    };

    st_REPLAY_DATA *d = replay_data_new();
    bool ok = true;
    const char *entries[] = { REPLAY_ENTRY_MANIFEST, REPLAY_ENTRY_START, REPLAY_ENTRY_INPUTS, REPLAY_ENTRY_HASHES };
    for (size_t i = 0; i < G_N_ELEMENTS(entries) && ok; i++)
    {
        if (!snapshot_io_entry_exists(io, entries[i]))
        {
            replay_set_err(err, "'%s' is missing in '%s'", entries[i], path);
            ok = false;
        };
    };

    if (ok)
    {
        char *manifest = replay_read_text(io, REPLAY_ENTRY_MANIFEST);
        ok = manifest && replay_manifest_from_json(d, manifest, err);
        g_free(manifest);
    };
    if (ok)
    {
        ok = (snapshot_io_read_bin(io, REPLAY_ENTRY_START, &d->start_snapshot, &d->start_snapshot_size) == SNAPSHOT_OK)
             && d->start_snapshot_size > 0;
        if (!ok) replay_set_err(err, "cannot read '%s'", REPLAY_ENTRY_START);
    };
    if (ok)
    {
        char *inputs = replay_read_text(io, REPLAY_ENTRY_INPUTS);
        ok = inputs && replay_data_inputs_from_jsonl(d, inputs, err);
        g_free(inputs);
    };
    if (ok)
    {
        char *hashes = replay_read_text(io, REPLAY_ENTRY_HASHES);
        ok = hashes && replay_hashes_from_csv(d, hashes, err);
        g_free(hashes);
    };
    if (ok && d->hashes->len
        && g_array_index(d->hashes, st_REPLAY_HASH_ROW, d->hashes->len - 1).frame > d->frames)
    {
        replay_set_err(err, "hashes.csv: frame after the end of the replay");
        ok = false;
    };
    for (guint i = 0; ok && i < d->media->len; i++)
    {
        st_REPLAY_MEDIA *m = (st_REPLAY_MEDIA *)g_ptr_array_index(d->media, i);
        if (!m->data || m->size == 0) continue; /* prázdné médium entry nemá, značka stačí */
        g_free(m->data);
        m->data = NULL;
        char *entry = g_strdup_printf(REPLAY_ENTRY_MEDIA_PREFIX "%s.bin", m->slot);
        uint8_t *data = NULL;
        size_t size = 0;
        en_SNAPSHOT_RESULT r = snapshot_io_read_bin(io, entry, &data, &size);
        char sha[REPLAY_HASH_HEX_LEN + 1];
        if (r == SNAPSHOT_OK) replay_sha256_hex(data, size, sha);
        if (r != SNAPSHOT_OK || size != m->size || strcmp(sha, m->sha256) != 0)
        {
            replay_set_err(err, "'%s' is missing or damaged", entry);
            g_free(data);
            ok = false;
        }
        else
        {
            m->data = data ? data : (uint8_t *)g_malloc0(1);
        };
        g_free(entry);
    };
    snapshot_io_close(io);

    if (!ok)
    {
        replay_data_free(d);
        return NULL;
    };
    return d;
}
