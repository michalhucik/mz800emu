/**
 * @file   videorec_sidecar.c
 * @brief  Implementace sidecar modelu video záznamu (viz videorec_sidecar.h).
 *
 * @par Licence: GPLv3
 */

#include "videorec_sidecar.h"

#include <string.h>

/** @brief Jeden segment [start, end) s přechodem na začátku. */
typedef struct st_VIDEOREC_SEGMENT {
    uint64_t start;                 /**< První snímek. */
    uint64_t end;                   /**< Exkluzivní konec; u otevřeného segmentu průběžná hodnota. */
    en_VIDEOREC_TRANSITION tin;     /**< Přechod na začátku. */
} st_VIDEOREC_SEGMENT;

/** @brief Marker; vlastní kopii labelu (g_free). */
typedef struct st_VIDEOREC_MARKER {
    uint64_t frame;   /**< Globální index snímku. */
    char *label;      /**< Vlastněný popisek, nikdy NULL. */
} st_VIDEOREC_MARKER;

/** @brief Událost stavu (verze 3); vlastní kopie řetězců (g_free). */
typedef struct st_VIDEOREC_SC_EVENT {
    uint64_t frame; /**< Globální index snímku, od kterého stav platí. */
    char *kind;     /**< Vlastněný druh (VIDEOREC_SC_EVENT_*), nikdy NULL. */
    char *value;    /**< Vlastněná hodnota, nikdy NULL. */
} st_VIDEOREC_SC_EVENT;

/** @brief AVI part; vlastní kopii jména (g_free). */
typedef struct st_VIDEOREC_PART {
    char *file;            /**< Jméno souboru bez cesty. */
    uint64_t first_frame;  /**< Globální index prvního snímku partu. */
} st_VIDEOREC_PART;

/**
 * @brief Vnitřní stav modelu.
 * @invariant segments jsou seřazené a navazující; open => poslední segment je otevřený;
 *            platform != NULL <=> tv_system != NULL <=> popis platformy nastaven (JSON verze 4);
 *            markers a events seřazené podle frame (stejný frame v pořadí přidání),
 *            parts podle first_frame;
 *            první segment má tin == NONE.
 */
struct st_VIDEOREC_SIDECAR {
    unsigned width, height, fps_num, fps_den, audio_rate, transition_ms;
    en_VIDEOREC_TRANSITION default_transition;
    bool line_doubled;  /**< AVI má každý řádek framebufferu dvakrát (JSON pole `line_doubled`). */
    char *platform;     /**< Platforma (verze 4), vlastněná kopie; NULL = nenastaveno (verze 3). */
    char *tv_system;    /**< TV norma (verze 4), vlastněná kopie; NULL = nenastaveno. */
    unsigned fb_width, fb_height;                         /**< Nativní framebuffer (verze 4). */
    unsigned canvas_x, canvas_y, canvas_w, canvas_h;      /**< Canvas v souřadnicích framebufferu (verze 4). */
    GArray *segments;   /**< st_VIDEOREC_SEGMENT. */
    GArray *markers;    /**< st_VIDEOREC_MARKER. */
    GArray *parts;      /**< st_VIDEOREC_PART. */
    GArray *events;     /**< st_VIDEOREC_SC_EVENT (verze 3). */
    bool open;          /**< Poslední segment je otevřený. */
};

static const char *const TRANS_NAMES[] = { "none", "cut", "fade", "crossfade", "card" };
#define TRANS_COUNT ((int)(sizeof(TRANS_NAMES) / sizeof(TRANS_NAMES[0])))

const char *videorec_transition_name(en_VIDEOREC_TRANSITION t)
{
    return ((int)t >= 0 && (int)t < TRANS_COUNT) ? TRANS_NAMES[t] : TRANS_NAMES[0];
}

bool videorec_transition_from_name(const char *s, en_VIDEOREC_TRANSITION *out)
{
    for (int i = 0; i < TRANS_COUNT; i++) {
        if (strcmp(s, TRANS_NAMES[i]) == 0) {
            *out = (en_VIDEOREC_TRANSITION)i;
            return true;
        }
    }
    return false;
}

