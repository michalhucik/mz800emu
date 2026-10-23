/**
 * @file   replay_dbg.c
 * @brief  Zásahy debuggeru v záznamu vstupů (viz replay_dbg.h).
 */

#include "../mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <string.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include "replay_dbg.h"
#include "dbgapi_cmd_props.h"
#include "dbgapi_emu.h"
#include "dbgapi_regions.h"
#include "replay.h"
#include "trace/cputrack.h"


en_REPLAY_DBG_CLASS replay_dbg_classify(en_DBGAPI_CMD cmd)
{
    /* Třída je sloupec tabulky vlastností příkazů (dbgapi_cmd_props.c). */
    return (en_REPLAY_DBG_CLASS)dbgapi_cmd_props(cmd)->replay;
}


/** @brief Dvojice příkaz - jméno ve skupině RECORD. */
static const struct
{
    en_DBGAPI_CMD cmd;
    const char *name;
} s_record_names[] = {
    { DBGAPI_CMD_MEM_WRITE, "mem_write" },
    { DBGAPI_CMD_MEM_WRITE_CHECKED, "mem_write_checked" },
    { DBGAPI_CMD_MEM_WRITE_FORCE, "mem_write_force" },
    { DBGAPI_CMD_REGIONS_WRITE, "regions_write" },
    { DBGAPI_CMD_SET_REG, "set_reg" },
    { DBGAPI_CMD_SET_CPU_FLAGS, "set_cpu_flags" },
    { DBGAPI_CMD_IO_WRITE, "io_write" },
    { DBGAPI_CMD_IRQ_INJECT, "irq_inject" },
    { DBGAPI_CMD_NMI_INJECT, "nmi_inject" },
};


const char *replay_dbg_cmd_name(en_DBGAPI_CMD cmd)
{
    for (size_t i = 0; i < G_N_ELEMENTS(s_record_names); i++)
    {
        if (s_record_names[i].cmd == cmd) return s_record_names[i].name;
    };
    return NULL;
}


/** @brief Bajty jako hex řetězec malými písmeny (g_free). */
static char *replay_dbg_hex(const uint8_t *data, size_t len)
{
    char *s = g_malloc(len * 2 + 1);
    for (size_t i = 0; i < len; i++)
    {
        g_snprintf(s + i * 2, 3, "%02x", data[i]);
    };
    s[len * 2] = '\0';
    return s;
}


/** @brief Hex řetězec -> bajty (g_free). @return NULL = neplatný zápis. */
static uint8_t *replay_dbg_unhex(const char *hex, size_t *out_len)
{
    size_t n = hex ? strlen(hex) : 0;
    if (n == 0 || (n % 2) != 0) return NULL;
    uint8_t *out = g_malloc(n / 2);
    for (size_t i = 0; i < n / 2; i++)
    {
        int hi = g_ascii_xdigit_value(hex[i * 2]);
        int lo = g_ascii_xdigit_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
        {
            g_free(out);
            return NULL;
        };
        out[i] = (uint8_t)((hi << 4) | lo);
    };
    *out_len = n / 2;
    return out;
}


/** @brief Člen objektu jako celé číslo, @p def pokud chybí. */
static gint64 replay_dbg_int(JsonObject *o, const char *name, gint64 def)
{
    JsonNode *n = json_object_get_member(o, name);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return def;
    return json_node_get_int(n);
}


/** @brief Kompaktní text objektu z builderu (uvolní builder). */
static char *replay_dbg_finish(JsonBuilder *b)
{
    json_builder_end_object(b);
    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    char *s = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_unref(root);
    g_object_unref(b);
    return s;
}


/** @brief Přidá celočíselný člen. */
static void replay_dbg_put(JsonBuilder *b, const char *name, gint64 v)
{
    json_builder_set_member_name(b, name);
    json_builder_add_int_value(b, v);
}


/** @brief Přidá hex člen `data`. */
static void replay_dbg_put_data(JsonBuilder *b, const uint8_t *data, size_t len)
{
    char *hex = replay_dbg_hex(data, len);
    json_builder_set_member_name(b, "data");
    json_builder_add_string_value(b, hex);
    g_free(hex);
}


