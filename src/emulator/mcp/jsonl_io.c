/*
 * File:   jsonl_io.c
 *
 * Implementace JSONL transportu pro MCP backend (V0.A.2 - core).
 *
 * Závisí na json-glib (`JsonParser`, `JsonNode`, `JsonObject`,
 * `JsonBuilder`, `JsonGenerator`) a GLib (`g_malloc`, `g_strdup`,
 * `GString`). Pro čtení řádku používáme vlastní reader přes `fgetc`
 * + `GString` (POSIX `getline` není v MSYS2 UCRT runtime dostupný).
 *
 * Implementace záměrně NENÍ port `ai2-mz800emu/mcp-emu/jsonl_io.c`
 * (= cJSON, jiná architektura, full-stack dispatch). Sdílí jen
 * koncept "1 řádek = 1 zpráva".
 *
 * Soubor je celý obalen guardem `MZ800EMU_CFG_MCP_SERVER_ENABLED`,
 * aby při buildu s `NO_MCP=1` zůstal prázdný (= žádné nedefinované
 * symboly, žádná závislost na json-glib pokud uživatel MCP vypnul).
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ---------------------------------------------------------------------------
 */

/*
 * Pro non-MCP buildy musíme nejprve načíst config header s
 * MZ800EMU_CFG_MCP_SERVER_ENABLED define-em. Pak teprve obalovat tělo.
 *
 * Pozn.: V testech (STANDALONE Unity test) se kompiluje tento soubor
 * přímo bez emulator config infrastruktury, takže pro testy se předává
 * `MZ800EMU_CFG_MCP_SERVER_ENABLED` jako `-D` flag (viz tests/mcp/
 * CMakeLists.txt). Z stejného důvodu se `emulator/mzarch/mzarch_config.h`
 * includuje jen pokud není v testovacím režimu.
 */
#ifndef MZ800EMU_MCP_TEST_BUILD
#include "../mzarch/mzcommon_config.h"
#endif

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED

#include "jsonl_io.h"

#include <glib.h>
#include <json-glib/json-glib.h>

#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------------ */
/* Opaque struct                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Interní reprezentace parsované JSONL zprávy.
 *
 * Drží JsonParser (vlastní celý JSON strom) + cache klasifikovaného
 * typu a root JsonObject pro rychlý přístup z getterů.
 *
 * Invarianty:
 *  - `parser != NULL` po úspěšném `jsonl_parse_line`
 *  - `root_obj` ukazuje do stromu vlastněného `parser` (neuvolňovat
 *    samostatně)
 *  - `type` je vždy validní hodnota z `en_JSONL_MSG_TYPE`
 */
struct st_JSONL_MESSAGE {
    JsonParser        *parser;    /**< Vlastník JSON stromu. */
    JsonObject        *root_obj;  /**< Root objekt (view do parser stromu). */
    en_JSONL_MSG_TYPE  type;      /**< Klasifikovaný typ zprávy. */
};


/* ------------------------------------------------------------------ */
/* Helpery - bezpečné čtení polí z JsonObject                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Bezpečně vrátí string hodnotu pole nebo default.
 *
 * Vrací default pokud pole chybí, je null, není JSON value, nebo
 * není string typu.
 */
static const char *jsonl_obj_str_or(JsonObject *obj, const char *key,
                                    const char *def) {
    if (!obj || !json_object_has_member(obj, key)) return def;
    JsonNode *node = json_object_get_member(obj, key);
    if (!node || json_node_is_null(node)) return def;
    if (json_node_get_node_type(node) != JSON_NODE_VALUE) return def;
    /* json_node_get_string vrací NULL pokud value není string. */
    const char *s = json_node_get_string(node);
    return s ? s : def;
}


/**
 * @brief Bezpečně vrátí int hodnotu pole nebo default.
 */
static gint64 jsonl_obj_int_or(JsonObject *obj, const char *key,
                               gint64 def) {
    if (!obj || !json_object_has_member(obj, key)) return def;
    JsonNode *node = json_object_get_member(obj, key);
    if (!node || json_node_is_null(node)) return def;
    if (json_node_get_node_type(node) != JSON_NODE_VALUE) return def;
    return json_node_get_int(node);
}


/**
 * @brief Bezpečně vrátí bool hodnotu pole nebo default.
 */