st_VIDEOREC_SIDECAR *videorec_sidecar_new(unsigned width, unsigned height, unsigned fps_num, unsigned fps_den,
                                          unsigned audio_rate, en_VIDEOREC_TRANSITION default_transition,
                                          unsigned transition_ms)
{
    st_VIDEOREC_SIDECAR *s = g_new0(st_VIDEOREC_SIDECAR, 1);
    s->width = width;
    s->height = height;
    s->fps_num = fps_num;
    s->fps_den = fps_den;
    s->audio_rate = audio_rate;
    s->default_transition = default_transition;
    s->transition_ms = transition_ms;
    s->segments = g_array_new(FALSE, FALSE, sizeof(st_VIDEOREC_SEGMENT));
    s->markers = g_array_new(FALSE, FALSE, sizeof(st_VIDEOREC_MARKER));
    s->parts = g_array_new(FALSE, FALSE, sizeof(st_VIDEOREC_PART));
    s->events = g_array_new(FALSE, FALSE, sizeof(st_VIDEOREC_SC_EVENT));
    return s;
}

void videorec_sidecar_set_line_doubled(st_VIDEOREC_SIDECAR *s, bool line_doubled)
{
    s->line_doubled = line_doubled;
}

void videorec_sidecar_set_platform(st_VIDEOREC_SIDECAR *s, const char *platform, const char *tv_system,
                                   unsigned fb_width, unsigned fb_height, unsigned canvas_x, unsigned canvas_y,
                                   unsigned canvas_w, unsigned canvas_h)
{
    g_free(s->platform);
    g_free(s->tv_system);
    s->platform = g_strdup(platform ? platform : "");
    s->tv_system = g_strdup(tv_system ? tv_system : "");
    s->fb_width = fb_width;
    s->fb_height = fb_height;
    s->canvas_x = canvas_x;
    s->canvas_y = canvas_y;
    s->canvas_w = canvas_w;
    s->canvas_h = canvas_h;
}

/** @brief Uvolní řetězce markerů od indexu `from` a pole zkrátí na `from` prvků. */
static void markers_truncate_from(st_VIDEOREC_SIDECAR *s, guint from)
{
    for (guint i = from; i < s->markers->len; i++) g_free(g_array_index(s->markers, st_VIDEOREC_MARKER, i).label);
    g_array_set_size(s->markers, from);
}

/** @brief Uvolní řetězce událostí od indexu `from` a pole zkrátí na `from` prvků. */
static void events_truncate_from(st_VIDEOREC_SIDECAR *s, guint from)
{
    for (guint i = from; i < s->events->len; i++) {
        st_VIDEOREC_SC_EVENT *e = &g_array_index(s->events, st_VIDEOREC_SC_EVENT, i);
        g_free(e->kind);
        g_free(e->value);
    }
    g_array_set_size(s->events, from);
}

/** @brief Uvolní jména partů od indexu `from` a pole zkrátí na `from` prvků. */
static void parts_truncate_from(st_VIDEOREC_SIDECAR *s, guint from)
{
    for (guint i = from; i < s->parts->len; i++) g_free(g_array_index(s->parts, st_VIDEOREC_PART, i).file);
    g_array_set_size(s->parts, from);
}

void videorec_sidecar_free(st_VIDEOREC_SIDECAR *s)
{
    if (!s) return;
    markers_truncate_from(s, 0);
    parts_truncate_from(s, 0);
    events_truncate_from(s, 0);
    g_array_free(s->events, TRUE);
    g_array_free(s->segments, TRUE);
    g_array_free(s->markers, TRUE);
    g_array_free(s->parts, TRUE);
    g_free(s->platform);
    g_free(s->tv_system);
    g_free(s);
}

void videorec_sidecar_add_part(st_VIDEOREC_SIDECAR *s, const char *filename, uint64_t first_frame)
{
    st_VIDEOREC_PART p = { g_strdup(filename ? filename : ""), first_frame };
    g_array_append_val(s->parts, p);
}