char *replay_dbg_args_to_json(en_DBGAPI_CMD cmd, const void *data_ptr)
{
    if (!replay_dbg_cmd_name(cmd)) return NULL;
    if (!data_ptr && cmd != DBGAPI_CMD_NMI_INJECT) return NULL;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    switch (cmd)
    {
        case DBGAPI_CMD_MEM_WRITE:
        case DBGAPI_CMD_MEM_WRITE_FORCE:
        {
            const st_DBGAPI_MEM_PARAM *p = (const st_DBGAPI_MEM_PARAM *)data_ptr;
            if (!p->buf || p->len == 0) break;
            replay_dbg_put(b, "addr", p->addr);
            replay_dbg_put_data(b, p->buf, p->len);
            break;
        };
        case DBGAPI_CMD_MEM_WRITE_CHECKED:
        {
            const st_DBGAPI_MEM_WRITE_CHECKED_PARAM *p = (const st_DBGAPI_MEM_WRITE_CHECKED_PARAM *)data_ptr;
            if (!p->data || p->length == 0) break;
            replay_dbg_put(b, "addr", p->addr);
            replay_dbg_put_data(b, p->data, p->length);
            break;
        };
        case DBGAPI_CMD_REGIONS_WRITE:
        {
            const st_DBGAPI_REGIONS_WRITE_PARAM *p = (const st_DBGAPI_REGIONS_WRITE_PARAM *)data_ptr;
            st_REGION_DESC desc;
            if (!p->data || p->len == 0 || !dbgapi_regions_get(p->region_id, &desc))
            {
                g_object_unref(b);
                return NULL;
            };
            replay_dbg_put(b, "kind", desc.kind);
            replay_dbg_put(b, "sub_id", desc.sub_id);
            replay_dbg_put(b, "offset", p->offset);
            replay_dbg_put_data(b, p->data, p->len);
            break;
        };
        case DBGAPI_CMD_SET_REG:
        {
            const st_DBGAPI_REG_PARAM *p = (const st_DBGAPI_REG_PARAM *)data_ptr;
            replay_dbg_put(b, "reg", p->reg_id);
            replay_dbg_put(b, "value", p->value);
            break;
        };
        case DBGAPI_CMD_SET_CPU_FLAGS:
        {
            const st_DBGAPI_CPU_FLAGS *p = (const st_DBGAPI_CPU_FLAGS *)data_ptr;
            replay_dbg_put(b, "update_mask", p->update_mask);
            replay_dbg_put(b, "iff1", p->iff1);
            replay_dbg_put(b, "iff2", p->iff2);
            replay_dbg_put(b, "im", p->im);
            replay_dbg_put(b, "i", p->i_reg);
            replay_dbg_put(b, "r", p->r_reg);
            break;
        };
        case DBGAPI_CMD_IO_WRITE:
        {
            const st_DBGAPI_IO_PARAM *p = (const st_DBGAPI_IO_PARAM *)data_ptr;
            replay_dbg_put(b, "port", p->port);
            replay_dbg_put(b, "value", p->value);
            break;
        };
        case DBGAPI_CMD_IRQ_INJECT:
        {
            const st_DBGAPI_IRQ_INJECT_PARAM *p = (const st_DBGAPI_IRQ_INJECT_PARAM *)data_ptr;
            json_builder_set_member_name(b, "source");
            if (p->source) json_builder_add_string_value(b, p->source);
            else json_builder_add_null_value(b);
            replay_dbg_put(b, "vector", p->vector);
            replay_dbg_put(b, "vector_valid", p->vector_valid);
            break;
        };
        case DBGAPI_CMD_NMI_INJECT:
        default:
            break;
    };
    return replay_dbg_finish(b);
}


/** @brief Nastaví *err na nový text, pokud err != NULL. */
static void replay_dbg_err(char **err, const char *msg)
{
    if (!err) return;
    g_free(*err);
    *err = g_strdup(msg);
}