static gboolean jsonl_obj_bool_or(JsonObject *obj, const char *key,
                                  gboolean def) {
    if (!obj || !json_object_has_member(obj, key)) return def;
    JsonNode *node = json_object_get_member(obj, key);
    if (!node || json_node_is_null(node)) return def;
    if (json_node_get_node_type(node) != JSON_NODE_VALUE) return def;
    return json_node_get_boolean(node);
}


/**
 * @brief Klasifikuje typ zprávy podle pole `type` nebo přítomnosti
 *        klíčových polí. Vrací -1 pokud nelze určit (SCHEMA error).
 */
static int jsonl_classify(JsonObject *obj) {
    /* Primární klasifikace přes pole "type". */
    const char *type_str = jsonl_obj_str_or(obj, "type", NULL);
    if (type_str) {
        if (strcmp(type_str, "hello") == 0)         return JSONL_MSG_HELLO;
        if (strcmp(type_str, "event") == 0)         return JSONL_MSG_EVENT;
        if (strcmp(type_str, "trap") == 0)          return JSONL_MSG_TRAP;
        if (strcmp(type_str, "trap_response") == 0) return JSONL_MSG_TRAP_RESPONSE;
        if (strcmp(type_str, "request") == 0)       return JSONL_MSG_REQUEST;
        if (strcmp(type_str, "response") == 0)      return JSONL_MSG_RESPONSE;
        /* Neznámý type -> neumíme klasifikovat. */
        return -1;
    }

    /* Sekundární klasifikace přes pole-fingerprint. */
    gboolean has_req = json_object_has_member(obj, "req_id");
    gboolean has_cmd = json_object_has_member(obj, "cmd");
    gboolean has_success = json_object_has_member(obj, "success");
    gboolean has_trap = json_object_has_member(obj, "trap_id");
    gboolean has_action = json_object_has_member(obj, "action");

    if (has_req && has_cmd)     return JSONL_MSG_REQUEST;
    if (has_req && has_success) return JSONL_MSG_RESPONSE;
    if (has_trap && has_action) return JSONL_MSG_TRAP_RESPONSE;

    return -1;
}


/* ------------------------------------------------------------------ */
/* Parser API                                                          */
/* ------------------------------------------------------------------ */

en_JSONL_RESULT jsonl_parse_line(const char *line,
                                 st_JSONL_MESSAGE **out_msg) {
    if (!line || !out_msg) return JSONL_ERR_PARSE;
    *out_msg = NULL;

    /* Prázdný / whitespace-only řádek = parse error (caller filtruje). */
    {
        const char *p = line;
        while (*p && g_ascii_isspace((guchar) *p)) p++;
        if (*p == '\0') return JSONL_ERR_PARSE;
    }

    JsonParser *parser = json_parser_new();
    if (!parser) return JSONL_ERR_ALLOC;

    GError *err = NULL;
    if (!json_parser_load_from_data(parser, line, -1, &err)) {
        if (err) g_error_free(err);
        g_object_unref(parser);
        return JSONL_ERR_PARSE;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || json_node_get_node_type(root) != JSON_NODE_OBJECT) {
        g_object_unref(parser);
        return JSONL_ERR_SCHEMA;
    }

    JsonObject *obj = json_node_get_object(root);
    int classified = jsonl_classify(obj);
    if (classified < 0) {
        g_object_unref(parser);
        return JSONL_ERR_SCHEMA;
    }

    st_JSONL_MESSAGE *msg = g_new0(st_JSONL_MESSAGE, 1);
    msg->parser   = parser;
    msg->root_obj = obj;
    msg->type     = (en_JSONL_MSG_TYPE) classified;

    *out_msg = msg;
    return JSONL_OK;
}


void jsonl_msg_free(st_JSONL_MESSAGE *msg) {
    if (!msg) return;
    if (msg->parser) g_object_unref(msg->parser);
    g_free(msg);
}


/* ------------------------------------------------------------------ */
/* Getters                                                             */
/* ------------------------------------------------------------------ */

en_JSONL_MSG_TYPE jsonl_msg_get_type(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    return msg->type;
}


int64_t jsonl_msg_get_req_id(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    return (int64_t) jsonl_obj_int_or(msg->root_obj, "req_id", -1);
}


const char *jsonl_msg_get_cmd(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    return jsonl_obj_str_or(msg->root_obj, "cmd", NULL);
}


const char *jsonl_msg_get_topic(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    return jsonl_obj_str_or(msg->root_obj, "topic", NULL);
}