void videorec_sidecar_segment_end(st_VIDEOREC_SIDECAR *s, uint64_t frame)
{
    if (!s->open || s->segments->len == 0) return;
    guint last = s->segments->len - 1;
    st_VIDEOREC_SEGMENT *seg = &g_array_index(s->segments, st_VIDEOREC_SEGMENT, last);
    if (frame < seg->start) frame = seg->start; /* konec před začátkem = prázdný segment */
    seg->end = frame;
    s->open = false;
    if (seg->start == seg->end) g_array_remove_index(s->segments, last);
}

void videorec_sidecar_segment_begin(st_VIDEOREC_SIDECAR *s, uint64_t frame, en_VIDEOREC_TRANSITION transition_in)
{
    videorec_sidecar_segment_end(s, frame);
    st_VIDEOREC_SEGMENT seg = { frame, frame, s->segments->len == 0 ? VIDEOREC_TRANS_NONE : transition_in };
    g_array_append_val(s->segments, seg);
    s->open = true;
}

bool videorec_sidecar_segment_open(const st_VIDEOREC_SIDECAR *s)
{
    return s->open;
}

unsigned videorec_sidecar_segment_count(const st_VIDEOREC_SIDECAR *s)
{
    return s->segments->len;
}

void videorec_sidecar_add_marker(st_VIDEOREC_SIDECAR *s, uint64_t frame, const char *label)
{
    st_VIDEOREC_MARKER m = { frame, g_strdup(label ? label : "") };
    /* zařadit za poslední marker se snímkem <= frame (stabilní řazení) */
    guint at = s->markers->len;
    while (at > 0 && g_array_index(s->markers, st_VIDEOREC_MARKER, at - 1).frame > frame) at--;
    g_array_insert_val(s->markers, at, m);
}

void videorec_sidecar_add_event(st_VIDEOREC_SIDECAR *s, uint64_t frame, const char *kind, const char *value)
{
    st_VIDEOREC_SC_EVENT e = { frame, g_strdup(kind ? kind : ""), g_strdup(value ? value : "") };
    guint at = s->events->len;
    while (at > 0 && g_array_index(s->events, st_VIDEOREC_SC_EVENT, at - 1).frame > frame) at--;
    g_array_insert_val(s->events, at, e);
}

unsigned videorec_sidecar_event_count(const st_VIDEOREC_SIDECAR *s)
{
    return s->events->len;
}

void videorec_sidecar_truncate(st_VIDEOREC_SIDECAR *s, uint64_t frame)
{
    guint keep = 0;
    while (keep < s->markers->len && g_array_index(s->markers, st_VIDEOREC_MARKER, keep).frame < frame) keep++;
    markers_truncate_from(s, keep);

    keep = 0;
    while (keep < s->events->len && g_array_index(s->events, st_VIDEOREC_SC_EVENT, keep).frame < frame) keep++;
    events_truncate_from(s, keep);

    keep = s->parts->len ? 1 : 0;
    while (keep < s->parts->len && g_array_index(s->parts, st_VIDEOREC_PART, keep).first_frame < frame) keep++;
    parts_truncate_from(s, keep);

    keep = s->segments->len ? 1 : 0;
    while (keep < s->segments->len && g_array_index(s->segments, st_VIDEOREC_SEGMENT, keep).start < frame) keep++;
    g_array_set_size(s->segments, keep);
    if (keep > 0) {
        st_VIDEOREC_SEGMENT *last = &g_array_index(s->segments, st_VIDEOREC_SEGMENT, keep - 1);
        last->end = frame < last->start ? last->start : frame;
        s->open = true;
    } else {
        s->open = false;
    }
}

/** @brief Připojí JSON řetězec s escapováním (", \, \n, ostatní < 0x20 jako \u00XX). */
static void append_json_string(GString *g, const char *str)
{
    g_string_append_c(g, '"');
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        switch (*p) {
            case '"': g_string_append(g, "\\\""); break;
            case '\\': g_string_append(g, "\\\\"); break;
            case '\n': g_string_append(g, "\\n"); break;
            default:
                if (*p < 0x20) g_string_append_printf(g, "\\u%04x", *p);
                else g_string_append_c(g, (char)*p);
        }
    }
    g_string_append_c(g, '"');
}