bool replay_dbg_execute(const char *cmd_name, const char *args_json, char **err)
{
    en_DBGAPI_CMD cmd = DBGAPI_CMD_NONE;
    for (size_t i = 0; i < G_N_ELEMENTS(s_record_names); i++)
    {
        if (cmd_name && strcmp(cmd_name, s_record_names[i].name) == 0) cmd = s_record_names[i].cmd;
    };
    if (cmd == DBGAPI_CMD_NONE)
    {
        replay_dbg_err(err, "unknown debugger event");
        return false;
    };

    JsonParser *parser = json_parser_new();
    if (!args_json || !json_parser_load_from_data(parser, args_json, -1, NULL)
        || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    {
        g_object_unref(parser);
        replay_dbg_err(err, "invalid debugger event arguments");
        return false;
    };
    JsonObject *o = json_node_get_object(json_parser_get_root(parser));

    /* Parametry všech příkazů skupiny; použije se jen ten podle cmd. */
    st_DBGAPI_MEM_PARAM mem;
    st_DBGAPI_MEM_WRITE_CHECKED_PARAM checked;
    st_DBGAPI_REGIONS_WRITE_PARAM region;
    st_DBGAPI_REG_PARAM reg;
    st_DBGAPI_CPU_FLAGS flags;
    st_DBGAPI_IO_PARAM io;
    st_DBGAPI_IRQ_INJECT_PARAM irq;
    memset(&mem, 0, sizeof(mem));
    memset(&checked, 0, sizeof(checked));
    memset(&region, 0, sizeof(region));
    memset(&reg, 0, sizeof(reg));
    memset(&flags, 0, sizeof(flags));
    memset(&io, 0, sizeof(io));
    memset(&irq, 0, sizeof(irq));

    uint8_t *data = NULL;
    size_t data_len = 0;
    char *source = NULL;
    void *data_ptr = NULL;
    const char *fail = NULL;

    switch (cmd)
    {
        case DBGAPI_CMD_MEM_WRITE:
        case DBGAPI_CMD_MEM_WRITE_FORCE:
        case DBGAPI_CMD_MEM_WRITE_CHECKED:
        {
            JsonNode *dn = json_object_get_member(o, "data");
            data = replay_dbg_unhex(dn && JSON_NODE_HOLDS_VALUE(dn) ? json_node_get_string(dn) : NULL, &data_len);
            if (!data || data_len > 0x10000)
            {
                fail = "invalid memory data";
                break;
            };
            if (cmd == DBGAPI_CMD_MEM_WRITE_CHECKED)
            {
                checked.addr = (uint16_t)replay_dbg_int(o, "addr", 0);
                checked.length = (uint32_t)data_len;
                checked.data = data;
                data_ptr = &checked;
            }
            else
            {
                mem.addr = (uint16_t)replay_dbg_int(o, "addr", 0);
                mem.len = (uint32_t)data_len;
                mem.buf = data;
                data_ptr = &mem;
            };
            break;
        };
        case DBGAPI_CMD_REGIONS_WRITE:
        {
            JsonNode *dn = json_object_get_member(o, "data");
            data = replay_dbg_unhex(dn && JSON_NODE_HOLDS_VALUE(dn) ? json_node_get_string(dn) : NULL, &data_len);
            st_REGION_DESC desc;
            dbgapi_regions_refresh();
            if (!data)
            {
                fail = "invalid region data";
            }
            else if (!dbgapi_regions_find_kind((en_REGION_KIND)replay_dbg_int(o, "kind", -1),
                                               (int)replay_dbg_int(o, "sub_id", -1), &desc))
            {
                fail = "region of the recorded write does not exist";
            }
            else
            {
                region.region_id = desc.id;
                region.offset = (uint32_t)replay_dbg_int(o, "offset", 0);
                region.data = data;
                region.len = (uint32_t)data_len;
                data_ptr = &region;
            };
            break;
        };
        case DBGAPI_CMD_SET_REG:
            reg.reg_id = (uint8_t)replay_dbg_int(o, "reg", 0);
            reg.value = (uint16_t)replay_dbg_int(o, "value", 0);
            data_ptr = &reg;
            break;
        case DBGAPI_CMD_SET_CPU_FLAGS:
            flags.update_mask = (uint16_t)replay_dbg_int(o, "update_mask", 0);
            flags.iff1 = (uint8_t)replay_dbg_int(o, "iff1", 0);
            flags.iff2 = (uint8_t)replay_dbg_int(o, "iff2", 0);
            flags.im = (uint8_t)replay_dbg_int(o, "im", 0);
            flags.i_reg = (uint8_t)replay_dbg_int(o, "i", 0);
            flags.r_reg = (uint8_t)replay_dbg_int(o, "r", 0);
            data_ptr = &flags;
            break;
        case DBGAPI_CMD_IO_WRITE:
            io.port = (uint16_t)replay_dbg_int(o, "port", 0);
            io.value = (uint8_t)replay_dbg_int(o, "value", 0);
            data_ptr = &io;
            break;
        case DBGAPI_CMD_IRQ_INJECT:
        {
            JsonNode *sn = json_object_get_member(o, "source");
            if (sn && JSON_NODE_HOLDS_VALUE(sn) && json_node_get_value_type(sn) == G_TYPE_STRING)
            {
                source = g_strdup(json_node_get_string(sn));
            };
            irq.source = source;
            irq.vector = (uint8_t)replay_dbg_int(o, "vector", 0);
            irq.vector_valid = (uint8_t)replay_dbg_int(o, "vector_valid", 0);
            data_ptr = &irq;
            break;
        };
        case DBGAPI_CMD_NMI_INJECT:
        default:
            break;
    };

    bool ok = false;
    if (fail)
    {
        replay_dbg_err(err, fail);
    }
    else
    {
        st_DBGAPI_CMDRQ rq;
        memset(&rq, 0, sizeof(rq));
        rq.cmd = cmd;
        rq.cmd_origin = DBGAPI_CMD_ORIGIN_INTERNAL;
        rq.data_ptr = data_ptr;
        dbgapi_emu_dispatch(&rq);
        /* Přehrání běží na hranici snímku mimo drain dbgapi
         * (mzarch_main_dbgapi_dispatch_one()): zaznamenaný set_reg nebo
         * set_cpu_flags = synchronizační záznam cputrack v3. */
        cputrack_sync_check();
        ok = rq.success;
        if (cmd == DBGAPI_CMD_MEM_WRITE_CHECKED) ok = ok && checked.success;
        if (!ok) replay_dbg_err(err, "recorded debugger command failed");
    };

    g_free(data);
    g_free(source);
    g_object_unref(parser);
    return ok;
}


bool replay_dbg_cmt_op(en_DBGAPI_CMD cmd, const void *data_ptr, int *op, int32_t *arg, const char **path)
{
    *op = -1;
    *arg = 0;
    *path = NULL;
    if (!data_ptr) return false;
    switch (cmd)
    {
        case DBGAPI_CMD_CMT_TRANSPORT:
        {
            const st_DBGAPI_CMT_TRANSPORT_PARAM *p = (const st_DBGAPI_CMT_TRANSPORT_PARAM *)data_ptr;
            switch (p->action)
            {
                case DBGAPI_CMT_TRANSPORT_PLAY: *op = REPLAY_CMT_PLAY; break;
                case DBGAPI_CMT_TRANSPORT_PLAY_PAUSED: *op = REPLAY_CMT_PLAY_PAUSED; break;
                case DBGAPI_CMT_TRANSPORT_STOP: *op = REPLAY_CMT_STOP; break;
                case DBGAPI_CMT_TRANSPORT_PAUSE:
                    *op = REPLAY_CMT_PAUSE;
                    *arg = p->pause_value ? 1 : 0;
                    break;
                case DBGAPI_CMT_TRANSPORT_EJECT: *op = REPLAY_CMT_EJECT; break;
                default: return false;
            };
            return true;
        };
        case DBGAPI_CMD_CMT_TAPE_SEEK:
            *op = REPLAY_CMT_SEEK;
            *arg = (int32_t)((const st_DBGAPI_CMT_TAPE_SEEK_PARAM *)data_ptr)->block_id;
            return true;
        case DBGAPI_CMD_CMT_RECORD:
            *op = REPLAY_CMT_RECORD;
            *path = ((const st_DBGAPI_CMT_RECORD_PARAM *)data_ptr)->filepath;
            return true;
        default:
            break;
    };
    return false;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