int64_t jsonl_msg_get_trap_id(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    return (int64_t) jsonl_obj_int_or(msg->root_obj, "trap_id", -1);
}


const char *jsonl_msg_get_action(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    return jsonl_obj_str_or(msg->root_obj, "action", NULL);
}


bool jsonl_msg_get_success(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    return jsonl_obj_bool_or(msg->root_obj, "success", FALSE) ? true : false;
}


const char *jsonl_msg_get_error(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    return jsonl_obj_str_or(msg->root_obj, "error", NULL);
}


int64_t jsonl_msg_get_timeout_ms(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    return (int64_t) jsonl_obj_int_or(msg->root_obj, "timeout_ms", -1);
}


void *jsonl_msg_get_data_node(const st_JSONL_MESSAGE *msg) {
    g_assert(msg != NULL);
    if (!json_object_has_member(msg->root_obj, "data")) return NULL;
    JsonNode *node = json_object_get_member(msg->root_obj, "data");
    if (!node || json_node_is_null(node)) return NULL;
    return (void *) node;
}


/* ------------------------------------------------------------------ */
/* Encoder helpery                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Serializuje JsonBuilder do single-line JSON stringu.
 *
 * Generator se přepne do non-pretty (= bez whitespace), takže
 * výstup je single-line vhodný pro JSONL framing. Vrácený string
 * je `g_strdup`-allocated (caller `free()` nebo `g_free()` - GLib na
 * MSYS2 i Linux používá stejný alokátor jako `malloc`).
 */
static char *jsonl_builder_to_line(JsonBuilder *builder) {
    JsonNode *root = json_builder_get_root(builder);
    if (!root) return NULL;

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, FALSE);

    gchar *out = json_generator_to_data(gen, NULL);

    g_object_unref(gen);
    json_node_free(root);

    return out;
}


/**
 * @brief Připojí pole `data` z opaque JsonNode * (s deep-copy).
 *
 * Builder API nemá přímé "add node value" pro objekty - musíme
 * získat hodnotu přes `json_builder_add_value` (přebírá ownership).
 * Proto kopírujeme node a předáme kopii.
 */
static void jsonl_builder_add_data_node(JsonBuilder *builder,
                                        const char *key,
                                        void *opaque_node) {
    if (!opaque_node) return;
    JsonNode *node = (JsonNode *) opaque_node;
    JsonNode *copy = json_node_copy(node);
    if (!copy) return;
    json_builder_set_member_name(builder, key);
    /* json_builder_add_value přebírá ownership node-u. */
    json_builder_add_value(builder, copy);
}


/* ------------------------------------------------------------------ */
/* Encoder API                                                         */
/* ------------------------------------------------------------------ */

char *jsonl_build_request(int64_t req_id, const char *cmd, void *data_node) {
    if (!cmd || cmd[0] == '\0') return NULL;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "request");

    json_builder_set_member_name(b, "req_id");
    json_builder_add_int_value(b, (gint64) req_id);

    json_builder_set_member_name(b, "cmd");
    json_builder_add_string_value(b, cmd);

    jsonl_builder_add_data_node(b, "data", data_node);

    json_builder_end_object(b);

    char *out = jsonl_builder_to_line(b);
    g_object_unref(b);
    return out;
}


char *jsonl_build_response(int64_t req_id, bool success,
                           void *data_node, const char *error_msg) {
    /* Při neúspěchu vyžadujeme error_msg (anglicky, viz CLAUDE.md). */
    if (!success && (!error_msg || error_msg[0] == '\0')) return NULL;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "response");

    json_builder_set_member_name(b, "req_id");
    json_builder_add_int_value(b, (gint64) req_id);

    json_builder_set_member_name(b, "success");
    json_builder_add_boolean_value(b, success ? TRUE : FALSE);

    if (success) {
        jsonl_builder_add_data_node(b, "data", data_node);
    } else {
        json_builder_set_member_name(b, "error");
        json_builder_add_string_value(b, error_msg);
    }

    json_builder_end_object(b);

    char *out = jsonl_builder_to_line(b);
    g_object_unref(b);
    return out;
}


char *jsonl_build_event(const char *topic, void *data_node) {
    if (!topic || topic[0] == '\0') return NULL;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "event");

    json_builder_set_member_name(b, "topic");
    json_builder_add_string_value(b, topic);

    jsonl_builder_add_data_node(b, "data", data_node);

    json_builder_end_object(b);

    char *out = jsonl_builder_to_line(b);
    g_object_unref(b);
    return out;
}