char *videorec_sidecar_to_json(const st_VIDEOREC_SIDECAR *s)
{
    GString *g = g_string_new(NULL);
    bool v4 = (s->platform != NULL);
    g_string_append_printf(g, "{\n  \"version\": %d,\n", v4 ? 4 : 3);
    if (v4) {
        g_string_append(g, "  \"platform\": ");
        append_json_string(g, s->platform);
        g_string_append(g, ", \"tv_system\": ");
        append_json_string(g, s->tv_system);
        g_string_append(g, ",\n");
    }
    g_string_append_printf(g, "  \"width\": %u, \"height\": %u,\n  \"line_doubled\": %s,\n", s->width, s->height,
                           s->line_doubled ? "true" : "false");
    if (v4) {
        g_string_append_printf(g,
            "  \"framebuffer_width\": %u, \"framebuffer_height\": %u,\n"
            "  \"canvas\": { \"x\": %u, \"y\": %u, \"width\": %u, \"height\": %u },\n",
            s->fb_width, s->fb_height, s->canvas_x, s->canvas_y, s->canvas_w, s->canvas_h);
    }
    g_string_append_printf(g,
        "  \"fps_num\": %u, \"fps_den\": %u,\n"
        "  \"audio_rate\": %u,\n  \"default_transition\": \"%s\",\n  \"transition_ms\": %u,\n",
        s->fps_num, s->fps_den, s->audio_rate, videorec_transition_name(s->default_transition), s->transition_ms);

    g_string_append(g, "  \"parts\": [");
    for (guint i = 0; i < s->parts->len; i++) {
        const st_VIDEOREC_PART *p = &g_array_index(s->parts, st_VIDEOREC_PART, i);
        g_string_append(g, i ? ",\n    { \"file\": " : "\n    { \"file\": ");
        append_json_string(g, p->file);
        g_string_append_printf(g, ", \"first_frame\": %" G_GUINT64_FORMAT " }", p->first_frame);
    }
    g_string_append(g, s->parts->len ? "\n  ],\n" : "],\n");

    g_string_append(g, "  \"segments\": [");
    for (guint i = 0; i < s->segments->len; i++) {
        const st_VIDEOREC_SEGMENT *e = &g_array_index(s->segments, st_VIDEOREC_SEGMENT, i);
        g_string_append_printf(g,
            "%s\n    { \"start\": %" G_GUINT64_FORMAT ", \"end\": %" G_GUINT64_FORMAT ", \"transition_in\": \"%s\" }",
            i ? "," : "", e->start, e->end, videorec_transition_name(e->tin));
    }
    g_string_append(g, s->segments->len ? "\n  ],\n" : "],\n");

    g_string_append(g, "  \"markers\": [");
    for (guint i = 0; i < s->markers->len; i++) {
        const st_VIDEOREC_MARKER *m = &g_array_index(s->markers, st_VIDEOREC_MARKER, i);
        g_string_append_printf(g, "%s\n    { \"frame\": %" G_GUINT64_FORMAT ", \"label\": ", i ? "," : "", m->frame);
        append_json_string(g, m->label);
        g_string_append(g, " }");
    }
    g_string_append(g, s->markers->len ? "\n  ],\n" : "],\n");

    g_string_append(g, "  \"events\": [");
    for (guint i = 0; i < s->events->len; i++) {
        const st_VIDEOREC_SC_EVENT *e = &g_array_index(s->events, st_VIDEOREC_SC_EVENT, i);
        g_string_append_printf(g, "%s\n    { \"frame\": %" G_GUINT64_FORMAT ", \"kind\": ", i ? "," : "", e->frame);
        append_json_string(g, e->kind);
        g_string_append(g, ", \"value\": ");
        append_json_string(g, e->value);
        g_string_append(g, " }");
    }
    g_string_append(g, s->events->len ? "\n  ]\n}\n" : "]\n}\n");
    return g_string_free(g, FALSE);
}

bool videorec_sidecar_save(const st_VIDEOREC_SIDECAR *s, const char *path, GError **err)
{
    char *json = videorec_sidecar_to_json(s);
    gboolean ok = g_file_set_contents(path, json, -1, err);
    g_free(json);
    return ok;
}