char *jsonl_build_trap(int64_t trap_id, const char *topic,
                       void *data_node, int timeout_ms) {
    if (!topic || topic[0] == '\0') return NULL;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "trap");

    json_builder_set_member_name(b, "trap_id");
    json_builder_add_int_value(b, (gint64) trap_id);

    json_builder_set_member_name(b, "topic");
    json_builder_add_string_value(b, topic);

    jsonl_builder_add_data_node(b, "data", data_node);

    json_builder_set_member_name(b, "timeout_ms");
    json_builder_add_int_value(b, (gint64) timeout_ms);

    json_builder_end_object(b);

    char *out = jsonl_builder_to_line(b);
    g_object_unref(b);
    return out;
}


char *jsonl_build_trap_response(int64_t trap_id, const char *action) {
    if (!action || action[0] == '\0') return NULL;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "trap_response");

    json_builder_set_member_name(b, "trap_id");
    json_builder_add_int_value(b, (gint64) trap_id);

    json_builder_set_member_name(b, "action");
    json_builder_add_string_value(b, action);

    json_builder_end_object(b);

    char *out = jsonl_builder_to_line(b);
    g_object_unref(b);
    return out;
}


char *jsonl_build_hello(const char *version,
                        void *capabilities_node,
                        const char *const *commands) {
    if (!version || version[0] == '\0') return NULL;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "hello");

    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, version);

    jsonl_builder_add_data_node(b, "capabilities", capabilities_node);

    /* Seznam commands - vždy prezent, případně prázdné pole. */
    json_builder_set_member_name(b, "commands");
    json_builder_begin_array(b);
    if (commands) {
        for (const char *const *p = commands; *p != NULL; ++p) {
            json_builder_add_string_value(b, *p);
        }
    }
    json_builder_end_array(b);

    json_builder_end_object(b);

    char *out = jsonl_builder_to_line(b);
    g_object_unref(b);
    return out;
}


/* ------------------------------------------------------------------ */
/* Stream I/O                                                          */
/* ------------------------------------------------------------------ */

en_JSONL_RESULT jsonl_read_line(FILE *fp, char **out_line) {
    if (!fp || !out_line) return JSONL_ERR_IO;
    *out_line = NULL;

    /*
     * Vlastní line reader přes `fgetc` + GString. POSIX `getline`
     * není v MSYS2 UCRT runtime dostupný (vyžaduje glibc-specific
     * deklaraci kterou UCRT `<stdio.h>` neexponuje), takže implementaci
     * postavíme nad portable building blocks. GString automaticky
     * realokuje buffer.
     *
     * Pomalejší než `getline` (per-byte read), ale JSONL řádky jsou
     * krátké (< 4 KB typicky), takže overhead je zanedbatelný proti
     * JSON parsing nákladu.
     */
    GString *acc = g_string_sized_new(256);
    if (!acc) return JSONL_ERR_ALLOC;

    int saw_any = 0;
    for (;;) {
        int c = fgetc(fp);
        if (c == EOF) {
            if (ferror(fp)) {
                g_string_free(acc, TRUE);
                return JSONL_ERR_IO;
            }
            /* EOF: pokud jsme něco načetli, vrátíme to jako poslední řádek
             * bez newline; jinak je to čistý EOF. */
            if (!saw_any) {
                g_string_free(acc, TRUE);
                return JSONL_ERR_EOF;
            }
            break;
        }
        saw_any = 1;
        if (c == '\n') break;
        /* CR ignorujeme, ať je vstup CRLF nebo CR-only - sjednotíme. */
        if (c == '\r') continue;
        g_string_append_c(acc, (char) c);
    }

    /* Předáme buffer jako malloc-compatible (caller `free()`). */
    *out_line = g_string_free(acc, FALSE);
    return JSONL_OK;
}


en_JSONL_RESULT jsonl_write_line(FILE *fp, const char *line) {
    if (!fp || !line) return JSONL_ERR_IO;

    if (fputs(line, fp) == EOF) return JSONL_ERR_IO;
    if (fputc('\n', fp) == EOF) return JSONL_ERR_IO;
    if (fflush(fp) != 0)        return JSONL_ERR_IO;

    return JSONL_OK;
}


#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
