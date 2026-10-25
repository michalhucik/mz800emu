/**
 * @file   test_dispatch.c
 * @brief  Unity testy pro MCP dispatch vrstvu (V0.A.3).
 *
 * Pokrývá:
 *   - mcp_dispatch_request roundtrip pro všech dbgapi handlerů +
 *     lokální (ping, shutdown)
 *   - dbgapi origin propagation - každý handler musí poslat
 *     DBGAPI_CMD_ORIGIN_MCP
 *   - parametry mem_read/mem_write/bp_add - validace rozsahu
 *   - mem_write region check fail path (V0.B.3)
 *   - chybové cesty: unknown cmd, invalid params, dbgapi fail
 *   - mcp_dispatch_build_hello - validní JSONL HELLO řádek s 18 příkazy
 *   - mcp_dispatch_get_supported_commands - non-NULL pole 18 položek
 *     (V0.A.3: 9 jádrových handlerů + V0.A.4 "shutdown" pipe signál +
 *      V0.B.3 "mem_write" sensitive operace + V0.B.6 7 chybějících
 *      tools: bp_remove, bp_clear, bp_enable, step_into, step_over,
 *      step_n, run_until_addr)
 *
 * Standalone test - linkuje dispatch.c + jsonl_io.c + stub dbgapi
 * (test_dispatch_dbgapi_stub.c). Žádný emu core.
 *
 * @par Licence:
 * GPL-3.0-or-later. Viz licence header v dispatch.c.
 */

#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#include "emulator/mcp/dispatch.h"
#include "emulator/mcp/jsonl_io.h"
#include "emulator/mcp/cooperation.h"
#include "emulator/debugger/dbgapi_cmdrq.h"
#include "emulator/debugger/dbgapi_regions.h"
#include "emulator/debugger/trace/trace_reader.h"
#include "emulator/state_hash.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/hw-generic/gdg/gdgclk.h"
#include "emulator/audio.h"
/* Pozn.: NEvkládáme dbgapi_ui.h zde - ten taháno main.h ->
 * mzarch_config.h a v testovacím režimu by způsobil redefinition
 * warning na MZ800EMU_CFG_MCP_SERVER_ENABLED. Stub poskytuje signaturu
 * v test_dispatch_dbgapi_stub.h. */

#include "test_dispatch_dbgapi_stub.h"


/**
 * @brief Aktuální očekávaný počet entries v dispatch cmd_map[].
 *
 * Udržuje se ručně při každém přidání nové Tools sekce - test je
 * "checksum" že implementace, hello payload a tato hodnota souhlasí.
 *
 * Aktuální stav: V1.E.5 (= 135 z V1.E.4 commit 2 + 6 Eventlog/TLOG
 * Tools = eventlog_start, eventlog_stop, eventlog_clear,
 * eventlog_set_capacity, eventlog_set_mask, eventlog_get_event).
 * Mirror lookup je backed `watch_emu_cache` (= UI publish + dispatch
 * read s GMutex).
 *
 * BACKLOG D doplnil emulation speed control (= get_speed, set_speed)
 * = 145 + 2 = 147.
 *
 * BACKLOG B doplnil bookmark write (= bookmark_add, bookmark_remove)
 * = 147 + 2 = 149.
 *
 * CMT-A doplnil CMT transport + recording + cmthack toggle
 * (= cmt_transport, cmt_record, cmt_hack_set) = 149 + 3 = 152.
 *
 * CMT-B doplnil CMT vlastnosti + práce s páskou (= cmt_set_property,
 * cmt_open, cmt_tape_seek, cmt_tape_block_speed, cmt_tape_list)
 * = 152 + 5 = 157.
 *
 * mzdos request 0009 doplnil screenshot_save_to_file (server-side PNG
 * zápis na disk, přidán na KONEC cmd_map[]) = 157 + 1 = 158.
 *
 * 0017 FÁZE 1 doplnil 4 Tracking lifecycle Tools (= trace_start,
 * trace_stop, trace_reset, trace_save, přidány na KONEC cmd_map[])
 * = 158 + 4 = 162.
 *
 * video-capture Task 15 doplnil 5 Tools video záznamu (= videorec_start,
 * videorec_stop, videorec_pause, videorec_marker, videorec_status,
 * přidány na KONEC cmd_map[]) = 162 + 5 = 167.
 *
 * video-capture Task 18 doplnil videorec_timebase (přepnutí časové základny
 * emulated / realtime, přidán na KONEC cmd_map[]) = 167 + 1 = 168.
 *
 * mcp-run-mzf-bootstrap doplnil media_run_mzf (reset + bootstrap jako
 * --run-mzf, přidán na KONEC cmd_map[]) = 168 + 1 = 169.
 *
 * screenshot-series doplnil screenshot_series_start, _stop a _status
 * (série PNG emulovaných snímků, na KONEC cmd_map[]) = 169 + 3 = 172.
 *
 * pc-stats doplnil profiler_pc_start, _stop, _reset, _get a _export
 * (profiler podle PC, na KONEC cmd_map[]) = 172 + 5 = 177.
 *
 * audio-capture doplnil audio_capture_start, _stop a _status (zachycení
 * zvuku do složky, na KONEC cmd_map[]) = 177 + 3 = 180.
 *
 * profiler-bitmap doplnil profiler_pc_bitmap (vykonané adresy jako
 * bitmapa, na KONEC cmd_map[]) = 180 + 1 = 181.
 *
 * mcp-trace-read doplnil trace_status a trace_read (stránkované čtení
 * Trace Suite, za trace_save) = 181 + 2 = 183.
 *
 * mcp-trace-read (část B) doplnil audio_capture_summary a audio_capture_pcm
 * (paměť zachycení zvuku, za audio_capture_status) = 183 + 2 = 185.
 *
 * ramlog doplnil ramlog_start/stop/clear/status/who_wrote/writers (záznam
 * zápisů do paměti, za trace_read) = 185 + 6 = 191.
 *
 * memsearch doplnil freeze_add/remove/list a memsearch_start/filter/sample/
 * undo/redo/list/eliminate/close (Freeze Bytes a postupné hledání proměnných,
 * na KONEC cmd_map[]) = 191 + 11 = 202.
 *
 * run-control doplnil wait_stop a get_stop_history (čekání na zastavení
 * běhu a historie zastavení, na KONEC cmd_map[]) = 202 + 2 = 204, a
 * frame_hash (SHA-256 obrazu a VRAM, na KONEC cmd_map[]) = 204 + 1 = 205.
 * replay-core doplnil replay_record_start, replay_record_stop, replay_play,
 * replay_play_stop a replay_status (záznam a přehrání vstupů, na KONEC
 * cmd_map[]) = 205 + 5 = 210.
 * replay-rewind doplnil replay_rewind a replay_seek (checkpointy záznamu
 * vstupů, na KONEC cmd_map[]) = 210 + 2 = 212.
 * observe-cas doplnil state_observe (registry a okna paměti z jednoho
 * okamžiku, na KONEC cmd_map[]) = 212 + 1 = 213.
 * coverage doplnil coverage_start, _stop, _reset, _status, _mark, _read
 * a _export (pokrytí kódu, na KONEC cmd_map[]) = 213 + 7 = 220.
 * bp-log-mcp doplnil bplog_clear (vyčištění ringu výstupu akcí BP, na
 * KONEC cmd_map[]) = 220 + 1 = 221.
 * replay-goto doplnil replay_goto (stav v čase přehrávaného záznamu, na
 * KONEC cmd_map[]) = 221 + 1 = 222.
 * machine-hash doplnil machine_hash (obsahový otisk stroje po částech, na
 * KONEC cmd_map[]) = 222 + 1 = 223.
 */
#define MCP_EXPECTED_CMD_COUNT 223


/* ====================================================================== */
/* Pomocné funkce                                                          */
/* ====================================================================== */

/**
 * @brief Sestaví REQUEST z JSON literálu, vrátí parsed st_JSONL_MESSAGE.
 *
 * Pro testy které potřebují REQUEST objekt pro mcp_dispatch_request.
 * Caller uvolní `jsonl_msg_free`.
 */
static st_JSONL_MESSAGE *_make_request(const char *line) {
    st_JSONL_MESSAGE *msg = NULL;
    en_JSONL_RESULT rc = jsonl_parse_line(line, &msg);
    TEST_ASSERT_EQUAL_INT(JSONL_OK, rc);
    TEST_ASSERT_NOT_NULL(msg);
    TEST_ASSERT_EQUAL_INT(JSONL_MSG_REQUEST, jsonl_msg_get_type(msg));
    return msg;
}


/**
 * @brief Parsuje RESPONSE řádek vrácený dispatcherem.
 *
 * Pomocí json-glib přímo (st_JSONL_MESSAGE parser by ho také zpracoval,
 * ale tady chceme přímý view do JsonObject pro hluboké ověření polí).
 */
static JsonObject *_parse_response_object(const char *line, JsonParser **out_parser) {
    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    gboolean ok = json_parser_load_from_data(parser, line, -1, &err);
    if (!ok) {
        fprintf(stderr, "JSON parse failed: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        g_object_unref(parser);
        *out_parser = NULL;
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(JSON_NODE_OBJECT, json_node_get_node_type(root));
    *out_parser = parser;
    return json_node_get_object(root);
}


/* ====================================================================== */
/* Unity setup / teardown                                                  */
/* ====================================================================== */

void setUp(void) {
    dispatch_stub_reset();
    /* V1.B.3: dispatch nyní vyžaduje init pro alokaci dynamického
     * supported_commands seznamu. Funkce je idempotentní (= druhé
     * volání je no-op), takže opakované volání mezi testy je bezpečné. */
    mcp_dispatch_init();
}


void tearDown(void) {
    /* Nevoláme mcp_dispatch_shutdown - chceme aby supported_commands
     * pole přežilo přes všechny testy (idempotence + výkon). Cleanup
     * proběhne při exit (alokace má ohraničený růst). */
}


/* ====================================================================== */
/* Tests - 9 handlerů (success path)                                       */
/* ====================================================================== */

void test_ping_handler_returns_pong(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":1,\"cmd\":\"ping\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    /* ping je lokální handler - žádné dbgapi volání. */
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(obj, "req_id"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "pong"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_get_state_running_true(void) {
    g_stub_state.is_running = true;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":2,\"cmd\":\"get_state\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_IS_RUNNING, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "running"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "paused"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_pause_handler_uses_origin_mcp(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":3,\"cmd\":\"pause\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_PAUSE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "paused"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_run_handler_uses_origin_mcp(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":4,\"cmd\":\"run\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_RUN, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);

    free(resp);
    jsonl_msg_free(req);
}


void test_reset_handler_uses_origin_mcp(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":5,\"cmd\":\"reset\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_RESET, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);

    free(resp);
    jsonl_msg_free(req);
}


void test_get_registers_returns_14_fields(void) {
    g_stub_state.fill_regs = true;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":6,\"cmd\":\"get_registers\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_GET_ALL_REGS, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    /* Stub naplnil regs[i] = 0x1000 + (i << 8) - tj. AF=0x1000, BC=0x1100,
     * ..., IR=0x1D00. Ověříme pár klíčových polí. */
    TEST_ASSERT_EQUAL_INT(0x1000, json_object_get_int_member(data, "AF"));
    TEST_ASSERT_EQUAL_INT(0x1100, json_object_get_int_member(data, "BC"));
    TEST_ASSERT_EQUAL_INT(0x1A00, json_object_get_int_member(data, "SP"));
    TEST_ASSERT_EQUAL_INT(0x1B00, json_object_get_int_member(data, "PC"));
    TEST_ASSERT_EQUAL_INT(0x1D00, json_object_get_int_member(data, "IR"));
    /* Všech 14 polí přítomno */
    TEST_ASSERT_TRUE(json_object_has_member(data, "AF_"));
    TEST_ASSERT_TRUE(json_object_has_member(data, "HL_"));
    TEST_ASSERT_TRUE(json_object_has_member(data, "WZ"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_mem_read_returns_data_b64(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":7,\"cmd\":\"mem_read\","
        "\"data\":{\"addr\":4096,\"len\":4}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_MEM_READ, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(4096, json_object_get_int_member(data, "addr"));
    TEST_ASSERT_EQUAL_INT(4, json_object_get_int_member(data, "len"));
    const char *b64 = json_object_get_string_member(data, "data_b64");
    TEST_ASSERT_NOT_NULL(b64);
    /* Stub naplnil 0xA0, 0xA1, 0xA2, 0xA3 -> base64 "oKGio0==" wait,
     * let's just test it's non-empty and decodes ke 4 bytům. */
    gsize out_len = 0;
    guchar *decoded = g_base64_decode(b64, &out_len);
    TEST_ASSERT_EQUAL_INT(4, out_len);
    TEST_ASSERT_EQUAL_UINT8(0xA0, decoded[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA1, decoded[1]);
    TEST_ASSERT_EQUAL_UINT8(0xA2, decoded[2]);
    TEST_ASSERT_EQUAL_UINT8(0xA3, decoded[3]);
    g_free(decoded);
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_mem_write_writes_bytes_and_returns_length(void) {
    /* hex "DEADBEEF" = 4 bytes 0xDE 0xAD 0xBE 0xEF, addr=0x4000 */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":71,\"cmd\":\"mem_write\","
        "\"data\":{\"addr\":16384,\"data_hex\":\"DEADBEEF\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_MEM_WRITE_CHECKED, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);

    /* Ověříme že dispatch.c skutečně dekódoval hex.
     * Dispatch.c po _submit_dbgapi() bytes ihned uvolní (= p->data v
     * last_data je freed), takže obsah ověřujeme přes zachycenou kopii
     * v stubu (mem_write_buf, naplněno v handleru CMD_MEM_WRITE_CHECKED). */
    TEST_ASSERT_EQUAL_UINT16(0x4000, g_stub_state.mem_write_addr);
    TEST_ASSERT_EQUAL_UINT16(4, g_stub_state.mem_write_buf_len);
    TEST_ASSERT_EQUAL_UINT8(0xDE, g_stub_state.mem_write_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAD, g_stub_state.mem_write_buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xBE, g_stub_state.mem_write_buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0xEF, g_stub_state.mem_write_buf[3]);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(16384, json_object_get_int_member(data, "addr"));
    TEST_ASSERT_EQUAL_INT(4, json_object_get_int_member(data, "length"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_mem_write_region_check_fail(void) {
    /* Stub simuluje region fail na adrese 0x0000 (= ROM_LOW). */
    g_stub_state.mem_write_fail      = true;
    g_stub_state.mem_write_fail_addr = 0x0000;
    g_stub_state.mem_write_fail_kind = 1; /* libovolný non-zero */

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":72,\"cmd\":\"mem_write\","
        "\"data\":{\"addr\":0,\"data_hex\":\"AABB\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(resp);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    const char *err = json_object_get_string_member(obj, "error");
    TEST_ASSERT_NOT_NULL(err);
    /* Wire error text je anglicky a obsahuje adresu. */
    TEST_ASSERT_NOT_NULL(strstr(err, "MEM_WRITE region check failed"));
    TEST_ASSERT_NOT_NULL(strstr(err, "0x0000"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_mem_write_invalid_hex_rejected(void) {
    /* Lichý počet hex znaků => parser musí odmítnout */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":73,\"cmd\":\"mem_write\","
        "\"data\":{\"addr\":4096,\"data_hex\":\"DEAD0\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    /* dbgapi se vůbec nemělo volat */
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


void test_mem_write_overflow_rejected(void) {
    /* addr + decoded > 0x10000 => odmítnout */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":74,\"cmd\":\"mem_write\","
        "\"data\":{\"addr\":65535,\"data_hex\":\"AABB\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief mem_write a mem_write_force s celými 64 KiB (addr 0, 65536 bajtů)
 *        předají jádru délku 65536 a odpověď ji vrátí.
 *
 * Dřív se délka ukládala do uint16_t a 65536 přeteklo na 0: mem_write
 * hlásil chybu kontroly regionu, mem_write_force ohlásil úspěch
 * a nezapsal nic. O jeden bajt víc (addr 1) se odmítne.
 */
void test_mem_write_full_64k_length(void) {
    static const char *const cmds[] = { "mem_write", "mem_write_force" };
    GString *hex = g_string_sized_new(2 * 0x10000 + 2);
    for (int i = 0; i < 0x10000; i++) g_string_append(hex, "5A");

    for (size_t k = 0; k < G_N_ELEMENTS(cmds); k++) {
        dispatch_stub_reset();
        gchar *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":75,\"cmd\":\"%s\","
                                      "\"data\":{\"addr\":0,\"data_hex\":\"%s\"}}",
                                      cmds[k], hex->str);
        st_JSONL_MESSAGE *req = _make_request(line);
        char *resp = NULL;
        en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_OK, rc, cmds[k]);
        uint32_t req_len = (k == 0) ? g_stub_state.mem_write_req_len : g_stub_state.mwf_req_len;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0x10000, req_len, cmds[k]);

        JsonParser *parser = NULL;
        JsonObject *obj = _parse_response_object(resp, &parser);
        JsonObject *data = json_object_get_object_member(obj, "data");
        TEST_ASSERT_EQUAL_INT(65536, json_object_get_int_member(data, "length"));
        g_object_unref(parser);
        free(resp);
        jsonl_msg_free(req);
        g_free(line);

        /* addr 1 + 65536 bajtů přesahuje 0x10000 => odmítnout. */
        dispatch_stub_reset();
        line = g_strdup_printf("{\"type\":\"request\",\"req_id\":76,\"cmd\":\"%s\","
                               "\"data\":{\"addr\":1,\"data_hex\":\"%s\"}}",
                               cmds[k], hex->str);
        req = _make_request(line);
        resp = NULL;
        rc = mcp_dispatch_request(req, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, rc, cmds[k]);
        TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
        free(resp);
        jsonl_msg_free(req);
        g_free(line);
    }
    g_string_free(hex, TRUE);
}


void test_bp_add_returns_assigned_id(void) {
    g_stub_state.fill_bp_id = 42;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":8,\"cmd\":\"bp_add\","
        "\"data\":{\"addr\":12288}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_BP_ADD, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(42, json_object_get_int_member(data, "id"));
    TEST_ASSERT_EQUAL_INT(12288, json_object_get_int_member(data, "addr"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_bp_list_returns_array(void) {
    g_stub_state.fill_bp_list_count = 3;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":9,\"cmd\":\"bp_list\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_BP_LIST, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(3, json_object_get_int_member(data, "count"));
    JsonArray *arr = json_object_get_array_member(data, "breakpoints");
    TEST_ASSERT_EQUAL_INT(3, json_array_get_length(arr));
    /* První BP: id=100, addr=0x1000, enabled=true */
    JsonObject *bp0 = json_array_get_object_element(arr, 0);
    TEST_ASSERT_EQUAL_INT(100, json_object_get_int_member(bp0, "id"));
    TEST_ASSERT_EQUAL_INT(0x1000, json_object_get_int_member(bp0, "addr"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(bp0, "enabled"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* F015b - typed BP create + bp_list serializace + leak smoke             */
/* ====================================================================== */

/*
 * Typed BP přes bp_create_with_init musí předat backendu (dbgapi) správný
 * en_BPT_TYPE a UM_TYPE bit + případný condition expr. type=2 = BPT_TYPE_MEM_W
 * (breakpoints.h:103), což je přesně cesta, kterou Python emu_bp_add(memw)
 * remapuje na bp_create_with_init (F015a). Ověřujeme zachycení ve stubu, ne
 * jen "vrátilo OK".
 */
void test_bp_create_typed_passes_type_to_dbgapi(void) {
    g_stub_state.bp_create_fake_id = 7;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":20,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"addr\",\"expr\"],"
        "\"type\":2,\"addr\":4660,\"expr\":\"A==1\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_BP_CREATE_WITH_INIT,
                          g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    /* Backend dostal typed parametry, ne addr-only. */
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.bp_create_calls);
    TEST_ASSERT_EQUAL_UINT8(2, g_stub_state.bp_create_last_type); /* MEM_W */
    TEST_ASSERT_EQUAL_UINT16(4660, g_stub_state.bp_create_last_addr);
    TEST_ASSERT_NOT_NULL(g_stub_state.bp_create_last_expr);
    TEST_ASSERT_EQUAL_STRING("A==1", g_stub_state.bp_create_last_expr);
    /* update_mask musí mít TYPE + ADDR + EXPR bity (= fields[] respektováno). */
    TEST_ASSERT_TRUE(g_stub_state.bp_create_last_mask & DBGAPI_BP_UM_TYPE);
    TEST_ASSERT_TRUE(g_stub_state.bp_create_last_mask & DBGAPI_BP_UM_ADDR);
    TEST_ASSERT_TRUE(g_stub_state.bp_create_last_mask & DBGAPI_BP_UM_EXPR);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(7, json_object_get_int_member(data, "id"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "created"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/*
 * bp_list musí serializovat type/zone přes kanonické UPPER_SNAKE řetězce a
 * vrátit bank_id/hits/condition. Stub naplní bp[0] typed atributy + condition.
 * (type=2 -> "MEM_W", zone=3 -> "RAM" - viz breakpoints.c s_bpt_type_names /
 * s_bp_zone_names.)
 */
void test_bp_list_serializes_typed_fields(void) {
    g_stub_state.fill_bp_list_count = 1;
    g_stub_state.bp_list_fake_type      = 2;   /* MEM_W */
    g_stub_state.bp_list_fake_zone      = 3;   /* RAM */
    g_stub_state.bp_list_fake_bank_id   = 5;
    g_stub_state.bp_list_fake_hits      = 42;
    g_stub_state.bp_list_fake_condition = "B>0x10";
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":21,\"cmd\":\"bp_list\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(data, "count"));
    JsonArray *arr = json_object_get_array_member(data, "breakpoints");
    JsonObject *bp0 = json_array_get_object_element(arr, 0);
    TEST_ASSERT_EQUAL_STRING("MEM_W",
        json_object_get_string_member(bp0, "type"));
    TEST_ASSERT_EQUAL_STRING("RAM",
        json_object_get_string_member(bp0, "zone"));
    TEST_ASSERT_EQUAL_INT(5, json_object_get_int_member(bp0, "bank_id"));
    TEST_ASSERT_EQUAL_INT(42, json_object_get_int_member(bp0, "hits"));
    TEST_ASSERT_EQUAL_STRING("B>0x10",
        json_object_get_string_member(bp0, "condition"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/*
 * bp_list musí vracet addr_end, addr_match_mode (string) a addr_mask - bez
 * nich nešlo poznat, že BP s addr_end v režimu SINGLE hlídá jen addr.
 */
void test_bp_list_serializes_addr_match_fields(void) {
    g_stub_state.fill_bp_list_count = 2;
    g_stub_state.bp_list_fake_type            = 2;   /* MEM_W */
    g_stub_state.bp_list_fake_addr_end        = 0x10FF;
    g_stub_state.bp_list_fake_addr_match_mode = 1;   /* RANGE */
    g_stub_state.bp_list_fake_addr_mask       = 0xFFFF;
    g_stub_state.bp_list_fake_bank_id_end     = 6;
    g_stub_state.bp_list_fake_bank_match_mode = 2;   /* MASK */
    g_stub_state.bp_list_fake_bank_id_mask    = 0xFE;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":23,\"cmd\":\"bp_list\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    JsonArray *arr = json_object_get_array_member(data, "breakpoints");
    JsonObject *bp0 = json_array_get_object_element(arr, 0);
    TEST_ASSERT_EQUAL_INT(0x10FF, json_object_get_int_member(bp0, "addr_end"));
    TEST_ASSERT_EQUAL_STRING("RANGE",
        json_object_get_string_member(bp0, "addr_match_mode"));
    TEST_ASSERT_EQUAL_INT(0xFFFF, json_object_get_int_member(bp0, "addr_mask"));
    TEST_ASSERT_EQUAL_INT(6, json_object_get_int_member(bp0, "bank_id_end"));
    TEST_ASSERT_EQUAL_STRING("MASK",
        json_object_get_string_member(bp0, "bank_match_mode"));
    TEST_ASSERT_EQUAL_INT(0xFE, json_object_get_int_member(bp0, "bank_id_mask"));
    /* Druhý BP bez nastavení -> výchozí SINGLE. */
    JsonObject *bp1 = json_array_get_object_element(arr, 1);
    TEST_ASSERT_EQUAL_STRING("SINGLE",
        json_object_get_string_member(bp1, "addr_match_mode"));
    TEST_ASSERT_TRUE(json_object_has_member(bp1, "addr_end"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Pošle bp_create_with_init a vrátí, zda odpověď nese "warning".
 *
 * @param json Celý JSONL request.
 * @return true, pokud úspěšná odpověď obsahuje data.warning.
 */
static bool _bp_create_has_warning(const char *json) {
    st_JSONL_MESSAGE *req = _make_request(json);
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "created"));
    bool has = json_object_has_member(data, "warning");
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
    return has;
}

/*
 * BP s addr_end != addr, ale bez addr_match_mode RANGE: vznikne (chování
 * beze změny), odpověď ale nese warning. S RANGE, nebo s addr_end == addr,
 * warning není.
 */
void test_bp_create_addr_end_without_range_warns(void) {
    g_stub_state.bp_create_fake_id = 12;
    TEST_ASSERT_TRUE(_bp_create_has_warning(
        "{\"type\":\"request\",\"req_id\":34,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"addr\",\"addr_end\"],"
        "\"type\":\"MEM_W\",\"addr\":4096,\"addr_end\":4351}}"));
    TEST_ASSERT_TRUE(_bp_create_has_warning(
        "{\"type\":\"request\",\"req_id\":35,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"addr\",\"addr_end\",\"addr_match_mode\"],"
        "\"type\":\"MEM_W\",\"addr\":4096,\"addr_end\":4351,"
        "\"addr_match_mode\":\"SINGLE\"}}"));
    TEST_ASSERT_FALSE(_bp_create_has_warning(
        "{\"type\":\"request\",\"req_id\":36,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"addr\",\"addr_end\",\"addr_match_mode\"],"
        "\"type\":\"MEM_W\",\"addr\":4096,\"addr_end\":4351,"
        "\"addr_match_mode\":\"RANGE\"}}"));
    TEST_ASSERT_FALSE(_bp_create_has_warning(
        "{\"type\":\"request\",\"req_id\":37,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"addr\",\"addr_end\"],"
        "\"type\":\"MEM_W\",\"addr\":4096,\"addr_end\":4096}}"));
    TEST_ASSERT_FALSE(_bp_create_has_warning(
        "{\"type\":\"request\",\"req_id\":38,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"addr\"],"
        "\"type\":\"MEM_W\",\"addr\":4096}}"));
    /* Chování beze změny: backend dostal create pětkrát. */
    TEST_ASSERT_EQUAL_INT(5, g_stub_state.bp_create_calls);
}


/*
 * Neznámé jméno v poli fields[] = invalid_params (= "neznámý type" cesta z
 * akceptačního kritéria; remap by takový vstup neměl propustit do backendu).
 */
void test_bp_create_unknown_field_invalid_params(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":22,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"no_such_field\"],\"addr\":4096}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    /* Backend nesmí být vůbec osloven. */
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.bp_create_calls);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/*
 * Leak smoke: bp_clear nad BP s condition výrazem. Před F015b _handle_bp_clear
 * uvolnil jen result, NE bp[i].condition (= leak heap stringu per BP). Stub
 * nyní condition g_strdup()-uje; tento test ji protočí přes bp_clear a ověří
 * korektní count. Sám leak detekuje až ASan/Valgrind, ale test pojistí, že
 * cesta projde a sjednocený helper _free_bp_list_result se použije bez crashe.
 */
void test_bp_clear_with_condition_smoke(void) {
    g_stub_state.fill_bp_list_count = 1;
    g_stub_state.bp_list_fake_condition = "PC==0x2000";
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":23,\"cmd\":\"bp_clear\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    /* 1 BP nalezen -> 1 odstraněn (BP_REMOVE stub vrací success). */
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(data, "count"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "cleared"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* H2 - enum string hranice (type/zone/event_trigger/match_mode)          */
/* ====================================================================== */

/*
 * Enum pole posílaná jako kanonický UPPER_SNAKE string se musí přeložit na
 * správný enum index a dorazit do backendu. "MEM_W" -> 2 (BPT_TYPE_MEM_W),
 * "RAM" -> 3 (BP_ZONE_RAM). Ověřuje sjednocenou string cestu z H2 (NE jen
 * číselnou z F015a).
 */
void test_bp_create_type_zone_string_parsed(void) {
    g_stub_state.bp_create_fake_id = 9;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":30,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"zone\",\"addr\"],"
        "\"type\":\"MEM_W\",\"zone\":\"RAM\",\"addr\":4096}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.bp_create_calls);
    TEST_ASSERT_EQUAL_UINT8(2, g_stub_state.bp_create_last_type);  /* MEM_W */
    TEST_ASSERT_EQUAL_UINT8(3, g_stub_state.bp_create_last_zone);  /* RAM */
    TEST_ASSERT_EQUAL_UINT16(4096, g_stub_state.bp_create_last_addr);

    free(resp);
    jsonl_msg_free(req);
}


/*
 * event_trigger používá vlastní lowercase slovník (bp_event.c s_trigger_names:
 * rising/falling/changed/low/high). "high" -> BP_EVT_TRIG_HIGH (4). Ověřuje, že
 * H2 sjednocení sahá i na non-UPPER_SNAKE enum konvertor.
 */
void test_bp_create_event_trigger_string_parsed(void) {
    g_stub_state.bp_create_fake_id = 11;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":31,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"event_name\",\"event_trigger\"],"
        "\"type\":\"HW_EVENT\",\"event_name\":\"vsync\","
        "\"event_trigger\":\"high\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.bp_create_calls);
    TEST_ASSERT_EQUAL_UINT8(6, g_stub_state.bp_create_last_type);  /* HW_EVENT */
    TEST_ASSERT_EQUAL_UINT8(4, g_stub_state.bp_create_last_event_trigger); /* HIGH */

    free(resp);
    jsonl_msg_free(req);
}


/*
 * Číselný type zůstává akceptován (zpětná kompat) i po H2 sjednocení.
 * type=4 (int) -> BPT_TYPE_IORQ_W. Bez tohoto by H2 rozbilo číselné klienty.
 */
void test_bp_create_type_number_still_works(void) {
    g_stub_state.bp_create_fake_id = 12;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":32,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"port\"],"
        "\"type\":4,\"port\":206}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.bp_create_calls);
    TEST_ASSERT_EQUAL_UINT8(4, g_stub_state.bp_create_last_type);  /* IORQ_W */

    free(resp);
    jsonl_msg_free(req);
}


/*
 * Neznámý enum string (zde lowercase "memw", který NENÍ v kanonickém slovníku)
 * musí vrátit invalid_params, NE tichý fallback na PC_EXEC (0). Backend nesmí
 * být osloven. Toto je hlavní akceptační kritérium H2.
 */
void test_bp_create_unknown_type_string_invalid_params(void) {
    g_stub_state.bp_create_calls = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":33,\"cmd\":\"bp_create_with_init\","
        "\"data\":{\"fields\":[\"type\",\"addr\"],"
        "\"type\":\"memw\",\"addr\":4096}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.bp_create_calls);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    const char *err = json_object_get_string_member(obj, "error");
    TEST_ASSERT_NOT_NULL(err);
    /* Chybová zpráva pojmenuje konkrétní pole. */
    TEST_ASSERT_NOT_NULL(strstr(err, "type"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/*
 * bp_update sdílí stejnou fill cestu - neznámý zone string také musí vrátit
 * invalid_params bez oslovení backendu.
 */
void test_bp_update_unknown_zone_string_invalid_params(void) {
    g_stub_state.call_count = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":34,\"cmd\":\"bp_update\","
        "\"data\":{\"id\":1,\"fields\":[\"zone\"],\"zone\":\"NOSUCH\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    const char *err = json_object_get_string_member(obj, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_NOT_NULL(strstr(err, "zone"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* Tests - chybové cesty                                                   */
/* ====================================================================== */

void test_unknown_cmd_returns_error_response(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":10,\"cmd\":\"xyz_no_such\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_UNKNOWN_CMD, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_EQUAL_INT(10, json_object_get_int_member(obj, "req_id"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    const char *err = json_object_get_string_member(obj, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_STRING("Unknown command", err);
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_mem_read_invalid_params_rejected(void) {
    /* addr+len = 65537 > 65536 -> reject */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":11,\"cmd\":\"mem_read\","
        "\"data\":{\"addr\":65535,\"len\":2}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    TEST_ASSERT_EQUAL_STRING("Invalid parameters",
        json_object_get_string_member(obj, "error"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_bp_add_invalid_addr_rejected(void) {
    /* addr chybí v data -> reject */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":12,\"cmd\":\"bp_add\","
        "\"data\":{}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);

    free(resp);
    jsonl_msg_free(req);
}


void test_pause_emu_failure_reports_error(void) {
    g_stub_state.fail_next = true;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":13,\"cmd\":\"pause\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(resp);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    TEST_ASSERT_EQUAL_STRING("Pause failed",
        json_object_get_string_member(obj, "error"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Pomocník: provede request a vrátí text pole "error" (g_strdup).
 *
 * Ověří, že odpověď je neúspěch s kódem MCP_DISPATCH_EMU_ERROR.
 * Vlastnictví: volající uvolní vrácený řetězec přes g_free.
 */
static char *_dispatch_expect_emu_error(const char *request_json) {
    st_JSONL_MESSAGE *req = _make_request(request_json);
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(resp);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    char *err = g_strdup(json_object_get_string_member(obj, "error"));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
    return err;
}


/** Timeout ve frontě: zpráva musí říct, že se příkaz neprovedl
 *  (dřív zavádějící "unknown id?"). */
void test_bp_remove_timeout_reports_not_executed(void) {
    g_stub_state.fail_next = true;
    g_stub_state.fail_status = DBGAPI_SUBMIT_TIMEOUT;
    char *err = _dispatch_expect_emu_error(
        "{\"type\":\"request\",\"req_id\":1581,\"cmd\":\"bp_remove\","
        "\"data\":{\"id\":181}}");
    TEST_ASSERT_TRUE(g_str_has_prefix(err, "Emulator busy: command not executed"));
    TEST_ASSERT_NOT_NULL(strstr(err, "safe to retry"));
    /* Původní zpráva handleru zůstává v hranatých závorkách (kompatibilita). */
    TEST_ASSERT_TRUE(g_str_has_suffix(err, " [bp_remove failed (unknown id?)]"));
    g_free(err);
}


/** Skutečné selhání handleru (neexistující ID) ponechá původní zprávu. */
void test_bp_remove_handler_failure_keeps_message(void) {
    g_stub_state.fail_next = true;   /* fail_status 0 = DBGAPI_SUBMIT_FAILED */
    char *err = _dispatch_expect_emu_error(
        "{\"type\":\"request\",\"req_id\":1582,\"cmd\":\"bp_remove\","
        "\"data\":{\"id\":999}}");
    TEST_ASSERT_EQUAL_STRING("bp_remove failed (unknown id?)", err);
    g_free(err);
}


/** Plná fronta a ukončování emulátoru mají vlastní zprávy. */
void test_queue_full_and_ending_messages(void) {
    g_stub_state.fail_next = true;
    g_stub_state.fail_status = DBGAPI_SUBMIT_QUEUE_FULL;
    char *err = _dispatch_expect_emu_error(
        "{\"type\":\"request\",\"req_id\":201,\"cmd\":\"bp_list\"}");
    TEST_ASSERT_TRUE(g_str_has_prefix(err, "Emulator busy: command queue full"));
    g_free(err);

    g_stub_state.fail_next = true;
    g_stub_state.fail_status = DBGAPI_SUBMIT_ENDING;
    err = _dispatch_expect_emu_error(
        "{\"type\":\"request\",\"req_id\":202,\"cmd\":\"pause\"}");
    TEST_ASSERT_TRUE(g_str_has_prefix(err, "Emulator is shutting down"));
    g_free(err);
}


/** Vícekrokový handler: první submit proveden, druhý zrušen timeoutem
 *  -> "partially executed", opakování není bezpečné. */
void test_partial_execution_reported(void) {
    g_stub_state.fail_on_call = 2;
    g_stub_state.fail_status = DBGAPI_SUBMIT_TIMEOUT;
    char *err = _dispatch_expect_emu_error(
        "{\"type\":\"request\",\"req_id\":203,"
        "\"cmd\":\"set_user_cycle_origin\"}");
    TEST_ASSERT_TRUE(g_str_has_prefix(err, "Emulator busy: command only partially executed"));
    g_free(err);
}


/** Sledování se nuluje per request: předchozí timeout neovlivní
 *  validační ani handlerovou chybu dalšího požadavku. */
void test_submit_track_reset_per_request(void) {
    g_stub_state.fail_next = true;
    g_stub_state.fail_status = DBGAPI_SUBMIT_TIMEOUT;
    char *err = _dispatch_expect_emu_error(
        "{\"type\":\"request\",\"req_id\":204,\"cmd\":\"pause\"}");
    g_free(err);

    g_stub_state.fail_next = true;
    g_stub_state.fail_status = DBGAPI_SUBMIT_OK;  /* = FAILED */
    err = _dispatch_expect_emu_error(
        "{\"type\":\"request\",\"req_id\":205,\"cmd\":\"pause\"}");
    TEST_ASSERT_EQUAL_STRING("Pause failed", err);
    g_free(err);
}


void test_non_request_message_rejected(void) {
    /* HELLO message není REQUEST - dispatcher musí odmítnout */
    st_JSONL_MESSAGE *msg = NULL;
    jsonl_parse_line("{\"type\":\"hello\",\"version\":\"1.0\"}", &msg);
    TEST_ASSERT_NOT_NULL(msg);
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(msg, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_NOT_A_REQUEST, rc);
    TEST_ASSERT_NULL(resp);

    jsonl_msg_free(msg);
}


void test_null_args_not_crash(void) {
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(NULL, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_NOT_A_REQUEST, rc);
    TEST_ASSERT_NULL(resp);
}


/* ====================================================================== */
/* Tests - hello builder + supported commands                              */
/* ====================================================================== */

void test_build_hello_contains_19_commands(void) {
    char *line = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_build_hello(&line);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(line);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(line, &parser);
    TEST_ASSERT_EQUAL_STRING("hello",
        json_object_get_string_member(obj, "type"));
    TEST_ASSERT_EQUAL_STRING("1.0",
        json_object_get_string_member(obj, "version"));

    JsonArray *cmds = json_object_get_array_member(obj, "commands");
    TEST_ASSERT_NOT_NULL(cmds);
    /* Historie počtu Tools v hello payload (programaticky generovaný
     * z g_cmd_map[] od V1.B.3):
     *   V0.A.3=9, +shutdown=10, +mem_write=11, V0.B.6 +7=18,
     *   +get_mcp_config=19, V1.A.1 +5=24, V1.A.2 +4=28, V1.A.3 +4=32,
     *   V1.A.4 +4=36, V1.A.5 +5=41, V1.A.6 +9=50, V1.A.7 +5=55,
     *   V1.B.1 +5=60, V1.B.2 +5=65, V1.B.3 +1 (emu_stop, pipe-only)=66,
     *   V1.C.1 +6 (HID Tools)=72, V1.D.1 +8 (Core+CPU Resource backings)=80,
     *   V1.D.2.A +5 (Easy reuse Resource backings)=85. */
    TEST_ASSERT_EQUAL_INT(MCP_EXPECTED_CMD_COUNT,
                          json_array_get_length(cmds));

    /* Ověřme přesné pořadí - deterministické per cmd_map[] */
    TEST_ASSERT_EQUAL_STRING("ping",
        json_array_get_string_element(cmds, 0));
    TEST_ASSERT_EQUAL_STRING("get_state",
        json_array_get_string_element(cmds, 1));
    TEST_ASSERT_EQUAL_STRING("mem_read",
        json_array_get_string_element(cmds, 6));
    TEST_ASSERT_EQUAL_STRING("mem_write",
        json_array_get_string_element(cmds, 7));
    TEST_ASSERT_EQUAL_STRING("bp_add",
        json_array_get_string_element(cmds, 8));
    TEST_ASSERT_EQUAL_STRING("bp_list",
        json_array_get_string_element(cmds, 9));
    TEST_ASSERT_EQUAL_STRING("shutdown",
        json_array_get_string_element(cmds, 10));
    /* V0.B.6 - pořadí přesně podle cmd_map[]. */
    TEST_ASSERT_EQUAL_STRING("bp_remove",
        json_array_get_string_element(cmds, 11));
    TEST_ASSERT_EQUAL_STRING("bp_clear",
        json_array_get_string_element(cmds, 12));
    TEST_ASSERT_EQUAL_STRING("bp_enable",
        json_array_get_string_element(cmds, 13));
    TEST_ASSERT_EQUAL_STRING("step_into",
        json_array_get_string_element(cmds, 14));
    TEST_ASSERT_EQUAL_STRING("step_over",
        json_array_get_string_element(cmds, 15));
    TEST_ASSERT_EQUAL_STRING("step_n",
        json_array_get_string_element(cmds, 16));
    TEST_ASSERT_EQUAL_STRING("run_until_addr",
        json_array_get_string_element(cmds, 17));
    /* V0.B.7 - backing handler pro emulator://config/mcp Resource. */
    TEST_ASSERT_EQUAL_STRING("get_mcp_config",
        json_array_get_string_element(cmds, 18));
    /* V1.A.1 - snapshot Tools + cooperation hint. */
    TEST_ASSERT_EQUAL_STRING("snapshot_save",
        json_array_get_string_element(cmds, 19));
    TEST_ASSERT_EQUAL_STRING("snapshot_save_buffer",
        json_array_get_string_element(cmds, 20));
    TEST_ASSERT_EQUAL_STRING("snapshot_load",
        json_array_get_string_element(cmds, 21));
    TEST_ASSERT_EQUAL_STRING("snapshot_load_buffer",
        json_array_get_string_element(cmds, 22));
    TEST_ASSERT_EQUAL_STRING("cooperation_hint_set",
        json_array_get_string_element(cmds, 23));
    /* V1.A.2 - symbol management Tools. */
    TEST_ASSERT_EQUAL_STRING("symbol_add",
        json_array_get_string_element(cmds, 24));
    TEST_ASSERT_EQUAL_STRING("symbol_remove",
        json_array_get_string_element(cmds, 25));
    TEST_ASSERT_EQUAL_STRING("symbol_lookup",
        json_array_get_string_element(cmds, 26));
    TEST_ASSERT_EQUAL_STRING("symbol_list",
        json_array_get_string_element(cmds, 27));
    /* V1.A.3 - step out + run_until_* Tools. */
    TEST_ASSERT_EQUAL_STRING("step_out",
        json_array_get_string_element(cmds, 28));
    TEST_ASSERT_EQUAL_STRING("run_until_raster",
        json_array_get_string_element(cmds, 29));
    TEST_ASSERT_EQUAL_STRING("run_until_tstate",
        json_array_get_string_element(cmds, 30));
    TEST_ASSERT_EQUAL_STRING("run_until_event",
        json_array_get_string_element(cmds, 31));
    /* V1.A.4 - EVENT subscribe + TRAP Tools. */
    TEST_ASSERT_EQUAL_STRING("event_subscribe",
        json_array_get_string_element(cmds, 32));
    TEST_ASSERT_EQUAL_STRING("event_unsubscribe",
        json_array_get_string_element(cmds, 33));
    TEST_ASSERT_EQUAL_STRING("event_poll",
        json_array_get_string_element(cmds, 34));
    TEST_ASSERT_EQUAL_STRING("trap_respond",
        json_array_get_string_element(cmds, 35));

    /* Capabilities */
    JsonObject *caps = json_object_get_object_member(obj, "capabilities");
    TEST_ASSERT_NOT_NULL(caps);
    TEST_ASSERT_EQUAL_STRING("JSONL/MCP",
        json_object_get_string_member(caps, "protocol"));

    g_object_unref(parser);
    free(line);
}


/**
 * @brief Tabulka dodatečného limitu převzatého příkazu
 *        (mcp_dispatch_stall_limit_ms): souborové I/O má dlouhý limit,
 *        ostatní běžný; BLOCKING flag se ignoruje.
 */
void test_stall_limit_table(void) {
    const int normal = mcp_dispatch_stall_limit_ms(DBGAPI_CMD_GET_ALL_REGS);
    TEST_ASSERT_EQUAL_INT(10000, normal);
    TEST_ASSERT_EQUAL_INT(10000, mcp_dispatch_stall_limit_ms(DBGAPI_CMD_PAUSE));
    TEST_ASSERT_EQUAL_INT(10000, mcp_dispatch_stall_limit_ms(DBGAPI_CMD_REGIONS_WRITE));

    const int long_cmds[] = {
        DBGAPI_CMD_TRACE_SAVE, DBGAPI_CMD_TRACE_STOP,
        DBGAPI_CMD_SNAPSHOT_SAVE_FILE, DBGAPI_CMD_SNAPSHOT_LOAD_FILE,
        DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER, DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER,
        DBGAPI_CMD_PROFILER_EXPORT, DBGAPI_CMD_CDL_EXPORT,
        DBGAPI_CMD_VIDEOREC, DBGAPI_CMD_MEDIA_LOAD_MZF,
        DBGAPI_CMD_MEDIA_RUN_MZF, DBGAPI_CMD_MEDIA_LOAD_BINARY, DBGAPI_CMD_MEDIA_INSERT,
        DBGAPI_CMD_MEDIA_EJECT, DBGAPI_CMD_CMT_OPEN, DBGAPI_CMD_CMT_RECORD,
        DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG,
    };
    for (size_t i = 0; i < sizeof(long_cmds) / sizeof(long_cmds[0]); i++) {
        TEST_ASSERT_EQUAL_INT(600000, mcp_dispatch_stall_limit_ms(long_cmds[i]));
    }
    /* BLOCKING flag (horní bit) nesmí výběr změnit. */
    TEST_ASSERT_EQUAL_INT(600000, mcp_dispatch_stall_limit_ms(
        (int)(DBGAPI_CMD_TRACE_SAVE | DBGAPI_CMDFLAG_BLOCKING)));
    TEST_ASSERT_EQUAL_INT(10000, mcp_dispatch_stall_limit_ms(
        (int)(DBGAPI_CMD_PAUSE | DBGAPI_CMDFLAG_BLOCKING)));
}


void test_supported_commands_list(void) {
    const char *const *cmds = mcp_dispatch_get_supported_commands();
    TEST_ASSERT_NOT_NULL(cmds);
    int count = 0;
    for (const char *const *p = cmds; *p; p++) count++;
    /* Aktuální celkový počet - viz MCP_EXPECTED_CMD_COUNT komentář. */
    TEST_ASSERT_EQUAL_INT(MCP_EXPECTED_CMD_COUNT, count);
    TEST_ASSERT_EQUAL_STRING("ping",           cmds[0]);
    TEST_ASSERT_EQUAL_STRING("mem_write",      cmds[7]);
    TEST_ASSERT_EQUAL_STRING("bp_add",         cmds[8]);
    TEST_ASSERT_EQUAL_STRING("bp_list",        cmds[9]);
    TEST_ASSERT_EQUAL_STRING("shutdown",       cmds[10]);
    TEST_ASSERT_EQUAL_STRING("bp_remove",      cmds[11]);
    TEST_ASSERT_EQUAL_STRING("bp_clear",       cmds[12]);
    TEST_ASSERT_EQUAL_STRING("bp_enable",      cmds[13]);
    TEST_ASSERT_EQUAL_STRING("step_into",      cmds[14]);
    TEST_ASSERT_EQUAL_STRING("step_over",      cmds[15]);
    TEST_ASSERT_EQUAL_STRING("step_n",         cmds[16]);
    TEST_ASSERT_EQUAL_STRING("run_until_addr", cmds[17]);
    /* V0.B.7 - backing handler pro emulator://config/mcp Resource. */
    TEST_ASSERT_EQUAL_STRING("get_mcp_config", cmds[18]);
    /* V1.A.1 - snapshot Tools + cooperation hint. */
    TEST_ASSERT_EQUAL_STRING("snapshot_save",        cmds[19]);
    TEST_ASSERT_EQUAL_STRING("snapshot_save_buffer", cmds[20]);
    TEST_ASSERT_EQUAL_STRING("snapshot_load",        cmds[21]);
    TEST_ASSERT_EQUAL_STRING("snapshot_load_buffer", cmds[22]);
    TEST_ASSERT_EQUAL_STRING("cooperation_hint_set", cmds[23]);
    /* V1.A.2 - symbol management Tools. */
    TEST_ASSERT_EQUAL_STRING("symbol_add",    cmds[24]);
    TEST_ASSERT_EQUAL_STRING("symbol_remove", cmds[25]);
    TEST_ASSERT_EQUAL_STRING("symbol_lookup", cmds[26]);
    TEST_ASSERT_EQUAL_STRING("symbol_list",   cmds[27]);
    /* V1.A.3 - step out + run_until_* Tools. */
    TEST_ASSERT_EQUAL_STRING("step_out",         cmds[28]);
    TEST_ASSERT_EQUAL_STRING("run_until_raster", cmds[29]);
    TEST_ASSERT_EQUAL_STRING("run_until_tstate", cmds[30]);
    TEST_ASSERT_EQUAL_STRING("run_until_event",  cmds[31]);
    /* V1.A.4 - EVENT subscribe + TRAP Tools. */
    TEST_ASSERT_EQUAL_STRING("event_subscribe",   cmds[32]);
    TEST_ASSERT_EQUAL_STRING("event_unsubscribe", cmds[33]);
    TEST_ASSERT_EQUAL_STRING("event_poll",        cmds[34]);
    TEST_ASSERT_EQUAL_STRING("trap_respond",      cmds[35]);
    /* V1.A.5 - chip-level fault injection Tools. */
    TEST_ASSERT_EQUAL_STRING("io_read",         cmds[36]);
    TEST_ASSERT_EQUAL_STRING("io_write",        cmds[37]);
    TEST_ASSERT_EQUAL_STRING("irq_inject",      cmds[38]);
    TEST_ASSERT_EQUAL_STRING("nmi_inject",      cmds[39]);
    TEST_ASSERT_EQUAL_STRING("mem_write_force", cmds[40]);
    /* V1.A.6 - Watch + Callstack + CDL Tools. */
    TEST_ASSERT_EQUAL_STRING("watch_add",     cmds[41]);
    TEST_ASSERT_EQUAL_STRING("callstack_get", cmds[45]);
    TEST_ASSERT_EQUAL_STRING("cdl_export",    cmds[49]);
    /* V1.A.7 - Profiler Tools. */
    TEST_ASSERT_EQUAL_STRING("profiler_start", cmds[50]);
    TEST_ASSERT_EQUAL_STRING("profiler_get",   cmds[54]);
    /* V1.B.1 - Media Tools. */
    TEST_ASSERT_EQUAL_STRING("media_load_mzf", cmds[55]);
    TEST_ASSERT_EQUAL_STRING("media_state",    cmds[59]);
    /* V1.B.2 - Platform + Config Tools. */
    TEST_ASSERT_EQUAL_STRING("settings_set",  cmds[60]);
    TEST_ASSERT_EQUAL_STRING("periph_detach", cmds[64]);
    /* V1.B.3 - hot-swap (pipe-only). */
    TEST_ASSERT_EQUAL_STRING("emu_stop",      cmds[65]);
    /* V1.C.1 - HID Tools. */
    TEST_ASSERT_EQUAL_STRING("input_send_key",              cmds[66]);
    TEST_ASSERT_EQUAL_STRING("input_send_keys",             cmds[67]);
    TEST_ASSERT_EQUAL_STRING("input_press_key",             cmds[68]);
    TEST_ASSERT_EQUAL_STRING("input_release_key",           cmds[69]);
    TEST_ASSERT_EQUAL_STRING("input_send_joystick",         cmds[70]);
    TEST_ASSERT_EQUAL_STRING("input_send_keys_with_delays", cmds[71]);
}


/**
 * @brief V1.B.3: supported_commands musí být generované programaticky
 *        z g_cmd_map[], ne hardcoded mirror.
 *
 * Ověřuje, že:
 *   1. Idempotence init (= druhé volání nealokuje znovu, žádný leak),
 *   2. Po shutdown vrací accessor prázdné NULL-terminated pole,
 *   3. Po re-init je pole znovu zaplněné se stejným počtem,
 *   4. Každé jméno v poli existuje jako platný cmd_map handler
 *      (= verifikace dispatch_request(..., resp).rc != UNKNOWN_CMD).
 */
void test_supported_cmd_names_generated_from_cmd_map(void) {
    /* setUp už volal init. */
    const char *const *cmds1 = mcp_dispatch_get_supported_commands();
    TEST_ASSERT_NOT_NULL(cmds1);
    int count1 = 0;
    for (const char *const *p = cmds1; *p; p++) count1++;
    TEST_ASSERT_EQUAL_INT(MCP_EXPECTED_CMD_COUNT, count1);

    /* Idempotence: druhé volání init nesmí změnit pointer ani count
     * (= žádná re-alokace). */
    mcp_dispatch_init();
    const char *const *cmds2 = mcp_dispatch_get_supported_commands();
    TEST_ASSERT_EQUAL_PTR(cmds1, cmds2);

    /* Shutdown + accessor vrací empty NULL-terminated pole. */
    mcp_dispatch_shutdown();
    const char *const *empty = mcp_dispatch_get_supported_commands();
    TEST_ASSERT_NOT_NULL(empty);
    TEST_ASSERT_NULL(empty[0]);

    /* Re-init - pole opět zaplněné. */
    mcp_dispatch_init();
    const char *const *cmds3 = mcp_dispatch_get_supported_commands();
    int count3 = 0;
    for (const char *const *p = cmds3; *p; p++) count3++;
    TEST_ASSERT_EQUAL_INT(MCP_EXPECTED_CMD_COUNT, count3);

    /* První jméno v poli musí být "ping" (= deterministické pořadí
     * podle cmd_map). */
    TEST_ASSERT_EQUAL_STRING("ping", cmds3[0]);
}


/**
 * @brief V1.B.3: transport kind setter/getter.
 *
 * Default po init je NONE. Setter mění hodnotu na PIPE / TCP.
 * Žádný side effect mimo internal flag (= žádný dispatch handler
 * není v tomto testu volán).
 */
void test_transport_kind_default_and_setter(void) {
    /* setUp dělá _reset i init. Po reset (= testovací init bez
     * external transport setteru) by mělo být NONE. */
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_NONE);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_TRANSPORT_NONE,
                          mcp_dispatch_get_transport_kind());

    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_PIPE);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_TRANSPORT_PIPE,
                          mcp_dispatch_get_transport_kind());

    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_TCP);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_TRANSPORT_TCP,
                          mcp_dispatch_get_transport_kind());

    /* Reset zpět na NONE pro další testy. */
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_NONE);
}


/* ====================================================================== */
/* Tests - get_mcp_config handler (V0.B.7)                                 */
/* ====================================================================== */

/**
 * @brief Ověří, že `get_mcp_config` handler vrátí všechny očekávané
 *        klíče se správnými typy.
 *
 * V testovacím buildu (`MZ800EMU_MCP_TEST_BUILD`) handler běží ve
 * stub větvi - vrací default hodnoty + `tcp_enabled=false`. Test
 * neověřuje konkrétní port nebo profile (= závisí na buildu emu),
 * pouze přítomnost a typ klíčů.
 */
void test_get_mcp_config_returns_payload(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":700,\"cmd\":\"get_mcp_config\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    /* Lokální handler - žádné dbgapi volání. */
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_EQUAL_INT(700, json_object_get_int_member(obj, "req_id"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));

    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);

    /* Všechny klíče musí být přítomné. */
    TEST_ASSERT_TRUE(json_object_has_member(data, "tcp_port"));
    TEST_ASSERT_TRUE(json_object_has_member(data, "bind_address"));
    TEST_ASSERT_TRUE(json_object_has_member(data, "profile"));
    TEST_ASSERT_TRUE(json_object_has_member(data, "auto_start_tcp"));
    TEST_ASSERT_TRUE(json_object_has_member(data, "tcp_enabled"));

    /* tcp_port je int v rozumném rozsahu. */
    gint64 port = json_object_get_int_member(data, "tcp_port");
    TEST_ASSERT_TRUE(port >= 1024 && port <= 65535);

    /* bind_address je validní string. */
    const char *bind = json_object_get_string_member(data, "bind_address");
    TEST_ASSERT_NOT_NULL(bind);
    TEST_ASSERT_TRUE(strcmp(bind, "127.0.0.1") == 0
                     || strcmp(bind, "0.0.0.0") == 0);

    /* profile je jeden ze 4 stringů. */
    const char *prof = json_object_get_string_member(data, "profile");
    TEST_ASSERT_NOT_NULL(prof);
    TEST_ASSERT_TRUE(strcmp(prof, "wild")      == 0
                     || strcmp(prof, "confined")  == 0
                     || strcmp(prof, "sandboxed") == 0
                     || strcmp(prof, "observer")  == 0);

    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* Tests - shutdown handler + callback (V0.A.4)                            */
/* ====================================================================== */

/** @brief Counter zvýšený stub callbackem v testech shutdown. */
static int g_shutdown_cb_calls = 0;

/** @brief Stub shutdown callback - pouze inkrementuje counter. */
static void _stub_shutdown_cb(void) {
    g_shutdown_cb_calls++;
}


void test_shutdown_handler_returns_success(void) {
    /* Bez registrovaného callbacku - handler musí stále vrátit success. */
    mcp_dispatch_set_shutdown_callback(NULL);
    g_shutdown_cb_calls = 0;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":42,\"cmd\":\"shutdown\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    /* Lokální handler - žádné dbgapi volání. */
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
    /* Callback registrovaný NULL - counter zůstává 0. */
    TEST_ASSERT_EQUAL_INT(0, g_shutdown_cb_calls);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_EQUAL_INT(42, json_object_get_int_member(obj, "req_id"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "shutdown"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_shutdown_handler_invokes_callback(void) {
    mcp_dispatch_set_shutdown_callback(_stub_shutdown_cb);
    g_shutdown_cb_calls = 0;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":7,\"cmd\":\"shutdown\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    /* Callback měl být přesně jednou zavolán. */
    TEST_ASSERT_EQUAL_INT(1, g_shutdown_cb_calls);

    /* Odregistrace - aby další testy neměly callback registrovaný. */
    mcp_dispatch_set_shutdown_callback(NULL);

    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* Tests V1.B.3 - emu_stop (hot-swap workflow)                             */
/* ====================================================================== */

/**
 * @brief emu_stop v pipe transportu vrátí success a triggernuje
 *        shutdown callback (= identicky se `shutdown` handlerem).
 */
void test_emu_stop_pipe_success(void) {
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_PIPE);
    mcp_dispatch_set_shutdown_callback(_stub_shutdown_cb);
    g_shutdown_cb_calls = 0;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":901,\"cmd\":\"emu_stop\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
    TEST_ASSERT_EQUAL_INT(1, g_shutdown_cb_calls);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_EQUAL_INT(901, json_object_get_int_member(obj, "req_id"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "stopped"));
    TEST_ASSERT_EQUAL_STRING("pipe",
        json_object_get_string_member(data, "transport"));
    g_object_unref(parser);

    /* Cleanup pro další testy. */
    mcp_dispatch_set_shutdown_callback(NULL);
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_NONE);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief emu_stop na TCP transportu vrátí error a NEzavolá shutdown
 *        callback (= nesmí zabít živou GUI session).
 */
void test_emu_stop_tcp_returns_error(void) {
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_TCP);
    mcp_dispatch_set_shutdown_callback(_stub_shutdown_cb);
    g_shutdown_cb_calls = 0;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":902,\"cmd\":\"emu_stop\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    /* Implementace vrací MCP_DISPATCH_INVALID_PARAMS - klient dostane
     * error response s textem (= dispatch result je informativní pro
     * caller logging). */
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_NOT_NULL(resp);
    /* Klíčové: callback NESMÍ být zavolán. */
    TEST_ASSERT_EQUAL_INT(0, g_shutdown_cb_calls);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    /* Error message obsahuje očekávaný text. */
    const char *err = json_object_get_string_member(obj, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_NOT_NULL(strstr(err, "pipe transport"));
    g_object_unref(parser);

    mcp_dispatch_set_shutdown_callback(NULL);
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_NONE);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief emu_stop bez registrovaného shutdown callback vrátí error
 *        i v pipe módu (= ochrana proti tiché chybě).
 */
void test_emu_stop_no_callback_returns_error(void) {
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_PIPE);
    mcp_dispatch_set_shutdown_callback(NULL);

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":903,\"cmd\":\"emu_stop\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(resp);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    g_object_unref(parser);

    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_NONE);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief emu_stop v transport=NONE (= žádný main, unit testovací
 *        kontext) také vrátí error - hot-swap musí mít explicit pipe.
 */
void test_emu_stop_transport_none_returns_error(void) {
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_NONE);
    mcp_dispatch_set_shutdown_callback(_stub_shutdown_cb);
    g_shutdown_cb_calls = 0;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":904,\"cmd\":\"emu_stop\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_shutdown_cb_calls);

    mcp_dispatch_set_shutdown_callback(NULL);
    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* Tests V0.B.6 - 7 chybějících V0 Tools                                   */
/* ====================================================================== */

void test_bp_remove_happy(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":601,\"cmd\":\"bp_remove\","
        "\"data\":{\"id\":42}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_BP_REMOVE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    /* Ověř že handler poslal správné ID. */
    TEST_ASSERT_EQUAL_INT(42, g_stub_state.bp_remove_last_id);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(42, json_object_get_int_member(data, "id"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "removed"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_bp_remove_missing_id(void) {
    /* Request bez "id" pole - musí dostat MCP_DISPATCH_INVALID_PARAMS. */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":602,\"cmd\":\"bp_remove\","
        "\"data\":{}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    /* Žádný dbgapi call nesměl proběhnout. */
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_bp_clear_iterates_and_removes(void) {
    /* Nastav stub aby BP_LIST vrátil 3 BP s id=100,101,102. */
    g_stub_state.fill_bp_list_count = 3;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":603,\"cmd\":\"bp_clear\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    /* Stub volaný 1x BP_LIST + 3x BP_REMOVE = 4 dbgapi transakce. */
    TEST_ASSERT_EQUAL_INT(4, g_stub_state.call_count);
    /* Poslední cmd = BP_REMOVE (přes ID 102 = 100+2). */
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_BP_REMOVE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(102, g_stub_state.bp_remove_last_id);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(3, json_object_get_int_member(data, "count"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "cleared"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_bp_enable_happy(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":604,\"cmd\":\"bp_enable\","
        "\"data\":{\"id\":7,\"enabled\":false}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_BP_SET_ENABLED, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    TEST_ASSERT_EQUAL_INT(7, g_stub_state.bp_set_enabled_last_id);
    TEST_ASSERT_FALSE(g_stub_state.bp_set_enabled_last_enabled);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(7, json_object_get_int_member(data, "id"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "enabled"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_bp_enable_missing_param(void) {
    /* Bez "enabled" pole. */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":605,\"cmd\":\"bp_enable\","
        "\"data\":{\"id\":7}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


void test_step_into_increments_counter(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":606,\"cmd\":\"step_into\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    /* STEP_INTO a po něm prázdný příkaz (čekání na provedení kroku). */
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_NONE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(2, g_stub_state.call_count);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.step_into_calls);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "stepped"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "completed"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "running"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_step_over_increments_counter(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":607,\"cmd\":\"step_over\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    /* Krok bez CALL: STEP_OVER a čekání na provedení (prázdný příkaz). */
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_NONE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.step_over_calls);

    free(resp);
    jsonl_msg_free(req);
}


void test_step_n_loops_count_times(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":608,\"cmd\":\"step_n\","
        "\"data\":{\"count\":5}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    /* Stub musel vidět STEP_INTO 5x a jedno čekání na poslední krok. */
    TEST_ASSERT_EQUAL_INT(5, g_stub_state.step_into_calls);
    TEST_ASSERT_EQUAL_INT(6, g_stub_state.call_count);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(5, json_object_get_int_member(data, "count"));
    TEST_ASSERT_EQUAL_INT(5, json_object_get_int_member(data, "requested"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "partial"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_step_n_partial_on_failure(void) {
    /* Stub vrátí false po 3 úspěšných step_into voláních. */
    g_stub_state.step_into_fail_after_n = 3;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":609,\"cmd\":\"step_n\","
        "\"data\":{\"count\":10}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    /* Counter sedí na 3 úspěšných step_into volání. */
    TEST_ASSERT_EQUAL_INT(3, g_stub_state.step_into_calls);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(3,  json_object_get_int_member(data, "count"));
    TEST_ASSERT_EQUAL_INT(10, json_object_get_int_member(data, "requested"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "partial"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_step_n_invalid_range_rejected(void) {
    /* count=0 musí být odmítnuto. */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":610,\"cmd\":\"step_n\","
        "\"data\":{\"count\":0}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);

    /* A taky count=1001 musí padnout. */
    dispatch_stub_reset();
    req = _make_request(
        "{\"type\":\"request\",\"req_id\":611,\"cmd\":\"step_n\","
        "\"data\":{\"count\":1001}}");
    resp = NULL;
    rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
    free(resp);
    jsonl_msg_free(req);
}


void test_run_until_addr_happy(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":612,\"cmd\":\"run_until_addr\","
        "\"data\":{\"addr\":1024,\"max_cycles\":5000000}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_RUN_TO, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    /* Stub zachytil cílovou adresu z uint16_t* data_ptr. */
    TEST_ASSERT_EQUAL_UINT16(1024, g_stub_state.run_to_last_addr);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(1024, json_object_get_int_member(data, "addr"));
    TEST_ASSERT_EQUAL_INT(5000000,
        json_object_get_int_member(data, "max_cycles"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "running"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


void test_run_until_addr_invalid_addr(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":613,\"cmd\":\"run_until_addr\","
        "\"data\":{\"addr\":99999}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* V1.A.1 - Snapshot Tools + cooperation hint                              */
/* ====================================================================== */

/**
 * @brief snapshot_save happy path - handler předá filepath dbgapi
 *        SAVE_FILE handleru s correct origin.
 */
void test_snapshot_save_happy(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":700,\"cmd\":\"snapshot_save\","
        "\"data\":{\"path\":\"/tmp/test.mzs\",\"description\":\"unit test\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SNAPSHOT_SAVE_FILE,
                          g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    TEST_ASSERT_EQUAL_STRING("/tmp/test.mzs",
                             g_stub_state.snapshot_last_filepath);
    TEST_ASSERT_EQUAL_STRING("unit test",
                             g_stub_state.snapshot_last_description);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_STRING("/tmp/test.mzs",
        json_object_get_string_member(data, "path"));

    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief snapshot_save odmítne request bez path field.
 */
void test_snapshot_save_invalid_path(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":701,\"cmd\":\"snapshot_save\","
        "\"data\":{\"description\":\"no path\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief snapshot_save_buffer vrací base64 payload + size.
 */
void test_snapshot_save_buffer_returns_b64(void) {
    /* Stub naplní deterministický pattern 0xAB x 8 bajtů. */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":702,\"cmd\":\"snapshot_save_buffer\","
        "\"data\":{\"description\":\"inline\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER,
                          g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_STRING("inline",
                             g_stub_state.snapshot_last_description);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_INT(8, json_object_get_int_member(data, "size"));
    const char *b64 = json_object_get_string_member(data, "bytes_b64");
    TEST_ASSERT_NOT_NULL(b64);
    /* 8 bajtů base64 = 12 znaků (8 / 3 = 2 + 2/3 -> 4 znaky group * 3) */
    gsize dec_len = 0;
    guchar *dec = g_base64_decode(b64, &dec_len);
    TEST_ASSERT_EQUAL_INT(8, dec_len);
    for (gsize i = 0; i < dec_len; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAB, dec[i]);
    }
    g_free(dec);

    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief snapshot_load_buffer roundtrip - dekódování base64 musí dorazit
 *        do dbgapi handleru jako platná data.
 */
void test_snapshot_load_buffer_roundtrip(void) {
    /* Připravíme 4 bajty 0xDE,0xAD,0xBE,0xEF, zakódujeme do base64. */
    uint8_t in[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    gchar *b64 = g_base64_encode(in, sizeof(in));
    char *line = g_strdup_printf(
        "{\"type\":\"request\",\"req_id\":703,\"cmd\":\"snapshot_load_buffer\","
        "\"data\":{\"bytes_b64\":\"%s\"}}", b64);
    g_free(b64);

    st_JSONL_MESSAGE *req = _make_request(line);
    g_free(line);
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER,
                          g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(4, g_stub_state.snapshot_load_buf_size);
    TEST_ASSERT_EQUAL_UINT8(0xDE, g_stub_state.snapshot_load_buf_first);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief snapshot_load_buffer odmítne nevalidní base64.
 */
void test_snapshot_load_buffer_invalid_b64(void) {
    /* Prázdný bytes_b64 je odmítnut na úrovni "missing required field". */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":704,\"cmd\":\"snapshot_load_buffer\","
        "\"data\":{\"bytes_b64\":\"\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief run_until_addr: když dbgapi RUN_TO hlásí, že emulace běžela a jen
 *        se pauzla (DBGAPI_RUN_TO_PAUSED_ONLY), odpověď je chyba, ne
 *        `running: true` (mcp-inbox-fixes; dřív klient čekající na pauzu
 *        považoval zastavení mimo cíl za dosažení cíle).
 */
void test_run_until_addr_running_is_error(void) {
    dispatch_stub_reset();
    g_stub_state.run_to_result = DBGAPI_RUN_TO_PAUSED_ONLY;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":614,\"cmd\":\"run_until_addr\","
        "\"data\":{\"addr\":388}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_RUN_TO, g_stub_state.last_cmd);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_NOT_NULL(strstr(resp, "Emulator was running"));
    TEST_ASSERT_NULL(strstr(resp, "\"running\":true"));

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief snapshot_load / snapshot_load_buffer: chyba z běžícího emulátoru
 *        uvádí důvod (SNAPSHOT_ERR_NOT_PAUSED) a radu pauznout, ne jen
 *        obecné "failed" (mcp-inbox-fixes).
 */
void test_snapshot_load_not_paused_reason(void) {
    dispatch_stub_reset();
    g_stub_state.snapshot_load_fail_result = SNAPSHOT_ERR_NOT_PAUSED;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":705,\"cmd\":\"snapshot_load\","
        "\"data\":{\"path\":\"x.mzs\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_NOT_NULL(strstr(resp, "snapshot_load failed: Emulator is not paused"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "emu_pause"));
    free(resp);
    jsonl_msg_free(req);

    /* "AAAA" = base64 tří nulových bajtů (neprázdný payload). */
    req = _make_request(
        "{\"type\":\"request\",\"req_id\":706,\"cmd\":\"snapshot_load_buffer\","
        "\"data\":{\"bytes_b64\":\"AAAA\"}}");
    resp = NULL;
    rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "snapshot_load_buffer failed: Emulator is not paused"));
    free(resp);
    jsonl_msg_free(req);

    /* Jiný kód: důvod + result_code. */
    dispatch_stub_reset();
    g_stub_state.snapshot_load_fail_result = SNAPSHOT_ERR_IO;
    req = _make_request(
        "{\"type\":\"request\",\"req_id\":707,\"cmd\":\"snapshot_load\","
        "\"data\":{\"path\":\"x.mzs\"}}");
    resp = NULL;
    rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "result_code 1"));
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief cooperation_hint_set - 3 valid módy + 1 invalid.
 */
void test_cooperation_hint_set_modes(void) {
    /* 1) read_only */
    cooperation_hint_init();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":710,\"cmd\":\"cooperation_hint_set\","
        "\"data\":{\"mode\":\"read_only\",\"until\":\"next user message\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(COOPERATION_HINT_READ_ONLY,
                          g_cooperation_hint.mode);
    TEST_ASSERT_EQUAL_STRING("next user message", g_cooperation_hint.until);
    free(resp);
    jsonl_msg_free(req);

    /* 2) paused_only bez until -> until == NULL */
    req = _make_request(
        "{\"type\":\"request\",\"req_id\":711,\"cmd\":\"cooperation_hint_set\","
        "\"data\":{\"mode\":\"paused_only\"}}");
    rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(COOPERATION_HINT_PAUSED_ONLY,
                          g_cooperation_hint.mode);
    TEST_ASSERT_NULL(g_cooperation_hint.until);
    free(resp);
    jsonl_msg_free(req);

    /* 3) free (clear) */
    req = _make_request(
        "{\"type\":\"request\",\"req_id\":712,\"cmd\":\"cooperation_hint_set\","
        "\"data\":{\"mode\":\"free\"}}");
    rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(COOPERATION_HINT_FREE, g_cooperation_hint.mode);
    free(resp);
    jsonl_msg_free(req);

    /* 4) invalid mode -> INVALID_PARAMS, stav nezměněn */
    req = _make_request(
        "{\"type\":\"request\",\"req_id\":713,\"cmd\":\"cooperation_hint_set\","
        "\"data\":{\"mode\":\"yolo\"}}");
    rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(COOPERATION_HINT_FREE, g_cooperation_hint.mode);
    free(resp);
    jsonl_msg_free(req);

    /* Cleanup po testu - vrátit globál do default stavu. */
    cooperation_hint_init();
}


/* ====================================================================== */
/* V1.A.2 - Symbol management Tools                                        */
/* ====================================================================== */

/**
 * @brief symbol_add happy path - handler předá addr/name/comment dbgapi
 *        SYMBOL_ADD handleru s correct origin.
 */
void test_symbol_add_happy(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":800,\"cmd\":\"symbol_add\","
        "\"data\":{\"addr\":17152,\"name\":\"ROM_CHR_OUT\","
        "\"comment\":\"print char in A\",\"kind\":\"LABEL\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SYMBOL_ADD, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    TEST_ASSERT_EQUAL_UINT16(17152, g_stub_state.sym_last_addr);
    TEST_ASSERT_EQUAL_STRING("ROM_CHR_OUT", g_stub_state.sym_last_name);
    TEST_ASSERT_EQUAL_STRING("print char in A",
                             g_stub_state.sym_last_comment);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "added"));
    TEST_ASSERT_EQUAL_INT(17152,
        json_object_get_int_member(data, "addr"));
    TEST_ASSERT_EQUAL_STRING("ROM_CHR_OUT",
        json_object_get_string_member(data, "name"));
    TEST_ASSERT_EQUAL_STRING("LABEL",
        json_object_get_string_member(data, "kind"));

    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_add odmítne whitespace / prázdné name.
 */
void test_symbol_add_invalid_name(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":801,\"cmd\":\"symbol_add\","
        "\"data\":{\"addr\":4242,\"name\":\"bad name\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_add odmítne addr mimo rozsah.
 */
void test_symbol_add_invalid_addr(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":802,\"cmd\":\"symbol_add\","
        "\"data\":{\"addr\":70000,\"name\":\"BIG\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_remove by name happy path.
 */
void test_symbol_remove_by_name(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":810,\"cmd\":\"symbol_remove\","
        "\"data\":{\"name\":\"ROM_CHR_OUT\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SYMBOL_REMOVE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_STRING("ROM_CHR_OUT", g_stub_state.sym_last_name);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "removed"));
    TEST_ASSERT_EQUAL_STRING("ROM_CHR_OUT",
        json_object_get_string_member(data, "name"));

    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_remove by addr happy path.
 */
void test_symbol_remove_by_addr(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":811,\"cmd\":\"symbol_remove\","
        "\"data\":{\"addr\":17152}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SYMBOL_REMOVE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_UINT16(17152, g_stub_state.sym_last_addr);
    TEST_ASSERT_NULL(g_stub_state.sym_last_name);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_remove odmítne both name AND addr (= exclusive).
 */
void test_symbol_remove_exclusive(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":812,\"cmd\":\"symbol_remove\","
        "\"data\":{\"name\":\"X\",\"addr\":4242}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_lookup hex string -> dispatch detekuje hex a předá addr.
 */
void test_symbol_lookup_hex_address(void) {
    /* Stub vrátí fake found, dispatch ho předá v response. */
    g_stub_state.sym_lookup_found        = true;
    g_stub_state.sym_lookup_fake_addr    = 0x4242;
    g_stub_state.sym_lookup_fake_name    = "TARGET";
    g_stub_state.sym_lookup_fake_comment = "hit";
    g_stub_state.sym_lookup_fake_source  = 3;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":820,\"cmd\":\"symbol_lookup\","
        "\"data\":{\"query\":\"0x4242\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SYMBOL_LOOKUP, g_stub_state.last_cmd);
    /* hex -> name == NULL, addr == 0x4242 */
    TEST_ASSERT_NULL(g_stub_state.sym_last_name);
    TEST_ASSERT_EQUAL_UINT16(0x4242, g_stub_state.sym_last_addr);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "found"));
    TEST_ASSERT_EQUAL_INT(0x4242,
        json_object_get_int_member(data, "addr"));
    TEST_ASSERT_EQUAL_STRING("TARGET",
        json_object_get_string_member(data, "name"));
    TEST_ASSERT_EQUAL_STRING("hit",
        json_object_get_string_member(data, "comment"));
    TEST_ASSERT_EQUAL_INT(3,
        json_object_get_int_member(data, "source"));

    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_lookup s name (= not hex) -> dispatch předá name.
 */
void test_symbol_lookup_by_name(void) {
    g_stub_state.sym_lookup_found        = true;
    g_stub_state.sym_lookup_fake_addr    = 0xE800;
    g_stub_state.sym_lookup_fake_name    = "ROM_CHR_OUT";
    g_stub_state.sym_lookup_fake_comment = NULL;
    g_stub_state.sym_lookup_fake_source  = 3;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":821,\"cmd\":\"symbol_lookup\","
        "\"data\":{\"query\":\"ROM_CHR_OUT\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_STRING("ROM_CHR_OUT", g_stub_state.sym_last_name);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_lookup not found -> found=false v response, žádné addr/name.
 */
void test_symbol_lookup_not_found(void) {
    g_stub_state.sym_lookup_found = false;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":822,\"cmd\":\"symbol_lookup\","
        "\"data\":{\"query\":\"NOPE\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "found"));

    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_list happy path s prefix filterem - stub vrátí 3 záznamy.
 */
void test_symbol_list_prefix_and_items(void) {
    g_stub_state.sym_list_fake_count = 3;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":830,\"cmd\":\"symbol_list\","
        "\"data\":{\"prefix\":\"ROM_\",\"limit\":50}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SYMBOL_LIST, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_STRING("ROM_", g_stub_state.sym_last_prefix);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_INT(3, json_object_get_int_member(data, "count"));
    JsonArray *items = json_object_get_array_member(data, "items");
    TEST_ASSERT_NOT_NULL(items);
    TEST_ASSERT_EQUAL_INT(3, json_array_get_length(items));
    JsonObject *item0 = json_array_get_object_element(items, 0);
    TEST_ASSERT_EQUAL_INT(0x4000,
        json_object_get_int_member(item0, "addr"));
    TEST_ASSERT_EQUAL_STRING("SYM_0",
        json_object_get_string_member(item0, "name"));
    TEST_ASSERT_EQUAL_INT(3,
        json_object_get_int_member(item0, "source"));

    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief symbol_list odmítne limit mimo 1..1000.
 */
void test_symbol_list_invalid_limit(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":831,\"cmd\":\"symbol_list\","
        "\"data\":{\"limit\":5000}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* V1.A.3 - step out + run_until_* Tools                                   */
/* ====================================================================== */

/**
 * @brief step_out happy path - stub vrátí success=0 + return_addr.
 *
 * Dispatch.c handler musí předat max_cycles do payloadu, ověřit
 * návratnost CMD_STEP_OUT a sestavit response s return_addr.
 */
void test_step_out_happy(void) {
    g_stub_state.step_out_fake_status      = 0;
    g_stub_state.step_out_fake_return_addr = 0x1234;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":900,\"cmd\":\"step_out\","
        "\"data\":{\"max_cycles\":500000}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_STEP_OUT, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(0x1234,
        json_object_get_int_member(data, "return_addr"));
    TEST_ASSERT_EQUAL_INT(500000,
        json_object_get_int_member(data, "max_cycles"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "running"));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief step_out musí vrátit chybu když callstack tracking neaktivní
 *        (= status=1 ze stub).
 */
void test_step_out_no_callstack_fallback(void) {
    g_stub_state.step_out_fake_status = 1;  /* callstack inactive */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":901,\"cmd\":\"step_out\"}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    const char *err = json_object_get_string_member(obj, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(strstr(err, "callstack") != NULL);
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief run_until_raster musí odmítnout line mimo 0..511.
 */
void test_run_until_raster_invalid_line(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":902,"
        "\"cmd\":\"run_until_raster\","
        "\"data\":{\"line\":1000}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief run_until_raster reached path - polling smyčka detekuje
 *        cílovou scanline.
 *
 * Stub raster sekvence: výchozí (frame=10, line=100), step_lines=20
 * per call, step_cycles=80. Target line=120 → smyčka by měla provést
 * cca 1 STEP_INTO + GET_RASTER druhé volání = reached.
 */
void test_run_until_raster_reached(void) {
    g_stub_state.raster_initial_scanline = 100;
    g_stub_state.raster_seq_step_lines   = 20;
    g_stub_state.raster_seq_step_cycles  = 80;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":903,"
        "\"cmd\":\"run_until_raster\","
        "\"data\":{\"line\":120,\"col\":-1,\"max_cycles\":10000}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "reached"));
    /* scanline by měla být >= 120. */
    int line = (int)json_object_get_int_member(data, "scanline");
    TEST_ASSERT_TRUE(line >= 120);
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief run_until_tstate musí odmítnout target <= current cycle counter.
 */
void test_run_until_tstate_target_in_past(void) {
    g_stub_state.raster_initial_total_cycles = 5000;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":904,"
        "\"cmd\":\"run_until_tstate\","
        "\"data\":{\"target_total_cycles\":1000}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    const char *err = json_object_get_string_member(obj, "error");
    TEST_ASSERT_TRUE(err && strstr(err, "past") != NULL);
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief run_until_tstate reached path - polling smyčka detekuje
 *        target cycles.
 */
void test_run_until_tstate_reached(void) {
    g_stub_state.raster_initial_total_cycles = 1000;
    g_stub_state.raster_seq_step_cycles      = 200;
    g_stub_state.raster_seq_step_lines       = 1;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":905,"
        "\"cmd\":\"run_until_tstate\","
        "\"data\":{\"target_total_cycles\":1500,\"max_cycles\":5000}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "reached"));
    int cycles = (int)json_object_get_int_member(data, "total_cycles");
    TEST_ASSERT_TRUE(cycles >= 1500);
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief run_until_tstate s target_ticks: cíl v minulosti nebo teď vrátí
 *        hned reached a aktuální čas (bez běhu).
 */
void test_run_until_tstate_ticks_reached_now(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":907,"
        "\"cmd\":\"run_until_tstate\","
        "\"data\":{\"target_ticks\":1000}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "reached"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "exact"));
    TEST_ASSERT_EQUAL_INT64(1000, json_object_get_int_member(data, "ticks"));
    TEST_ASSERT_EQUAL_INT64(0, json_object_get_int_member(data, "delta_ticks"));
    TEST_ASSERT_EQUAL_STRING("run_until_ticks", json_object_get_string_member(data, "stopped_by"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "time_limited"));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief run_until_tstate: target_ticks a target_total_cycles naráz
 *        a záporný target_ticks jsou chyba parametrů.
 */
void test_run_until_tstate_ticks_invalid(void) {
    static const char *const reqs[] = {
        "{\"type\":\"request\",\"req_id\":908,\"cmd\":\"run_until_tstate\","
        "\"data\":{\"target_ticks\":5000,\"target_total_cycles\":10}}",
        "{\"type\":\"request\",\"req_id\":909,\"cmd\":\"run_until_tstate\","
        "\"data\":{\"target_ticks\":-1}}",
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(reqs); i++) {
        st_JSONL_MESSAGE *req = _make_request(reqs[i]);
        char *resp = NULL;
        en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
        free(resp);
        jsonl_msg_free(req);
    }
}


/**
 * @brief run_until_event musí odmítnout neznámý kind.
 */
void test_run_until_event_unsupported_kind(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":906,"
        "\"cmd\":\"run_until_event\","
        "\"data\":{\"kind\":\"madeup_kind\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "success"));
    const char *err = json_object_get_string_member(obj, "error");
    TEST_ASSERT_TRUE(err && strstr(err, "Unsupported event kind") != NULL);
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief run_until_event io_write vrátí chybu (V1.A.3 not implemented).
 */
void test_run_until_event_io_write_not_implemented(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":907,"
        "\"cmd\":\"run_until_event\","
        "\"data\":{\"kind\":\"io_write\",\"params\":{\"port\":160}}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    const char *err = json_object_get_string_member(obj, "error");
    TEST_ASSERT_TRUE(err && strstr(err, "eventlog") != NULL);
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief run_until_event frame_done detekuje uplynutí N framů.
 *
 * Stub: výchozí frame=10, step_lines=300 (= téměř celé PAL frame
 * 312), takže každé 2 volání ~= 1 frame.
 */
void test_run_until_event_frame_done(void) {
    g_stub_state.raster_initial_frame    = 10;
    g_stub_state.raster_initial_scanline = 0;
    g_stub_state.raster_seq_step_lines   = 312;   /* 1 frame na call */
    g_stub_state.raster_seq_step_cycles  = 100;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":908,"
        "\"cmd\":\"run_until_event\","
        "\"data\":{\"kind\":\"frame_done\","
        "\"params\":{\"count\":2},\"max_cycles\":10000}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "reached"));
    int frames = (int)json_object_get_int_member(data, "frames_done");
    TEST_ASSERT_TRUE(frames >= 2);
    TEST_ASSERT_EQUAL_STRING("frame_done",
        json_object_get_string_member(data, "kind"));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* V1.A.4 - EVENT subscribe + TRAP forwarding tests                        */
/* ====================================================================== */

/* Pozn.: tyto testy ověřují wire-level chování dispatch handlerů.
 * event_bus + trap_manager mají vlastní glob state - reset přes
 * event_bus_shutdown + event_bus_init na začátku každého testu zde
 * by byl možný, ale pro standalone dispatch testy stačí dependency
 * na monotonicitě - tests běží sekvenčně a state se akumuluje
 * benigně (= subscribe je idempotentní union, unsubscribe je clean). */

#include "../../src/emulator/mcp/event_bus.h"
#include "../../src/emulator/mcp/trap_manager.h"


/**
 * @brief event_subscribe vrací success + echo topics.
 */
void test_event_subscribe_topics(void) {
    event_bus_init();
    event_bus_unsubscribe(EVENT_BUS_CONN_PIPE, NULL); /* clean slate */

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":1000,"
        "\"cmd\":\"event_subscribe\","
        "\"data\":{\"topics\":[\"breakpoint_hit\",\"paused\"]}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(2,
        (int)json_object_get_int_member(data, "topics_count"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "ok"));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief event_subscribe s prázdným topics arrayem vrací INVALID_PARAMS.
 */
void test_event_subscribe_empty_rejected(void) {
    event_bus_init();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":1001,"
        "\"cmd\":\"event_subscribe\","
        "\"data\":{\"topics\":[]}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    /* Error response je validní JSONL. */
    TEST_ASSERT_NOT_NULL(resp);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief event_poll s prázdnou queue + timeout 0 vrátí prázdné pole.
 */
void test_event_poll_empty_queue(void) {
    event_bus_init();
    /* Zajistíme prázdnou queue - unsubscribe all. */
    event_bus_unsubscribe(EVENT_BUS_CONN_PIPE, NULL);

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":1002,"
        "\"cmd\":\"event_poll\","
        "\"data\":{\"timeout_ms\":0,\"max_events\":10}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(0, (int)json_object_get_int_member(data, "count"));
    JsonArray *events = json_object_get_array_member(data, "events");
    TEST_ASSERT_EQUAL_INT(0, (int)json_array_get_length(events));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Emit na subscribed topic se dostane do poll responsu.
 */
void test_event_emit_pushes_to_subscriber(void) {
    event_bus_init();
    event_bus_unsubscribe(EVENT_BUS_CONN_PIPE, NULL);

    /* Subscribe na "paused" topic. */
    const char *topics[] = { "paused", NULL };
    event_bus_attach(EVENT_BUS_CONN_PIPE);
    bool ok = event_bus_subscribe(EVENT_BUS_CONN_PIPE, topics);
    TEST_ASSERT_TRUE(ok);

    /* Emit jako kdyby z BP hooku. */
    JsonObject *payload = json_object_new();
    json_object_set_string_member(payload, "reason", "test");
    json_object_set_int_member(payload, "pc", 0x1234);
    event_bus_emit("paused", payload);

    /* Poll - musí vrátit 1 event. */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":1003,"
        "\"cmd\":\"event_poll\","
        "\"data\":{\"timeout_ms\":0,\"max_events\":10}}");
    char *resp = NULL;
    mcp_dispatch_request(req, &resp);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    int count = (int)json_object_get_int_member(data, "count");
    TEST_ASSERT_EQUAL_INT(1, count);
    JsonArray *events = json_object_get_array_member(data, "events");
    JsonObject *e0 = json_array_get_object_element(events, 0);
    TEST_ASSERT_EQUAL_STRING("paused",
        json_object_get_string_member(e0, "topic"));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);

    /* Cleanup pro další test. */
    event_bus_unsubscribe(EVENT_BUS_CONN_PIPE, NULL);
}


/**
 * @brief trap_respond pro neznámý trap_id vrátí ok=false, nesubmituje CMD.
 */
void test_trap_respond_unknown_id(void) {
    trap_manager_init();
    dispatch_stub_reset();
    g_stub_state.is_running = false;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":1004,"
        "\"cmd\":\"trap_respond\","
        "\"data\":{\"trap_id\":99999,\"action\":\"continue\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "ok"));
    /* CMD neměl být submitnut - stub last_cmd zůstal NONE. */
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_NONE, g_stub_state.last_cmd);
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief trap_respond s validním ID a action="continue" submituje CMD_RUN.
 */
void test_trap_respond_continue_submits_run(void) {
    trap_manager_init();
    dispatch_stub_reset();

    /* Zaregistruj trap přímo (= simulace BP hooku). */
    int64_t tid = trap_manager_register();
    TEST_ASSERT_TRUE(tid > 0);

    char body[256];
    g_snprintf(body, sizeof(body),
        "{\"type\":\"request\",\"req_id\":1005,"
        "\"cmd\":\"trap_respond\","
        "\"data\":{\"trap_id\":%lld,\"action\":\"continue\"}}",
        (long long)tid);
    st_JSONL_MESSAGE *req = _make_request(body);
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "ok"));
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_RUN, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief trap_respond s neplatnou action vrátí INVALID_PARAMS.
 */
void test_trap_respond_invalid_action(void) {
    trap_manager_init();
    int64_t tid = trap_manager_register();
    char body[256];
    g_snprintf(body, sizeof(body),
        "{\"type\":\"request\",\"req_id\":1006,"
        "\"cmd\":\"trap_respond\","
        "\"data\":{\"trap_id\":%lld,\"action\":\"foobar\"}}",
        (long long)tid);
    st_JSONL_MESSAGE *req = _make_request(body);
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_NOT_NULL(resp);
    /* Trap zůstává v mapě - test_trap_respond_continue ho consumne (= sekvenční
     * závislost benigní díky uniqueness ID). */
    free(resp);
    jsonl_msg_free(req);
    trap_manager_consume(tid); /* explicit cleanup */
}


/* ====================================================================== */
/* V1.A.5 - chip-level fault injection tests                              */
/* ====================================================================== */

void test_io_read_happy(void) {
    g_stub_state.io_read_fake_value = 0xCD;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":901,\"cmd\":\"io_read\","
        "\"data\":{\"port\":240}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_IO_READ, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    TEST_ASSERT_EQUAL_UINT16(240, g_stub_state.io_last_port);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(240, json_object_get_int_member(data, "port"));
    TEST_ASSERT_EQUAL_INT(0xCD, json_object_get_int_member(data, "value"));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


void test_io_read_invalid_port(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":902,\"cmd\":\"io_read\","
        "\"data\":{\"port\":99999}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_io_write_happy(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":903,\"cmd\":\"io_write\","
        "\"data\":{\"port\":224,\"value\":42}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_IO_WRITE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_UINT16(224, g_stub_state.io_last_port);
    TEST_ASSERT_EQUAL_UINT8(42, g_stub_state.io_last_value);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_INT(224, json_object_get_int_member(data, "port"));
    TEST_ASSERT_EQUAL_INT(42,  json_object_get_int_member(data, "value"));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


void test_io_write_invalid_value(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":904,\"cmd\":\"io_write\","
        "\"data\":{\"port\":0,\"value\":256}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_irq_inject_with_vector(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":905,\"cmd\":\"irq_inject\","
        "\"data\":{\"source\":\"psg\",\"vector\":131}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_IRQ_INJECT, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.irq_inject_last_vector_valid);
    TEST_ASSERT_EQUAL_UINT8(131, g_stub_state.irq_inject_last_vector);
    TEST_ASSERT_NOT_NULL(g_stub_state.irq_inject_last_source);
    TEST_ASSERT_EQUAL_STRING("psg", g_stub_state.irq_inject_last_source);
    free(resp);
    jsonl_msg_free(req);
}


void test_irq_inject_default(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":906,\"cmd\":\"irq_inject\","
        "\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.irq_inject_last_vector_valid);
    TEST_ASSERT_EQUAL_STRING("manual", g_stub_state.irq_inject_last_source);
    free(resp);
    jsonl_msg_free(req);
}


void test_nmi_inject_happy(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":907,\"cmd\":\"nmi_inject\"}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_NMI_INJECT, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.nmi_inject_calls);
    free(resp);
    jsonl_msg_free(req);
}


void test_mem_write_force_happy(void) {
    /* data_hex "DEADBEEF" = 4 bajty */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":908,\"cmd\":\"mem_write_force\","
        "\"data\":{\"addr\":4096,\"data_hex\":\"DEADBEEF\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_MEM_WRITE_FORCE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_UINT16(4096, g_stub_state.mwf_addr);
    TEST_ASSERT_EQUAL_UINT16(4, g_stub_state.mwf_len);
    TEST_ASSERT_EQUAL_UINT8(0xDE, g_stub_state.mwf_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xEF, g_stub_state.mwf_buf[3]);
    free(resp);
    jsonl_msg_free(req);
}


void test_mem_write_force_odd_hex(void) {
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":909,\"cmd\":\"mem_write_force\","
        "\"data\":{\"addr\":0,\"data_hex\":\"AAB\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* V1.A.6 - Watch + Callstack + CDL Tools                                  */
/* ====================================================================== */

void test_watch_add_address_happy(void) {
    /* mode=address (default), addr=0x4000, name="foo", type="u8". Stub
     * vrátí out_index=3 (= cokoliv). Test ověří origin=MCP + správný
     * cmd + zachycený obsah parametru. */
    dispatch_stub_reset();
    g_stub_state.watch_add_fake_index = 3;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1001,\"cmd\":\"watch_add\","
        "\"data\":{\"name\":\"foo\",\"addr\":16384,\"type\":\"u8\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_WATCH_ADD, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    TEST_ASSERT_EQUAL_INT(DBGAPI_WATCH_MODE_ADDRESS,
                           g_stub_state.watch_add_last_mode);
    TEST_ASSERT_EQUAL_UINT16(16384, g_stub_state.watch_add_last_addr);
    TEST_ASSERT_NOT_NULL(g_stub_state.watch_add_last_name);
    TEST_ASSERT_EQUAL_STRING("foo", g_stub_state.watch_add_last_name);
    TEST_ASSERT_TRUE(strstr(resp, "\"index\":3") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_watch_add_expr_scalar(void) {
    /* mode=expr_scalar + expr="HL+1" + type=u16le. Test ověří že stub
     * obdržel expr_text + správný mode. */
    dispatch_stub_reset();
    g_stub_state.watch_add_fake_index = 7;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1002,\"cmd\":\"watch_add\","
        "\"data\":{\"mode\":\"expr_scalar\",\"expr\":\"HL+1\","
        "\"type\":\"u16le\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_WATCH_MODE_EXPR_SCALAR,
                           g_stub_state.watch_add_last_mode);
    TEST_ASSERT_EQUAL_INT(DBGAPI_WATCH_TYPE_U16LE,
                           g_stub_state.watch_add_last_type);
    TEST_ASSERT_NOT_NULL(g_stub_state.watch_add_last_expr_text);
    TEST_ASSERT_EQUAL_STRING("HL+1",
                              g_stub_state.watch_add_last_expr_text);
    free(resp);
    jsonl_msg_free(req);
}


void test_watch_add_expr_missing_text(void) {
    /* mode=expr_deref bez expr field => INVALID_PARAMS. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1003,\"cmd\":\"watch_add\","
        "\"data\":{\"mode\":\"expr_deref\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_watch_remove_by_name_happy(void) {
    /* watch_remove podle jména. Stub má watch_remove_found=1 + zapíše
     * removed=true. */
    dispatch_stub_reset();
    g_stub_state.watch_remove_found = 1;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1004,\"cmd\":\"watch_remove\","
        "\"data\":{\"name\":\"bar\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_WATCH_REMOVE, g_stub_state.last_cmd);
    TEST_ASSERT_NOT_NULL(g_stub_state.watch_remove_last_name);
    TEST_ASSERT_EQUAL_STRING("bar",
                              g_stub_state.watch_remove_last_name);
    TEST_ASSERT_TRUE(strstr(resp, "\"removed\":true") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_watch_remove_missing_params(void) {
    /* Bez name a bez index => INVALID_PARAMS. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1005,\"cmd\":\"watch_remove\","
        "\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_watch_list_returns_items(void) {
    /* Stub vyplní 2 fake záznamy. Test ověří, že JSON resp obsahuje
     * count=2 + items array s W_0 / W_1. */
    dispatch_stub_reset();
    g_stub_state.watch_list_fake_count = 2;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1006,\"cmd\":\"watch_list\"}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_WATCH_LIST, g_stub_state.last_cmd);
    TEST_ASSERT_TRUE(strstr(resp, "\"count\":2") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"W_0\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"W_1\"") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_watch_eval_inline_expr(void) {
    /* Ad-hoc eval výrazu "0x4000+10". Stub vyplní out_value_int=42,
     * out_value_str="42". */
    dispatch_stub_reset();
    g_stub_state.watch_eval_fake_value_int = 42;
    g_stub_state.watch_eval_fake_value_str = "42";
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1007,\"cmd\":\"watch_eval\","
        "\"data\":{\"expr\":\"0x4000+10\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_WATCH_EVAL, g_stub_state.last_cmd);
    TEST_ASSERT_NOT_NULL(g_stub_state.watch_eval_last_expr_text);
    TEST_ASSERT_EQUAL_STRING("0x4000+10",
                              g_stub_state.watch_eval_last_expr_text);
    TEST_ASSERT_TRUE(strstr(resp, "\"value_int\":42") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_watch_eval_missing_params(void) {
    /* Bez name / index / expr => INVALID_PARAMS. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1008,\"cmd\":\"watch_eval\","
        "\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_callstack_get_max_depth_limit(void) {
    /* Stub vyplní 5 fake entries, klient žádá max_depth=3 => emit=3. */
    dispatch_stub_reset();
    g_stub_state.callstack_fake_count = 5;
    g_stub_state.callstack_fake_active = 1;
    g_stub_state.callstack_fake_current_depth = 5;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1009,\"cmd\":\"callstack_get\","
        "\"data\":{\"max_depth\":3}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_GET_CALLSTACK,
                           g_stub_state.last_cmd);
    TEST_ASSERT_TRUE(strstr(resp, "\"count\":3") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"active\":true") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_callstack_get_invalid_depth(void) {
    /* max_depth mimo 1..256 => INVALID_PARAMS. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1010,\"cmd\":\"callstack_get\","
        "\"data\":{\"max_depth\":0}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_cdl_start_stop_reset_lifecycle(void) {
    /* Sekvence start -> stop -> reset. Každé volání musí zvýšit
     * příslušný counter ve stubu o 1. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req1 = _make_request(
        "{\"type\":\"request\",\"id\":1011,\"cmd\":\"cdl_start\"}");
    char *resp1 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req1, &resp1));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.cdl_start_calls);
    free(resp1);
    jsonl_msg_free(req1);

    st_JSONL_MESSAGE *req2 = _make_request(
        "{\"type\":\"request\",\"id\":1012,\"cmd\":\"cdl_stop\"}");
    char *resp2 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req2, &resp2));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.cdl_stop_calls);
    free(resp2);
    jsonl_msg_free(req2);

    st_JSONL_MESSAGE *req3 = _make_request(
        "{\"type\":\"request\",\"id\":1013,\"cmd\":\"cdl_reset\"}");
    char *resp3 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req3, &resp3));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.cdl_reset_calls);
    free(resp3);
    jsonl_msg_free(req3);
}


void test_cdl_export_happy(void) {
    /* Export s validním path. Stub zachytí path + naplní fake_region_count. */
    dispatch_stub_reset();
    g_stub_state.cdl_export_fake_result = 0;
    g_stub_state.cdl_export_fake_region_count = 7;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1014,\"cmd\":\"cdl_export\","
        "\"data\":{\"path\":\"/tmp/snap1.json\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_CDL_EXPORT, g_stub_state.last_cmd);
    TEST_ASSERT_NOT_NULL(g_stub_state.cdl_export_last_path);
    TEST_ASSERT_EQUAL_STRING("/tmp/snap1.json",
                              g_stub_state.cdl_export_last_path);
    TEST_ASSERT_TRUE(strstr(resp, "\"region_count\":7") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_cdl_export_missing_path(void) {
    /* Export bez path => INVALID_PARAMS. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1015,\"cmd\":\"cdl_export\","
        "\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* video-capture Task 15 - videorec_* Tools                                */
/* ====================================================================== */

/**
 * @brief Pomocník: dispatchne request, vrátí rc a odpověď (caller uvolní free()).
 */
static en_MCP_DISPATCH_RESULT _vr_dispatch(const char *line, char **out_resp) {
    st_JSONL_MESSAGE *req = _make_request(line);
    *out_resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, out_resp);
    jsonl_msg_free(req);
    return rc;
}

/**
 * @brief videorec_start s cestou a frames: předá START, path, frames; odpověď
 *        nese start_requested a stav (čekající start, cesta, sidecar).
 */
void test_videorec_start_with_path_and_frames(void) {
    dispatch_stub_reset();
    g_stub_state.videorec_fake.out_supported = 1;
    g_stub_state.videorec_fake.out_start_pending = 1;
    g_stub_state.videorec_fake.out_fps = 50;
    g_strlcpy(g_stub_state.videorec_fake.out_path, "C:/rec/game.avi",
              sizeof(g_stub_state.videorec_fake.out_path));
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1501,\"cmd\":\"videorec_start\","
        "\"data\":{\"path\":\"C:/rec/game.avi\",\"frames\":250}}", &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_VIDEOREC, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_VIDEOREC_OP_START, g_stub_state.videorec_last_op);
    TEST_ASSERT_EQUAL_STRING("C:/rec/game.avi", g_stub_state.videorec_last_path);
    TEST_ASSERT_EQUAL_UINT64(250, g_stub_state.videorec_last_frames);

    JsonParser *parser = NULL;
    JsonObject *root = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(root, "success"));
    JsonObject *d = json_object_get_object_member(root, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "start_requested"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "start_pending"));
    TEST_ASSERT_EQUAL_STRING("idle", json_object_get_string_member(d, "state"));
    TEST_ASSERT_EQUAL_STRING("C:/rec/game.avi", json_object_get_string_member(d, "path"));
    TEST_ASSERT_EQUAL_STRING("C:/rec/game.cuts.json", json_object_get_string_member(d, "sidecar"));
    TEST_ASSERT_EQUAL_INT(250, json_object_get_int_member(d, "stop_after_frames"));
    g_object_unref(parser);
    free(resp);
}

/** @brief videorec_start bez parametrů: path NULL (vygenerované jméno), frames 0. */
void test_videorec_start_without_path(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1502,\"cmd\":\"videorec_start\"}", &resp));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.videorec_calls);
    TEST_ASSERT_NULL(g_stub_state.videorec_last_path);
    TEST_ASSERT_EQUAL_UINT64(0, g_stub_state.videorec_last_frames);
    /* Prázdná cesta ve stavu => path null. */
    TEST_ASSERT_TRUE(strstr(resp, "\"path\":null") != NULL);
    free(resp);
}

/** @brief Neplatné parametry startu (frames záporné / ne-int, path ne-string) => INVALID_PARAMS bez submitu. */
void test_videorec_start_invalid_params(void) {
    const char *bad[] = {
        "{\"type\":\"request\",\"req_id\":1503,\"cmd\":\"videorec_start\",\"data\":{\"frames\":-1}}",
        "{\"type\":\"request\",\"req_id\":1504,\"cmd\":\"videorec_start\",\"data\":{\"frames\":\"10\"}}",
        "{\"type\":\"request\",\"req_id\":1505,\"cmd\":\"videorec_start\",\"data\":{\"path\":42}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        dispatch_stub_reset();
        char *resp = NULL;
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _vr_dispatch(bad[i], &resp));
        TEST_ASSERT_EQUAL_INT(0, g_stub_state.videorec_calls);
        TEST_ASSERT_TRUE(strstr(resp, "\"success\":false") != NULL);
        free(resp);
    }
}

/** @brief Chyba jádra (např. nepodporovaná platforma) se vrátí čitelně anglicky. */
void test_videorec_start_core_error(void) {
    dispatch_stub_reset();
    g_stub_state.videorec_fake.out_result = DBGAPI_VIDEOREC_RESULT_FAILED;
    g_strlcpy(g_stub_state.videorec_fake.out_error,
              "Video recording is not possible on this platform: test",
              sizeof(g_stub_state.videorec_fake.out_error));
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1506,\"cmd\":\"videorec_start\"}", &resp));
    TEST_ASSERT_TRUE(strstr(resp, "\"success\":false") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "Video recording is not possible on this platform: test") != NULL);
    free(resp);
}

/** @brief videorec_stop: op STOP, odpověď stop_requested + start_cancelled. */
void test_videorec_stop(void) {
    dispatch_stub_reset();
    g_stub_state.videorec_fake.out_start_cancelled = 1;
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1507,\"cmd\":\"videorec_stop\"}", &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_VIDEOREC_OP_STOP, g_stub_state.videorec_last_op);
    TEST_ASSERT_TRUE(strstr(resp, "\"stop_requested\":true") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"start_cancelled\":true") != NULL);
    free(resp);
}

/** @brief videorec_pause: bez paused = toggle (-1, paused_target null), true = 1, false = 0, jiný typ = INVALID_PARAMS. */
void test_videorec_pause_variants(void) {
    struct { const char *line; int expect; const char *target; } ok[] = {
        { "{\"type\":\"request\",\"req_id\":1508,\"cmd\":\"videorec_pause\"}", -1, "\"paused_target\":null" },
        { "{\"type\":\"request\",\"req_id\":1509,\"cmd\":\"videorec_pause\",\"data\":{\"paused\":null}}", -1, "\"paused_target\":null" },
        { "{\"type\":\"request\",\"req_id\":1510,\"cmd\":\"videorec_pause\",\"data\":{\"paused\":true}}", 1, "\"paused_target\":true" },
        { "{\"type\":\"request\",\"req_id\":1511,\"cmd\":\"videorec_pause\",\"data\":{\"paused\":false}}", 0, "\"paused_target\":false" },
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        dispatch_stub_reset();
        char *resp = NULL;
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _vr_dispatch(ok[i].line, &resp));
        TEST_ASSERT_EQUAL_INT(DBGAPI_VIDEOREC_OP_PAUSE, g_stub_state.videorec_last_op);
        TEST_ASSERT_EQUAL_INT(ok[i].expect, g_stub_state.videorec_last_paused);
        TEST_ASSERT_TRUE(strstr(resp, "\"pause_requested\":true") != NULL);
        TEST_ASSERT_TRUE_MESSAGE(strstr(resp, ok[i].target) != NULL, ok[i].target);
        free(resp);
    }
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1512,\"cmd\":\"videorec_pause\",\"data\":{\"paused\":\"yes\"}}", &resp));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.videorec_calls);
    free(resp);
}

/** @brief videorec_marker: předá label, vrátí skutečně použitý popisek. */
void test_videorec_marker_label(void) {
    dispatch_stub_reset();
    g_strlcpy(g_stub_state.videorec_fake.out_label, "Level 2",
              sizeof(g_stub_state.videorec_fake.out_label));
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1513,\"cmd\":\"videorec_marker\",\"data\":{\"label\":\"Level 2\"}}", &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_VIDEOREC_OP_MARKER, g_stub_state.videorec_last_op);
    TEST_ASSERT_EQUAL_STRING("Level 2", g_stub_state.videorec_last_label);
    TEST_ASSERT_TRUE(strstr(resp, "\"label\":\"Level 2\"") != NULL);
    free(resp);

    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1514,\"cmd\":\"videorec_marker\",\"data\":{\"label\":7}}", &resp));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.videorec_calls);
    free(resp);
}

/** @brief Marker / pauza bez nahrávání => čitelná chyba z jádra (NOT_RUNNING). */
void test_videorec_marker_not_running(void) {
    dispatch_stub_reset();
    g_stub_state.videorec_fake.out_result = DBGAPI_VIDEOREC_RESULT_NOT_RUNNING;
    g_strlcpy(g_stub_state.videorec_fake.out_error, "Video recording is not running",
              sizeof(g_stub_state.videorec_fake.out_error));
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1515,\"cmd\":\"videorec_marker\"}", &resp));
    TEST_ASSERT_TRUE(strstr(resp, "Video recording is not running") != NULL);
    free(resp);
}

/** @brief videorec_status: kompletní payload stavu včetně poslední události. */
/**
 * @brief videorec_timebase: "realtime" / "emulated" => op TIMEBASE s 1 / 0,
 *        odpověď timebase_requested + stav (timebase, timebase_effective,
 *        rt_activity); chybějící nebo neplatná hodnota => INVALID_PARAMS bez submitu.
 */
void test_videorec_timebase(void) {
    struct { const char *line; int expect; } ok[] = {
        { "{\"type\":\"request\",\"req_id\":1520,\"cmd\":\"videorec_timebase\",\"data\":{\"timebase\":\"realtime\"}}", 1 },
        { "{\"type\":\"request\",\"req_id\":1521,\"cmd\":\"videorec_timebase\",\"data\":{\"timebase\":\"emulated\"}}", 0 },
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        dispatch_stub_reset();
        g_stub_state.videorec_fake.out_timebase = 1;
        g_stub_state.videorec_fake.out_timebase_effective = 0;
        g_stub_state.videorec_fake.out_rt_activity = 0;
        char *resp = NULL;
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _vr_dispatch(ok[i].line, &resp));
        TEST_ASSERT_EQUAL_INT(DBGAPI_VIDEOREC_OP_TIMEBASE, g_stub_state.videorec_last_op);
        TEST_ASSERT_EQUAL_INT(ok[i].expect, g_stub_state.videorec_last_timebase);
        TEST_ASSERT_TRUE(strstr(resp, "\"timebase_requested\":true") != NULL);
        TEST_ASSERT_TRUE(strstr(resp, "\"timebase\":\"realtime\"") != NULL);
        TEST_ASSERT_TRUE(strstr(resp, "\"timebase_effective\":\"emulated\"") != NULL);
        TEST_ASSERT_TRUE(strstr(resp, "\"rt_activity\":\"off\"") != NULL);
        free(resp);
    }
    const char *bad[] = {
        "{\"type\":\"request\",\"req_id\":1522,\"cmd\":\"videorec_timebase\"}",
        "{\"type\":\"request\",\"req_id\":1523,\"cmd\":\"videorec_timebase\",\"data\":{\"timebase\":\"fast\"}}",
        "{\"type\":\"request\",\"req_id\":1524,\"cmd\":\"videorec_timebase\",\"data\":{\"timebase\":1}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        dispatch_stub_reset();
        char *resp = NULL;
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _vr_dispatch(bad[i], &resp));
        TEST_ASSERT_EQUAL_INT(0, g_stub_state.videorec_calls);
        free(resp);
    }
}

void test_videorec_status_payload(void) {
    dispatch_stub_reset();
    st_DBGAPI_VIDEOREC_PARAM *f = &g_stub_state.videorec_fake;
    f->out_supported = 1;
    f->out_state = 2; /* PAUSED */
    f->out_frames = 125;
    f->out_fps = 50;
    f->out_segment = 3;
    f->out_segment_open = 0;
    f->out_bytes = 123456;
    f->out_parts = 1;
    f->out_retake_mode = 1; /* DISCARD */
    g_strlcpy(f->out_path, "out/x.avi", sizeof(f->out_path));
    f->out_event_seq = 7;
    f->out_event_kind = 4; /* RETAKE */
    f->out_event_frame = 100;
    g_strlcpy(f->out_event_text, "Retake: rewound to 00:00:02", sizeof(f->out_event_text));
    f->out_timebase = 1;           /* REALTIME */
    f->out_timebase_effective = 1; /* REALTIME */
    f->out_rt_activity = 2;        /* FROZEN */
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1516,\"cmd\":\"videorec_status\"}", &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_VIDEOREC_OP_STATUS, g_stub_state.videorec_last_op);

    JsonParser *parser = NULL;
    JsonObject *root = _parse_response_object(resp, &parser);
    JsonObject *d = json_object_get_object_member(root, "data");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "supported"));
    TEST_ASSERT_EQUAL_STRING("paused", json_object_get_string_member(d, "state"));
    TEST_ASSERT_EQUAL_INT(125, json_object_get_int_member(d, "frames"));
    TEST_ASSERT_EQUAL_INT(50, json_object_get_int_member(d, "fps"));
    TEST_ASSERT_TRUE(json_object_get_double_member(d, "duration_s") > 2.49
                     && json_object_get_double_member(d, "duration_s") < 2.51);
    TEST_ASSERT_EQUAL_INT(3, json_object_get_int_member(d, "segment"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "segment_open"));
    TEST_ASSERT_EQUAL_INT(123456, json_object_get_int_member(d, "bytes"));
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(d, "parts"));
    TEST_ASSERT_EQUAL_STRING("discard", json_object_get_string_member(d, "retake_mode"));
    TEST_ASSERT_EQUAL_STRING("out/x.avi", json_object_get_string_member(d, "path"));
    TEST_ASSERT_EQUAL_STRING("out/x.cuts.json", json_object_get_string_member(d, "sidecar"));
    TEST_ASSERT_EQUAL_STRING("realtime", json_object_get_string_member(d, "timebase"));
    TEST_ASSERT_EQUAL_STRING("realtime", json_object_get_string_member(d, "timebase_effective"));
    TEST_ASSERT_EQUAL_STRING("frozen", json_object_get_string_member(d, "rt_activity"));
    TEST_ASSERT_TRUE(json_object_get_null_member(d, "last_error"));
    JsonObject *ev = json_object_get_object_member(d, "last_event");
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_INT(7, json_object_get_int_member(ev, "seq"));
    TEST_ASSERT_EQUAL_STRING("retake", json_object_get_string_member(ev, "kind"));
    TEST_ASSERT_EQUAL_INT(100, json_object_get_int_member(ev, "frame"));
    TEST_ASSERT_EQUAL_STRING("Retake: rewound to 00:00:02",
                             json_object_get_string_member(ev, "text"));
    g_object_unref(parser);
    free(resp);
}

/** @brief Bez jediné události je last_event null. */
void test_videorec_status_no_event(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _vr_dispatch(
        "{\"type\":\"request\",\"req_id\":1517,\"cmd\":\"videorec_status\"}", &resp));
    TEST_ASSERT_TRUE(strstr(resp, "\"last_event\":null") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"state\":\"idle\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"retake_mode\":\"off\"") != NULL);
    free(resp);
}


/* ====================================================================== */
/* 0017 FÁZE 1 - Tracking lifecycle Tools tests                            */
/* ====================================================================== */

void test_trace_start_stop_reset_lifecycle(void) {
    /* Sekvence start -> stop -> reset na kanálu cputrack. Každé volání
     * inkrementuje příslušný counter ve stubu a předá správný channel. */
    dispatch_stub_reset();

    st_JSONL_MESSAGE *req1 = _make_request(
        "{\"type\":\"request\",\"id\":1201,\"cmd\":\"trace_start\","
        "\"data\":{\"channel\":\"cputrack\"}}");
    char *resp1 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req1, &resp1));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.trace_start_calls);
    TEST_ASSERT_EQUAL_INT(DBGAPI_TRACE_CHANNEL_CPUTRACK,
                           g_stub_state.trace_last_channel);
    TEST_ASSERT_TRUE(strstr(resp1, "\"started\":true") != NULL);
    free(resp1);
    jsonl_msg_free(req1);

    st_JSONL_MESSAGE *req2 = _make_request(
        "{\"type\":\"request\",\"id\":1202,\"cmd\":\"trace_stop\","
        "\"data\":{\"channel\":\"cputrack\"}}");
    char *resp2 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req2, &resp2));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.trace_stop_calls);
    TEST_ASSERT_TRUE(strstr(resp2, "\"stopped\":true") != NULL);
    free(resp2);
    jsonl_msg_free(req2);

    st_JSONL_MESSAGE *req3 = _make_request(
        "{\"type\":\"request\",\"id\":1203,\"cmd\":\"trace_reset\","
        "\"data\":{\"channel\":\"cputrack\"}}");
    char *resp3 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req3, &resp3));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.trace_reset_calls);
    TEST_ASSERT_TRUE(strstr(resp3, "\"reset\":true") != NULL);
    free(resp3);
    jsonl_msg_free(req3);
}


void test_trace_save_with_path(void) {
    /* trace_save s path na kanálu iorqlog - stub zachytí channel + path. */
    dispatch_stub_reset();
    g_stub_state.trace_fake_result = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1204,\"cmd\":\"trace_save\","
        "\"data\":{\"channel\":\"iorqlog\",\"path\":\"/tmp/seg1.bin\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_TRACE_SAVE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.trace_save_calls);
    TEST_ASSERT_EQUAL_INT(DBGAPI_TRACE_CHANNEL_IORQLOG,
                           g_stub_state.trace_last_channel);
    TEST_ASSERT_NOT_NULL(g_stub_state.trace_save_last_path);
    TEST_ASSERT_EQUAL_STRING("/tmp/seg1.bin",
                              g_stub_state.trace_save_last_path);
    TEST_ASSERT_TRUE(strstr(resp, "\"saved\":true") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_trace_save_without_path(void) {
    /* trace_save bez path => OK, path v paramu NULL, odpověď path:null. */
    dispatch_stub_reset();
    g_stub_state.trace_fake_result = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1205,\"cmd\":\"trace_save\","
        "\"data\":{\"channel\":\"hwlog\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.trace_save_calls);
    TEST_ASSERT_EQUAL_INT(DBGAPI_TRACE_CHANNEL_HWLOG,
                           g_stub_state.trace_last_channel);
    TEST_ASSERT_NULL(g_stub_state.trace_save_last_path);
    TEST_ASSERT_TRUE(strstr(resp, "\"path\":null") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_trace_missing_channel(void) {
    /* trace_start bez channel => INVALID_PARAMS. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1206,\"cmd\":\"trace_start\","
        "\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_trace_unknown_channel(void) {
    /* trace_start s neznámým channel => INVALID_PARAMS. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1207,\"cmd\":\"trace_start\","
        "\"data\":{\"channel\":\"bogus\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* V1.A.7 - Profiler Tools tests                                            */
/* ====================================================================== */

void test_profiler_start_stop_lifecycle(void) {
    /* profiler_start -> active=1, profiler_stop -> active=0. Stub counter
     * inkrementován v obou voláních. */
    dispatch_stub_reset();

    st_JSONL_MESSAGE *req1 = _make_request(
        "{\"type\":\"request\",\"id\":1101,\"cmd\":\"profiler_start\"}");
    char *resp1 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req1, &resp1));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.profiler_set_active_calls);
    TEST_ASSERT_EQUAL_UINT8(1, g_stub_state.profiler_last_active);
    TEST_ASSERT_TRUE(strstr(resp1, "\"active\":true") != NULL);
    free(resp1);
    jsonl_msg_free(req1);

    st_JSONL_MESSAGE *req2 = _make_request(
        "{\"type\":\"request\",\"id\":1102,\"cmd\":\"profiler_stop\"}");
    char *resp2 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req2, &resp2));
    TEST_ASSERT_EQUAL_INT(2, g_stub_state.profiler_set_active_calls);
    TEST_ASSERT_EQUAL_UINT8(0, g_stub_state.profiler_last_active);
    TEST_ASSERT_TRUE(strstr(resp2, "\"active\":false") != NULL);
    free(resp2);
    jsonl_msg_free(req2);
}


void test_profiler_reset_clears_data(void) {
    /* profiler_reset musí vystavit DBGAPI_CMD_PROFILER_RESET (= stub
     * counter inkrementován). Response { "reset": true }. */
    dispatch_stub_reset();

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1103,\"cmd\":\"profiler_reset\"}");
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req, &resp));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.profiler_reset_calls);
    TEST_ASSERT_TRUE(strstr(resp, "\"reset\":true") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_profiler_export_happy_csv(void) {
    /* Export s validním path + format=csv (default). Stub zachytí
     * path + format + naplní fake_entry_count. */
    dispatch_stub_reset();
    g_stub_state.profiler_export_fake_result = 0;
    g_stub_state.profiler_export_fake_entry_count = 42;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1104,\"cmd\":\"profiler_export\","
        "\"data\":{\"path\":\"/tmp/prof.csv\",\"format\":\"csv\"}}");
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req, &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_PROFILER_EXPORT,
                           g_stub_state.last_cmd);
    TEST_ASSERT_NOT_NULL(g_stub_state.profiler_export_last_path);
    TEST_ASSERT_EQUAL_STRING("/tmp/prof.csv",
                              g_stub_state.profiler_export_last_path);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.profiler_export_last_format);
    TEST_ASSERT_TRUE(strstr(resp, "\"format\":\"csv\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"entry_count\":42") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_profiler_export_invalid_format(void) {
    /* format mimo csv/json => INVALID_PARAMS. */
    dispatch_stub_reset();

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1105,\"cmd\":\"profiler_export\","
        "\"data\":{\"path\":\"/tmp/p.txt\",\"format\":\"xml\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_profiler_export_missing_path(void) {
    /* Export bez path => INVALID_PARAMS (parametry musí mít alespoň
     * path string, format je volitelný). */
    dispatch_stub_reset();

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1106,\"cmd\":\"profiler_export\","
        "\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_profiler_get_limit_range(void) {
    /* limit mimo 1..1000 => INVALID_PARAMS. Test 0 a 1001. */
    dispatch_stub_reset();

    st_JSONL_MESSAGE *req1 = _make_request(
        "{\"type\":\"request\",\"id\":1107,\"cmd\":\"profiler_get\","
        "\"data\":{\"limit\":0}}");
    char *resp1 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
                           mcp_dispatch_request(req1, &resp1));
    free(resp1);
    jsonl_msg_free(req1);

    st_JSONL_MESSAGE *req2 = _make_request(
        "{\"type\":\"request\",\"id\":1108,\"cmd\":\"profiler_get\","
        "\"data\":{\"limit\":1001}}");
    char *resp2 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
                           mcp_dispatch_request(req2, &resp2));
    free(resp2);
    jsonl_msg_free(req2);
}


void test_profiler_get_default_returns_stats(void) {
    /* Bez parametrů (= limit default 50). Stub naplní fake stats +
     * 3 entries. Response obsahuje active + total_calls + entry_count
     * + entries pole. */
    dispatch_stub_reset();
    g_stub_state.profiler_get_fake_active = 1;
    g_stub_state.profiler_get_fake_total_cycles_64 = 123456;
    g_stub_state.profiler_get_fake_total_calls = 100;
    g_stub_state.profiler_get_fake_irq_entries = 5;
    g_stub_state.profiler_get_fake_count = 3;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1109,\"cmd\":\"profiler_get\"}");
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req, &resp));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.profiler_get_calls);
    TEST_ASSERT_TRUE(strstr(resp, "\"active\":true") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"entry_count\":3") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"limit\":50") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"total_calls\":100") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"irq_entries\":5") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"entries\":[") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* V1.B.1 - Media Tools tests                                              */
/* ====================================================================== */

void test_media_load_mzf_path_happy(void) {
    /* Path varianta - stub zachytí filepath, vrátí success. */
    dispatch_stub_reset();
    g_stub_state.media_fake_result = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1101,\"cmd\":\"media_load_mzf\","
        "\"data\":{\"path\":\"/tmp/test.mzf\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_MEDIA_LOAD_MZF,
                           g_stub_state.media_last_cmd);
    TEST_ASSERT_EQUAL_STRING("/tmp/test.mzf",
                              g_stub_state.media_last_filepath);
    TEST_ASSERT_TRUE(strstr(resp, "\"ok\":true") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_media_load_mzf_both_path_and_b64_rejected(void) {
    /* Současně path i bytes_b64 = INVALID_PARAMS. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1102,\"cmd\":\"media_load_mzf\","
        "\"data\":{\"path\":\"/tmp/test.mzf\","
        "\"bytes_b64\":\"AAA=\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief media_run_mzf: path varianta jde přes DBGAPI_CMD_MEDIA_RUN_MZF
 *        a odpověď nese "reset":true.
 */
void test_media_run_mzf_path_happy(void) {
    dispatch_stub_reset();
    g_stub_state.media_fake_result = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1105,\"cmd\":\"media_run_mzf\","
        "\"data\":{\"path\":\"/tmp/test.mzf\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_MEDIA_RUN_MZF,
                           g_stub_state.media_last_cmd);
    TEST_ASSERT_EQUAL_STRING("/tmp/test.mzf",
                              g_stub_state.media_last_filepath);
    TEST_ASSERT_TRUE(strstr(resp, "\"ok\":true") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"reset\":true") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief media_run_mzf: chyby parametrů a emulátoru.
 *
 * Bez path i bytes_b64 = INVALID_PARAMS (nic se neodešle). out_result -4
 * (soubor kratší než fsize) se hlásí jako chyba s informací, že reset
 * neproběhl.
 */
void test_media_run_mzf_errors(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1106,\"cmd\":\"media_run_mzf\","
        "\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.media_last_cmd);
    free(resp);
    jsonl_msg_free(req);

    dispatch_stub_reset();
    g_stub_state.media_fake_result = -4;
    req = _make_request(
        "{\"type\":\"request\",\"id\":1107,\"cmd\":\"media_run_mzf\","
        "\"data\":{\"path\":\"/tmp/short.mzf\"}}");
    resp = NULL;
    rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_TRUE(strstr(resp, "not reset") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Pomocník testů vlastních pulzů: odešle request a vrátí výsledek dispatch.
 *
 * @param json Celý request řádek.
 * @param[out] out_resp Odpověď (vlastník je volající, free); smí být NULL.
 * @return Výsledek mcp_dispatch_request().
 */
static en_MCP_DISPATCH_RESULT _cmt_pulses_dispatch(const char *json, char **out_resp) {
    st_JSONL_MESSAGE *req = _make_request(json);
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    jsonl_msg_free(req);
    if (out_resp) {
        *out_resp = resp;
    } else {
        free(resp);
    }
    return rc;
}


/**
 * @brief cmt_set_property "custom_pulses": validace pulses_us před dbgapi.
 *
 * Chybějící pole, jiný počet prvků než 4 nebo nečíselný prvek se odmítne
 * jako INVALID_PARAMS bez volání emulátoru. Platné pole se předá a vrátí
 * v odpovědi; selhání emulátoru (neplatné délky) = EMU_ERROR.
 */
void test_cmt_set_property_custom_pulses(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1201,\"cmd\":\"cmt_set_property\","
        "\"data\":{\"property\":\"custom_pulses\"}}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1202,\"cmd\":\"cmt_set_property\","
        "\"data\":{\"property\":\"custom_pulses\",\"pulses_us\":[156,164,80]}}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1203,\"cmd\":\"cmt_set_property\","
        "\"data\":{\"property\":\"custom_pulses\",\"pulses_us\":[\"156\",164,80,92]}}", NULL));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1204,\"cmd\":\"cmt_set_property\","
        "\"data\":{\"property\":\"custom_pulses\",\"pulses_us\":[156,164,80.5,92]}}", &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_CMT_SET_PROPERTY, g_stub_state.last_cmd);
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"pulses_us\""));
    TEST_ASSERT_NOT_NULL(strstr(resp, "80.5"));
    free(resp);

    dispatch_stub_reset();
    g_stub_state.fail_next = true;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1205,\"cmd\":\"cmt_set_property\","
        "\"data\":{\"property\":\"custom_pulses\",\"pulses_us\":[0,164,80,92]}}", NULL));

    /* ostatní vlastnosti dál vyžadují value */
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1206,\"cmd\":\"cmt_set_property\","
        "\"data\":{\"property\":\"speed\"}}", NULL));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
}


/**
 * @brief cmt_tape_block_speed s pulses_us: blok dostane speed 10 (CUSTOM).
 */
void test_cmt_tape_block_speed_pulses(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1211,\"cmd\":\"cmt_tape_block_speed\","
        "\"data\":{\"block_id\":1}}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1212,\"cmd\":\"cmt_tape_block_speed\","
        "\"data\":{\"block_id\":1,\"pulses_us\":[156,164]}}", NULL));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);

    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1213,\"cmd\":\"cmt_tape_block_speed\","
        "\"data\":{\"block_id\":1,\"pulses_us\":[156,164,80,92]}}", &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED, g_stub_state.last_cmd);
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"speed\":10"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"pulses_us\""));
    free(resp);

    /* poměr beze změny */
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _cmt_pulses_dispatch(
        "{\"type\":\"request\",\"id\":1214,\"cmd\":\"cmt_tape_block_speed\","
        "\"data\":{\"block_id\":1,\"speed\":4}}", &resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"speed\":4"));
    TEST_ASSERT_NULL(strstr(resp, "pulses_us"));
    free(resp);
}


void test_media_load_binary_addr_validation(void) {
    /* addr=-1 nebo addr=65536 -> INVALID_PARAMS. addr=0x4000 OK. */
    dispatch_stub_reset();

    /* Neplatný addr (-1) */
    st_JSONL_MESSAGE *req1 = _make_request(
        "{\"type\":\"request\",\"id\":1103,\"cmd\":\"media_load_binary\","
        "\"data\":{\"path\":\"/tmp/raw.bin\",\"addr\":-1}}");
    char *resp1 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
                           mcp_dispatch_request(req1, &resp1));
    free(resp1);
    jsonl_msg_free(req1);

    /* Validní addr 0x4000 - stub neselže, vrátí ok */
    g_stub_state.media_fake_result   = 0;
    g_stub_state.media_fake_out_size = 256;
    st_JSONL_MESSAGE *req2 = _make_request(
        "{\"type\":\"request\",\"id\":1104,\"cmd\":\"media_load_binary\","
        "\"data\":{\"path\":\"/tmp/raw.bin\",\"addr\":16384}}");
    char *resp2 = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req2, &resp2);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(0x4000, g_stub_state.media_last_load_addr);
    TEST_ASSERT_TRUE(strstr(resp2, "\"size\":256") != NULL);
    free(resp2);
    jsonl_msg_free(req2);
}


void test_media_insert_each_slot(void) {
    /* Pro každý z 11 slotů insert s path - stub vrátí success a
     * zachytí slot enum. */
    dispatch_stub_reset();
    static const struct { const char *name; int enum_val; } slots[] = {
        { "cmt",      DBGAPI_MEDIA_SLOT_CMT      },
        { "fdc0_fd0", DBGAPI_MEDIA_SLOT_FDC0_FD0 },
        { "fdc0_fd1", DBGAPI_MEDIA_SLOT_FDC0_FD1 },
        { "fdc0_fd2", DBGAPI_MEDIA_SLOT_FDC0_FD2 },
        { "fdc0_fd3", DBGAPI_MEDIA_SLOT_FDC0_FD3 },
        { "fdc1_fd0", DBGAPI_MEDIA_SLOT_FDC1_FD0 },
        { "fdc1_fd1", DBGAPI_MEDIA_SLOT_FDC1_FD1 },
        { "fdc1_fd2", DBGAPI_MEDIA_SLOT_FDC1_FD2 },
        { "fdc1_fd3", DBGAPI_MEDIA_SLOT_FDC1_FD3 },
        { "qd",       DBGAPI_MEDIA_SLOT_QD       },
        { "ide8",     DBGAPI_MEDIA_SLOT_IDE8     },
    };
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        g_stub_state.media_fake_result = 0;
        char json_req[256];
        snprintf(json_req, sizeof(json_req),
                  "{\"type\":\"request\",\"id\":%d,\"cmd\":\"media_insert\","
                  "\"data\":{\"slot\":\"%s\",\"path\":\"/tmp/%s.img\"}}",
                  1110 + (int)i, slots[i].name, slots[i].name);
        st_JSONL_MESSAGE *req = _make_request(json_req);
        char *resp = NULL;
        en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
        TEST_ASSERT_EQUAL_INT(slots[i].enum_val,
                               g_stub_state.media_last_slot);
        char expect_slot[32];
        snprintf(expect_slot, sizeof(expect_slot), "\"slot\":\"%s\"",
                  slots[i].name);
        TEST_ASSERT_TRUE(strstr(resp, expect_slot) != NULL);
        free(resp);
        jsonl_msg_free(req);
    }
}


void test_media_insert_invalid_slot(void) {
    /* Neznámý slot -> INVALID_PARAMS, žádný dbgapi cmd. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1120,\"cmd\":\"media_insert\","
        "\"data\":{\"slot\":\"floppy0\","
        "\"path\":\"/tmp/x.dsk\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    /* Stub by neměl být zavolán */
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.media_last_cmd);
    free(resp);
    jsonl_msg_free(req);
}


void test_media_eject_lifecycle(void) {
    /* Insert + eject sekvence. Po eject je media_last_cmd=EJECT,
     * media_last_filepath je NULL (eject nepředává path). */
    dispatch_stub_reset();
    g_stub_state.media_fake_result = 0;
    /* Insert */
    st_JSONL_MESSAGE *req1 = _make_request(
        "{\"type\":\"request\",\"id\":1121,\"cmd\":\"media_insert\","
        "\"data\":{\"slot\":\"fdc0_fd0\",\"path\":\"/tmp/a.dsk\"}}");
    char *resp1 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req1, &resp1));
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_MEDIA_INSERT,
                           g_stub_state.media_last_cmd);
    free(resp1);
    jsonl_msg_free(req1);

    /* Eject */
    st_JSONL_MESSAGE *req2 = _make_request(
        "{\"type\":\"request\",\"id\":1122,\"cmd\":\"media_eject\","
        "\"data\":{\"slot\":\"fdc0_fd0\"}}");
    char *resp2 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                           mcp_dispatch_request(req2, &resp2));
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_MEDIA_EJECT,
                           g_stub_state.media_last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_MEDIA_SLOT_FDC0_FD0,
                           g_stub_state.media_last_slot);
    TEST_ASSERT_TRUE(strstr(resp2, "\"slot\":\"fdc0_fd0\"") != NULL);
    free(resp2);
    jsonl_msg_free(req2);
}


void test_media_state_returns_all_slots(void) {
    /* Stub naplní 11 slot info; response musí obsahovat slot strings
     * a count=11. */
    dispatch_stub_reset();
    g_stub_state.media_state_fake_count = 11;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1130,\"cmd\":\"media_state\"}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.media_state_calls);
    TEST_ASSERT_TRUE(strstr(resp, "\"count\":11") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"slot\":\"cmt\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"slot\":\"fdc0_fd0\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"slot\":\"fdc1_fd0\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"slot\":\"ide8\"") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* V1.B.2 - Platform + Config Tools tests                                  */
/* ====================================================================== */

void test_settings_set_live_key_happy(void) {
    /* Live-settable klíč (AUDIO/volume_8253) projde validací; stub
     * vrátí fake_value "50" jako předchozí hodnotu. */
    dispatch_stub_reset();
    g_stub_state.settings_fake_value  = g_strdup("50");
    g_stub_state.settings_fake_type   = 1;  /* UNSIGNED */
    g_stub_state.settings_fake_result = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1200,\"cmd\":\"settings_set\","
        "\"data\":{\"key\":\"AUDIO/volume_8253\",\"value\":\"75\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_STRING("AUDIO", g_stub_state.settings_last_module);
    TEST_ASSERT_EQUAL_STRING("volume_8253",
                              g_stub_state.settings_last_element);
    TEST_ASSERT_EQUAL_STRING("75",
                              g_stub_state.settings_last_new_value);
    TEST_ASSERT_TRUE(strstr(resp, "\"previous_value\":\"50\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"new_value\":\"75\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"type\":\"unsigned\"") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_settings_set_boot_only_rejected(void) {
    /* Klíč MEMEXT/active není v live whitelistu; očekáváme
     * INVALID_PARAMS bez volání dbgapi. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1201,\"cmd\":\"settings_set\","
        "\"data\":{\"key\":\"MEMEXT/active\",\"value\":\"true\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.settings_last_cmd);
    free(resp);
    jsonl_msg_free(req);
}


void test_settings_set_qdisk_keys_rejected(void) {
    /* QDISK/filename a QDISK/write_protected v whitelistu byly, ale
     * prvek INI takového jména neexistuje (N86); QD se připojuje přes
     * media_insert. Očekáváme INVALID_PARAMS bez volání dbgapi. */
    static const char *const keys[] = { "QDISK/filename", "QDISK/write_protected" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        dispatch_stub_reset();
        char *json = g_strdup_printf(
            "{\"type\":\"request\",\"id\":1203,\"cmd\":\"settings_set\","
            "\"data\":{\"key\":\"%s\",\"value\":\"1\"}}", keys[i]);
        st_JSONL_MESSAGE *req = _make_request(json);
        char *resp = NULL;
        en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
        TEST_ASSERT_EQUAL_INT(0, g_stub_state.settings_last_cmd);
        free(resp);
        jsonl_msg_free(req);
        g_free(json);
    }
}


void test_settings_set_invalid_key_format_rejected(void) {
    /* "audio_volume" bez slash separátoru = INVALID_PARAMS. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1202,\"cmd\":\"settings_set\","
        "\"data\":{\"key\":\"audio_volume\",\"value\":\"1\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_settings_get_returns_value_and_type(void) {
    /* settings_get nemá whitelist; libovolný klíč projde. */
    dispatch_stub_reset();
    g_stub_state.settings_fake_value  = g_strdup("true");
    g_stub_state.settings_fake_type   = 2;  /* BOOL */
    g_stub_state.settings_fake_result = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1210,\"cmd\":\"settings_get\","
        "\"data\":{\"key\":\"DISPLAY/locked_window_aspect_ratio\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SETTINGS_GET,
                           g_stub_state.settings_last_cmd);
    TEST_ASSERT_EQUAL_STRING("DISPLAY",
                              g_stub_state.settings_last_module);
    TEST_ASSERT_EQUAL_STRING("locked_window_aspect_ratio",
                              g_stub_state.settings_last_element);
    TEST_ASSERT_TRUE(strstr(resp, "\"value\":\"true\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"type\":\"bool\"") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_settings_get_live_value(void) {
    /* Prvek s přímou vazbou na proměnnou: odpověď nese "live" vedle
     * "value" (hodnota prvku INI se může lišit od živého stavu). */
    dispatch_stub_reset();
    g_stub_state.settings_fake_value  = g_strdup("UNICARD");
    g_stub_state.settings_fake_live   = g_strdup("STANDARD");
    g_stub_state.settings_fake_type   = 4;  /* KEYWORD */
    g_stub_state.settings_fake_result = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1212,\"cmd\":\"settings_get\","
        "\"data\":{\"key\":\"QDISK/mz1f11_type\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_TRUE(strstr(resp, "\"value\":\"UNICARD\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"live\":\"STANDARD\"") != NULL);
    free(resp);
    jsonl_msg_free(req);

    /* Bez vazby pole "live" chybí. */
    dispatch_stub_reset();
    g_stub_state.settings_fake_value = g_strdup("abc");
    g_stub_state.settings_fake_type  = 3;  /* TEXT */
    req = _make_request(
        "{\"type\":\"request\",\"id\":1213,\"cmd\":\"settings_get\","
        "\"data\":{\"key\":\"MEMEXT/flash_filepath\"}}");
    resp = NULL;
    rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NULL(strstr(resp, "\"live\""));
    free(resp);
    jsonl_msg_free(req);
}


void test_settings_set_custom_fps_range(void) {
    /* DISPLAY/custom_fps mimo 50..1000 = INVALID_PARAMS bez dbgapi,
     * hranice projdou. */
    static const struct { const char *v; int ok; } cases[] = {
        { "49", 0 }, { "50", 1 }, { "1000", 1 }, { "1001", 0 }, { "x", 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dispatch_stub_reset();
        g_stub_state.settings_fake_value = g_strdup("100");
        g_stub_state.settings_fake_type  = 1;  /* UNSIGNED */
        char *json = g_strdup_printf(
            "{\"type\":\"request\",\"id\":1204,\"cmd\":\"settings_set\","
            "\"data\":{\"key\":\"DISPLAY/custom_fps\",\"value\":\"%s\"}}",
            cases[i].v);
        st_JSONL_MESSAGE *req = _make_request(json);
        char *resp = NULL;
        en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
        if (cases[i].ok) {
            TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
            TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_SETTINGS_SET,
                                   g_stub_state.settings_last_cmd);
        } else {
            TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
            TEST_ASSERT_EQUAL_INT(0, g_stub_state.settings_last_cmd);
        }
        free(resp);
        jsonl_msg_free(req);
        g_free(json);
    }
}


void test_settings_get_module_not_found(void) {
    /* Stub vrátí result=-2 (= module not found); dispatch vrátí
     * EMU_ERROR. */
    dispatch_stub_reset();
    g_stub_state.settings_fake_result = -2;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1211,\"cmd\":\"settings_get\","
        "\"data\":{\"key\":\"NONEXISTENT/foo\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_TRUE(strstr(resp, "Module not found") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_platform_set_invalid_kind(void) {
    /* "amiga" není v whitelistu mz700/mz800/mz1500. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1220,\"cmd\":\"platform_set\","
        "\"data\":{\"kind\":\"amiga\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


void test_platform_set_requires_restart(void) {
    /* Stub: active=2 (mz800), result=-10. Volání s kind=mz1500 ->
     * ok=false + active_kind=mz800 + error msg. Dispatch vrací
     * MCP_DISPATCH_OK protože payload je validní (jen success=true s
     * ok=false uvnitř - viz handler). */
    dispatch_stub_reset();
    g_stub_state.platform_fake_active_kind = 2;
    g_stub_state.platform_fake_result      = -10;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1221,\"cmd\":\"platform_set\","
        "\"data\":{\"kind\":\"mz1500\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(3, g_stub_state.platform_last_target_kind);
    TEST_ASSERT_TRUE(strstr(resp, "\"ok\":false") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "\"active_kind\":\"mz800\"") != NULL);
    TEST_ASSERT_TRUE(strstr(resp, "Runtime platform switch") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_periph_attach_each_kind(void) {
    /* Pro každý ze 5 kind řetězců provedeme attach, stub vrátí
     * success, changed a type "stub"; requires_restart = 0 (za běhu). */
    static const struct {
        const char *name;
        int enum_val;
    } kinds[] = {
        { "memext", DBGAPI_PERIPH_KIND_MEMEXT },
        { "fdc",    DBGAPI_PERIPH_KIND_FDC    },
        { "qd",     DBGAPI_PERIPH_KIND_QD     },
        { "ide8",   DBGAPI_PERIPH_KIND_IDE8   },
        { "gal5",   DBGAPI_PERIPH_KIND_GAL5   },
    };
    for (size_t i = 0; i < sizeof(kinds)/sizeof(kinds[0]); i++) {
        dispatch_stub_reset();
        g_stub_state.periph_fake_result = 0;
        g_stub_state.periph_fake_requires_restart = 0;
        char json_req[256];
        snprintf(json_req, sizeof(json_req),
                  "{\"type\":\"request\",\"id\":%d,"
                  "\"cmd\":\"periph_attach\","
                  "\"data\":{\"kind\":\"%s\"}}",
                  1300 + (int)i, kinds[i].name);
        st_JSONL_MESSAGE *req = _make_request(json_req);
        char *resp = NULL;
        en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
        TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_PERIPH_ATTACH,
                               g_stub_state.periph_last_cmd);
        TEST_ASSERT_EQUAL_INT(kinds[i].enum_val,
                               g_stub_state.periph_last_kind);
        TEST_ASSERT_TRUE(strstr(resp, "\"requires_restart\":false")
                          != NULL);
        TEST_ASSERT_TRUE(strstr(resp, "\"changed\":true") != NULL);
        TEST_ASSERT_TRUE(strstr(resp, "\"type\":\"stub\"") != NULL);
        char slot_assert[64];
        snprintf(slot_assert, sizeof(slot_assert),
                  "\"kind\":\"%s\"", kinds[i].name);
        TEST_ASSERT_TRUE(strstr(resp, slot_assert) != NULL);
        free(resp);
        jsonl_msg_free(req);
    }
}


void test_periph_attach_memext_options_type(void) {
    /* periph_attach s options.type = "luftner" musí předat
     * option_value do dbgapi handleru. */
    dispatch_stub_reset();
    g_stub_state.periph_fake_result = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1310,\"cmd\":\"periph_attach\","
        "\"data\":{\"kind\":\"memext\","
        "\"options\":{\"type\":\"luftner\"}}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_STRING("luftner",
                              g_stub_state.periph_last_option_value);
    free(resp);
    jsonl_msg_free(req);
}


void test_periph_detach_not_in_arch(void) {
    /* Stub vrací -10 = periferie není v této arch sestavě. */
    dispatch_stub_reset();
    g_stub_state.periph_fake_result = -10;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1320,\"cmd\":\"periph_detach\","
        "\"data\":{\"kind\":\"gal5\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_TRUE(strstr(resp, "not available in this build")
                      != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_periph_detach_passes_options_type(void) {
    /* periph_detach předá options.type (jednotka fdc / ide8). */
    dispatch_stub_reset();
    g_stub_state.periph_fake_result = 0;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1321,\"cmd\":\"periph_detach\","
        "\"data\":{\"kind\":\"fdc\",\"options\":{\"type\":\"fdc1\"}}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_PERIPH_DETACH,
                           g_stub_state.periph_last_cmd);
    TEST_ASSERT_EQUAL_STRING("fdc1", g_stub_state.periph_last_option_value);
    free(resp);
    jsonl_msg_free(req);
}


void test_periph_attach_invalid_option_type(void) {
    /* dbgapi -4 = neplatná varianta -> INVALID_PARAMS se seznamem. */
    dispatch_stub_reset();
    g_stub_state.periph_fake_result = -4;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1322,\"cmd\":\"periph_attach\","
        "\"data\":{\"kind\":\"memext\",\"options\":{\"type\":\"luftner4k\"}}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_TRUE(strstr(resp, "Invalid options.type") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_periph_attach_device_refused(void) {
    /* dbgapi -5 = zařízení změnu nepřijalo. */
    dispatch_stub_reset();
    g_stub_state.periph_fake_result = -5;
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1323,\"cmd\":\"periph_attach\","
        "\"data\":{\"kind\":\"ide8\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_TRUE(strstr(resp, "did not accept") != NULL);
    free(resp);
    jsonl_msg_free(req);
}


void test_periph_attach_invalid_kind(void) {
    /* "floppy_5_25" není ve whitelistu memext/fdc/qd/ide8/gal5. */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":1330,\"cmd\":\"periph_attach\","
        "\"data\":{\"kind\":\"floppy_5_25\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


/* ====================================================================== */
/* V1.C.1 - HID Tools tests                                                */
/* ====================================================================== */

/**
 * @brief Ověří kompletní press-hold-release flow pro input_send_key.
 *
 * Klávesa "A" by se měla rozpoznat (col 4 / bit 7 / shift=false v naší
 * test ASCII tabulce). Handler pošle jednu sekvenci vstupu
 * (DBGAPI_CMD_HID_SCRIPT_START), kterou stub vykoná synchronně: jeden
 * press, jeden release. Odpověď nese emu_frames = frames a complete.
 */
void test_input_send_key_lifecycle(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2001,\"cmd\":\"input_send_key\","
        "\"data\":{\"key\":\"A\",\"frames\":3}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.hid_press_calls);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.hid_release_calls);
    TEST_ASSERT_EQUAL_INT(4, g_stub_state.hid_last_col);
    TEST_ASSERT_EQUAL_INT(7, g_stub_state.hid_last_bit);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_HID_SCRIPT_START, g_stub_state.last_cmd);
    JsonParser *p = NULL;
    JsonObject *o = _parse_response_object(resp, &p);
    JsonObject *d = json_object_get_object_member(o, "data");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(3, (int)json_object_get_int_member(d, "frames"));
    TEST_ASSERT_EQUAL_INT(3, (int)json_object_get_int_member(d, "emu_frames"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "complete"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "interrupted"));
    g_object_unref(p);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Ověří, že send_key i send_keys signalizují landing caveat.
 *
 * Fix 0016: response NESMÍ hlásit holý success bez upozornění, že guest
 * landing není garantovaný. Oba handlery proto vrací v data objektu
 * `landing_verified: false`. Test ověří přítomnost a hodnotu flagu pro
 * send_key (klávesa "A") i send_keys (text "RUN").
 */
void test_input_send_key_landing_caveat(void) {
    dispatch_stub_reset();

    /* send_key */
    st_JSONL_MESSAGE *req1 = _make_request(
        "{\"type\":\"request\",\"id\":2010,\"cmd\":\"input_send_key\","
        "\"data\":{\"key\":\"A\",\"frames\":1}}");
    char *resp1 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                          mcp_dispatch_request(req1, &resp1));
    TEST_ASSERT_NOT_NULL(resp1);
    JsonParser *p1 = NULL;
    JsonObject *o1 = _parse_response_object(resp1, &p1);
    JsonObject *d1 = json_object_get_object_member(o1, "data");
    TEST_ASSERT_NOT_NULL(d1);
    TEST_ASSERT_TRUE(json_object_has_member(d1, "landing_verified"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d1, "landing_verified"));
    g_object_unref(p1);
    free(resp1);
    jsonl_msg_free(req1);

    /* send_keys */
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req2 = _make_request(
        "{\"type\":\"request\",\"id\":2011,\"cmd\":\"input_send_keys\","
        "\"data\":{\"text\":\"RUN\",\"encoding\":\"ascii\","
        "\"frame_per_key\":1}}");
    char *resp2 = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
                          mcp_dispatch_request(req2, &resp2));
    TEST_ASSERT_NOT_NULL(resp2);
    JsonParser *p2 = NULL;
    JsonObject *o2 = _parse_response_object(resp2, &p2);
    JsonObject *d2 = json_object_get_object_member(o2, "data");
    TEST_ASSERT_NOT_NULL(d2);
    TEST_ASSERT_TRUE(json_object_has_member(d2, "landing_verified"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d2, "landing_verified"));
    g_object_unref(p2);
    free(resp2);
    jsonl_msg_free(req2);
}


/**
 * @brief Klávesa "XYZ_NOT_A_KEY" se nesmí resolvovat - vrátí 422.
 */
void test_input_send_key_unknown_rejected(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2002,\"cmd\":\"input_send_key\","
        "\"data\":{\"key\":\"XYZ_NOT_A_KEY\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.hid_press_calls);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief input_send_keys s encoding=ascii rozparsuje text "RUN" + RETURN.
 *
 * "RUN\r" = 4 znaky, všechny mají Sharp ASCII mapping v stub tabulce.
 * Stub by měl zaznamenat 4 press + 4 release volání.
 */
void test_input_send_keys_ascii_mapping(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2003,\"cmd\":\"input_send_keys\","
        "\"data\":{\"text\":\"RUN\\r\",\"encoding\":\"ascii\","
        "\"frame_per_key\":1}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    /* R + U + N + CR = 4 znaky */
    TEST_ASSERT_EQUAL_INT(4, g_stub_state.hid_press_calls);
    TEST_ASSERT_EQUAL_INT(4, g_stub_state.hid_release_calls);
    /* 4 x 1 snímek držení + 3 mezery po 1 snímku mezi klávesami. */
    JsonParser *p = NULL;
    JsonObject *o = _parse_response_object(resp, &p);
    JsonObject *d = json_object_get_object_member(o, "data");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(7, (int)json_object_get_int_member(d, "total_frames"));
    TEST_ASSERT_EQUAL_INT(7, (int)json_object_get_int_member(d, "emu_frames"));
    g_object_unref(p);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief input_send_keys s encoding=key_names rozparsuje JSON array.
 *
 * Array ["RETURN", "SHIFT", "F1"] = 3 klávesy s rezolvovatelnými jmény.
 */
void test_input_send_keys_key_names_parsing(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2004,\"cmd\":\"input_send_keys\","
        "\"data\":{\"text\":\"[\\\"RETURN\\\",\\\"SHIFT\\\",\\\"F1\\\"]\","
        "\"encoding\":\"key_names\",\"frame_per_key\":1}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(3, g_stub_state.hid_press_calls);
    TEST_ASSERT_EQUAL_INT(3, g_stub_state.hid_release_calls);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Nepodporovaný encoding (= "unicode") vrátí 422.
 */
void test_input_send_keys_bad_encoding(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2005,\"cmd\":\"input_send_keys\","
        "\"data\":{\"text\":\"x\",\"encoding\":\"unicode\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief input_press_key + input_release_key drží klávesu trvale.
 *
 * Po press_key musí být press_calls=1, release_calls=0.
 * Po release_key se stejnou klávesou: release_calls=1.
 */
void test_input_press_release_hold_pattern(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req1 = _make_request(
        "{\"type\":\"request\",\"id\":2010,\"cmd\":\"input_press_key\","
        "\"data\":{\"key\":\"SHIFT\"}}");
    char *resp1 = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req1, &resp1);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.hid_press_calls);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.hid_release_calls);
    free(resp1);
    jsonl_msg_free(req1);

    st_JSONL_MESSAGE *req2 = _make_request(
        "{\"type\":\"request\",\"id\":2011,\"cmd\":\"input_release_key\","
        "\"data\":{\"key\":\"SHIFT\"}}");
    char *resp2 = NULL;
    rc = mcp_dispatch_request(req2, &resp2);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.hid_release_calls);
    free(resp2);
    jsonl_msg_free(req2);
}


/**
 * @brief Pomocná: pošle input_press_key s daným jménem a vrátí odpověď.
 *
 * @param[in]  key     jméno klávesy
 * @param[out] out_rc  výsledek dispatche
 * @return vlastněný JSONL řádek odpovědi (caller free)
 */
static char *_press_key_resp(const char *key, en_MCP_DISPATCH_RESULT *out_rc) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"request\",\"id\":2030,\"cmd\":\"input_press_key\","
        "\"data\":{\"key\":\"%s\"}}", key);
    st_JSONL_MESSAGE *req = _make_request(buf);
    char *resp = NULL;
    *out_rc = mcp_dispatch_request(req, &resp);
    jsonl_msg_free(req);
    return resp;
}


/**
 * @brief press_key přijímá názvy ze send_keys / mz800_keyboard (CURSOR_*,
 *        INST, UP_ARROW, DOWN_ARROW) a mapuje je na stejnou polohu
 *        jako staré názvy (ARROW_*, INSERT).
 */
void test_input_press_key_aliases(void) {
    static const struct { const char *name; int col; int bit; } k[] = {
        { "CURSOR_RIGHT", 7, 3 }, { "CURSOR_LEFT", 7, 2 },
        { "CURSOR_UP", 7, 5 },    { "CURSOR_DOWN", 7, 4 },
        { "INST", 7, 7 },         { "UP_ARROW", 6, 6 },
        { "DOWN_ARROW", 0, 5 },   { "cursor_right", 7, 3 },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        dispatch_stub_reset();
        en_MCP_DISPATCH_RESULT rc;
        char *resp = _press_key_resp(k[i].name, &rc);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
        TEST_ASSERT_EQUAL_INT(1, g_stub_state.hid_press_calls);
        TEST_ASSERT_EQUAL_INT(k[i].col, g_stub_state.hid_last_col);
        TEST_ASSERT_EQUAL_INT(k[i].bit, g_stub_state.hid_last_bit);
        free(resp);
    }
}


/**
 * @brief Staré názvy (RIGHT, ARROW_RIGHT, INSERT) dál fungují a mají
 *        stejnou polohu jako jejich aliasy.
 */
void test_input_press_key_old_names_still_work(void) {
    static const char *const names[] = { "RIGHT", "ARROW_RIGHT" };
    for (size_t i = 0; i < 2; i++) {
        dispatch_stub_reset();
        en_MCP_DISPATCH_RESULT rc;
        char *resp = _press_key_resp(names[i], &rc);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
        TEST_ASSERT_EQUAL_INT(7, g_stub_state.hid_last_col);
        TEST_ASSERT_EQUAL_INT(3, g_stub_state.hid_last_bit);
        free(resp);
    }
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    char *resp = _press_key_resp("INSERT", &rc);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(7, g_stub_state.hid_last_col);
    TEST_ASSERT_EQUAL_INT(7, g_stub_state.hid_last_bit);
    free(resp);
}


/**
 * @brief release_key přijímá aliasy ze send_keys.
 */
void test_input_release_key_alias(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2031,\"cmd\":\"input_release_key\","
        "\"data\":{\"key\":\"CURSOR_DOWN\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.hid_release_calls);
    TEST_ASSERT_EQUAL_INT(7, g_stub_state.hid_last_col);
    TEST_ASSERT_EQUAL_INT(4, g_stub_state.hid_last_bit);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Neznámá klávesa: chyba "Unknown key" s nejbližšími názvy,
 *        nic se nestiskne.
 */
void test_input_press_key_unknown_suggests(void) {
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    char *resp = _press_key_resp("CURSOR_RIGT", &rc);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.hid_press_calls);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_NOT_NULL(strstr(resp, "Unknown key"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "Closest valid names"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "CURSOR_RIGHT"));
    free(resp);

    /* Prefixový překlep: "CURSOR" navrhne CURSOR_* jména. */
    resp = _press_key_resp("CURSOR", &rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "CURSOR_"));
    free(resp);

    /* Nesmysl bez podobného jména: odkaz na dokumentaci. */
    resp = _press_key_resp("QQQQQQQQQQQQQQQQ", &rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "Unknown key"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "mz800_keyboard"));
    free(resp);
}


/**
 * @brief send_keys key_names: alias CURSOR_RIGHT se odešle; neznámé jméno
 *        vrátí chybu s nápovědou a NEodešle nic (dřív se tiše přeskočilo).
 */
void test_input_send_keys_key_names_alias_and_unknown(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2032,\"cmd\":\"input_send_keys\","
        "\"data\":{\"text\":\"[\\\"CURSOR_RIGHT\\\",\\\"RIGHT\\\"]\","
        "\"encoding\":\"key_names\",\"frame_per_key\":1}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(2, g_stub_state.hid_press_calls);
    free(resp);
    jsonl_msg_free(req);

    dispatch_stub_reset();
    req = _make_request(
        "{\"type\":\"request\",\"id\":2033,\"cmd\":\"input_send_keys\","
        "\"data\":{\"text\":\"[\\\"SHIFT\\\",\\\"CURSOR_RIGT\\\"]\","
        "\"encoding\":\"key_names\",\"frame_per_key\":1}}");
    resp = NULL;
    rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.hid_press_calls);
    TEST_ASSERT_NOT_NULL(strstr(resp, "Unknown key"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "CURSOR_RIGHT"));
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Release bez key argumentu = release-all (= vyresetuje vkbd
 *        matrix celou).
 */
void test_input_release_all_when_no_key(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2012,\"cmd\":\"input_release_key\","
        "\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.hid_release_all_calls);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Joystick bit mask se předá set + clear sekvenci.
 *
 * Mask 0x11 = UP + FIRE1. Stub by měl zachytit port=0, mask=0x11 na
 * set call a port=0 na clear call.
 */
void test_input_send_joystick_bit_mask(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2020,\"cmd\":\"input_send_joystick\","
        "\"data\":{\"port\":0,\"state\":17,\"frames\":1}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.hid_joy_set_calls);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.hid_joy_clear_calls);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.hid_joy_last_port);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Joystick port > 1 = 422 (nevykonal žádný submit).
 */
void test_input_send_joystick_invalid_port(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2021,\"cmd\":\"input_send_joystick\","
        "\"data\":{\"port\":5,\"state\":0}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.hid_joy_set_calls);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief send_keys_with_delays iteruje přes pole eventů.
 *
 * Pole [{key=A, hold=2, gap=1}, {key=RETURN, hold=2, gap=0}] = 2
 * press/release páry.
 */
void test_input_send_keys_with_delays_event_list(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2030,"
        "\"cmd\":\"input_send_keys_with_delays\","
        "\"data\":{\"events\":["
            "{\"key\":\"A\",\"hold_frames\":2,\"gap_frames\":1},"
            "{\"key\":\"RETURN\",\"hold_frames\":2,\"gap_frames\":0}]}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(2, g_stub_state.hid_press_calls);
    TEST_ASSERT_EQUAL_INT(2, g_stub_state.hid_release_calls);
    /* Jedna sekvence pro celé pole, ne press/release po jednom. */
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_HID_SCRIPT_START, g_stub_state.last_cmd);
    JsonParser *p = NULL;
    JsonObject *o = _parse_response_object(resp, &p);
    JsonObject *d = json_object_get_object_member(o, "data");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(2, (int)json_object_get_int_member(d, "events_processed"));
    /* (2 + 1) + (2 + 0) */
    TEST_ASSERT_EQUAL_INT(5, (int)json_object_get_int_member(d, "total_frames"));
    TEST_ASSERT_EQUAL_INT(5, (int)json_object_get_int_member(d, "emu_frames"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "complete"));
    g_object_unref(p);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief send_keys_with_delays: neznámá klávesa kdekoli v poli = 422
 *        s nápovědou a nic se neodešle (ani klávesy před ní).
 */
void test_input_send_keys_with_delays_unknown_key_sends_nothing(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2032,"
        "\"cmd\":\"input_send_keys_with_delays\","
        "\"data\":{\"events\":["
            "{\"key\":\"A\",\"hold_frames\":2},"
            "{\"key\":\"CURSOR_RIGT\",\"hold_frames\":2}]}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.hid_press_calls);
    TEST_ASSERT_NOT_NULL(strstr(resp, "Unknown key"));
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief Prázdné pole eventů = 422.
 */
void test_input_send_keys_with_delays_empty_rejected(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":2031,"
        "\"cmd\":\"input_send_keys_with_delays\","
        "\"data\":{\"events\":[]}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    jsonl_msg_free(req);
}


/* ======================================================================== */
/* V1.D.1 - Core + CPU extras Resource backing handlers (9 testů)            */
/* ======================================================================== */

/**
 * @brief Pomocné: dispatch ze SOAPed cmd a vrátí parsed data JsonNode.
 *        Caller uvolní výsledek přes json_node_free + free(resp).
 */
static JsonNode *_v1d1_dispatch_and_get_data(const char *cmd,
                                              char **out_raw) {
    char *line = g_strdup_printf(
        "{\"type\":\"request\",\"id\":9001,\"cmd\":\"%s\"}", cmd);
    st_JSONL_MESSAGE *req = _make_request(line);
    g_free(line);
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(resp);
    jsonl_msg_free(req);
    *out_raw = resp;
    JsonParser *jp = json_parser_new();
    GError *err = NULL;
    json_parser_load_from_data(jp, resp, -1, &err);
    TEST_ASSERT_NULL(err);
    JsonNode *root = json_parser_get_root(jp);
    /* Vytáhneme data field a vrátíme jeho kopii (caller uvolní). */
    JsonObject *obj = json_node_get_object(root);
    JsonNode *data = json_object_get_member(obj, "data");
    JsonNode *copy = data ? json_node_copy(data) : NULL;
    g_object_unref(jp);
    return copy;
}


/**
 * @brief set_cpu_flags {"r": N} zapíše celý bajt R včetně bitu 7 (jako LD R,A):
 *        dispatch posílá DBGAPI_CPU_FLAGS_UM_R_FULL, ne UM_R (7 bitů pro okno CPU).
 */
void test_set_cpu_flags_r_writes_full_byte(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"id\":9101,\"cmd\":\"set_cpu_flags\",\"data\":{\"r\":133}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_TRUE(g_stub_state.cpu_flags_last_mask & DBGAPI_CPU_FLAGS_UM_R_FULL);
    TEST_ASSERT_FALSE(g_stub_state.cpu_flags_last_mask & DBGAPI_CPU_FLAGS_UM_R);
    TEST_ASSERT_EQUAL_HEX8(0x85, g_stub_state.cpu_flags_last_r);
    free(resp);
    jsonl_msg_free(req);
}


void test_get_config_settings_returns_filtered_or_sections(void) {
    dispatch_stub_reset();
    /* Stub vrátí non-OK pro SETTINGS_GET (= settings_fake_result = -2),
     * čímž ověříme, že handler vrátí prázdné sections (= žádné klíče). */
    g_stub_state.settings_fake_result = -2;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_config_settings", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_has_member(obj, "profile"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "filtered"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "sections"));
    json_node_free(data);
    free(raw);
}


void test_get_media_state_returns_slots_array_and_note(void) {
    dispatch_stub_reset();
    g_stub_state.media_state_fake_count = 3;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_media_state", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_has_member(obj, "slots"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "count"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "note"));
    TEST_ASSERT_EQUAL_INT(3,
        (int)json_object_get_int_member(obj, "count"));
    json_node_free(data);
    free(raw);
}


void test_get_cpu_im2_vector_available_with_stub(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_cpu_im2_vector", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(2,
        (int)json_object_get_int_member(obj, "im"));
    TEST_ASSERT_EQUAL_INT(0x40,
        (int)json_object_get_int_member(obj, "i"));
    TEST_ASSERT_EQUAL_INT(0x80,
        (int)json_object_get_int_member(obj, "vec"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_INT(0x4080,
        (int)json_object_get_int_member(obj, "isr_addr"));
    TEST_ASSERT_EQUAL_INT(0x1234,
        (int)json_object_get_int_member(obj, "isr_target"));
    json_node_free(data);
    free(raw);
}


void test_get_cpu_interrupt_bus_z80_state_and_placeholders(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_cpu_interrupt_bus", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_has_member(obj, "z80_state"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "platform_note"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "daisy_chain"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "non_chain_sources"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "nmi_sources"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "recent_acks"));
    JsonObject *dc = json_object_get_object_member(obj, "daisy_chain");
    TEST_ASSERT_FALSE(json_object_get_boolean_member(dc, "available"));
    json_node_free(data);
    free(raw);
}


void test_get_cooperation_policy_returns_default_free(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_cooperation_policy", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_has_member(obj, "mode"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "set_by"));
    TEST_ASSERT_EQUAL_STRING("ai",
        json_object_get_string_member(obj, "set_by"));
    json_node_free(data);
    free(raw);
}


void test_get_security_profile_capabilities_present(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_security_profile", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_has_member(obj, "profile"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "capabilities"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "auth"));
    JsonArray *caps = json_object_get_array_member(obj, "capabilities");
    TEST_ASSERT_NOT_NULL(caps);
    TEST_ASSERT_TRUE(json_array_get_length(caps) >= 3);
    json_node_free(data);
    free(raw);
}


void test_get_memory_map_returns_16_slots(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_memory_map", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_has_member(obj, "platform"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "slots"));
    JsonArray *slots = json_object_get_array_member(obj, "slots");
    TEST_ASSERT_NOT_NULL(slots);
    TEST_ASSERT_EQUAL_INT(16, (int)json_array_get_length(slots));
    json_node_free(data);
    free(raw);
}


void test_get_memext_info_none_when_disconnected(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_memext_info", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_STRING("none",
        json_object_get_string_member(obj, "type"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "connected"));
    json_node_free(data);
    free(raw);
}


/* ======================================================================== */
/* V1.D.2.A - Easy reuse Resource backing handlers (6 testů)                 */
/* ======================================================================== */

/**
 * @brief Empty callstack (= callstack_fake_count == 0). Ověří layout:
 *        active flag z param.active, count=0, frames pole prázdné.
 */
void test_get_callstack_empty_when_no_frames(void) {
    dispatch_stub_reset();
    g_stub_state.callstack_fake_active        = 1;
    g_stub_state.callstack_fake_count         = 0;
    g_stub_state.callstack_fake_current_depth = 0;
    g_stub_state.callstack_fake_cycles_now    = 12345ull;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_callstack", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "active"));
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "count"));
    TEST_ASSERT_EQUAL_INT(12345,
        (int)json_object_get_int_member(obj, "cycles_now"));
    JsonArray *frames = json_object_get_array_member(obj, "frames");
    TEST_ASSERT_NOT_NULL(frames);
    TEST_ASSERT_EQUAL_INT(0, (int)json_array_get_length(frames));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Populated callstack (= 3 fake frames). Ověří, že frames pole
 *        má správný počet entries a top-of-stack je depth=0.
 */
void test_get_callstack_populated_frames(void) {
    dispatch_stub_reset();
    g_stub_state.callstack_fake_active        = 1;
    g_stub_state.callstack_fake_count         = 3;
    g_stub_state.callstack_fake_current_depth = 3;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_callstack", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(3,
        (int)json_object_get_int_member(obj, "count"));
    JsonArray *frames = json_object_get_array_member(obj, "frames");
    TEST_ASSERT_NOT_NULL(frames);
    TEST_ASSERT_EQUAL_INT(3, (int)json_array_get_length(frames));
    /* První záznam (= entries[0] = nejstarší frame) má depth = 2;
     * poslední (= top) má depth = 0. */
    JsonObject *first = json_array_get_object_element(frames, 0);
    TEST_ASSERT_EQUAL_INT(2,
        (int)json_object_get_int_member(first, "depth"));
    JsonObject *last = json_array_get_object_element(frames, 2);
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(last, "depth"));
    /* Kind field musí být anglický string (= _cs_kind_to_str). Stub
     * neexplicit nastavuje e->kind takže to bude CS_KIND_CALL = "call". */
    TEST_ASSERT_EQUAL_STRING("call",
        json_object_get_string_member(first, "kind"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Profiler snapshot s 2 fake entries. Ověří layout polí
 *        (entry_count, total_cycles_64, entries pole).
 */
void test_get_profiler_returns_stats_and_entries(void) {
    dispatch_stub_reset();
    g_stub_state.profiler_get_fake_active          = 1;
    g_stub_state.profiler_get_fake_count           = 2;
    g_stub_state.profiler_get_fake_total_cycles_64 = 99999ull;
    g_stub_state.profiler_get_fake_total_calls     = 1234ull;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_profiler", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "active"));
    TEST_ASSERT_EQUAL_INT(2,
        (int)json_object_get_int_member(obj, "entry_count"));
    TEST_ASSERT_EQUAL_INT(99999,
        (int)json_object_get_int_member(obj, "total_cycles_64"));
    TEST_ASSERT_EQUAL_INT(1234,
        (int)json_object_get_int_member(obj, "total_calls"));
    JsonArray *entries = json_object_get_array_member(obj, "entries");
    TEST_ASSERT_NOT_NULL(entries);
    TEST_ASSERT_EQUAL_INT(2, (int)json_array_get_length(entries));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Symbol list - stub vrátí 3 fake záznamy (SYM_0/1/2 @ 0x4000+i),
 *        source=3 (LBL). Resource handler musí mapovat source na "user"
 *        + nastavit truncated=false (3 < cap).
 */
void test_get_symbols_returns_user_source_mapping(void) {
    dispatch_stub_reset();
    g_stub_state.sym_list_fake_count = 3;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_symbols", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(3,
        (int)json_object_get_int_member(obj, "count"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "truncated"));
    JsonArray *syms = json_object_get_array_member(obj, "symbols");
    TEST_ASSERT_NOT_NULL(syms);
    TEST_ASSERT_EQUAL_INT(3, (int)json_array_get_length(syms));
    JsonObject *first = json_array_get_object_element(syms, 0);
    TEST_ASSERT_EQUAL_INT(0x4000,
        (int)json_object_get_int_member(first, "addr"));
    TEST_ASSERT_EQUAL_STRING("SYM_0",
        json_object_get_string_member(first, "name"));
    /* Source 3 (LBL) -> "user". */
    TEST_ASSERT_EQUAL_STRING("user",
        json_object_get_string_member(first, "source"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Stack history disabled - active=0 musí vrátit enabled=false +
 *        prázdné samples (= brief acceptance).
 */
void test_get_stack_history_disabled_returns_empty_samples(void) {
    dispatch_stub_reset();
    g_stub_state.stack_history_fake_active = 0;
    g_stub_state.stack_history_fake_count  = 100;  /* ignored when disabled */
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_stack_history", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "enabled"));
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "count"));
    JsonArray *samples = json_object_get_array_member(obj, "samples");
    TEST_ASSERT_NOT_NULL(samples);
    TEST_ASSERT_EQUAL_INT(0, (int)json_array_get_length(samples));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Stack history enabled + 5 fake samples. Ověří first/last cycle
 *        pattern (cycles = 1000 + i*10).
 */
void test_get_stack_history_enabled_serializes_samples(void) {
    dispatch_stub_reset();
    g_stub_state.stack_history_fake_active = 1;
    g_stub_state.stack_history_fake_count  = 5;
    g_stub_state.stack_history_fake_slope  = -0.25f;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_stack_history", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "enabled"));
    TEST_ASSERT_EQUAL_INT(5,
        (int)json_object_get_int_member(obj, "count"));
    JsonArray *samples = json_object_get_array_member(obj, "samples");
    TEST_ASSERT_EQUAL_INT(5, (int)json_array_get_length(samples));
    JsonObject *s0 = json_array_get_object_element(samples, 0);
    TEST_ASSERT_EQUAL_INT(1000,
        (int)json_object_get_int_member(s0, "cycles"));
    TEST_ASSERT_EQUAL_INT(0xF000,
        (int)json_object_get_int_member(s0, "sp"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Stack regions - 2 fake regions, ověří layout (name, base,
 *        limit, watermark, push_count, current_sp_in_region).
 */
void test_get_stack_regions_returns_two_named_regions(void) {
    dispatch_stub_reset();
    g_stub_state.stack_regions_fake_count  = 2;
    g_stub_state.stack_regions_fake_sp_now = 0xEFD0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_stack_regions", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(2,
        (int)json_object_get_int_member(obj, "count"));
    TEST_ASSERT_EQUAL_INT(0xEFD0,
        (int)json_object_get_int_member(obj, "sp_now"));
    JsonArray *regs = json_object_get_array_member(obj, "regions");
    TEST_ASSERT_NOT_NULL(regs);
    TEST_ASSERT_EQUAL_INT(2, (int)json_array_get_length(regs));
    JsonObject *r0 = json_array_get_object_element(regs, 0);
    TEST_ASSERT_EQUAL_STRING("stack_0",
        json_object_get_string_member(r0, "name"));
    TEST_ASSERT_EQUAL_INT(0xF000,
        (int)json_object_get_int_member(r0, "base"));
    TEST_ASSERT_EQUAL_INT(0xEF80,
        (int)json_object_get_int_member(r0, "limit"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(r0,
        "current_sp_in_region"));
    /* r1 nemá current_sp_in_region. */
    JsonObject *r1 = json_array_get_object_element(regs, 1);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(r1,
        "current_sp_in_region"));
    json_node_free(data);
    free(raw);
}


/* ====================================================================== */
/* V1.D.2.B - Medium debug Resources                                       */
/* ====================================================================== */

/**
 * @brief Empty watch list = count=0, watches pole prázdné.
 */
void test_get_watch_empty(void) {
    dispatch_stub_reset();
    g_stub_state.watch_list_fake_count = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_watch", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "count"));
    JsonArray *arr = json_object_get_array_member(obj, "watches");
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_EQUAL_INT(0, (int)json_array_get_length(arr));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Populated watch list = 3 fake záznamy ze stubu. Ověří layout
 *        per-řádku (index, name, mode, type, addr, value).
 */
void test_get_watch_populated(void) {
    dispatch_stub_reset();
    g_stub_state.watch_list_fake_count = 3;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_watch", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(3,
        (int)json_object_get_int_member(obj, "count"));
    JsonArray *arr = json_object_get_array_member(obj, "watches");
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_EQUAL_INT(3, (int)json_array_get_length(arr));
    /* První záznam = stub pattern: index=0, name="W_0", mode=ADDRESS,
     * type=U8, addr=0x100, value="v_0". */
    JsonObject *r0 = json_array_get_object_element(arr, 0);
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(r0, "index"));
    TEST_ASSERT_EQUAL_STRING("W_0",
        json_object_get_string_member(r0, "name"));
    TEST_ASSERT_EQUAL_STRING("address",
        json_object_get_string_member(r0, "mode"));
    TEST_ASSERT_EQUAL_STRING("u8",
        json_object_get_string_member(r0, "type"));
    TEST_ASSERT_EQUAL_INT(0x100,
        (int)json_object_get_int_member(r0, "addr"));
    TEST_ASSERT_EQUAL_STRING("v_0",
        json_object_get_string_member(r0, "value"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief get_stack základní layout - sp_now, count_words=32, words pole.
 *
 * Stub default sp_now=0xF000, sp_odd=0, pattern_base=0x10. Handler
 * volá v SP-anchored mode (lines_above=32), buf je naplněn ramp
 * patternem. Words[0] = addr=sp_now, words[31] = addr=sp_now+62.
 */
void test_get_stack_basic(void) {
    dispatch_stub_reset();
    g_stub_state.stack_dump_fake_sp_now      = 0xF000;
    g_stub_state.stack_dump_fake_sp_odd      = 0;
    g_stub_state.stack_dump_fake_pattern_base = 0x10;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_stack", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(0xF000,
        (int)json_object_get_int_member(obj, "sp_now"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "sp_odd"));
    TEST_ASSERT_EQUAL_INT(32,
        (int)json_object_get_int_member(obj, "count_words"));
    JsonArray *arr = json_object_get_array_member(obj, "words");
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_EQUAL_INT(32, (int)json_array_get_length(arr));
    /* words[0]: addr = sp_now = 0xF000. words[31]: addr = sp_now + 62. */
    JsonObject *w0 = json_array_get_object_element(arr, 0);
    TEST_ASSERT_EQUAL_INT(0xF000,
        (int)json_object_get_int_member(w0, "addr"));
    JsonObject *w31 = json_array_get_object_element(arr, 31);
    TEST_ASSERT_EQUAL_INT(0xF000 + 62,
        (int)json_object_get_int_member(w31, "addr"));
    /* sp_now je v buf[LEN-1] a buf[LEN-2] (DESC) = pattern u nejvyšších
     * indexů. buf[63] = base+63 = 0x10+63 = 0x4F (low), buf[62] = 0x4E
     * (high) -> value = (0x4E << 8) | 0x4F = 0x4E4F.
     * Ověřme jen existenci value pole + typ. */
    TEST_ASSERT_TRUE(json_object_has_member(w0, "value"));
    int v0 = (int)json_object_get_int_member(w0, "value");
    TEST_ASSERT_TRUE(v0 >= 0 && v0 <= 0xFFFF);
    json_node_free(data);
    free(raw);
}


/**
 * @brief get_stack respektuje sp_odd flag.
 */
void test_get_stack_sp_odd(void) {
    dispatch_stub_reset();
    g_stub_state.stack_dump_fake_sp_now = 0xEFFF;
    g_stub_state.stack_dump_fake_sp_odd = 1;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_stack", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(0xEFFF,
        (int)json_object_get_int_member(obj, "sp_now"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "sp_odd"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Empty bp_vars storage = count=0, truncated=false, vars prázdné.
 */
void test_get_vars_empty(void) {
    dispatch_stub_reset();
    g_stub_state.bp_vars_fake_count = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_vars", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "count"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "truncated"));
    JsonArray *arr = json_object_get_array_member(obj, "vars");
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_EQUAL_INT(0, (int)json_array_get_length(arr));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Populated bp_vars (3 fake records). Verifies record layout
 *        a střídání has_comment/persist_value pole stubu.
 */
void test_get_vars_populated(void) {
    dispatch_stub_reset();
    g_stub_state.bp_vars_fake_count = 3;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_vars", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(3,
        (int)json_object_get_int_member(obj, "count"));
    JsonArray *arr = json_object_get_array_member(obj, "vars");
    TEST_ASSERT_EQUAL_INT(3, (int)json_array_get_length(arr));
    /* VAR_0: even index = has_comment + persist. */
    JsonObject *v0 = json_array_get_object_element(arr, 0);
    TEST_ASSERT_EQUAL_STRING("VAR_0",
        json_object_get_string_member(v0, "name"));
    TEST_ASSERT_EQUAL_INT(0x1000,
        (int)json_object_get_int_member(v0, "value"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(v0, "has_comment"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(v0, "persist_value"));
    TEST_ASSERT_EQUAL_STRING("cmt_0",
        json_object_get_string_member(v0, "comment"));
    /* VAR_1: odd index = no comment, no persist. */
    JsonObject *v1 = json_array_get_object_element(arr, 1);
    TEST_ASSERT_EQUAL_STRING("VAR_1",
        json_object_get_string_member(v1, "name"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(v1, "has_comment"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(v1, "persist_value"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Empty bookmarks storage.
 */
void test_get_bookmarks_empty(void) {
    dispatch_stub_reset();
    g_stub_state.bookmarks_fake_count = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_bookmarks", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "count"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "truncated"));
    JsonArray *arr = json_object_get_array_member(obj, "bookmarks");
    TEST_ASSERT_EQUAL_INT(0, (int)json_array_get_length(arr));
    json_node_free(data);
    free(raw);
}


/**
 * @brief Populated bookmarks - ověří id, input, owner mapping.
 */
void test_get_bookmarks_populated(void) {
    dispatch_stub_reset();
    g_stub_state.bookmarks_fake_count = 2;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_bookmarks", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(2,
        (int)json_object_get_int_member(obj, "count"));
    JsonArray *arr = json_object_get_array_member(obj, "bookmarks");
    TEST_ASSERT_EQUAL_INT(2, (int)json_array_get_length(arr));
    /* BM_0: id=1, owner=user, addr=0x4000. */
    JsonObject *b0 = json_array_get_object_element(arr, 0);
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(b0, "id"));
    TEST_ASSERT_EQUAL_STRING("BM_0",
        json_object_get_string_member(b0, "input"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(b0, "addr_resolved"));
    TEST_ASSERT_EQUAL_INT(0x4000,
        (int)json_object_get_int_member(b0, "addr"));
    TEST_ASSERT_EQUAL_STRING("user",
        json_object_get_string_member(b0, "owner"));
    /* BM_1: id=2, owner=mcp. */
    JsonObject *b1 = json_array_get_object_element(arr, 1);
    TEST_ASSERT_EQUAL_INT(2,
        (int)json_object_get_int_member(b1, "id"));
    TEST_ASSERT_EQUAL_STRING("mcp",
        json_object_get_string_member(b1, "owner"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.A - get_periph_i8255 vrátí 8255 PPI snapshot.
 *
 * Stub naplní deterministické fields (PA=0xAB, PC=0x55, CW=0x90 = Mode
 * Set, PA=input, ostatní output, PC0/PC2/PC4=1, PC1/PC3=0). Test ověří
 * že dispatch handler serializuje JSON klíče, enum-to-string mapping
 * (pa_dir = "input"), a PC signal bity.
 */
void test_get_periph_i8255_layout(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_i8255", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(0xAB,
        (int)json_object_get_int_member(obj, "port_a"));
    TEST_ASSERT_EQUAL_INT(0x55,
        (int)json_object_get_int_member(obj, "port_c"));
    TEST_ASSERT_EQUAL_INT(0x90,
        (int)json_object_get_int_member(obj, "control_word"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "cw_decoded"));
    /* CW 0x90 = bit 7=1 (Mode Set), bit 4=1 (PA=input), ostatní 0. */
    TEST_ASSERT_EQUAL_STRING("input",
        json_object_get_string_member(obj, "pa_dir"));
    TEST_ASSERT_EQUAL_STRING("output",
        json_object_get_string_member(obj, "pb_dir"));
    TEST_ASSERT_EQUAL_STRING("output",
        json_object_get_string_member(obj, "pc_upper_dir"));
    TEST_ASSERT_EQUAL_STRING("output",
        json_object_get_string_member(obj, "pc_lower_dir"));
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "mode_group_a"));
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "mode_group_b"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "signal_pc00"));
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "signal_pc01"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "signal_pc02"));
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "signal_pc03"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "signal_pc04"));
    TEST_ASSERT_EQUAL_INT(5,
        (int)json_object_get_int_member(obj, "pa_keyboard_column"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "pa_joy1_enabled"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "pa_joy2_enabled"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_i8255_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.A - get_periph_i8255 s CW bez Mode Set flagu.
 *
 * Pokud nejvyšší bit control_word je 0 (= Bit Set/Reset operace),
 * cw_decoded = false a mode/dir fields zůstávají 0. Test verifikuje
 * že dispatch zachová pa_dir/... = "output" (kvůli memset 0) a
 * cw_decoded=false.
 */
void test_get_periph_i8255_cw_not_decoded(void) {
    dispatch_stub_reset();
    /* CW 0x05 = Bit Set/Reset (bit 7=0). */
    g_stub_state.periph_i8255_fake_cw = 0x05;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_i8255", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(0x05,
        (int)json_object_get_int_member(obj, "control_word"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "cw_decoded"));
    /* Decoder fields jsou 0 (= memset), pa_dir = "output" (0 = output). */
    TEST_ASSERT_EQUAL_STRING("output",
        json_object_get_string_member(obj, "pa_dir"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.A - get_periph_i8253 vrátí 3 kanály CTC.
 *
 * Stub naplní per-channel pattern (value = 0x1000 + i*0x100, mode = i,
 * rlf=3 LSB+MSB, state=8 countdown). Test ověří JSON layout, enum
 * mapování (mode "mode0"/"mode1"/"mode2", rlf "lsb_msb", state
 * "countdown") a last_cw_byte mirror.
 */
void test_get_periph_i8253_layout(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_i8253", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(0x36,
        (int)json_object_get_int_member(obj, "last_cw_byte"));
    JsonArray *arr = json_object_get_array_member(obj, "channels");
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_EQUAL_INT(3, (int)json_array_get_length(arr));
    /* CTC0 - index=0, mode=mode0, value=0x1000, preset=0x2000. */
    JsonObject *c0 = json_array_get_object_element(arr, 0);
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(c0, "index"));
    TEST_ASSERT_EQUAL_INT(0x1000,
        (int)json_object_get_int_member(c0, "value"));
    TEST_ASSERT_EQUAL_INT(0x2000,
        (int)json_object_get_int_member(c0, "preset_value"));
    TEST_ASSERT_EQUAL_STRING("mode0",
        json_object_get_string_member(c0, "mode"));
    TEST_ASSERT_EQUAL_STRING("lsb_msb",
        json_object_get_string_member(c0, "rlf"));
    TEST_ASSERT_EQUAL_STRING("countdown",
        json_object_get_string_member(c0, "state"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(c0, "bcd"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(c0, "load_done"));
    /* CTC1 - mode=mode1. */
    JsonObject *c1 = json_array_get_object_element(arr, 1);
    TEST_ASSERT_EQUAL_STRING("mode1",
        json_object_get_string_member(c1, "mode"));
    /* CTC2 - mode=mode2, value=0x1200. */
    JsonObject *c2 = json_array_get_object_element(arr, 2);
    TEST_ASSERT_EQUAL_INT(0x1200,
        (int)json_object_get_int_member(c2, "value"));
    TEST_ASSERT_EQUAL_STRING("mode2",
        json_object_get_string_member(c2, "mode"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_i8253_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.A - get_periph_z80_pio na platformě s chipem (default).
 *
 * Stub default available=1 (= MZ-800/1500). Test ověří agregátní
 * interrupt string "pending", interrupt_port_id=0, oba porty + enum
 * mapování (port_a.mode="input", port_a.port_int="pending",
 * port_b.mode="output").
 */
void test_get_periph_z80_pio_available(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_z80_pio", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_STRING("pending",
        json_object_get_string_member(obj, "interrupt"));
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "interrupt_port_id"));
    /* Port A: mode=input, data_output=0x10, masked_input=0x20,
     * int_vec=0x40, int_enable=true, port_int=pending. */
    JsonObject *pa = json_object_get_object_member(obj, "port_a");
    TEST_ASSERT_NOT_NULL(pa);
    TEST_ASSERT_EQUAL_INT(0x10,
        (int)json_object_get_int_member(pa, "data_output"));
    TEST_ASSERT_EQUAL_INT(0x20,
        (int)json_object_get_int_member(pa, "masked_input"));
    TEST_ASSERT_EQUAL_INT(0x40,
        (int)json_object_get_int_member(pa, "int_vec"));
    TEST_ASSERT_EQUAL_STRING("input",
        json_object_get_string_member(pa, "mode"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(pa, "int_enable"));
    TEST_ASSERT_EQUAL_STRING("pending",
        json_object_get_string_member(pa, "port_int"));
    /* Port B: mode=output, port_int=none. */
    JsonObject *pb = json_object_get_object_member(obj, "port_b");
    TEST_ASSERT_NOT_NULL(pb);
    TEST_ASSERT_EQUAL_STRING("output",
        json_object_get_string_member(pb, "mode"));
    TEST_ASSERT_EQUAL_STRING("none",
        json_object_get_string_member(pb, "port_int"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(pb, "int_enable"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_z80_pio_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.A - get_periph_z80_pio na platformě bez chipu (MZ-700).
 *
 * Stub s periph_z80_pio_fake_available=0 simuluje MZ-700 build. Dispatch
 * vrátí JSON {"available": false, "reason": "platform has no Z80 PIO"}
 * bez polí port_a / port_b. Klient musí check available před čtením.
 */
void test_get_periph_z80_pio_unavailable_mz700(void) {
    dispatch_stub_reset();
    g_stub_state.periph_z80_pio_fake_available = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_z80_pio", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_STRING("platform has no Z80 PIO",
        json_object_get_string_member(obj, "reason"));
    /* port_a / port_b NEjsou v JSON odpovědi (= klient by se neměl
     * spoléhat na jejich přítomnost při available=false). */
    TEST_ASSERT_FALSE(json_object_has_member(obj, "port_a"));
    TEST_ASSERT_FALSE(json_object_has_member(obj, "port_b"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_z80_pio_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.B - get_periph_sn76489 mono (= MZ-800 mono default).
 *
 * Stub default available=1, stereo=0, psg_count=1. Test ověří agregátní
 * fields (available=true, stereo=false, psg_count=1), PSG0 latch + 4
 * kanály s pattern. PSG1 NEsmí být v JSON odpovědi.
 */
void test_get_periph_sn76489_mono(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_sn76489", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "stereo"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "psg_count"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "psg0"));
    /* PSG1 NEsmí být přítomen v mono režimu. */
    TEST_ASSERT_FALSE(json_object_has_member(obj, "psg1"));

    JsonObject *psg0 = json_object_get_object_member(obj, "psg0");
    TEST_ASSERT_NOT_NULL(psg0);
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(psg0, "latch_cs"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(psg0, "latch_attn"));
    JsonArray *ch_arr = json_object_get_array_member(psg0, "channels");
    TEST_ASSERT_NOT_NULL(ch_arr);
    TEST_ASSERT_EQUAL_INT(4, (int)json_array_get_length(ch_arr));

    /* Kanál 0: type=tone, attn=0, divider=0x100. */
    JsonObject *c0 = json_array_get_object_element(ch_arr, 0);
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(c0, "index"));
    TEST_ASSERT_EQUAL_STRING("tone",
        json_object_get_string_member(c0, "type"));
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(c0, "attenuation"));
    TEST_ASSERT_EQUAL_INT(0x100,
        (int)json_object_get_int_member(c0, "tone_divider"));

    /* Kanál 3: type=noise, attn=12, noise_div=div_64, noise_type=white. */
    JsonObject *c3 = json_array_get_object_element(ch_arr, 3);
    TEST_ASSERT_EQUAL_STRING("noise",
        json_object_get_string_member(c3, "type"));
    TEST_ASSERT_EQUAL_INT(12,
        (int)json_object_get_int_member(c3, "attenuation"));
    TEST_ASSERT_EQUAL_STRING("div_64",
        json_object_get_string_member(c3, "noise_div_type"));
    TEST_ASSERT_EQUAL_STRING("white",
        json_object_get_string_member(c3, "noise_type"));

    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_sn76489_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.B - get_periph_sn76489 stereo (= MZ-1500 / MZ-800
 *        stereo).
 *
 * Stub s fake_stereo=1, psg_count=2. Test ověří přítomnost psg1 s
 * inverzním attn pattern (15/11/7/3).
 */
void test_get_periph_sn76489_stereo(void) {
    dispatch_stub_reset();
    g_stub_state.periph_sn76489_fake_stereo = 1;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_sn76489", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "stereo"));
    TEST_ASSERT_EQUAL_INT(2,
        (int)json_object_get_int_member(obj, "psg_count"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "psg1"));

    JsonObject *psg1 = json_object_get_object_member(obj, "psg1");
    TEST_ASSERT_NOT_NULL(psg1);
    TEST_ASSERT_EQUAL_INT(2,
        (int)json_object_get_int_member(psg1, "latch_cs"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(psg1, "latch_attn"));
    JsonArray *ch_arr = json_object_get_array_member(psg1, "channels");
    TEST_ASSERT_NOT_NULL(ch_arr);
    TEST_ASSERT_EQUAL_INT(4, (int)json_array_get_length(ch_arr));

    /* PSG1 kanál 0: attn=15 (silent). */
    JsonObject *c0 = json_array_get_object_element(ch_arr, 0);
    TEST_ASSERT_EQUAL_INT(15,
        (int)json_object_get_int_member(c0, "attenuation"));
    /* PSG1 kanál 3 noise: noise_div=tone2_controlled, noise_type=periodic. */
    JsonObject *c3 = json_array_get_object_element(ch_arr, 3);
    TEST_ASSERT_EQUAL_STRING("noise",
        json_object_get_string_member(c3, "type"));
    TEST_ASSERT_EQUAL_STRING("tone2_controlled",
        json_object_get_string_member(c3, "noise_div_type"));
    TEST_ASSERT_EQUAL_STRING("periodic",
        json_object_get_string_member(c3, "noise_type"));

    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_sn76489_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.B - get_periph_sn76489 na MZ-700 (= HAVE_PSG=0).
 *
 * Stub s fake_available=0 simuluje MZ-700 build. Dispatch vrátí
 * {"available": false, "reason": "platform has no PSG"} bez psg0/psg1
 * fields.
 */
void test_get_periph_sn76489_unavailable_mz700(void) {
    dispatch_stub_reset();
    g_stub_state.periph_sn76489_fake_available = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_sn76489", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_STRING("platform has no PSG",
        json_object_get_string_member(obj, "reason"));
    TEST_ASSERT_FALSE(json_object_has_member(obj, "psg0"));
    TEST_ASSERT_FALSE(json_object_has_member(obj, "psg1"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_sn76489_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.B - get_periph_ay3_8910 placeholder (chip neimplementován).
 *
 * Dispatch handler vždy vrátí {"available": false, "reason": ...}.
 * Klient nesmí spoléhat na registr fields.
 */
void test_get_periph_ay3_8910_placeholder(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_ay3_8910", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_STRING("AY-3-8910 not implemented in this emulator",
        json_object_get_string_member(obj, "reason"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_ay3_8910_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.B - get_periph_beeper default audible (= level=1).
 *
 * Stub default ctc0_out=1, gate0=1, pc0=1 → level=1. Ověř kompletní
 * JSON layout a source string "PC0".
 */
void test_get_periph_beeper_audible(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_beeper", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "level"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "ctc0_out"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "gate0"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "pc0"));
    TEST_ASSERT_EQUAL_STRING("PC0",
        json_object_get_string_member(obj, "source"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_beeper_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.B - get_periph_beeper s PC0=0 (= audio gate zavřen,
 *        level=0 i když CTC0 OUT pulzuje).
 *
 * Verifikuje že level dopočet `ctc0_out AND gate0 AND pc0` v dispatch
 * handleru funguje pro různé bit combinace.
 */
void test_get_periph_beeper_silent_pc0_low(void) {
    dispatch_stub_reset();
    g_stub_state.periph_beeper_fake_pc0 = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_beeper", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    /* level = 1 & 1 & 0 = 0. */
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "level"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "ctc0_out"));
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "pc0"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.C - get_periph_gdg pro MZ-800 layout (default).
 *
 * Default stub vrací platform="mz800", palette_count=16,
 * has_border_reg=1, has_pal_group=1, has_cksw=1. Ověř JSON layout,
 * paleta array má 16 entries, jednotlivé pole.
 */
void test_get_periph_gdg_mz800_layout(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_gdg", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_STRING("mz800",
        json_object_get_string_member(obj, "platform"));
    TEST_ASSERT_EQUAL_INT(16,
        (int)json_object_get_int_member(obj, "palette_count"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "has_border_reg"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "has_pal_group"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "has_cksw"));
    TEST_ASSERT_EQUAL_INT(42,
        (int)json_object_get_int_member(obj, "beam_row"));
    TEST_ASSERT_EQUAL_INT(100,
        (int)json_object_get_int_member(obj, "total_screens"));
    JsonArray *pal = json_object_get_array_member(obj, "palette");
    TEST_ASSERT_EQUAL_INT(16, (int)json_array_get_length(pal));
    /* entries 0..15 = i*16. */
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_array_get_int_element(pal, 0));
    TEST_ASSERT_EQUAL_INT(16,
        (int)json_array_get_int_element(pal, 1));
    TEST_ASSERT_EQUAL_INT(15 * 16,
        (int)json_array_get_int_element(pal, 15));
    /* MZ-800: řadič VRAM (WF/RF) a HW scroll v jednotkách portu. */
    TEST_ASSERT_TRUE(json_object_has_member(obj, "vramctrl"));
    JsonObject *vc = json_object_get_object_member(obj, "vramctrl");
    TEST_ASSERT_EQUAL_STRING("replace", json_object_get_string_member(vc, "wf_mode"));
    TEST_ASSERT_EQUAL_INT(4, (int)json_object_get_int_member(vc, "wf_mode_code"));
    TEST_ASSERT_EQUAL_INT(0x05, (int)json_object_get_int_member(vc, "wf_planes"));
    TEST_ASSERT_EQUAL_INT(0x02, (int)json_object_get_int_member(vc, "rf_planes"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(vc, "rf_search"));
    TEST_ASSERT_EQUAL_INT(1, (int)json_object_get_int_member(vc, "vbank"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "hwscroll"));
    JsonObject *hs = json_object_get_object_member(obj, "hwscroll");
    TEST_ASSERT_TRUE(json_object_get_boolean_member(hs, "enabled"));
    TEST_ASSERT_EQUAL_INT(0x123, (int)json_object_get_int_member(hs, "sof"));
    TEST_ASSERT_EQUAL_INT(5, (int)json_object_get_int_member(hs, "ssa"));
    TEST_ASSERT_EQUAL_INT(0x7D, (int)json_object_get_int_member(hs, "sea"));
    TEST_ASSERT_EQUAL_INT(0x78, (int)json_object_get_int_member(hs, "sw"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.periph_gdg_calls);
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.C - get_periph_gdg pro MZ-1500 layout (= 8-entry
 *        paleta, žádný regBOR/regPALGRP/cksw).
 */
void test_get_periph_gdg_mz1500_layout(void) {
    dispatch_stub_reset();
    memcpy(g_stub_state.periph_gdg_fake_platform, "mz1500", 7);
    g_stub_state.periph_gdg_fake_palette_count  = 8;
    g_stub_state.periph_gdg_fake_has_border_reg = 0;
    g_stub_state.periph_gdg_fake_has_pal_group  = 0;
    g_stub_state.periph_gdg_fake_has_cksw       = 0;
    g_stub_state.periph_gdg_fake_has_vramctrl   = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_gdg", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_STRING("mz1500",
        json_object_get_string_member(obj, "platform"));
    TEST_ASSERT_EQUAL_INT(8,
        (int)json_object_get_int_member(obj, "palette_count"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "has_border_reg"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "has_pal_group"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "has_cksw"));
    JsonArray *pal = json_object_get_array_member(obj, "palette");
    TEST_ASSERT_EQUAL_INT(8, (int)json_array_get_length(pal));
    /* MZ-1500 nemá řadič VRAM MZ-800 ani HW scroll. */
    TEST_ASSERT_FALSE(json_object_has_member(obj, "vramctrl"));
    TEST_ASSERT_FALSE(json_object_has_member(obj, "hwscroll"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.C - get_periph_wd1793 default layout (= available=1,
 *        drive 0 empty).
 */
void test_get_periph_wd1793_default(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_wd1793", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "bus_xlate_invert"));
    TEST_ASSERT_EQUAL_INT(5,
        (int)json_object_get_int_member(obj, "reg_track"));
    TEST_ASSERT_EQUAL_INT(0xAB,
        (int)json_object_get_int_member(obj, "reg_data"));
    JsonArray *drives = json_object_get_array_member(obj, "drives");
    TEST_ASSERT_EQUAL_INT(4, (int)json_array_get_length(drives));
    JsonObject *d0 = json_array_get_object_element(drives, 0);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d0, "present"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.C - get_periph_wd1793 s drive 0 mounted.
 */
void test_get_periph_wd1793_drive0_mounted(void) {
    dispatch_stub_reset();
    g_stub_state.periph_wd1793_fake_drive0_present = 1;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_wd1793", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    JsonArray *drives = json_object_get_array_member(obj, "drives");
    JsonObject *d0 = json_array_get_object_element(drives, 0);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d0, "present"));
    TEST_ASSERT_EQUAL_STRING("test.dsk",
        json_object_get_string_member(d0, "image_basename"));
    TEST_ASSERT_EQUAL_INT(40,
        (int)json_object_get_int_member(d0, "tracks"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.C - get_periph_wd1793 detached (= FDC compiled ale
 *        runtime disconnected).
 */
void test_get_periph_wd1793_detached(void) {
    dispatch_stub_reset();
    g_stub_state.periph_wd1793_fake_available = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_wd1793", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "reason"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.C - get_periph_cmt default STOP state.
 */
void test_get_periph_cmt_stop(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_cmt", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_STRING("stop",
        json_object_get_string_member(obj, "state"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "filled"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.C - get_periph_cmt PLAY state.
 */
void test_get_periph_cmt_play(void) {
    dispatch_stub_reset();
    g_stub_state.periph_cmt_fake_state = 1; /* PLAY */
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_cmt", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_STRING("play",
        json_object_get_string_member(obj, "state"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "filled"));
    TEST_ASSERT_EQUAL_STRING("tape.mzf",
        json_object_get_string_member(obj, "image_basename"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.C - get_periph_qd default IMAGE mode.
 */
void test_get_periph_qd_image(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_qd", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_STRING("image",
        json_object_get_string_member(obj, "type"));
    TEST_ASSERT_EQUAL_STRING("disk.mzq",
        json_object_get_string_member(obj, "image_basename"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.3.C - get_periph_qd detached (= QD compiled ale runtime
 *        disconnected).
 */
void test_get_periph_qd_detached(void) {
    dispatch_stub_reset();
    g_stub_state.periph_qd_fake_available = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_periph_qd", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "reason"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_input_keyboard_state idle (= žádná klávesa stisknutá).
 */
void test_get_input_keyboard_state_idle(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_input_keyboard_state", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(0,
        (int)json_object_get_int_member(obj, "pressed_count"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "pressed_truncated"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "real_matrix"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "virtual_matrix"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "effective"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_input_keyboard_state s 1 stisknutou klávesou
 *        (col 0 bit 0 = RETURN v idle, ale stub stiskne col 6 bit 4).
 */
void test_get_input_keyboard_state_one_pressed(void) {
    dispatch_stub_reset();
    /* Klávesa: col 6 bit 4 = SPACE. Bit clear (= 0) v real matici. */
    g_stub_state.kbd_real_matrix[6] = (uint8_t)~(1u << 4);
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_input_keyboard_state", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "pressed_count"));
    JsonArray *keys = json_object_get_array_member(obj, "pressed_keys");
    TEST_ASSERT_NOT_NULL(keys);
    TEST_ASSERT_EQUAL_INT(1, (int)json_array_get_length(keys));
    JsonObject *k0 = json_array_get_object_element(keys, 0);
    TEST_ASSERT_EQUAL_INT(6, (int)json_object_get_int_member(k0, "col"));
    TEST_ASSERT_EQUAL_INT(4, (int)json_object_get_int_member(k0, "bit"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_input_keyboard_matrix_info návratová struktura.
 */
void test_get_input_keyboard_matrix_info(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_input_keyboard_matrix_info", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_STRING("mztest",
        json_object_get_string_member(obj, "platform"));
    TEST_ASSERT_EQUAL_INT(1,
        (int)json_object_get_int_member(obj, "key_count"));
    JsonArray *keys = json_object_get_array_member(obj, "keys");
    TEST_ASSERT_NOT_NULL(keys);
    TEST_ASSERT_EQUAL_INT(1, (int)json_array_get_length(keys));
    JsonObject *k0 = json_array_get_object_element(keys, 0);
    TEST_ASSERT_EQUAL_STRING("RETURN",
        json_object_get_string_member(k0, "name"));
    TEST_ASSERT_EQUAL_INT(0, (int)json_object_get_int_member(k0, "col"));
    TEST_ASSERT_EQUAL_INT(0, (int)json_object_get_int_member(k0, "bit"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(k0, "needs_shift"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_input_joystick_state s port0 connected.
 */
void test_get_input_joystick_state_port0_connected(void) {
    dispatch_stub_reset();
    g_stub_state.joy_port0_connected  = 1;
    g_stub_state.joy_port0_state_bits = 0x05; /* UP + LEFT */
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_input_joystick_state", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    JsonArray *ports = json_object_get_array_member(obj, "ports");
    TEST_ASSERT_NOT_NULL(ports);
    TEST_ASSERT_EQUAL_INT(2, (int)json_array_get_length(ports));
    JsonObject *p0 = json_array_get_object_element(ports, 0);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(p0, "connected"));
    TEST_ASSERT_EQUAL_INT(0x05,
        (int)json_object_get_int_member(p0, "state_bits"));
    TEST_ASSERT_EQUAL_STRING("joystick",
        json_object_get_string_member(p0, "device_name"));
    JsonObject *p1 = json_array_get_object_element(ports, 1);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(p1, "connected"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_input_joystick_state oba porty disconnected (default).
 */
void test_get_input_joystick_state_disconnected(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_input_joystick_state", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    JsonArray *ports = json_object_get_array_member(obj, "ports");
    TEST_ASSERT_EQUAL_INT(2, (int)json_array_get_length(ports));
    for (int i = 0; i < 2; i++) {
        JsonObject *po = json_array_get_object_element(ports, (guint)i);
        TEST_ASSERT_FALSE(json_object_get_boolean_member(po, "connected"));
        TEST_ASSERT_EQUAL_STRING("none",
            json_object_get_string_member(po, "device_name"));
    }
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_frame_framebuffer_info default shape + palette.
 */
void test_get_frame_framebuffer_info(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_frame_framebuffer_info", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_EQUAL_INT(928, (int)json_object_get_int_member(obj, "width"));
    TEST_ASSERT_EQUAL_INT(288, (int)json_object_get_int_member(obj, "height"));
    TEST_ASSERT_EQUAL_INT(1,   (int)json_object_get_int_member(obj, "bytes_per_pixel"));
    TEST_ASSERT_EQUAL_STRING("index8",
        json_object_get_string_member(obj, "pixel_format"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "has_palette"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "dirty"));
    TEST_ASSERT_EQUAL_INT(42,
        (int)json_object_get_int_member(obj, "last_screen_id"));
    JsonArray *pal = json_object_get_array_member(obj, "palette");
    TEST_ASSERT_NOT_NULL(pal);
    TEST_ASSERT_EQUAL_INT(16, (int)json_array_get_length(pal));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_frame_screenshot_raw default available.
 */
void test_get_frame_screenshot_raw_available(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_frame_screenshot_raw", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_STRING("rgba8888",
        json_object_get_string_member(obj, "pixel_format"));
    TEST_ASSERT_EQUAL_INT(4,
        (int)json_object_get_int_member(obj, "bytes_per_pixel"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "data_b64"));
    /* V1.E.6.C - fallback_source field je vždy přítomné v available=true
     * odpovědi. Stub default vrátí "sdl_snapshot". */
    TEST_ASSERT_TRUE(json_object_has_member(obj, "fallback_source"));
    TEST_ASSERT_EQUAL_STRING("sdl_snapshot",
        json_object_get_string_member(obj, "fallback_source"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_frame_screenshot_raw bez framebufferu (= available=false).
 */
void test_get_frame_screenshot_raw_unavailable(void) {
    dispatch_stub_reset();
    g_stub_state.fb_available = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_frame_screenshot_raw", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "reason"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_frame_screenshot PNG (= vždy available=false ve V1.D.4).
 */
void test_get_frame_screenshot_png_unavailable(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_frame_screenshot", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "reason"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief mzdos 0009 - screenshot_save_to_file bez 'path' => INVALID_PARAMS.
 *
 * Validace path je první krok handleru, PNG capture se nesmí vůbec volat.
 */
void test_screenshot_save_to_file_missing_path(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":910,"
        "\"cmd\":\"screenshot_save_to_file\",\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.frame_screenshot_png_calls);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief mzdos 0009 - screenshot_save_to_file s nepodporovaným formátem.
 *
 * Jen "png" je podporováno; jiný formát => INVALID_PARAMS, bez capture.
 */
void test_screenshot_save_to_file_bad_format(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":911,"
        "\"cmd\":\"screenshot_save_to_file\","
        "\"data\":{\"path\":\"test_screenshot_badfmt.png\",\"format\":\"jpg\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.frame_screenshot_png_calls);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief mzdos 0009 - screenshot_save_to_file když PNG capture není
 *        dostupný (stub vrací available=0).
 *
 * Handler musí vrátit success=true s data.available=false a NESMÍ
 * zapsat žádný soubor. (Success path se zápisem PNG vyžaduje reálný
 * encoder + framebuffer, ověřuje se GUI smoke testem - PNG stub je
 * v headless test buildu vždy unavailable.)
 */
void test_screenshot_save_to_file_png_unavailable(void) {
    dispatch_stub_reset();
    const char *path = "test_screenshot_save_unavail.png";
    remove(path); /* jistota - žádný reziduální soubor z minulého běhu */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":912,"
        "\"cmd\":\"screenshot_save_to_file\","
        "\"data\":{\"path\":\"test_screenshot_save_unavail.png\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    /* path prošel validací => PNG capture se volalo právě jednou. */
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.frame_screenshot_png_calls);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "available"));
    /* Při available=false se soubor NESMÍ vytvořit. */
    TEST_ASSERT_FALSE(g_file_test(path, G_FILE_TEST_EXISTS));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/** @brief Pomocník: screenshot_save_to_file s daty `data_json`, vrátí kód výsledku. */
static en_MCP_DISPATCH_RESULT _screenshot_request(const char *data_json) {
    char *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":913,"
                                 "\"cmd\":\"screenshot_save_to_file\",\"data\":%s}", data_json);
    st_JSONL_MESSAGE *req = _make_request(line);
    g_free(line);
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    free(resp);
    jsonl_msg_free(req);
    return rc;
}


/**
 * @brief screenshot_save_to_file: area, scale a crop se předají do capture.
 */
void test_screenshot_save_to_file_options_passed(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _screenshot_request(
        "{\"path\":\"x.png\",\"area\":\"canvas\",\"scale\":\"native\","
        "\"crop\":{\"x\":32,\"y\":4,\"width\":256,\"height\":192}}"));
    const st_SCREENSHOT_OPTS *o = &g_stub_state.frame_screenshot_png_last_opts;
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_AREA_CANVAS, o->area);
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_SCALE_NATIVE, o->scale_mode);
    TEST_ASSERT_TRUE(o->crop);
    TEST_ASSERT_EQUAL_UINT(32, o->crop_x);
    TEST_ASSERT_EQUAL_UINT(4, o->crop_y);
    TEST_ASSERT_EQUAL_UINT(256, o->crop_w);
    TEST_ASSERT_EQUAL_UINT(192, o->crop_h);

    /* scale jako číslo i jako řetězec; výchozí = framebuffer, full. */
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _screenshot_request("{\"path\":\"x.png\",\"scale\":3}"));
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_SCALE_WINDOW, o->scale_mode);
    TEST_ASSERT_EQUAL_UINT(3, o->window_scale);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _screenshot_request("{\"path\":\"x.png\",\"scale\":\"2\"}"));
    TEST_ASSERT_EQUAL_UINT(2, o->window_scale);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _screenshot_request("{\"path\":\"x.png\"}"));
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_AREA_FULL, o->area);
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_SCALE_FRAMEBUFFER, o->scale_mode);
    TEST_ASSERT_FALSE(o->crop);
}


/**
 * @brief screenshot_save_to_file: neplatné area / scale / crop a ořez mimo
 *        obraz (bad_opts z capture) => INVALID_PARAMS.
 */
void test_screenshot_save_to_file_invalid_options(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _screenshot_request("{\"path\":\"x.png\",\"area\":\"border\"}"));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _screenshot_request("{\"path\":\"x.png\",\"scale\":5}"));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _screenshot_request("{\"path\":\"x.png\",\"scale\":\"big\"}"));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _screenshot_request("{\"path\":\"x.png\",\"crop\":{\"x\":0,\"y\":0,\"width\":0,\"height\":5}}"));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.frame_screenshot_png_calls);

    g_stub_state.frame_screenshot_png_fake_bad_opts = 1;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _screenshot_request("{\"path\":\"x.png\",\"scale\":\"native\","
                            "\"crop\":{\"x\":300,\"y\":0,\"width\":100,\"height\":5}}"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.frame_screenshot_png_calls);
}


/** @brief Pomocník: get_frame_screenshot s daty `data_json`, vrátí kód výsledku. */
static en_MCP_DISPATCH_RESULT _frame_screenshot_request(const char *data_json) {
    char *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":915,"
                                 "\"cmd\":\"get_frame_screenshot\",\"data\":%s}", data_json);
    st_JSONL_MESSAGE *req = _make_request(line);
    g_free(line);
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    free(resp);
    jsonl_msg_free(req);
    return rc;
}


/**
 * @brief get_frame_screenshot (base64 PNG): stejné volby area, scale a crop
 *        jako screenshot_save_to_file; neplatné volby => INVALID_PARAMS.
 *
 * Dřív handler volby ignoroval a vracel vždy celý framebuffer.
 */
void test_get_frame_screenshot_options(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _frame_screenshot_request(
        "{\"area\":\"canvas\",\"scale\":\"native\","
        "\"crop\":{\"x\":32,\"y\":4,\"width\":256,\"height\":192}}"));
    const st_SCREENSHOT_OPTS *o = &g_stub_state.frame_screenshot_png_last_opts;
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_AREA_CANVAS, o->area);
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_SCALE_NATIVE, o->scale_mode);
    TEST_ASSERT_TRUE(o->crop);
    TEST_ASSERT_EQUAL_UINT(256, o->crop_w);

    /* Bez voleb = dřívější chování (celý framebuffer 1:1). */
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _frame_screenshot_request("{}"));
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_AREA_FULL, o->area);
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_SCALE_FRAMEBUFFER, o->scale_mode);
    TEST_ASSERT_FALSE(o->crop);
    TEST_ASSERT_EQUAL_INT(2, g_stub_state.frame_screenshot_png_calls);

    /* Neplatné volby: chyba parametrů bez snímání. */
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _frame_screenshot_request("{\"area\":\"border\"}"));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _frame_screenshot_request("{\"scale\":5}"));
    TEST_ASSERT_EQUAL_INT(2, g_stub_state.frame_screenshot_png_calls);

    /* Ořez mimo obraz (bad_opts ze snímání) => INVALID_PARAMS. */
    g_stub_state.frame_screenshot_png_fake_bad_opts = 1;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _frame_screenshot_request(
        "{\"scale\":\"native\",\"crop\":{\"x\":300,\"y\":0,\"width\":100,\"height\":5}}"));
    TEST_ASSERT_EQUAL_INT(3, g_stub_state.frame_screenshot_png_calls);
}


/** @brief Pomocník: request `cmd` s daty `data_json`; vrátí kód, odpověď do *out (free). */
static en_MCP_DISPATCH_RESULT _ss_request(const char *cmd, const char *data_json, char **out) {
    char *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":914,\"cmd\":\"%s\",\"data\":%s}",
                                 cmd, data_json);
    st_JSONL_MESSAGE *req = _make_request(line);
    g_free(line);
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    jsonl_msg_free(req);
    if (out) *out = resp; else free(resp);
    return rc;
}


/**
 * @brief screenshot_series_start: výchozí parametry (full, native, jen změněné)
 *        a předání dir, area, scale, crop, only_changed, frames, max_files.
 */
void test_screenshot_series_start_params(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("screenshot_series_start", "{}", NULL));
    const st_DBGAPI_SCREENSHOT_SERIES_PARAM *p = &g_stub_state.ss_last;
    TEST_ASSERT_EQUAL_INT(DBGAPI_SCREENSHOT_SERIES_OP_START, p->op);
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_AREA_FULL, p->params.opts.area);
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_SCALE_NATIVE, p->params.opts.scale_mode);
    TEST_ASSERT_TRUE(p->params.only_changed);
    TEST_ASSERT_EQUAL_UINT64(0, p->params.frames);
    TEST_ASSERT_EQUAL_STRING("", p->params.dir);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("screenshot_series_start",
        "{\"dir\":\"C:/tmp/s\",\"area\":\"canvas\",\"scale\":2,\"only_changed\":false,"
        "\"frames\":100,\"max_files\":50}", NULL));
    TEST_ASSERT_EQUAL_STRING("C:/tmp/s", p->params.dir);
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_AREA_CANVAS, p->params.opts.area);
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_SCALE_WINDOW, p->params.opts.scale_mode);
    TEST_ASSERT_EQUAL_UINT(2, p->params.opts.window_scale);
    TEST_ASSERT_FALSE(p->params.only_changed);
    TEST_ASSERT_EQUAL_UINT64(100, p->params.frames);
    TEST_ASSERT_EQUAL_UINT(50, p->params.max_files);
}


/**
 * @brief screenshot_series_*: neplatné parametry => INVALID_PARAMS bez
 *        předání, chyba jádra (např. série už běží) => EMU_ERROR s textem.
 */
void test_screenshot_series_errors(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _ss_request("screenshot_series_start", "{\"frames\":-1}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _ss_request("screenshot_series_start", "{\"only_changed\":\"yes\"}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _ss_request("screenshot_series_start", "{\"scale\":7}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _ss_request("screenshot_series_stop", "{\"wait\":1}", NULL));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.ss_calls);

    g_stub_state.ss_fake_result = DBGAPI_SCREENSHOT_SERIES_RESULT_FAILED;
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, _ss_request("screenshot_series_start", "{}", &resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "already running"));
    free(resp);
}


/**
 * @brief screenshot_series_status a stop: stav v odpovědi.
 */
void test_screenshot_series_status_stop(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("screenshot_series_status", "{}", &resp));
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_TRUE(json_object_has_member(data, "frames_seen"));
    TEST_ASSERT_TRUE(json_object_has_member(data, "unchanged"));
    TEST_ASSERT_EQUAL_STRING("C:/tmp/series", json_object_get_string_member(data, "dir"));
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("screenshot_series_stop", "{\"wait\":false}", NULL));
    TEST_ASSERT_EQUAL_INT(DBGAPI_SCREENSHOT_SERIES_OP_STOP, g_stub_state.ss_last.op);
}


/**
 * @brief profiler_pc_start/stop/reset: předaná operace a reset (výchozí true),
 *        odpověď = souhrn.
 */
void test_profiler_pc_lifecycle(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("profiler_pc_start", "{}", &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_PROFILER_PC_OP_START, g_stub_state.pc_last.op);
    TEST_ASSERT_TRUE(g_stub_state.pc_last.reset);
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"active\":true"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"elapsed_tstates\""));
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("profiler_pc_start", "{\"reset\":false}", NULL));
    TEST_ASSERT_FALSE(g_stub_state.pc_last.reset);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("profiler_pc_stop", "{}", NULL));
    TEST_ASSERT_EQUAL_INT(DBGAPI_PROFILER_PC_OP_STOP, g_stub_state.pc_last.op);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("profiler_pc_reset", "{}", NULL));
    TEST_ASSERT_EQUAL_INT(DBGAPI_PROFILER_PC_OP_RESET, g_stub_state.pc_last.op);
    TEST_ASSERT_EQUAL_INT(4, g_stub_state.pc_calls);
}


/**
 * @brief profiler_pc_get: výchozí parametry (tstates, 50, celý rozsah, se
 *        symboly), předání sort/limit/from/to a řádky v odpovědi.
 */
void test_profiler_pc_get_params(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("profiler_pc_get", "{}", &resp));
    const st_DBGAPI_PROFILER_PC_PARAM *p = &g_stub_state.pc_last;
    TEST_ASSERT_EQUAL_INT(DBGAPI_PROFILER_PC_OP_GET, p->op);
    TEST_ASSERT_EQUAL_INT(PROFILER_PC_SORT_TSTATES, p->sort);
    TEST_ASSERT_EQUAL_UINT(50, p->rows_max);
    TEST_ASSERT_EQUAL_HEX16(0x0000, p->lo);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, p->hi);
    TEST_ASSERT_TRUE(p->with_symbols);
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"total_rows\":3"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"pc\":4608"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"symbol\":\"loop\""));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"sort\":\"tstates\""));
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("profiler_pc_get",
        "{\"sort\":\"wait\",\"limit\":2,\"from\":4096,\"to\":8191}", &resp));
    TEST_ASSERT_EQUAL_INT(PROFILER_PC_SORT_WAIT, p->sort);
    TEST_ASSERT_EQUAL_UINT(2, p->rows_max);
    TEST_ASSERT_EQUAL_HEX16(0x1000, p->lo);
    TEST_ASSERT_EQUAL_HEX16(0x1FFF, p->hi);
    /* Stub vrátí 3 adresy, zapíše jen kapacitu 2. */
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"pc\":4609"));
    TEST_ASSERT_NULL(strstr(resp, "\"pc\":4610"));
    free(resp);
}


/**
 * @brief profiler_pc_get/export: neplatné parametry => INVALID_PARAMS bez
 *        předání; export předá cestu a výchozí řazení podle adresy.
 */
void test_profiler_pc_errors_export(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("profiler_pc_get", "{\"sort\":\"x\"}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("profiler_pc_get", "{\"limit\":0}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("profiler_pc_get", "{\"limit\":1001}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("profiler_pc_get", "{\"from\":5,\"to\":4}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("profiler_pc_get", "{\"to\":65536}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("profiler_pc_start", "{\"reset\":1}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("profiler_pc_export", "{}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("profiler_pc_export", "{\"path\":\"\"}", NULL));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.pc_calls);

    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("profiler_pc_export", "{\"path\":\"C:/tmp/pc.csv\"}", &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_PROFILER_PC_OP_EXPORT, g_stub_state.pc_last.op);
    TEST_ASSERT_EQUAL_INT(PROFILER_PC_SORT_ADDR, g_stub_state.pc_last.sort);
    TEST_ASSERT_EQUAL_STRING("C:/tmp/pc.csv", g_stub_state.pc_last_path);
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"rows\":3"));
    free(resp);
}


/**
 * @brief profiler_pc_bitmap: předá operaci s bufferem, odpověď = souhrn,
 *        formát, velikost a bitmapa v base64 (8192 B, bity ze stubu).
 */
void test_profiler_pc_bitmap(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("profiler_pc_bitmap", "{}", &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_PROFILER_PC_OP_BITMAP, g_stub_state.pc_last.op);
    TEST_ASSERT_NOT_NULL(g_stub_state.pc_last.bitmap);
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_EQUAL_STRING("pc-bitset-lsb0", json_object_get_string_member(data, "format"));
    TEST_ASSERT_EQUAL_INT64(8192, json_object_get_int_member(data, "map_bytes"));
    TEST_ASSERT_EQUAL_INT64(3, json_object_get_int_member(data, "unique_pcs"));
    gsize len = 0;
    guchar *bm = g_base64_decode(json_object_get_string_member(data, "bitmap_base64"), &len);
    TEST_ASSERT_EQUAL_UINT(8192, len);
    TEST_ASSERT_EQUAL_HEX8(0x07, bm[0x1200 >> 3]);
    TEST_ASSERT_EQUAL_HEX8(0x80, bm[0x1FFF]);
    TEST_ASSERT_EQUAL_HEX8(0x00, bm[0]);
    g_free(bm);
    g_object_unref(parser);
    free(resp);
}


/* ====================================================================== */
/* mcp-trace-read: trace_status / trace_read                              */
/* ====================================================================== */

/** @brief Datový objekt úspěšné odpovědi (parser uvolní volající). */
static JsonObject *_tr_data(const char *resp, JsonParser **parser) {
    JsonObject *obj = _parse_response_object(resp, parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    return json_object_get_object_member(obj, "data");
}

/** @brief trace_status bez kanálu vypíše všech 11 kanálů, nezaznamenaný = segment 0. */
void test_trace_status_all_channels(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_status", "{}", &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    JsonArray *ch = json_object_get_array_member(data, "channels");
    TEST_ASSERT_EQUAL_UINT(11, json_array_get_length(ch));
    TEST_ASSERT_EQUAL_STRING("bplog", json_object_get_string_member(json_array_get_object_element(ch, 10), "channel"));
    JsonObject *c0 = json_array_get_object_element(ch, 0);
    TEST_ASSERT_EQUAL_STRING("cputrack", json_object_get_string_member(c0, "channel"));
    TEST_ASSERT_EQUAL_INT64(0, json_object_get_int_member(c0, "segment"));
    TEST_ASSERT_EQUAL_INT64(2, json_object_get_int_member(c0, "format_version"));
    TEST_ASSERT_EQUAL_STRING("marklog", json_object_get_string_member(json_array_get_object_element(ch, 4), "channel"));
    TEST_ASSERT_EQUAL_STRING("eventlog", json_object_get_string_member(json_array_get_object_element(ch, 5), "channel"));
    TEST_ASSERT_EQUAL_STRING("audio_frames", json_object_get_string_member(json_array_get_object_element(ch, 8), "channel"));
    TEST_ASSERT_EQUAL_INT64(0, json_object_get_int_member(json_array_get_object_element(ch, 8), "segment"));
    g_object_unref(parser);
    free(resp);

    /* jeden kanál; neznámý kanál = INVALID_PARAMS */
    g_stub_state.tr_segment = 7;
    g_stub_state.tr_records = 3;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_status", "{\"channel\":\"iorqlog\"}", &resp));
    data = _tr_data(resp, &parser);
    ch = json_object_get_array_member(data, "channels");
    TEST_ASSERT_EQUAL_UINT(1, json_array_get_length(ch));
    c0 = json_array_get_object_element(ch, 0);
    TEST_ASSERT_EQUAL_INT64(7, json_object_get_int_member(c0, "segment"));
    TEST_ASSERT_EQUAL_INT64(3, json_object_get_int_member(c0, "next_seq"));
    TEST_ASSERT_EQUAL_INT64(24, json_object_get_int_member(c0, "record_size"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(c0, "live"));
    TEST_ASSERT_EQUAL_STRING("t", json_object_get_string_member(c0, "name"));
    g_object_unref(parser);
    free(resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("trace_status", "{\"channel\":\"nope\"}", NULL));
}

/** @brief trace_read: řádky iorqlog, kurzor, konec dat, segment_replaced. */
void test_trace_read_rows_cursor_and_segment(void) {
    dispatch_stub_reset();
    g_stub_state.tr_segment = 3;
    g_stub_state.tr_records = 5;
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
        _ss_request("trace_read", "{\"channel\":\"iorqlog\",\"limit\":3}", &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(3, json_object_get_int_member(data, "segment"));
    TEST_ASSERT_EQUAL_INT64(5, json_object_get_int_member(data, "next_seq"));
    JsonArray *rows = json_object_get_array_member(data, "rows");
    TEST_ASSERT_EQUAL_UINT(3, json_array_get_length(rows));
    JsonArray *r1 = json_array_get_array_element(rows, 1);
    TEST_ASSERT_EQUAL_INT64(1, json_array_get_int_element(r1, 0));
    TEST_ASSERT_EQUAL_INT64(100, json_array_get_int_element(r1, 1));
    TEST_ASSERT_EQUAL_STRING("out", json_array_get_string_element(r1, 5));
    TEST_ASSERT_EQUAL_STRING("0011", json_array_get_string_element(r1, 7));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "page_full"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "end_of_data"));
    char *cursor = g_strdup(json_object_get_string_member(data, "next_cursor"));
    /* snímek dvakrát: čtení + kontrola segmentu po čtení */
    TEST_ASSERT_EQUAL_INT(2, g_stub_state.tr_snap_calls);
    g_object_unref(parser);
    free(resp);

    /* pokračování kurzorem: zbylé 2 řádky a konec dat */
    char *req = g_strdup_printf("{\"channel\":\"iorqlog\",\"cursor\":\"%s\"}", cursor);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read", req, &resp));
    data = _tr_data(resp, &parser);
    rows = json_object_get_array_member(data, "rows");
    TEST_ASSERT_EQUAL_UINT(2, json_array_get_length(rows));
    TEST_ASSERT_EQUAL_INT64(3, json_array_get_int_element(json_array_get_array_element(rows, 0), 0));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "end_of_data"));
    g_object_unref(parser);
    free(resp);

    /* kurzor cizího kanálu */
    g_free(req);
    req = g_strdup_printf("{\"channel\":\"hwlog\",\"cursor\":\"%s\"}", cursor);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("trace_read", req, NULL));

    /* segment se změnil (nový záznam): segment_replaced */
    g_stub_state.tr_segment = 4;
    g_free(req);
    req = g_strdup_printf("{\"channel\":\"iorqlog\",\"cursor\":\"%s\"}", cursor);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("trace_read", req, &resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "segment_replaced:"));
    free(resp);

    /* segment se změní mezi čtením a kontrolou */
    dispatch_stub_reset();
    g_stub_state.tr_segment = 3;
    g_stub_state.tr_records = 5;
    g_stub_state.tr_bump_on_call = 2;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _ss_request("trace_read", "{\"channel\":\"iorqlog\"}", &resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "segment_replaced:"));
    free(resp);
    g_free(req);
    g_free(cursor);
}

/** @brief trace_read cputrack: asm, symboly přes TRACE_RESOLVE, sloupce, newest, raw. */
void test_trace_read_cputrack_symbols_columns_raw(void) {
    dispatch_stub_reset();
    g_stub_state.tr_segment = 1;
    g_stub_state.tr_records = 10;
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
        _ss_request("trace_read", "{\"channel\":\"cputrack\",\"from\":\"newest\",\"limit\":2}", &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    JsonArray *rows = json_object_get_array_member(data, "rows");
    TEST_ASSERT_EQUAL_UINT(2, json_array_get_length(rows));
    JsonArray *r0 = json_array_get_array_element(rows, 0);
    TEST_ASSERT_EQUAL_INT64(8, json_array_get_int_element(r0, 0));
    TEST_ASSERT_EQUAL_STRING("1008", json_array_get_string_element(r0, 1));
    TEST_ASSERT_EQUAL_STRING("NOP", json_array_get_string_element(r0, 3));
    TEST_ASSERT_EQUAL_STRING("S1008", json_array_get_string_element(r0, 7));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.tr_resolve_calls);
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
        _ss_request("trace_read", "{\"channel\":\"cputrack\",\"columns\":[\"seq\",\"t\"],\"limit\":2}", &resp));
    data = _tr_data(resp, &parser);
    JsonArray *cols = json_object_get_array_member(data, "columns");
    TEST_ASSERT_EQUAL_UINT(2, json_array_get_length(cols));
    r0 = json_array_get_array_element(json_object_get_array_member(data, "rows"), 1);
    TEST_ASSERT_EQUAL_INT64(1, json_array_get_int_element(r0, 0));
    TEST_ASSERT_EQUAL_INT64(4 * 5, json_array_get_int_element(r0, 1));   /* t = (t_std 4) * 5 */
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
        _ss_request("trace_read", "{\"channel\":\"cputrack\",\"format\":\"raw_b64\",\"limit\":4}", &resp));
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(12, json_object_get_int_member(data, "record_size"));
    TEST_ASSERT_EQUAL_INT64(4, json_object_get_int_member(data, "count"));
    TEST_ASSERT_EQUAL_INT64(2, json_object_get_int_member(data, "format_version"));
    gsize len = 0;
    guchar *raw = g_base64_decode(json_object_get_string_member(data, "data_b64"), &len);
    TEST_ASSERT_EQUAL_UINT(48, len);
    TEST_ASSERT_EQUAL_HEX8(0x03, raw[36]);
    g_free(raw);
    g_object_unref(parser);
    free(resp);
}

/** @brief trace_read eventlog: text detailu přes TRACE_RESOLVE; trace_start eventlog odmítne. */
void test_trace_read_eventlog(void) {
    dispatch_stub_reset();
    g_stub_state.tr_segment = 2;
    g_stub_state.tr_records = 4;
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK,
        _ss_request("trace_read", "{\"channel\":\"eventlog\",\"columns\":[\"seq\",\"text\"]}", &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    JsonArray *rows = json_object_get_array_member(data, "rows");
    TEST_ASSERT_EQUAL_UINT(4, json_array_get_length(rows));
    TEST_ASSERT_EQUAL_STRING("EV0", json_array_get_string_element(json_array_get_array_element(rows, 0), 1));
    g_object_unref(parser);
    free(resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS,
        _ss_request("trace_start", "{\"channel\":\"eventlog\"}", NULL));
}

/** @brief Řádek `i` stránky jako pole. */
static JsonArray *_tr_row(JsonObject *data, guint i) {
    return json_array_get_array_element(json_object_get_array_member(data, "rows"), i);
}

/** @brief trace_read audio_*: řádky z paměti zachycení zvuku, filtry channels a reg, Hz a nota. */
void test_trace_read_audio_channels(void) {
    dispatch_stub_reset();
    g_stub_state.aud_segment = 3;
    g_stub_state.aud_records = 10;
    g_stub_state.aud_origin = 1000;
    char *resp = NULL;
    JsonParser *parser = NULL;

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read", "{\"channel\":\"audio_levels\",\"limit\":20}", &resp));
    JsonObject *data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(3, json_object_get_int_member(data, "segment"));
    TEST_ASSERT_EQUAL_UINT(10, json_array_get_length(json_object_get_array_member(data, "rows")));
    TEST_ASSERT_EQUAL_UINT(6, json_array_get_length(json_object_get_array_member(data, "columns")));
    JsonArray *r1 = _tr_row(data, 1);
    TEST_ASSERT_EQUAL_INT64(1010, json_array_get_int_element(r1, 1));             /* t absolutně */
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 10.0 * 1e6 / GDGCLK_BASE, json_array_get_double_element(r1, 2));
    TEST_ASSERT_EQUAL_STRING("psg0_0", json_array_get_string_element(r1, 4));
    TEST_ASSERT_EQUAL_INT64(1, json_array_get_int_element(r1, 5));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "end_of_data"));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.tr_snap_calls);                         /* bez dbgapi */
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read",
        "{\"channel\":\"audio_levels\",\"filter\":{\"channels\":[\"psg0_0\",\"psg1_3\"]}}", &resp));
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_UINT(2, json_array_get_length(json_object_get_array_member(data, "rows")));   /* src 1 a 8 */
    TEST_ASSERT_EQUAL_INT64(8, json_array_get_int_element(_tr_row(data, 1), 0));
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read",
        "{\"channel\":\"audio_psg_writes\",\"filter\":{\"reg\":\"tone\"}}", &resp));
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_UINT(5, json_array_get_length(json_object_get_array_member(data, "rows")));
    JsonArray *w = _tr_row(data, 0);
    TEST_ASSERT_EQUAL_STRING("tone", json_array_get_string_element(w, 7));
    TEST_ASSERT_EQUAL_INT64(254, json_array_get_int_element(w, 8));
    double hz = (double) GDGCLK_BASE / (32.0 * 254.0 * GDGCLK2CPU_DIVIDER);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, hz, json_array_get_double_element(w, 9));
    TEST_ASSERT_NOT_NULL(json_array_get_string_element(w, 10));
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read",
        "{\"channel\":\"audio_psg_writes\",\"filter\":{\"reg\":\"volume\"},\"limit\":1}", &resp));
    data = _tr_data(resp, &parser);
    w = _tr_row(data, 0);
    TEST_ASSERT_TRUE(JSON_NODE_HOLDS_NULL(json_array_get_element(w, 9)));        /* hlasitost: bez Hz */
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read",
        "{\"channel\":\"audio_frames\",\"from\":\"seq:2\",\"limit\":1}", &resp));
    data = _tr_data(resp, &parser);
    JsonArray *f = _tr_row(data, 0);
    TEST_ASSERT_EQUAL_INT64(2, json_array_get_int_element(f, 0));
    TEST_ASSERT_EQUAL_INT64(1000, json_array_get_int_element(f, 3));
    JsonArray *edges = json_array_get_array_element(f, 4);
    TEST_ASSERT_EQUAL_UINT(AUDIO_SRC_CHANNELS_COUNT, json_array_get_length(edges));
    TEST_ASSERT_EQUAL_INT64(2, json_array_get_int_element(edges, 0));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 50.0, json_array_get_double_element(json_array_get_array_element(f, 5), 0));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 10.61, json_array_get_double_element(json_array_get_array_element(f, 6), 0));
    TEST_ASSERT_EQUAL_INT64(200, json_array_get_int_element(json_array_get_array_element(f, 8), 1));
    TEST_ASSERT_TRUE(JSON_NODE_HOLDS_NULL(json_array_get_element(f, 10)));        /* bez SDL cesty */
    g_object_unref(parser);
    free(resp);

    /* Bez zachycení: prázdná stránka; filtry jen pro své kanály. */
    g_stub_state.aud_segment = 0;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read", "{\"channel\":\"audio_frames\"}", &resp));
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(0, json_object_get_int_member(data, "segment"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "end_of_data"));
    g_object_unref(parser);
    free(resp);
    const char *bad[] = {
        "{\"channel\":\"iorqlog\",\"filter\":{\"channels\":[\"ctc0\"]}}",
        "{\"channel\":\"audio_levels\",\"filter\":{\"channels\":[\"psg9\"]}}",
        "{\"channel\":\"audio_levels\",\"filter\":{\"reg\":\"tone\"}}",
        "{\"channel\":\"audio_psg_writes\",\"filter\":{\"reg\":\"pitch\"}}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, _ss_request("trace_read", bad[i], NULL), bad[i]);
    }
}

/** @brief ramlog_start: rozsahy, režim, kapacita, validace. */
void test_ramlog_start_params(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("ramlog_start", "{}", &resp));
    const st_DBGAPI_RAMLOG_PARAM *p = &g_stub_state.rl_last;
    TEST_ASSERT_EQUAL_INT(DBGAPI_RAMLOG_OP_START, p->op);
    TEST_ASSERT_EQUAL_UINT(0, p->cfg.n_ranges);
    TEST_ASSERT_EQUAL_INT(RAMLOG_MODE_RING, p->cfg.mode);
    TEST_ASSERT_EQUAL_UINT32(RAMLOG_DEFAULT_CAPACITY, p->cfg.capacity);
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"segment\":4"));
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("ramlog_start",
        "{\"ranges\":[{\"from\":16384,\"to\":16385},{\"space\":\"vram\",\"from\":8192,\"to\":8447},{\"from\":32768}],"
        "\"mode\":\"fill\",\"capacity\":64}", &resp));
    TEST_ASSERT_EQUAL_UINT(3, p->cfg.n_ranges);
    TEST_ASSERT_FALSE(p->cfg.ranges[0].phys);
    TEST_ASSERT_EQUAL_UINT32(0x4001, p->cfg.ranges[0].to);
    TEST_ASSERT_TRUE(p->cfg.ranges[1].phys);
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SPACE_VRAM, p->cfg.ranges[1].space);
    TEST_ASSERT_EQUAL_UINT32(0x8000, p->cfg.ranges[2].to);    /* to chybí = from */
    TEST_ASSERT_EQUAL_INT(RAMLOG_MODE_FILL, p->cfg.mode);
    TEST_ASSERT_EQUAL_UINT32(64, p->cfg.capacity);
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"mode\":\"fill\""));
    free(resp);

    int calls = g_stub_state.rl_calls;
    const char *bad[] = {
        "{\"mode\":\"loop\"}", "{\"capacity\":1}", "{\"ranges\":5}", "{\"ranges\":[{\"to\":5}]}",
        "{\"ranges\":[{\"from\":70000}]}", "{\"ranges\":[{\"from\":5,\"to\":4}]}",
        "{\"ranges\":[{\"space\":\"rom\",\"from\":0}]}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, _ss_request("ramlog_start", bad[i], NULL), bad[i]);
    }
    TEST_ASSERT_EQUAL_INT(calls, g_stub_state.rl_calls);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("ramlog_stop", "{}", NULL));
    TEST_ASSERT_EQUAL_INT(DBGAPI_RAMLOG_OP_STOP, p->op);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("ramlog_clear", "{}", NULL));
    TEST_ASSERT_EQUAL_INT(DBGAPI_RAMLOG_OP_CLEAR, p->op);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("ramlog_status", "{}", NULL));
    TEST_ASSERT_EQUAL_INT(DBGAPI_RAMLOG_OP_STATUS, p->op);
}

/** @brief ramlog_who_wrote a ramlog_writers: cíl dotazu, tvar odpovědi. */
void test_ramlog_queries(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("ramlog_who_wrote", "{\"addr\":16385,\"limit\":5}", &resp));
    const st_DBGAPI_RAMLOG_PARAM *p = &g_stub_state.rl_last;
    TEST_ASSERT_EQUAL_INT(DBGAPI_RAMLOG_OP_WHO_WROTE, p->op);
    TEST_ASSERT_TRUE(p->by_cpu);
    TEST_ASSERT_EQUAL_UINT16(0x4001, p->cpu_from);
    TEST_ASSERT_EQUAL_UINT(5, p->limit);
    TEST_ASSERT_FALSE(p->include_unchanged);
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    JsonObject *c = json_array_get_object_element(json_object_get_array_member(data, "cells"), 0);
    TEST_ASSERT_EQUAL_STRING("ram", json_object_get_string_member(c, "space"));
    TEST_ASSERT_EQUAL_STRING("3009", json_object_get_string_member(json_object_get_object_member(c, "last"), "pc"));
    JsonObject *h = json_array_get_object_element(json_object_get_array_member(c, "history"), 0);
    TEST_ASSERT_EQUAL_STRING("66", json_object_get_string_member(h, "new"));
    TEST_ASSERT_EQUAL_INT64(1, json_object_get_int_member(h, "seq"));
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("ramlog_who_wrote", "{\"space\":\"vram\",\"offset\":8208,\"all\":true}", NULL));
    TEST_ASSERT_FALSE(p->by_cpu);
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SPACE_VRAM, p->space);
    TEST_ASSERT_EQUAL_UINT32(0x2010, p->off_from);
    TEST_ASSERT_TRUE(p->include_unchanged);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("ramlog_writers", "{\"from\":16384,\"to\":16431,\"limit\":1}", &resp));
    TEST_ASSERT_TRUE(p->by_cpu);
    TEST_ASSERT_EQUAL_UINT16(0x402F, p->cpu_to);
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(2, json_object_get_int_member(data, "total"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "truncated"));
    TEST_ASSERT_EQUAL_UINT(1, json_array_get_length(json_object_get_array_member(data, "writers")));
    g_object_unref(parser);
    free(resp);

    const char *bad_w[] = { "{}", "{\"addr\":70000}", "{\"space\":\"nope\",\"offset\":1}", "{\"addr\":1,\"limit\":2000}" };
    for (size_t i = 0; i < G_N_ELEMENTS(bad_w); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, _ss_request("ramlog_who_wrote", bad_w[i], NULL), bad_w[i]);
    }
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("ramlog_writers", "{}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("ramlog_writers", "{\"from\":1,\"limit\":0}", NULL));
}

/** @brief trace_read ramlog: nezměněné skryté bez all, filtry pc/addr/src, symbol. */
void test_trace_read_ramlog(void) {
    dispatch_stub_reset();
    g_stub_state.tr_segment = 3;
    g_stub_state.tr_records = 6;
    char *resp = NULL;
    JsonParser *parser = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read", "{\"channel\":\"ramlog\"}", &resp));
    JsonObject *data = _tr_data(resp, &parser);
    JsonArray *rows = json_object_get_array_member(data, "rows");
    TEST_ASSERT_EQUAL_UINT(3, json_array_get_length(rows));           /* seq 0, 2, 4 */
    JsonArray *r2 = json_array_get_array_element(rows, 1);
    TEST_ASSERT_EQUAL_INT64(2, json_array_get_int_element(r2, 0));
    TEST_ASSERT_EQUAL_STRING("3002", json_array_get_string_element(r2, 3));
    TEST_ASSERT_EQUAL_STRING("4002", json_array_get_string_element(r2, 5));
    TEST_ASSERT_EQUAL_STRING("ram", json_array_get_string_element(r2, 6));
    TEST_ASSERT_EQUAL_STRING("AA", json_array_get_string_element(r2, 9));
    TEST_ASSERT_EQUAL_STRING("insn", json_array_get_string_element(r2, 11));
    TEST_ASSERT_EQUAL_INT64(12, json_array_get_int_element(r2, 14));    /* tin */
    TEST_ASSERT_TRUE(g_stub_state.tr_resolve_calls > 0);                /* sloupec sym */
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read",
        "{\"channel\":\"ramlog\",\"filter\":{\"all\":true,\"addr_from\":16385,\"addr_to\":16387},\"columns\":[\"seq\",\"flags\"]}", &resp));
    data = _tr_data(resp, &parser);
    rows = json_object_get_array_member(data, "rows");
    TEST_ASSERT_EQUAL_UINT(3, json_array_get_length(rows));           /* seq 1, 2, 3 */
    TEST_ASSERT_EQUAL_STRING("unchanged", json_array_get_string_element(json_array_get_array_element(rows, 0), 1));
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read",
        "{\"channel\":\"ramlog\",\"filter\":{\"pc_from\":12292,\"pc_to\":12292,\"src\":\"insn\",\"space\":\"ram\"}}", &resp));
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_UINT(1, json_array_get_length(json_object_get_array_member(data, "rows")));
    g_object_unref(parser);
    free(resp);

    const char *bad[] = {
        "{\"channel\":\"ramlog\",\"filter\":{\"src\":\"cpu\"}}",
        "{\"channel\":\"ramlog\",\"filter\":{\"space\":\"x\"}}",
        "{\"channel\":\"iorqlog\",\"filter\":{\"all\":true}}",
        "{\"channel\":\"iorqlog\",\"filter\":{\"addr_from\":1}}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, _ss_request("trace_read", bad[i], NULL), bad[i]);
    }
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("trace_start", "{\"channel\":\"ramlog\"}", NULL));
}

/** @brief trace_read bplog: sloupce, symbol insn_pc, UTF-8, filtry bp/kind/pc; bplog_clear. */
void test_trace_read_bplog(void) {
    dispatch_stub_reset();
    g_stub_state.tr_segment = 4;
    g_stub_state.tr_records = 5;
    char *resp = NULL;
    JsonParser *parser = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read", "{\"channel\":\"bplog\"}", &resp));
    JsonObject *data = _tr_data(resp, &parser);
    JsonArray *cols = json_object_get_array_member(data, "columns");
    TEST_ASSERT_EQUAL_UINT(11, json_array_get_length(cols));
    TEST_ASSERT_EQUAL_STRING("insn_pc", json_array_get_string_element(cols, 3));
    TEST_ASSERT_EQUAL_STRING("text", json_array_get_string_element(cols, 9));
    JsonArray *rows = json_object_get_array_member(data, "rows");
    TEST_ASSERT_EQUAL_UINT(5, json_array_get_length(rows));
    JsonArray *r1 = json_array_get_array_element(rows, 1);
    TEST_ASSERT_EQUAL_INT64(100, json_array_get_int_element(r1, 1));     /* t */
    TEST_ASSERT_EQUAL_INT64(1, json_array_get_int_element(r1, 2));       /* frame */
    TEST_ASSERT_EQUAL_STRING("3001", json_array_get_string_element(r1, 3));
    TEST_ASSERT_EQUAL_STRING("S3001", json_array_get_string_element(r1, 4));
    TEST_ASSERT_EQUAL_STRING("3004", json_array_get_string_element(r1, 5));
    TEST_ASSERT_EQUAL_INT64(2, json_array_get_int_element(r1, 6));       /* bp */
    TEST_ASSERT_EQUAL_STRING("odd", json_array_get_string_element(r1, 7));
    TEST_ASSERT_EQUAL_STRING("log", json_array_get_string_element(r1, 8));
    TEST_ASSERT_EQUAL_STRING("v=1", json_array_get_string_element(r1, 9));
    TEST_ASSERT_EQUAL_STRING("", json_array_get_string_element(r1, 10));
    /* neplatné UTF-8 nahrazené U+FFFD, příznak ořezu */
    JsonArray *r2 = json_array_get_array_element(rows, 2);
    TEST_ASSERT_EQUAL_STRING("x\xEF\xBF\xBD", json_array_get_string_element(r2, 9));
    TEST_ASSERT_EQUAL_STRING("text_truncated", json_array_get_string_element(r2, 10));
    TEST_ASSERT_EQUAL_STRING("stop", json_array_get_string_element(json_array_get_array_element(rows, 4), 8));
    g_object_unref(parser);
    free(resp);

    /* filtr podle id, jména, druhu a insn_pc */
    const struct { const char *req; guint n; } cases[] = {
        { "{\"channel\":\"bplog\",\"filter\":{\"bp\":2}}", 2 },
        { "{\"channel\":\"bplog\",\"filter\":{\"bp\":\"even\"}}", 3 },
        { "{\"channel\":\"bplog\",\"filter\":{\"kind\":\"stop\"}}", 1 },
        { "{\"channel\":\"bplog\",\"filter\":{\"bp\":\"even\",\"kind\":\"log\"}}", 2 },
        { "{\"channel\":\"bplog\",\"filter\":{\"pc_from\":12289,\"pc_to\":12290}}", 2 },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_OK, _ss_request("trace_read", cases[i].req, &resp), cases[i].req);
        data = _tr_data(resp, &parser);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(cases[i].n, json_array_get_length(json_object_get_array_member(data, "rows")), cases[i].req);
        g_object_unref(parser);
        free(resp);
    }

    const char *bad[] = {
        "{\"channel\":\"bplog\",\"filter\":{\"kind\":\"mark\"}}",
        "{\"channel\":\"bplog\",\"filter\":{\"bp\":-1}}",
        "{\"channel\":\"bplog\",\"filter\":{\"bp\":\"\"}}",
        "{\"channel\":\"bplog\",\"filter\":{\"bp\":\"0123456789012345678901234567890123\"}}",
        "{\"channel\":\"bplog\",\"filter\":{\"bp\":true}}",
        "{\"channel\":\"iorqlog\",\"filter\":{\"bp\":1}}",
        "{\"channel\":\"iorqlog\",\"filter\":{\"kind\":\"log\"}}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, _ss_request("trace_read", bad[i], NULL), bad[i]);
    }
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("trace_start", "{\"channel\":\"bplog\"}", NULL));

    /* bplog_clear: příkaz CLEAR, odpověď se stavem */
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("bplog_clear", "{}", &resp));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.bpl_calls);
    TEST_ASSERT_EQUAL_INT(DBGAPI_BPLOG_OP_CLEAR, g_stub_state.bpl_last.op);
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(4, json_object_get_int_member(data, "segment"));
    TEST_ASSERT_EQUAL_INT64(BPLOG_CAPACITY, json_object_get_int_member(data, "capacity"));
    TEST_ASSERT_EQUAL_INT64(0, json_object_get_int_member(data, "next_seq"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "allocated"));
    g_object_unref(parser);
    free(resp);
}

/** @brief trace_read: validace parametrů. */
void test_trace_read_invalid_params(void) {
    dispatch_stub_reset();
    g_stub_state.tr_segment = 1;
    g_stub_state.tr_records = 3;
    const char *bad[] = {
        "{}",
        "{\"channel\":\"nope\"}",
        "{\"channel\":\"iorqlog\",\"format\":\"csv\"}",
        "{\"channel\":\"iorqlog\",\"limit\":0}",
        "{\"channel\":\"iorqlog\",\"limit\":5001}",
        "{\"channel\":\"iorqlog\",\"max_chars\":10}",
        "{\"channel\":\"iorqlog\",\"columns\":[\"pc\"]}",
        "{\"channel\":\"iorqlog\",\"filter\":{\"pc_from\":1}}",
        "{\"channel\":\"iorqlog\",\"filter\":{\"dir\":\"up\"}}",
        "{\"channel\":\"iorqlog\",\"from\":\"middle\"}",
        "{\"channel\":\"iorqlog\",\"cursor\":\"t1.###\"}",
        "{\"channel\":\"iorqlog\",\"from\":\"seq:99\"}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, _ss_request("trace_read", bad[i], NULL), bad[i]);
    }
    /* nezaznamenaný kanál: prázdná stránka, ne chyba */
    g_stub_state.tr_segment = 0;
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("trace_read", "{\"channel\":\"marklog\"}", &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(0, json_object_get_int_member(data, "segment"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "end_of_data"));
    TEST_ASSERT_EQUAL_UINT(0, json_array_get_length(json_object_get_array_member(data, "rows")));
    g_object_unref(parser);
    free(resp);
}


/**
 * @brief V1.D.4 - get_video_text_dump available (default v stubu).
 */
void test_get_video_text_dump_available(void) {
    dispatch_stub_reset();
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_video_text_dump", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_EQUAL_INT(40,
        (int)json_object_get_int_member(obj, "cols"));
    TEST_ASSERT_EQUAL_INT(25,
        (int)json_object_get_int_member(obj, "rows"));
    TEST_ASSERT_EQUAL_INT(1000,
        (int)json_object_get_int_member(obj, "cell_count"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "chars_b64"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "attributes_b64"));
    json_node_free(data);
    free(raw);
}


/**
 * @brief V1.D.4 - get_video_text_dump nedostupné (= MZ-800 v 800 mode equiv).
 */
void test_get_video_text_dump_unavailable(void) {
    dispatch_stub_reset();
    g_stub_state.text_dump_available = 0;
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_video_text_dump", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(obj, "available"));
    TEST_ASSERT_TRUE(json_object_has_member(obj, "reason"));
    json_node_free(data);
    free(raw);
}


/* ====================================================================== */
/* V1.D.2.C - per-watch snapshot Resource (`emulator://watch/snapshot/{name}`) */
/* ====================================================================== */


/**
 * @brief V1.D.2.C - get_watch_snapshot s found=1 vrací plný JSON payload.
 *
 * Stub vyplní deterministickými hodnotami, ověřujeme že každé pole
 * mapuje do JSON klíče (snap_int/cur_int/delta_int/min_int/max_int/
 * change_count + flagy + row_id + type string).
 */
void test_get_watch_snapshot_found_payload(void) {
    dispatch_stub_reset();
    g_stub_state.watch_snapshot_fake_found            = 1;
    g_stub_state.watch_snapshot_fake_snapshot_active  = 1;
    g_stub_state.watch_snapshot_fake_min_max_valid    = 1;
    g_stub_state.watch_snapshot_fake_row_id           = 17;
    /* DBGAPI_WATCH_TYPE_U16LE == 2 -> "u16le". */
    g_stub_state.watch_snapshot_fake_type_snap        = 2;
    g_stub_state.watch_snapshot_fake_snap_int         = 0x1000;
    g_stub_state.watch_snapshot_fake_cur_int          = 0x1234;
    g_stub_state.watch_snapshot_fake_delta_int        = 0x0234;
    g_stub_state.watch_snapshot_fake_min_int          = 0x0100;
    g_stub_state.watch_snapshot_fake_max_int          = 0x1234;
    g_stub_state.watch_snapshot_fake_change_count     = 42;

    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":9101,\"cmd\":\"get_watch_snapshot\","
        "\"data\":{\"name\":\"PLAYER_HP\"}}");
    char *resp = NULL;

    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_GET_WATCH_SNAPSHOT,
                          g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.watch_snapshot_calls);
    TEST_ASSERT_NOT_NULL(g_stub_state.watch_snapshot_last_name);
    TEST_ASSERT_EQUAL_STRING("PLAYER_HP",
                             g_stub_state.watch_snapshot_last_name);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_STRING("PLAYER_HP",
        json_object_get_string_member(data, "name"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "found"));
    TEST_ASSERT_EQUAL_INT(17,
        (int)json_object_get_int_member(data, "row_id"));
    TEST_ASSERT_EQUAL_STRING("u16le",
        json_object_get_string_member(data, "type"));
    TEST_ASSERT_TRUE(
        json_object_get_boolean_member(data, "snapshot_active"));
    TEST_ASSERT_TRUE(
        json_object_get_boolean_member(data, "min_max_valid"));
    TEST_ASSERT_EQUAL_INT(0x1000,
        (int)json_object_get_int_member(data, "snap_int"));
    TEST_ASSERT_EQUAL_INT(0x1234,
        (int)json_object_get_int_member(data, "cur_int"));
    TEST_ASSERT_EQUAL_INT(0x0234,
        (int)json_object_get_int_member(data, "delta_int"));
    TEST_ASSERT_EQUAL_INT(0x0100,
        (int)json_object_get_int_member(data, "min_int"));
    TEST_ASSERT_EQUAL_INT(0x1234,
        (int)json_object_get_int_member(data, "max_int"));
    TEST_ASSERT_EQUAL_INT(42,
        (int)json_object_get_int_member(data, "change_count"));
    g_object_unref(parser);

    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief V1.D.2.C - get_watch_snapshot s found=0 (= mirror ho nemá) vrací
 *        kompaktní JSON {name, found=false} bez ostatních polí.
 */
void test_get_watch_snapshot_not_found_payload(void) {
    dispatch_stub_reset();
    /* watch_snapshot_fake_found = 0 -> stub vyplní found=0 v param. */
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":9102,\"cmd\":\"get_watch_snapshot\","
        "\"data\":{\"name\":\"MISSING\"}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);

    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(obj, "success"));
    JsonObject *data = json_object_get_object_member(obj, "data");
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_STRING("MISSING",
        json_object_get_string_member(data, "name"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "found"));
    /* Ostatní fields se NEemitují (= kompaktní payload). */
    TEST_ASSERT_FALSE(json_object_has_member(data, "row_id"));
    TEST_ASSERT_FALSE(json_object_has_member(data, "snap_int"));
    g_object_unref(parser);
    free(resp);
    jsonl_msg_free(req);
}


/**
 * @brief V1.D.2.C - chybějící `name` v requestu => INVALID_PARAMS error.
 */
void test_get_watch_snapshot_missing_name_rejected(void) {
    dispatch_stub_reset();
    st_JSONL_MESSAGE *req = _make_request(
        "{\"type\":\"request\",\"req_id\":9103,\"cmd\":\"get_watch_snapshot\","
        "\"data\":{}}");
    char *resp = NULL;
    en_MCP_DISPATCH_RESULT rc = mcp_dispatch_request(req, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    /* Stub se nedostal k dispatchi (= dispatch handler vrátil err
     * před _submit_dbgapi). */
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.watch_snapshot_calls);
    free(resp);
    jsonl_msg_free(req);
}


void test_get_state_includes_last_user_action_when_present(void) {
    dispatch_stub_reset();
    dispatch_stub_set_last_user_action(true, DBGAPI_CMD_PAUSE, 1234567ull);
    char *raw = NULL;
    JsonNode *data = _v1d1_dispatch_and_get_data("get_state", &raw);
    TEST_ASSERT_NOT_NULL(data);
    JsonObject *obj = json_node_get_object(data);
    TEST_ASSERT_TRUE(json_object_has_member(obj, "last_user_action"));
    JsonNode *lua = json_object_get_member(obj, "last_user_action");
    TEST_ASSERT_EQUAL_INT(JSON_NODE_OBJECT,
                          json_node_get_node_type(lua));
    JsonObject *luao = json_node_get_object(lua);
    TEST_ASSERT_EQUAL_STRING("pause",
        json_object_get_string_member(luao, "kind"));
    TEST_ASSERT_EQUAL_INT(1234567,
        (int)json_object_get_int_member(luao, "timestamp_us"));
    /* Reset stub na false aby další testy nezachytávaly. */
    dispatch_stub_set_last_user_action(false, DBGAPI_CMD_NONE, 0);
    json_node_free(data);
    free(raw);
}


/* ====================================================================== */
/* Main runner                                                             */
/* ====================================================================== */


/**
 * @brief audio_capture_start: výchozí parametry (všechny soubory, bez limitu)
 *        a předání dir, frames a voleb souborů; stop a status.
 */
void test_audio_capture_start_params(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_start", "{}", &resp));
    const st_DBGAPI_AUDIO_CAPTURE_PARAM *p = &g_stub_state.ac_last;
    TEST_ASSERT_EQUAL_INT(DBGAPI_AUDIO_CAPTURE_OP_START, p->op);
    TEST_ASSERT_EQUAL_STRING("", p->params.dir);
    TEST_ASSERT_EQUAL_UINT64(0, p->params.frames);
    TEST_ASSERT_TRUE(p->params.events && p->params.psg_writes && p->params.reference_wav && p->params.sdl_wav);
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"pending\":true"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"sdl_blocks\""));
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_start",
        "{\"dir\":\"C:/tmp/a\",\"frames\":250,\"sdl_wav\":false,\"psg_writes\":false}", NULL));
    TEST_ASSERT_EQUAL_STRING("C:/tmp/a", p->params.dir);
    TEST_ASSERT_EQUAL_UINT64(250, p->params.frames);
    TEST_ASSERT_FALSE(p->params.sdl_wav);
    TEST_ASSERT_FALSE(p->params.psg_writes);
    TEST_ASSERT_TRUE(p->params.events);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_stop", "{}", NULL));
    TEST_ASSERT_EQUAL_INT(DBGAPI_AUDIO_CAPTURE_OP_STOP, p->op);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_status", "{}", &resp));
    TEST_ASSERT_EQUAL_INT(DBGAPI_AUDIO_CAPTURE_OP_STATUS, p->op);
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"frames\":7"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"memory\":null"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"segment\":"));
    free(resp);
}


/** @brief audio_capture_start: paměť přes MCP výchozí zapnutá, pcm, délky ringů. */
void test_audio_capture_start_memory_params(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_start", "{}", NULL));
    const st_DBGAPI_AUDIO_CAPTURE_PARAM *p = &g_stub_state.ac_last;
    TEST_ASSERT_TRUE(p->params.memory);
    TEST_ASSERT_TRUE(p->params.pcm_reference);
    TEST_ASSERT_FALSE(p->params.pcm_sdl);
    TEST_ASSERT_EQUAL_UINT32(AUDIO_CAPTURE_MEMORY_MINUTES_DEFAULT * 60u * VIDEO_SCREENS_PER_SEC, p->params.ring_frames);
    TEST_ASSERT_EQUAL_UINT32(AUDIO_CAPTURE_PCM_SECONDS_DEFAULT * AUDIO_CAPTURE_WAV_RATE, p->params.ring_pcm);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_start",
        "{\"pcm\":[\"sdl\"],\"memory_minutes\":2,\"pcm_seconds\":5}", NULL));
    TEST_ASSERT_FALSE(p->params.pcm_reference);
    TEST_ASSERT_TRUE(p->params.pcm_sdl);
    TEST_ASSERT_EQUAL_UINT32(2u * 60u * VIDEO_SCREENS_PER_SEC, p->params.ring_frames);
    TEST_ASSERT_EQUAL_UINT32(5u * AUDIO_CAPTURE_WAV_RATE, p->params.ring_pcm);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_start", "{\"memory\":false}", NULL));
    TEST_ASSERT_FALSE(p->params.memory);

    int calls = g_stub_state.ac_calls;
    const char *bad[] = { "{\"memory\":1}", "{\"pcm\":\"sdl\"}", "{\"pcm\":[\"wav\"]}",
                          "{\"memory_minutes\":0}", "{\"pcm_seconds\":4000}" };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, _ss_request("audio_capture_start", bad[i], NULL), bad[i]);
    }
    TEST_ASSERT_EQUAL_INT(calls, g_stub_state.ac_calls);
}


/** @brief audio_capture_summary: úseky, kanály, noty PSG, CTC0, reference vs SDL. */
void test_audio_capture_summary(void) {
    dispatch_stub_reset();
    char *resp = NULL;
    /* bez paměti zachycení */
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, _ss_request("audio_capture_summary", "{}", &resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "memory=true"));
    free(resp);

    g_stub_state.aud_segment = 2;
    g_stub_state.aud_records = 10;
    g_stub_state.aud_origin = 1000;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_summary", "{}", &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    JsonArray *fr = json_object_get_array_member(data, "frames");
    TEST_ASSERT_EQUAL_INT64(0, json_array_get_int_element(fr, 0));
    TEST_ASSERT_EQUAL_INT64(9, json_array_get_int_element(fr, 1));
    /* ref RMS 100/200 >= 64 => všechny snímky se zvukem */
    JsonArray *snd = json_object_get_array_member(data, "sound_spans");
    TEST_ASSERT_EQUAL_UINT(1, json_array_get_length(snd));
    TEST_ASSERT_EQUAL_UINT(0, json_array_get_length(json_object_get_array_member(data, "silent_spans")));
    JsonObject *c0 = json_object_get_object_member(json_object_get_object_member(data, "channels"), "ctc0");
    TEST_ASSERT_EQUAL_INT64(45, json_object_get_int_member(c0, "edges"));          /* 0 + 1 + ... + 9 */
    TEST_ASSERT_EQUAL_INT64(9, json_object_get_int_member(c0, "active_frames"));
    /* nota: hlasitost ve snímku 0 (dělič 0 = nehraje), tón 254 od snímku 1 do konce */
    JsonArray *notes = json_object_get_array_member(data, "psg_notes");
    TEST_ASSERT_EQUAL_UINT(1, json_array_get_length(notes));
    JsonObject *n = json_array_get_object_element(notes, 0);
    TEST_ASSERT_EQUAL_STRING("psg0_0", json_object_get_string_member(n, "ch"));
    TEST_ASSERT_EQUAL_INT64(254, json_object_get_int_member(n, "divider"));
    TEST_ASSERT_EQUAL_INT64(1, json_object_get_int_member(n, "from_frame"));
    TEST_ASSERT_EQUAL_INT64(9, json_object_get_int_member(n, "to_frame"));
    TEST_ASSERT_EQUAL_INT64(13, json_object_get_int_member(n, "max_level"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "psg_state_incomplete"));
    JsonObject *ctc = json_object_get_object_member(data, "ctc0");
    TEST_ASSERT_TRUE(json_object_has_member(ctc, "likely"));
    TEST_ASSERT_EQUAL_INT64(2, json_object_get_int_member(ctc, "edges"));          /* levels src 0: i = 0 a 9 */
    JsonObject *rv = json_object_get_object_member(data, "ref_vs_sdl");
    TEST_ASSERT_NOT_NULL(rv);
    TEST_ASSERT_EQUAL_INT64(0, json_object_get_int_member(rv, "lag_samples"));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, json_object_get_double_member(rv, "diff_rms"));
    g_object_unref(parser);
    free(resp);

    /* rozsah snímků */
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_summary", "{\"frame_from\":3,\"frame_to\":4}", &resp));
    data = _tr_data(resp, &parser);
    fr = json_object_get_array_member(data, "frames");
    TEST_ASSERT_EQUAL_INT64(3, json_array_get_int_element(fr, 0));
    TEST_ASSERT_EQUAL_INT64(4, json_array_get_int_element(fr, 1));
    g_object_unref(parser);
    free(resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("audio_capture_summary", "{\"frame_from\":50}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("audio_capture_summary", "{\"silence_rms\":-1}", NULL));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.ac_calls);                                /* bez dbgapi */
}


/** @brief audio_capture_pcm: přesná kopie 44100 stereo, převzorkovaný WAV, chyby. */
void test_audio_capture_pcm(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, _ss_request("audio_capture_pcm", "{}", NULL));
    g_stub_state.aud_segment = 2;
    g_stub_state.aud_records = 10;
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_pcm",
        "{\"sample_from\":2,\"samples\":5,\"rate\":44100,\"stereo\":true,\"encoding\":\"pcm_b64\"}", &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(2, json_object_get_int_member(data, "from_sample"));
    TEST_ASSERT_EQUAL_INT64(5, json_object_get_int_member(data, "samples"));
    TEST_ASSERT_EQUAL_INT64(7, json_object_get_int_member(data, "next_sample"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(data, "end_of_data"));
    gsize len = 0;
    guchar *raw = g_base64_decode(json_object_get_string_member(data, "data_b64"), &len);
    TEST_ASSERT_EQUAL_UINT(5 * 4, len);
    const int16_t *pcm = (const int16_t *) raw;
    TEST_ASSERT_EQUAL_INT16(2, pcm[0]);
    TEST_ASSERT_EQUAL_INT16(-2, pcm[1]);
    TEST_ASSERT_EQUAL_INT16(6, pcm[8]);
    g_free(raw);
    g_object_unref(parser);
    free(resp);

    /* výchozí: WAV mono 11025 Hz od snímku 0, zbytek proudu */
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("audio_capture_pcm", "{\"source\":\"sdl\"}", &resp));
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_STRING("sdl", json_object_get_string_member(data, "source"));
    TEST_ASSERT_EQUAL_INT64(1, json_object_get_int_member(data, "channels"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(data, "end_of_data"));
    raw = g_base64_decode(json_object_get_string_member(data, "data_b64"), &len);
    TEST_ASSERT_EQUAL_MEMORY("RIFF", raw, 4);
    TEST_ASSERT_EQUAL_UINT(44 + 2 * json_object_get_int_member(data, "frames_out"), len);
    g_free(raw);
    g_object_unref(parser);
    free(resp);

    const char *bad[] = { "{\"source\":\"mix\"}", "{\"rate\":500}", "{\"encoding\":\"mp3\"}",
                          "{\"max_bytes\":10}", "{\"frames\":0}", "{\"stereo\":1}" };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, _ss_request("audio_capture_pcm", bad[i], NULL), bad[i]);
    }
}


/**
 * @brief audio_capture_*: neplatné parametry => INVALID_PARAMS bez předání,
 *        chyba jádra => EMU_ERROR s textem.
 */
void test_audio_capture_errors(void) {
    dispatch_stub_reset();
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("audio_capture_start", "{\"frames\":-1}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("audio_capture_start", "{\"frames\":\"x\"}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("audio_capture_start", "{\"sdl_wav\":1}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("audio_capture_start", "{\"dir\":5}", NULL));
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.ac_calls);

    g_stub_state.ac_fake_fail = true;
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, _ss_request("audio_capture_start", "{}", &resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "already running"));
    free(resp);
}


/* ------------------------------------------------------------------ */
/* memsearch (M3 debug-gaps): freeze_* a memsearch_* přes MCP          */
/* ------------------------------------------------------------------ */

/** @brief Celé číslo z datového objektu odpovědi (uvolní odpověď). */
static gint64 _ms_int(char *resp, const char *key) {
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    gint64 v = json_object_get_int_member(data, key);
    g_object_unref(parser);
    free(resp);
    return v;
}

/** @brief freeze_add (value, bytes), freeze_list, freeze_remove (úsek, all) a validace. */
void test_freeze_mcp(void) {
    dispatch_stub_reset();
    g_stub_state.ms_regions = true;
    char *resp = NULL;

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("freeze_add",
        "{\"kind\":\"ram\",\"offset\":4096,\"value\":7}", &resp));
    TEST_ASSERT_EQUAL_INT64(1, _ms_int(resp, "frozen"));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("freeze_add",
        "{\"kind\":\"memext_ram\",\"sub_id\":65,\"offset\":16,\"bytes\":[1,2,3]}", &resp));
    TEST_ASSERT_EQUAL_INT64(3, _ms_int(resp, "added"));
    TEST_ASSERT_EQUAL_UINT32(4, g_stub_state.frz_count);
    TEST_ASSERT_EQUAL_UINT8(3, g_stub_state.frz_items[3].value);
    TEST_ASSERT_EQUAL_INT(0x41, g_stub_state.frz_items[3].sub_id);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("freeze_list", "{}", &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(4, json_object_get_int_member(data, "count"));
    JsonObject *e0 = json_array_get_object_element(json_object_get_array_member(data, "entries"), 0);
    TEST_ASSERT_EQUAL_STRING("ram", json_object_get_string_member(e0, "kind"));
    TEST_ASSERT_EQUAL_INT64(4096, json_object_get_int_member(e0, "offset"));
    TEST_ASSERT_EQUAL_INT64(7, json_object_get_int_member(e0, "value"));
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("freeze_remove",
        "{\"kind\":\"memext_ram\",\"sub_id\":65,\"offset\":16,\"length\":2}", &resp));
    TEST_ASSERT_EQUAL_INT64(2, _ms_int(resp, "removed"));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("freeze_remove", "{\"all\":true}", &resp));
    TEST_ASSERT_EQUAL_INT64(2, _ms_int(resp, "removed"));
    TEST_ASSERT_EQUAL_UINT32(0, g_stub_state.frz_count);

    const char *bad[] = {
        "{\"offset\":1,\"value\":1}",                                   /* chybí kind */
        "{\"kind\":\"rom\",\"offset\":1,\"value\":1}",                  /* neznámý kind */
        "{\"kind\":\"ram\",\"offset\":1}",                              /* chybí value i bytes */
        "{\"kind\":\"ram\",\"offset\":1,\"value\":256}",
        "{\"kind\":\"ram\",\"offset\":1,\"value\":1,\"bytes\":[1]}",
        "{\"kind\":\"ram\",\"offset\":65535,\"bytes\":[1,2]}",          /* za konec regionu */
        "{\"kind\":\"rom_lower\",\"offset\":1,\"value\":1}",            /* jen pro čtení */
        "{\"kind\":\"ramdisk_std\",\"offset\":1,\"value\":1}",          /* v sestavě není */
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, _ss_request("freeze_add", bad[i], NULL), bad[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0, g_stub_state.frz_count);
}

/** @brief Průběh start eq -> filter dec_by -> list -> undo/redo -> close. */
void test_memsearch_mcp_flow(void) {
    dispatch_stub_reset();
    g_stub_state.ms_regions = true;
    g_stub_state.ms_ram[0x20] = 5;
    g_stub_state.ms_ram[0x21] = 5;
    char *resp = NULL;

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_start",
        "{\"space\":\"ram\",\"type\":\"u8\",\"op\":\"eq\",\"value\":5}", &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    gint64 sid = json_object_get_int_member(data, "session_id");
    TEST_ASSERT_TRUE(sid > 0);
    TEST_ASSERT_EQUAL_INT64(2, json_object_get_int_member(data, "candidates"));
    TEST_ASSERT_EQUAL_INT64(65536, json_object_get_int_member(data, "space_size"));
    TEST_ASSERT_EQUAL_INT64(1, json_object_get_int_member(data, "sample_frame"));
    JsonObject *sp0 = json_array_get_object_element(json_object_get_array_member(data, "spans"), 0);
    TEST_ASSERT_EQUAL_STRING("ram", json_object_get_string_member(sp0, "kind"));
    g_object_unref(parser);
    free(resp);

    /* Ztráta života: 5 -> 4 na 20h. */
    g_stub_state.ms_ram[0x20] = 4;
    char req[256];
    g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT ",\"op\":\"dec_by\",\"value\":1}", sid);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_filter", req, &resp));
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(1, json_object_get_int_member(data, "candidates"));
    TEST_ASSERT_EQUAL_INT64(1, json_object_get_int_member(data, "removed"));
    TEST_ASSERT_EQUAL_INT64(2, json_object_get_int_member(data, "step"));
    g_object_unref(parser);
    free(resp);
    TEST_ASSERT_EQUAL_INT(2, g_stub_state.ms_sample_calls);

    g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT "}", sid);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_list", req, &resp));
    data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(1, json_object_get_int_member(data, "total"));
    JsonObject *it = json_array_get_object_element(json_object_get_array_member(data, "items"), 0);
    TEST_ASSERT_EQUAL_INT64(0x20, json_object_get_int_member(it, "region_offset"));
    TEST_ASSERT_EQUAL_INT64(0x20, json_object_get_int_member(it, "z80_addr"));
    TEST_ASSERT_EQUAL_INT64(4, json_object_get_int_member(it, "cur"));
    TEST_ASSERT_EQUAL_INT64(5, json_object_get_int_member(it, "prev"));
    TEST_ASSERT_EQUAL_STRING("04", json_object_get_string_member(it, "cur_hex"));
    JsonObject *hint = json_object_get_object_member(it, "bp_hint");
    TEST_ASSERT_EQUAL_INT64(0x20, json_object_get_int_member(hint, "addr"));
    JsonObject *hv = json_object_get_object_member(hint, "values");
    TEST_ASSERT_EQUAL_STRING("MEM_W", json_object_get_string_member(hv, "type"));
    TEST_ASSERT_EQUAL_STRING("RAM", json_object_get_string_member(hv, "zone"));
    g_object_unref(parser);
    free(resp);

    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_undo", req, &resp));
    TEST_ASSERT_EQUAL_INT64(2, _ms_int(resp, "candidates"));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_redo", req, &resp));
    TEST_ASSERT_EQUAL_INT64(1, _ms_int(resp, "candidates"));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("memsearch_redo", req, NULL));

    /* Vzorek bez filtrace, pak filtr bez nového vzorku (cur proti prev). */
    g_stub_state.ms_ram[0x20] = 3;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_sample", req, &resp));
    TEST_ASSERT_EQUAL_INT64(3, _ms_int(resp, "step"));
    g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT ",\"op\":\"dec\",\"sample\":false}", sid);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_filter", req, &resp));
    TEST_ASSERT_EQUAL_INT64(1, _ms_int(resp, "candidates"));
    TEST_ASSERT_EQUAL_INT(3, g_stub_state.ms_sample_calls);

    g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT ",\"space_offsets\":[32]}", sid);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_eliminate", req, &resp));
    TEST_ASSERT_EQUAL_INT64(0, _ms_int(resp, "candidates"));

    g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT "}", sid);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_close", req, NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("memsearch_list", req, NULL));
}

/** @brief Kandidát v MemExt PEHU: adresa okna a write BP v zóně MMEXT_BANK. */
void test_memsearch_mcp_pehu_hint(void) {
    dispatch_stub_reset();
    g_stub_state.ms_regions = true;
    g_stub_state.ms_pehu = true;
    char *resp = NULL;
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_start",
        "{\"space\":[{\"kind\":\"memext_ram\",\"sub_id\":65,\"offset\":256,\"length\":4}],\"type\":\"u16\"}", &resp));
    gint64 sid = _ms_int(resp, "session_id");
    char req[128];
    g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT ",\"limit\":1}", sid);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_list", req, &resp));
    JsonParser *parser = NULL;
    JsonObject *data = _tr_data(resp, &parser);
    TEST_ASSERT_EQUAL_INT64(3, json_object_get_int_member(data, "total"));   /* u16 ve 4 B */
    JsonObject *it = json_array_get_object_element(json_object_get_array_member(data, "items"), 0);
    TEST_ASSERT_EQUAL_STRING("memext_ram", json_object_get_string_member(it, "kind"));
    TEST_ASSERT_EQUAL_INT64(0x100, json_object_get_int_member(it, "region_offset"));
    TEST_ASSERT_EQUAL_INT64(0x4100, json_object_get_int_member(it, "z80_addr"));   /* slot 4 = kus 41h */
    JsonObject *hint = json_object_get_object_member(it, "bp_hint");
    TEST_ASSERT_EQUAL_INT64(0x1100, json_object_get_int_member(hint, "addr"));     /* horní půlka banky */
    JsonObject *hv = json_object_get_object_member(hint, "values");
    TEST_ASSERT_EQUAL_STRING("MMEXT_BANK", json_object_get_string_member(hv, "zone"));
    TEST_ASSERT_EQUAL_INT64(0x20, json_object_get_int_member(hv, "bank_id"));
    TEST_ASSERT_EQUAL_STRING("bank_offset", json_object_get_string_member(hv, "bp_addr_space"));
    TEST_ASSERT_EQUAL_INT64(0x1101, json_object_get_int_member(hv, "addr_end"));
    TEST_ASSERT_EQUAL_STRING("RANGE", json_object_get_string_member(hv, "addr_match_mode"));
    g_object_unref(parser);
    free(resp);
    g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT "}", sid);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_close", req, NULL));
}

/** @brief Neplatné parametry, neznámé sezení a strop počtu sezení. */
void test_memsearch_mcp_errors(void) {
    dispatch_stub_reset();
    g_stub_state.ms_regions = true;
    const char *bad_start[] = {
        "{\"type\":\"u64\"}",
        "{\"type\":\"u16\",\"width\":2}",
        "{\"type\":\"bcd\",\"width\":5}",
        "{\"type\":\"digits\",\"digit_encoding\":\"ebcdic\"}",
        "{\"endian\":\"middle\"}",
        "{\"space\":\"rom\"}",
        "{\"space\":\"ramdisk\"}",                                  /* v sestavě není */
        "{\"space\":[{\"kind\":\"ram\",\"offset\":65536}]}",
        "{\"op\":\"dec\"}",                                          /* první filtr jen s hodnotou */
        "{\"op\":\"eq\"}",                                           /* chybí value */
        "{\"op\":\"between\",\"value\":5}",
        "{\"op\":\"teleport\",\"value\":1}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad_start); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS,
                                      _ss_request("memsearch_start", bad_start[i], NULL), bad_start[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.ms_sample_calls);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("memsearch_filter",
        "{\"session_id\":999,\"op\":\"eq\",\"value\":1}", NULL));
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("memsearch_undo", "{}", NULL));

    /* 8 sezení projde, deváté ne; filtr bez op selže. */
    gint64 ids[8];
    char *resp = NULL;
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_start", "{}", &resp));
        ids[i] = _ms_int(resp, "session_id");
    }
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, _ss_request("memsearch_start", "{}", NULL));
    char req[96];
    g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT "}", ids[0]);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("memsearch_filter", req, NULL));
    g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT ",\"op\":\"inc\",\"compare_to\":\"value\",\"value\":1}", ids[0]);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, _ss_request("memsearch_filter", req, NULL));
    for (int i = 0; i < 8; i++) {
        g_snprintf(req, sizeof(req), "{\"session_id\":%" G_GINT64_FORMAT "}", ids[i]);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, _ss_request("memsearch_close", req, NULL));
    }
}



/* ====================================================================== */
/* run-control (M5): wait_stop, historie, deadline, pause(if_run_seq), kroky */
/* ====================================================================== */

#include "../../src/emulator/debugger/run_control/run_log.h"

/**
 * @brief Pošle požadavek a vrátí objekt `data` odpovědi (parser uvolní volající).
 */
static JsonObject *_rc_call(const char *json, en_MCP_DISPATCH_RESULT *rc_out,
                            JsonParser **parser, char **resp_out) {
    st_JSONL_MESSAGE *req = _make_request(json);
    char *resp = NULL;
    *rc_out = mcp_dispatch_request(req, &resp);
    jsonl_msg_free(req);
    *resp_out = resp;
    if (*rc_out != MCP_DISPATCH_OK) return NULL;
    JsonObject *obj = _parse_response_object(resp, parser);
    return json_object_get_object_member(obj, "data");
}


/** @brief Ukončí aktuální běh v run_log záznamem s důvodem a PC. */
static void _rc_end_run(const char *reason, uint16_t pc) {
    st_RUN_LOG_RECORD r;
    run_log_record_clear(&r);
    r.reason = reason;
    r.started_by = "mcp";
    r.start_cmd = "run_to";
    r.stopped_by = "emulator";
    r.pc = pc;
    r.bp_id = 7;
    run_log_end(&r);
}


/**
 * @brief wait_stop: skončený běh vrátí hned se záznamem, běžící po timeoutu.
 */
void test_wait_stop_done_and_timeout(void) {
    dispatch_stub_reset();
    run_log_reset_for_tests();
    run_log_begin();                       /* běh 1 */
    _rc_end_run("breakpoint", 0x1234);

    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9001,\"cmd\":\"wait_stop\","
                             "\"data\":{\"run_seq\":1,\"timeout_ms\":0}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "done"));
    JsonObject *rec = json_object_get_object_member(d, "record");
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_STRING("breakpoint", json_object_get_string_member(rec, "reason"));
    TEST_ASSERT_EQUAL_INT(0x1234, json_object_get_int_member(rec, "pc"));
    TEST_ASSERT_EQUAL_INT(7, json_object_get_int_member(rec, "bp_id"));
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(rec, "run_seq"));
    /* wait_stop je lokální, frontu dbgapi nevolá. */
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
    g_object_unref(parser);
    free(resp);

    run_log_begin();                       /* běh 2 běží */
    parser = NULL;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9002,\"cmd\":\"wait_stop\","
                 "\"data\":{\"timeout_ms\":20}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "done"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "running"));
    TEST_ASSERT_EQUAL_INT(2, json_object_get_int_member(d, "run_seq"));
    TEST_ASSERT_FALSE(json_object_has_member(d, "record"));
    g_object_unref(parser);
    free(resp);

    /* Neplatný timeout. */
    d = _rc_call("{\"type\":\"request\",\"req_id\":9003,\"cmd\":\"wait_stop\","
                 "\"data\":{\"timeout_ms\":30000}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    run_log_reset_for_tests();
}


/**
 * @brief get_stop_history: limit a pořadí od nejstaršího.
 */
void test_get_stop_history_limit(void) {
    dispatch_stub_reset();
    run_log_reset_for_tests();
    _rc_end_run("manual", 0x0100);         /* běh 0 */
    run_log_begin();
    _rc_end_run("step", 0x0101);           /* běh 1 */
    run_log_begin();
    _rc_end_run("deadline", 0x0102);       /* běh 2 */

    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9010,\"cmd\":\"get_stop_history\","
                             "\"data\":{\"limit\":2}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(2, json_object_get_int_member(d, "count"));
    JsonArray *arr = json_object_get_array_member(d, "records");
    TEST_ASSERT_EQUAL_UINT(2, json_array_get_length(arr));
    TEST_ASSERT_EQUAL_STRING("step", json_object_get_string_member(
        json_array_get_object_element(arr, 0), "reason"));
    TEST_ASSERT_EQUAL_STRING("deadline", json_object_get_string_member(
        json_array_get_object_element(arr, 1), "reason"));
    g_object_unref(parser);
    free(resp);

    d = _rc_call("{\"type\":\"request\",\"req_id\":9011,\"cmd\":\"get_stop_history\","
                 "\"data\":{\"limit\":0}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    run_log_reset_for_tests();
}


/**
 * @brief pause(if_run_seq): jiný běh se nezastaví, stejný ano.
 */
void test_pause_if_run_seq(void) {
    dispatch_stub_reset();
    g_stub_state.is_running = true;
    g_stub_state.run_seq = 5;

    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9020,\"cmd\":\"pause\","
                             "\"data\":{\"if_run_seq\":4}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_TRUE(g_stub_state.pause_last.has_if_run_seq);
    TEST_ASSERT_EQUAL_UINT64(4, g_stub_state.pause_last.if_run_seq);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "paused"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "paused_now"));
    TEST_ASSERT_EQUAL_STRING("run_seq_mismatch", json_object_get_string_member(d, "skipped"));
    g_object_unref(parser);
    free(resp);

    parser = NULL;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9021,\"cmd\":\"pause\","
                 "\"data\":{\"if_run_seq\":5}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "paused"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "paused_now"));
    TEST_ASSERT_FALSE(json_object_has_member(d, "skipped"));
    g_object_unref(parser);
    free(resp);

    /* Bez if_run_seq: pauza platí vždy, odpověď paused true. */
    parser = NULL;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9022,\"cmd\":\"pause\"}",
                 &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_FALSE(g_stub_state.pause_last.has_if_run_seq);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "paused"));
    g_object_unref(parser);
    free(resp);
}


/**
 * @brief deadline u run_until_addr: nastaví se před RUN_TO, při
 *        PAUSED_ONLY se zruší; neplatný tvar je chyba bez volání.
 */
void test_run_until_addr_deadline(void) {
    dispatch_stub_reset();
    g_stub_state.run_seq = 3;

    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9030,\"cmd\":\"run_until_addr\","
                             "\"data\":{\"addr\":512,\"deadline\":{\"frames\":5}}}",
                             &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.deadline_arms);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.deadline_last.kind);   /* RUN_DEADLINE_FRAMES */
    TEST_ASSERT_EQUAL_UINT64(5, g_stub_state.deadline_last.value);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_RUN_TO, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(50, json_object_get_int_member(d, "deadline_ticks"));
    TEST_ASSERT_EQUAL_INT(3, json_object_get_int_member(d, "run_seq"));
    g_object_unref(parser);
    free(resp);

    /* Emulace běžela: RUN_TO jen pauzne, deadline se zruší. */
    dispatch_stub_reset();
    g_stub_state.run_to_result = DBGAPI_RUN_TO_PAUSED_ONLY;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9031,\"cmd\":\"run_until_addr\","
                 "\"data\":{\"addr\":512,\"deadline\":{\"emulated_ms\":100}}}",
                 &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.deadline_arms);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.deadline_clears);
    free(resp);

    /* Dva klíče = chyba parametrů, nic se nevolá. */
    dispatch_stub_reset();
    d = _rc_call("{\"type\":\"request\",\"req_id\":9032,\"cmd\":\"run_until_addr\","
                 "\"data\":{\"addr\":512,\"deadline\":{\"frames\":5,\"ticks\":9}}}",
                 &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
    free(resp);

    /* run s frames i deadline = chyba parametrů. */
    dispatch_stub_reset();
    d = _rc_call("{\"type\":\"request\",\"req_id\":9033,\"cmd\":\"run\","
                 "\"data\":{\"frames\":10,\"deadline\":{\"frames\":5}}}",
                 &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
    free(resp);
    (void)d;
}


/**
 * @brief Kroky (N4): step_over přes CALL a step_into za běhu nejsou dokončené.
 */
void test_step_completed_flags(void) {
    dispatch_stub_reset();
    g_stub_state.step_over_runs = true;
    g_stub_state.run_seq = 9;

    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9040,\"cmd\":\"step_over\"}",
                             &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    /* Běh k dočasnému BP: bez čekání na krok. */
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_STEP_OVER, g_stub_state.last_cmd);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "stepped"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "completed"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "running"));
    TEST_ASSERT_EQUAL_INT(9, json_object_get_int_member(d, "run_seq"));
    g_object_unref(parser);
    free(resp);

    dispatch_stub_reset();
    g_stub_state.is_running = true;
    parser = NULL;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9041,\"cmd\":\"step_into\"}",
                 &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_STEP_INTO, g_stub_state.last_cmd);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "completed"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "running"));
    g_object_unref(parser);
    free(resp);

    /* step_over za běhu: emulaci jen pauzne. */
    dispatch_stub_reset();
    g_stub_state.is_running = true;
    parser = NULL;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9042,\"cmd\":\"step_over\"}",
                 &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "completed"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "running"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "paused_only"));
    g_object_unref(parser);
    free(resp);
}



/**
 * @brief mem_read: sha256 přečtených bajtů (proti GLib), want_data false bez dat.
 */
void test_mem_read_sha256_and_want_data(void) {
    dispatch_stub_reset();
    /* Stub plní bajty 0xA0 + (i & 0x0F). */
    uint8_t expect[20];
    for (int i = 0; i < 20; i++) expect[i] = (uint8_t)(0xA0 + (i & 0x0F));
    gchar *want = g_compute_checksum_for_data(G_CHECKSUM_SHA256, expect, sizeof(expect));

    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9050,\"cmd\":\"mem_read\","
                             "\"data\":{\"addr\":4096,\"len\":20}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_STRING(want, json_object_get_string_member(d, "sha256"));
    TEST_ASSERT_TRUE(json_object_has_member(d, "data_b64"));
    g_object_unref(parser);
    free(resp);

    parser = NULL;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9051,\"cmd\":\"mem_read\","
                 "\"data\":{\"addr\":4096,\"len\":20,\"want_data\":false}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_STRING(want, json_object_get_string_member(d, "sha256"));
    TEST_ASSERT_FALSE(json_object_has_member(d, "data_b64"));
    g_object_unref(parser);
    free(resp);

    d = _rc_call("{\"type\":\"request\",\"req_id\":9052,\"cmd\":\"mem_read\","
                 "\"data\":{\"addr\":4096,\"len\":20,\"want_data\":1}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    g_free(want);
}


/**
 * @brief mem_read s celými 64 KiB (addr 0, len 65536) přečte celý blok.
 *
 * Dřív handler odmítal len > 65535. SHA-256 a dekódovaný `data_b64`
 * dokazují, že jádro (stub) vyplnilo všech 65536 bajtů. Blok přesahující
 * 0x10000 (addr 1 + 65536, addr 0 + 65537) se odmítne bez volání jádra.
 */
void test_mem_read_full_64k(void) {
    dispatch_stub_reset();
    /* Stub plní bajty 0xA0 + (i & 0x0F). */
    uint8_t *expect = g_malloc(0x10000);
    for (int i = 0; i < 0x10000; i++) expect[i] = (uint8_t)(0xA0 + (i & 0x0F));
    gchar *want = g_compute_checksum_for_data(G_CHECKSUM_SHA256, expect, 0x10000);

    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9053,\"cmd\":\"mem_read\","
                             "\"data\":{\"addr\":0,\"len\":65536}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(65536, json_object_get_int_member(d, "len"));
    TEST_ASSERT_EQUAL_STRING(want, json_object_get_string_member(d, "sha256"));
    gsize got_len = 0;
    guchar *got = g_base64_decode(json_object_get_string_member(d, "data_b64"), &got_len);
    TEST_ASSERT_EQUAL_UINT32(0x10000, (uint32_t)got_len);
    TEST_ASSERT_EQUAL_MEMORY(expect, got, 0x10000);
    g_free(got);
    g_object_unref(parser);
    free(resp);

    static const char *const bad[] = {
        "{\"type\":\"request\",\"req_id\":9054,\"cmd\":\"mem_read\","
        "\"data\":{\"addr\":1,\"len\":65536}}",
        "{\"type\":\"request\",\"req_id\":9055,\"cmd\":\"mem_read\","
        "\"data\":{\"addr\":0,\"len\":65537}}",
    };
    for (size_t k = 0; k < G_N_ELEMENTS(bad); k++) {
        dispatch_stub_reset();
        _rc_call(bad[k], &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, rc, bad[k]);
        TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
        free(resp);
    }
    g_free(want);
    g_free(expect);
}


/**
 * @brief frame_hash: příkaz se předá emu vláknu (DBGAPI_CMD_FRAME_HASH).
 */
void test_frame_hash_submits(void) {
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9060,\"cmd\":\"frame_hash\"}",
                             &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_FRAME_HASH, g_stub_state.last_cmd);
    TEST_ASSERT_TRUE(json_object_has_member(d, "vram_sha256"));
    TEST_ASSERT_TRUE(json_object_has_member(d, "beam"));
    g_object_unref(parser);
    free(resp);
}



/**
 * @brief replay_record_start: povinná cesta, rozsah hash_interval, předání požadavku.
 */
void test_replay_record_start_params(void) {
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9100,\"cmd\":\"replay_record_start\",\"data\":{}}",
                   &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);

    (void)_rc_call("{\"type\":\"request\",\"req_id\":9101,\"cmd\":\"replay_record_start\","
                   "\"data\":{\"path\":\"a.mzr\",\"hash_interval\":0}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_replay.calls);
    free(resp);

    g_stub_replay.status.mode = REPLAY_MODE_RECORDING;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9102,\"cmd\":\"replay_record_start\","
                             "\"data\":{\"path\":\"a.mzr\",\"hash_interval\":7}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_replay.last_kind);
    TEST_ASSERT_EQUAL_STRING("a.mzr", g_stub_replay.last_path);
    TEST_ASSERT_EQUAL_UINT32(7, g_stub_replay.last_interval);
    TEST_ASSERT_EQUAL_STRING("recording", json_object_get_string_member(d, "mode"));
    g_object_unref(parser);
    free(resp);

    /* Chyba vyřízení z emu vlákna se vrátí jako chyba s textem. */
    g_stub_replay.fail = true;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9103,\"cmd\":\"replay_record_start\","
                   "\"data\":{\"path\":\"b.mzr\"}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "stub failure"));
    free(resp);
}


/**
 * @brief replay_play: bool pause_after_load; stop jen ve správném režimu.
 */
void test_replay_play_and_stop_modes(void) {
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9110,\"cmd\":\"replay_play\","
                   "\"data\":{\"path\":\"a.mzr\",\"pause_after_load\":1}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);

    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9111,\"cmd\":\"replay_play\","
                             "\"data\":{\"path\":\"a.mzr\",\"pause_after_load\":true}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(2, g_stub_replay.last_kind);
    TEST_ASSERT_TRUE(g_stub_replay.last_pause_after_load);
    g_object_unref(parser);
    free(resp);

    /* Nic neběží: oba stopy chyba bez požadavku. */
    int calls = g_stub_replay.calls;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9112,\"cmd\":\"replay_play_stop\"}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "no playback is running"));
    free(resp);
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9113,\"cmd\":\"replay_record_stop\"}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_EQUAL_INT(calls, g_stub_replay.calls);
    free(resp);

    g_stub_replay.status.mode = REPLAY_MODE_PLAYING;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9114,\"cmd\":\"replay_play_stop\"}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(3, g_stub_replay.last_kind);
    g_object_unref(parser);
    free(resp);
}


/**
 * @brief replay_status: pole stavu včetně rozjetých částí jako pole.
 */
void test_replay_status_fields(void) {
    dispatch_stub_reset();
    g_stub_replay.status.mode = REPLAY_MODE_OFF;
    g_stub_replay.status.outcome = REPLAY_OUTCOME_DESYNC;
    g_stub_replay.status.outcome_frame = 120;
    g_strlcpy(g_stub_replay.status.desync_parts, "cpu,ram", sizeof(g_stub_replay.status.desync_parts));
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9120,\"cmd\":\"replay_status\"}",
                             &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_STRING("off", json_object_get_string_member(d, "mode"));
    TEST_ASSERT_EQUAL_STRING("desync", json_object_get_string_member(d, "outcome"));
    TEST_ASSERT_EQUAL_INT(120, json_object_get_int_member(d, "outcome_frame"));
    JsonArray *parts = json_object_get_array_member(d, "desync_parts");
    TEST_ASSERT_EQUAL_UINT(2, json_array_get_length(parts));
    TEST_ASSERT_EQUAL_STRING("ram", json_array_get_string_element(parts, 1));
    TEST_ASSERT_EQUAL_INT(0, g_stub_replay.calls);
    JsonObject *ck = json_object_get_object_member(d, "checkpoints");
    TEST_ASSERT_NOT_NULL(ck);
    TEST_ASSERT_FALSE(json_object_has_member(ck, "first_frame"));
    TEST_ASSERT_TRUE(json_object_has_member(d, "rerecord_count"));
    TEST_ASSERT_TRUE(json_object_has_member(d, "seeking"));
    g_object_unref(parser);
    free(resp);
}


/**
 * @brief replay_rewind / replay_seek a parametry checkpointů u startu.
 */
void test_replay_rewind_seek_params(void) {
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;

    /* Checkpointy u záznamu: předají se, mimo rozsah = chyba bez požadavku. */
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9130,\"cmd\":\"replay_record_start\","
                   "\"data\":{\"path\":\"a.mzr\",\"checkpoint_interval\":0,\"checkpoint_count\":5}}",
                   &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_replay.last_ckpt.interval);
    TEST_ASSERT_EQUAL_INT(5, g_stub_replay.last_ckpt.count);
    g_object_unref(parser);
    free(resp);
    int calls = g_stub_replay.calls;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9131,\"cmd\":\"replay_play\","
                   "\"data\":{\"path\":\"a.mzr\",\"checkpoint_count\":0}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(calls, g_stub_replay.calls);
    free(resp);
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9132,\"cmd\":\"replay_play\","
                   "\"data\":{\"path\":\"a.mzr\"}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(-1, g_stub_replay.last_ckpt.interval);
    g_object_unref(parser);
    free(resp);

    /* Rewind: výchozí 0, záporné = chyba. */
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9133,\"cmd\":\"replay_rewind\"}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(4, g_stub_replay.last_kind);
    TEST_ASSERT_EQUAL_UINT32(0, g_stub_replay.last_frames);
    g_object_unref(parser);
    free(resp);
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9134,\"cmd\":\"replay_rewind\","
                   "\"data\":{\"frames\":-1}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);

    /* Seek: frame povinný; chyba z emu vlákna s textem. */
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9135,\"cmd\":\"replay_seek\"}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9136,\"cmd\":\"replay_seek\","
                   "\"data\":{\"frame\":1200}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(5, g_stub_replay.last_kind);
    TEST_ASSERT_EQUAL_UINT32(1200, g_stub_replay.last_frames);
    g_object_unref(parser);
    free(resp);
    g_stub_replay.fail = true;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9137,\"cmd\":\"replay_seek\","
                   "\"data\":{\"frame\":5}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "stub failure"));
    free(resp);
}


/**
 * @brief replay_goto: adresy a volby se předají požadavku, neplatné
 *        parametry jsou chyba bez požadavku, chyba emu vlákna s textem.
 */
void test_replay_goto_params(void) {
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;

    /* t + volby; čekání 0 (stub běh nespouští, cíl se bere jako dosažený). */
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9140,\"cmd\":\"replay_goto\","
                   "\"data\":{\"t\":123456789,\"breakpoints\":\"keep\",\"checkpoint\":true,"
                   "\"timeout_ms\":0}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(6, g_stub_replay.last_kind);
    TEST_ASSERT_EQUAL_UINT64(123456789ull, g_stub_replay.last_goto_ticks);
    TEST_ASSERT_TRUE(g_stub_replay.last_goto_keep);
    TEST_ASSERT_TRUE(g_stub_replay.last_goto_ckpt);
    g_object_unref(parser);
    free(resp);

    /* frame + frame_ticks = frame * takty snímku + frame_ticks. */
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9141,\"cmd\":\"replay_goto\","
                   "\"data\":{\"frame\":3,\"frame_ticks\":100}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_UINT64(3ull * VIDEO_SCREEN_TICKS + 100u, g_stub_replay.last_goto_ticks);
    TEST_ASSERT_FALSE(g_stub_replay.last_goto_keep);
    TEST_ASSERT_FALSE(g_stub_replay.last_goto_ckpt);
    g_object_unref(parser);
    free(resp);

    /* Neplatné: žádná / dvě adresy, breakpoints, timeout, frame_ticks mimo snímek. */
    static const char *const bad[] = {
        "{\"type\":\"request\",\"req_id\":9142,\"cmd\":\"replay_goto\",\"data\":{}}",
        "{\"type\":\"request\",\"req_id\":9143,\"cmd\":\"replay_goto\",\"data\":{\"t\":1,\"frame\":1}}",
        "{\"type\":\"request\",\"req_id\":9144,\"cmd\":\"replay_goto\",\"data\":{\"t\":1,\"breakpoints\":\"off\"}}",
        "{\"type\":\"request\",\"req_id\":9145,\"cmd\":\"replay_goto\",\"data\":{\"t\":1,\"timeout_ms\":30000}}",
        "{\"type\":\"request\",\"req_id\":9146,\"cmd\":\"replay_goto\",\"data\":{\"frame\":1,\"frame_ticks\":-1}}",
        "{\"type\":\"request\",\"req_id\":9147,\"cmd\":\"replay_goto\",\"data\":{\"t\":-5}}",
    };
    int calls = g_stub_replay.calls;
    for (unsigned i = 0; i < G_N_ELEMENTS(bad); i++) {
        (void)_rc_call(bad[i], &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
        free(resp);
    }
    TEST_ASSERT_EQUAL_INT(calls, g_stub_replay.calls);

    /* Chyba emu vlákna. */
    g_stub_replay.fail = true;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9148,\"cmd\":\"replay_goto\","
                   "\"data\":{\"t\":5}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "stub failure"));
    free(resp);
}


/**
 * @brief Revize stavu (state-rev M18): get_state a zápis vracejí state_rev,
 *        zápis se starou expected_state_rev je Precondition failed a nic
 *        neprovede, čtecí nástroj podmínku odmítne, state_observe
 *        s if_state_rev_differs při shodě vrátí not_modified.
 */
void test_state_rev_fields_and_conditions(void) {
    dispatch_stub_reset();
    g_stub_state.state_rev = 5000;
    g_stub_state.is_running = false;
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;

    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9300,\"cmd\":\"get_state\"}",
                             &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT64(5000, json_object_get_int_member(d, "state_rev"));
    g_object_unref(parser);
    free(resp);

    d = _rc_call("{\"type\":\"request\",\"req_id\":9301,\"cmd\":\"mem_write\","
                 "\"data\":{\"addr\":16384,\"data_hex\":\"AA\",\"expected_state_rev\":5000}}",
                 &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT64(5001, json_object_get_int_member(d, "state_rev"));
    g_object_unref(parser);
    free(resp);

    int calls = g_stub_state.call_count;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9302,\"cmd\":\"mem_write\","
                   "\"data\":{\"addr\":16384,\"data_hex\":\"BB\",\"expected_state_rev\":5000}}",
                   &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "Precondition failed: state changed (expected_state_rev=5000, current_state_rev=5001)"));
    TEST_ASSERT_EQUAL_INT(calls, g_stub_state.call_count);
    free(resp);

    g_stub_state.is_running = true;
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9303,\"cmd\":\"mem_write\","
                   "\"data\":{\"addr\":16384,\"data_hex\":\"BB\",\"expected_state_rev\":5001}}",
                   &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "Precondition failed: emulator running"));
    free(resp);
    g_stub_state.is_running = false;

    (void)_rc_call("{\"type\":\"request\",\"req_id\":9304,\"cmd\":\"get_registers\","
                   "\"data\":{\"expected_state_rev\":5001}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    free(resp);

    d = _rc_call("{\"type\":\"request\",\"req_id\":9305,\"cmd\":\"state_observe\","
                 "\"data\":{\"windows\":[],\"if_state_rev_differs\":5001}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "not_modified"));
    TEST_ASSERT_EQUAL_INT64(5001, json_object_get_int_member(d, "state_rev"));
    g_object_unref(parser);
    free(resp);

    d = _rc_call("{\"type\":\"request\",\"req_id\":9306,\"cmd\":\"state_observe\","
                 "\"data\":{\"windows\":[],\"if_state_rev_differs\":4000}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_FALSE(json_object_has_member(d, "not_modified"));
    TEST_ASSERT_EQUAL_INT64(5001, json_object_get_int_member(d, "state_rev"));
    TEST_ASSERT_TRUE(json_object_has_member(d, "regs"));
    g_object_unref(parser);
    free(resp);
}


/* ====================================================================== */
/* state_observe (observe-cas, M10)                                        */
/* ====================================================================== */

/**
 * @brief state_observe: okna obou forem, echo, hash = SHA-256 dat, registry, čas.
 */
void test_state_observe_windows_and_core(void) {
    dispatch_stub_reset();
    g_stub_state.is_running = false;
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call(
        "{\"type\":\"request\",\"req_id\":9200,\"cmd\":\"state_observe\",\"data\":{"
        "\"windows\":[{\"name\":\"ptr\",\"addr\":4096,\"len\":8},"
        "{\"region\":{\"kind\":\"ram\",\"sub_id\":0},\"offset\":16,\"length\":4,"
        "\"want_data\":false}]}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_STATE_OBSERVE, g_stub_state.last_cmd);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_ORIGIN_MCP, g_stub_state.last_origin);
    TEST_ASSERT_EQUAL_UINT32(2, g_stub_state.obs_last.window_count);
    TEST_ASSERT_EQUAL_UINT8(0, g_stub_state.obs_last_win[0].by_region);
    TEST_ASSERT_EQUAL_UINT32(4096, g_stub_state.obs_last_win[0].start);
    TEST_ASSERT_EQUAL_UINT8(1, g_stub_state.obs_last_win[1].by_region);
    TEST_ASSERT_EQUAL_INT(REGION_KIND_RAM, g_stub_state.obs_last_win[1].region_kind);
    TEST_ASSERT_EQUAL_UINT32(0, g_stub_state.obs_last.which);
    TEST_ASSERT_EQUAL_UINT32(0, g_stub_state.obs_last.hash_mask);

    TEST_ASSERT_EQUAL_INT(7, json_object_get_int_member(d, "run_seq"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "running"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "stable"));
    TEST_ASSERT_EQUAL_STRING("paused", json_object_get_string_member(d, "drain"));
    JsonObject *tm = json_object_get_object_member(d, "time");
    TEST_ASSERT_EQUAL_INT(42, json_object_get_int_member(tm, "frame"));
    TEST_ASSERT_EQUAL_INT(26, json_object_get_int_member(tm, "frame_ticks"));
    TEST_ASSERT_EQUAL_INT64(42LL * 354432 + 26, json_object_get_int_member(tm, "ticks"));
    TEST_ASSERT_EQUAL_INT(123456, json_object_get_int_member(tm, "total_cycles"));
    JsonObject *regs = json_object_get_object_member(d, "regs");
    TEST_ASSERT_EQUAL_INT(0x1B00, json_object_get_int_member(regs, "PC"));
    TEST_ASSERT_TRUE(json_object_has_member(d, "flags"));
    TEST_ASSERT_FALSE(json_object_has_member(d, "raster"));
    TEST_ASSERT_FALSE(json_object_has_member(d, "hashes"));
    TEST_ASSERT_FALSE(json_object_has_member(d, "memory_map"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "complete"));

    JsonArray *wa = json_object_get_array_member(d, "windows");
    TEST_ASSERT_EQUAL_UINT(2, json_array_get_length(wa));
    JsonObject *w0 = json_array_get_object_element(wa, 0);
    uint8_t cpu_bytes[8];
    for (int i = 0; i < 8; i++) cpu_bytes[i] = (uint8_t)(4096 + i);
    gchar *h0 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, cpu_bytes, sizeof(cpu_bytes));
    TEST_ASSERT_EQUAL_STRING("ptr", json_object_get_string_member(w0, "name"));
    TEST_ASSERT_EQUAL_INT(4096, json_object_get_int_member(w0, "addr"));
    TEST_ASSERT_EQUAL_INT(8, json_object_get_int_member(w0, "len"));
    TEST_ASSERT_EQUAL_STRING(h0, json_object_get_string_member(w0, "sha256"));
    TEST_ASSERT_TRUE(json_object_has_member(w0, "data_b64"));
    JsonObject *w1 = json_array_get_object_element(wa, 1);
    uint8_t reg_bytes[4];
    for (int i = 0; i < 4; i++) reg_bytes[i] = (uint8_t)(0x5A ^ (16 + i));
    gchar *h1 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reg_bytes, sizeof(reg_bytes));
    JsonObject *r1 = json_object_get_object_member(w1, "region");
    TEST_ASSERT_EQUAL_STRING("ram", json_object_get_string_member(r1, "kind"));
    TEST_ASSERT_EQUAL_INT(0, json_object_get_int_member(r1, "sub_id"));
    TEST_ASSERT_EQUAL_INT(16, json_object_get_int_member(w1, "offset"));
    TEST_ASSERT_EQUAL_INT(4, json_object_get_int_member(w1, "length"));
    TEST_ASSERT_EQUAL_STRING(h1, json_object_get_string_member(w1, "sha256"));
    TEST_ASSERT_FALSE(json_object_has_member(w1, "data_b64"));
    TEST_ASSERT_FALSE(json_object_has_member(w1, "name"));
    g_free(h0);
    g_free(h1);
    g_object_unref(parser);
    free(resp);

    /* Za běhu: running, stable false, drain frame; prázdná okna. */
    g_stub_state.is_running = true;
    parser = NULL;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9201,\"cmd\":\"state_observe\","
                 "\"data\":{\"windows\":[]}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "running"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "stable"));
    TEST_ASSERT_EQUAL_STRING("frame", json_object_get_string_member(d, "drain"));
    TEST_ASSERT_EQUAL_UINT(0, json_array_get_length(json_object_get_array_member(d, "windows")));
    g_object_unref(parser);
    free(resp);
}


/**
 * @brief state_observe: sekce include, hashe částí, nedostupné okno.
 */
void test_state_observe_sections_hashes_unavailable(void) {
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call(
        "{\"type\":\"request\",\"req_id\":9210,\"cmd\":\"state_observe\",\"data\":{"
        "\"windows\":[{\"name\":\"gone\",\"region\":{\"kind\":\"ramdisk_pezik\",\"sub_id\":3},"
        "\"offset\":0,\"length\":16}],"
        "\"include\":[\"raster\",\"memory_map\"],\"hashes\":[\"cpu\",\"ram\",\"memext\"]}}",
        &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(DBGAPI_CPU_PANEL_WANT_RASTER, g_stub_state.obs_last.which);
    TEST_ASSERT_EQUAL_UINT8(1, g_stub_state.obs_last.want_memory_map);
    TEST_ASSERT_EQUAL_UINT32((1u << STATE_HASH_CPU) | (1u << STATE_HASH_RAM)
                             | (1u << STATE_HASH_MEMEXT), g_stub_state.obs_last.hash_mask);

    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "complete"));
    JsonObject *w = json_array_get_object_element(json_object_get_array_member(d, "windows"), 0);
    TEST_ASSERT_EQUAL_STRING("region_unavailable", json_object_get_string_member(w, "error"));
    TEST_ASSERT_EQUAL_STRING("gone", json_object_get_string_member(w, "name"));
    TEST_ASSERT_EQUAL_INT(0, json_object_get_int_member(w, "length"));
    TEST_ASSERT_FALSE(json_object_has_member(w, "sha256"));

    JsonObject *h = json_object_get_object_member(d, "hashes");
    TEST_ASSERT_EQUAL_INT(64, (int)strlen(json_object_get_string_member(h, "cpu")));
    TEST_ASSERT_EQUAL_INT(64, (int)strlen(json_object_get_string_member(h, "ram")));
    TEST_ASSERT_TRUE(json_object_has_member(h, "memext"));
    TEST_ASSERT_TRUE(JSON_NODE_HOLDS_NULL(json_object_get_member(h, "memext")));
    TEST_ASSERT_FALSE(json_object_has_member(h, "vram"));

    JsonObject *ra = json_object_get_object_member(d, "raster");
    TEST_ASSERT_EQUAL_INT(12, json_object_get_int_member(ra, "scanline"));
    JsonObject *mm = json_object_get_object_member(d, "memory_map");
    TEST_ASSERT_EQUAL_STRING("mz800", json_object_get_string_member(mm, "platform"));
    JsonArray *slots = json_object_get_array_member(mm, "slots");
    TEST_ASSERT_EQUAL_UINT(16, json_array_get_length(slots));
    TEST_ASSERT_EQUAL_STRING("rom_low",
        json_object_get_string_member(json_array_get_object_element(slots, 0), "kind"));
    TEST_ASSERT_EQUAL_STRING("ram",
        json_object_get_string_member(json_array_get_object_element(slots, 1), "kind"));
    g_object_unref(parser);
    free(resp);
}


/**
 * @brief state_observe: neplatné parametry se odmítnou bez volání emu vlákna,
 *        okno přes konec regionu (zjistí emu vlákno) je také chyba parametrů.
 */
void test_state_observe_invalid_params(void) {
    static const char *const bad[] = {
        "{\"windows\":5}",
        "{}",
        "{\"windows\":[{\"addr\":0}]}",
        "{\"windows\":[{\"addr\":0,\"len\":0}]}",
        "{\"windows\":[{\"addr\":65535,\"len\":2}]}",
        "{\"windows\":[{\"addr\":\"1\",\"len\":2}]}",
        "{\"windows\":[{\"addr\":0,\"len\":1,\"region\":{\"kind\":\"ram\"},\"length\":1}]}",
        "{\"windows\":[{\"region\":{\"kind\":\"nope\"},\"length\":1}]}",
        "{\"windows\":[{\"region\":{\"kind\":\"ram\",\"sub_id\":-1},\"length\":1}]}",
        "{\"windows\":[{\"region\":\"ram\",\"length\":1}]}",
        "{\"windows\":[{\"addr\":0,\"len\":1,\"want_data\":1}]}",
        "{\"windows\":[{\"addr\":0,\"len\":1,\"name\":5}]}",
        "{\"windows\":[{\"addr\":0,\"len\":32768},{\"addr\":32768,\"len\":32768},"
            "{\"addr\":0,\"len\":1}]}",
        "{\"windows\":[],\"include\":[\"regs\"]}",
        "{\"windows\":[],\"hashes\":[\"screen\"]}",
        "{\"windows\":[],\"want_data\":\"yes\"}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        dispatch_stub_reset();
        gchar *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":%d,"
                                      "\"cmd\":\"state_observe\",\"data\":%s}",
                                      9220 + (int)i, bad[i]);
        en_MCP_DISPATCH_RESULT rc;
        JsonParser *parser = NULL;
        char *resp = NULL;
        (void)_rc_call(line, &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, rc, bad[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "Invalid parameters"), bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_stub_state.obs_calls, bad[i]);
        free(resp);
        g_free(line);
    }

    /* 17 oken. */
    GString *many = g_string_new("{\"type\":\"request\",\"req_id\":9240,"
                                 "\"cmd\":\"state_observe\",\"data\":{\"windows\":[");
    for (int i = 0; i < 17; i++) {
        g_string_append_printf(many, "%s{\"addr\":%d,\"len\":1}", i ? "," : "", i);
    }
    g_string_append(many, "]}}");
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    (void)_rc_call(many->str, &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.obs_calls);
    free(resp);
    g_string_free(many, TRUE);

    /* Přesně 64 KiB ve dvou oknech projde. */
    dispatch_stub_reset();
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9241,\"cmd\":\"state_observe\","
                             "\"data\":{\"windows\":[{\"addr\":0,\"len\":32768},"
                             "{\"addr\":32768,\"len\":32768}],\"want_data\":false}}",
                             &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_UINT(2, json_array_get_length(json_object_get_array_member(d, "windows")));
    g_object_unref(parser);
    free(resp);

    /* Okno přes konec regionu: emu vlákno vrátí OUT_OF_RANGE. */
    dispatch_stub_reset();
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9242,\"cmd\":\"state_observe\","
                   "\"data\":{\"windows\":[{\"region\":{\"kind\":\"ram\",\"sub_id\":99},"
                   "\"offset\":4000,\"length\":200}]}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_NOT_NULL(strstr(resp, "region size 4096"));
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.obs_calls);
    free(resp);
}


/**
 * @brief machine_hash: výběr částí, include_media, tvar odpovědi
 *        (chybějící část = null, covered, not_covered, revize z okamžiku výpočtu).
 */
void test_machine_hash_parts_and_response(void) {
    dispatch_stub_reset();
    g_stub_state.state_rev = 4242;
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9300,\"cmd\":\"machine_hash\","
                             "\"data\":{\"parts\":[\"cpu\",\"memext\",\"cmt\"]}}",
                             &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_stub_state.mh_calls);
    TEST_ASSERT_EQUAL_UINT32((1u << MACHINE_HASH_CPU) | (1u << MACHINE_HASH_MEMEXT)
                             | (1u << MACHINE_HASH_CMT), g_stub_state.mh_last.mask);
    TEST_ASSERT_EQUAL_UINT32(0, g_stub_state.mh_last.flags);
    TEST_ASSERT_EQUAL_INT(64, (int)strlen(json_object_get_string_member(d, "sha256")));
    JsonObject *parts = json_object_get_object_member(d, "parts");
    TEST_ASSERT_EQUAL_UINT(3, json_object_get_size(parts));
    TEST_ASSERT_EQUAL_INT(64, (int)strlen(json_object_get_string_member(parts, "cpu")));
    TEST_ASSERT_TRUE(JSON_NODE_HOLDS_NULL(json_object_get_member(parts, "memext")));
    TEST_ASSERT_FALSE(json_object_has_member(parts, "ram"));
    JsonArray *cov = json_object_get_array_member(d, "covered");
    TEST_ASSERT_EQUAL_UINT(2, json_array_get_length(cov));
    TEST_ASSERT_EQUAL_STRING("cpu", json_array_get_string_element(cov, 0));
    TEST_ASSERT_EQUAL_STRING("cmt", json_array_get_string_element(cov, 1));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"media_content\""));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"screen\""));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(d, "include_media"));
    TEST_ASSERT_EQUAL_INT(4242, json_object_get_int_member(d, "state_rev"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "stable"));
    TEST_ASSERT_EQUAL_INT(123, json_object_get_int_member(d, "elapsed_us"));
    g_object_unref(parser);
    free(resp);

    /* Bez parts = všechny části; include_media přejde do příznaků. */
    dispatch_stub_reset();
    d = _rc_call("{\"type\":\"request\",\"req_id\":9301,\"cmd\":\"machine_hash\","
                 "\"data\":{\"include_media\":true}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(MACHINE_HASH_ALL_PARTS, g_stub_state.mh_last.mask);
    TEST_ASSERT_EQUAL_UINT32(MACHINE_HASH_FLAG_INCLUDE_MEDIA, g_stub_state.mh_last.flags);
    TEST_ASSERT_EQUAL_UINT(MACHINE_HASH_PART_COUNT,
                           json_object_get_size(json_object_get_object_member(d, "parts")));
    TEST_ASSERT_NULL(strstr(resp, "\"media_content\""));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(d, "include_media"));
    g_object_unref(parser);
    free(resp);
}


/**
 * @brief machine_hash: neplatné parametry se odmítnou bez volání emu vlákna.
 */
void test_machine_hash_invalid_params(void) {
    static const char *const bad[] = {
        "{\"parts\":\"cpu\"}",
        "{\"parts\":[\"screen\"]}",
        "{\"parts\":[1]}",
        "{\"include_media\":1}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        dispatch_stub_reset();
        gchar *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":%d,"
                                      "\"cmd\":\"machine_hash\",\"data\":%s}",
                                      9310 + (int)i, bad[i]);
        en_MCP_DISPATCH_RESULT rc;
        JsonParser *parser = NULL;
        char *resp = NULL;
        (void)_rc_call(line, &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, rc, bad[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "Invalid parameters"), bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_stub_state.mh_calls, bad[i]);
        free(resp);
        g_free(line);
    }
}


/* ====================================================================== */
/* Podmíněné zápisy (observe-cas M10, V1)                                  */
/* ====================================================================== */

/** @brief 64 hex znaků ze dvojice `hh` opakované 32krát (statický buffer). */
static const char *_hex32(const char *pair) {
    static char buf[65];
    for (int i = 0; i < 32; i++) {
        buf[2 * i] = pair[0];
        buf[2 * i + 1] = pair[1];
    }
    buf[64] = '\0';
    return buf;
}


/**
 * @brief mem_write / mem_write_force / region_write: expected_sha256
 *        a want_hashes, nesplněná podmínka, neplatný hash, bez podmínky.
 */
void test_cond_memory_writes(void) {
    static const char *const cmds[] = { "mem_write", "mem_write_force", "region_write" };
    for (size_t k = 0; k < G_N_ELEMENTS(cmds); k++) {
        const char *base = (k == 2)
            ? "\"region_id\":0,\"offset\":16,\"data_b64\":\"AQI=\""
            : "\"addr\":49152,\"data_hex\":\"0102\"";
        en_MCP_DISPATCH_RESULT rc;
        JsonParser *parser = NULL;
        char *resp = NULL;

        /* Splněná podmínka: before/after. */
        dispatch_stub_reset();
        gchar *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9300,\"cmd\":\"%s\","
                                      "\"data\":{%s,\"expected_sha256\":\"%s\"}}",
                                      cmds[k], base, _hex32("AB"));
        JsonObject *d = _rc_call(line, &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_OK, rc, cmds[k]);
        TEST_ASSERT_EQUAL_STRING(_hex32("22"), json_object_get_string_member(d, "before_sha256"));
        TEST_ASSERT_EQUAL_STRING(_hex32("33"), json_object_get_string_member(d, "after_sha256"));
        g_object_unref(parser);
        free(resp);
        g_free(line);

        /* Nesplněná podmínka: Precondition failed s oběma hashi. */
        dispatch_stub_reset();
        line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9301,\"cmd\":\"%s\","
                               "\"data\":{%s,\"expected_sha256\":\"%s\"}}",
                               cmds[k], base, _hex32("ee"));
        (void)_rc_call(line, &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_EMU_ERROR, rc, cmds[k]);
        gchar *want = g_strdup_printf("Precondition failed: memory changed (expected_sha256=%s, ",
                                      _hex32("ee"));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, want), resp);
        TEST_ASSERT_NOT_NULL(strstr(resp, _hex32("11")));
        g_free(want);
        free(resp);
        g_free(line);

        /* Neplatný hash: chyba parametrů, nic se neodešle. */
        static const char *const bad[] = { "\"abc\"", "5", "\"zz00000000000000000000000000000000000000000000000000000000000000\"" };
        for (size_t b = 0; b < G_N_ELEMENTS(bad); b++) {
            dispatch_stub_reset();
            line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9302,\"cmd\":\"%s\","
                                   "\"data\":{%s,\"expected_sha256\":%s}}", cmds[k], base, bad[b]);
            (void)_rc_call(line, &rc, &parser, &resp);
            TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, rc, bad[b]);
            TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
            free(resp);
            g_free(line);
        }

        /* Jen want_hashes a bez podmínky. */
        dispatch_stub_reset();
        line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9303,\"cmd\":\"%s\","
                               "\"data\":{%s,\"want_hashes\":true}}", cmds[k], base);
        d = _rc_call(line, &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
        TEST_ASSERT_TRUE(json_object_has_member(d, "before_sha256"));
        g_object_unref(parser);
        free(resp);
        g_free(line);

        dispatch_stub_reset();
        line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9304,\"cmd\":\"%s\","
                               "\"data\":{%s}}", cmds[k], base);
        d = _rc_call(line, &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
        TEST_ASSERT_FALSE(json_object_has_member(d, "before_sha256"));
        TEST_ASSERT_FALSE(json_object_has_member(d, "after_sha256"));
        g_object_unref(parser);
        free(resp);
        g_free(line);
    }
}


/**
 * @brief set_register: expected (CAS), previous, chybný rozsah expected.
 */
void test_cond_set_register(void) {
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    dispatch_stub_reset();
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9310,\"cmd\":\"set_register\","
                             "\"data\":{\"reg\":\"HL\",\"value\":1,\"expected\":17185}}",
                             &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(0x4321, json_object_get_int_member(d, "previous"));
    g_object_unref(parser);
    free(resp);

    dispatch_stub_reset();
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9311,\"cmd\":\"set_register\","
                   "\"data\":{\"reg\":\"HL\",\"value\":1,\"expected\":61166}}",
                   &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "Precondition failed: register HL changed "
                                              "(expected=0xEEEE, current=0x4321)"), resp);
    free(resp);

    dispatch_stub_reset();
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9312,\"cmd\":\"set_register\","
                   "\"data\":{\"reg\":\"HL\",\"value\":1,\"expected\":65536}}",
                   &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
    free(resp);
}


/**
 * @brief set_cpu_flags: expected jen pro zapisovaná pole, previous,
 *        nesplněná podmínka s hodnotami v textu.
 */
void test_cond_set_cpu_flags(void) {
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    dispatch_stub_reset();
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9320,\"cmd\":\"set_cpu_flags\","
                             "\"data\":{\"iff1\":false,\"r\":5,\"expected\":{\"iff1\":true,\"r\":66}}}",
                             &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    JsonObject *prev = json_object_get_object_member(d, "previous");
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(prev, "iff1"));
    TEST_ASSERT_EQUAL_INT(0x42, json_object_get_int_member(prev, "r"));
    TEST_ASSERT_FALSE(json_object_has_member(prev, "im"));
    g_object_unref(parser);
    free(resp);

    dispatch_stub_reset();
    (void)_rc_call("{\"type\":\"request\",\"req_id\":9321,\"cmd\":\"set_cpu_flags\","
                   "\"data\":{\"r\":5,\"expected\":{\"r\":238}}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_EMU_ERROR, rc);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "Precondition failed: cpu flags changed "
                                              "(r: expected=0xEE, current=0x42)"), resp);
    free(resp);

    static const char *const bad[] = {
        "{\"r\":5,\"expected\":{\"im\":1}}",
        "{\"r\":5,\"expected\":{\"bogus\":1}}",
        "{\"r\":5,\"expected\":{\"r\":256}}",
        "{\"im\":1,\"expected\":{\"im\":3}}",
        "{\"r\":5,\"expected\":[1]}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        dispatch_stub_reset();
        gchar *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9322,"
                                      "\"cmd\":\"set_cpu_flags\",\"data\":%s}", bad[i]);
        (void)_rc_call(line, &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, rc, bad[i]);
        TEST_ASSERT_EQUAL_INT(0, g_stub_state.call_count);
        free(resp);
        g_free(line);
    }
}


/* ---------------- Pokrytí kódu (coverage_*) ---------------- */

/** @brief Chybová zpráva z odpovědi (nová kopie, g_free). */
static gchar *_cov_error_text(const char *resp) {
    JsonParser *parser = NULL;
    JsonObject *obj = _parse_response_object(resp, &parser);
    gchar *e = g_strdup(json_object_get_string_member(obj, "error"));
    g_object_unref(parser);
    return e;
}


/**
 * @brief coverage_start: parametry dojdou do jádra, odpověď je stav.
 */
void test_coverage_start_params(void) {
    dispatch_stub_reset();
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    JsonObject *d = _rc_call("{\"type\":\"request\",\"req_id\":9400,\"cmd\":\"coverage_start\","
                             "\"data\":{\"level\":\"detail\",\"key\":\"full_bytes\",\"capacity\":100,"
                             "\"reset\":true,\"reset_on_discontinuity\":true}}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_COVERAGE_OP_START, g_stub_state.cov_last.op);
    TEST_ASSERT_EQUAL_INT(COVERAGE_LEVEL_DETAIL, g_stub_state.cov_last.cfg.level);
    TEST_ASSERT_EQUAL_INT(COVERAGE_KEY_FULL_BYTES, g_stub_state.cov_last.cfg.key);
    TEST_ASSERT_EQUAL_UINT32(100, g_stub_state.cov_last.cfg.capacity);
    TEST_ASSERT_TRUE(g_stub_state.cov_last.reset);
    TEST_ASSERT_TRUE(g_stub_state.cov_last.cfg.reset_on_discontinuity);
    TEST_ASSERT_EQUAL_STRING("detail", json_object_get_string_member(d, "level"));
    TEST_ASSERT_EQUAL_INT(5, json_object_get_int_member(d, "generation"));
    TEST_ASSERT_TRUE(json_object_has_member(d, "branches"));
    g_object_unref(parser);
    free(resp);

    /* Výchozí hodnoty: branch, opcode, 65536, bez resetu. */
    parser = NULL;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9401,\"cmd\":\"coverage_start\"}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(COVERAGE_LEVEL_BRANCH, g_stub_state.cov_last.cfg.level);
    TEST_ASSERT_EQUAL_UINT32(COVERAGE_DEFAULT_CAPACITY, g_stub_state.cov_last.cfg.capacity);
    TEST_ASSERT_FALSE(g_stub_state.cov_last.reset);
    g_object_unref(parser);
    free(resp);

    parser = NULL;
    d = _rc_call("{\"type\":\"request\",\"req_id\":9402,\"cmd\":\"coverage_mark\"}", &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(DBGAPI_COVERAGE_OP_MARK, g_stub_state.cov_last.op);
    TEST_ASSERT_EQUAL_INT(3, json_object_get_int_member(d, "epoch"));
    g_object_unref(parser);
    free(resp);
}


/** @brief coverage_start s neplatnými parametry neodešle nic do jádra. */
void test_coverage_start_invalid(void) {
    static const char *const bad[] = {
        "{\"level\":\"full\"}",
        "{\"key\":\"bytes\"}",
        "{\"capacity\":0}",
        "{\"capacity\":1048577}",
        "{\"reset\":1}",
        "{\"level\":7}",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        dispatch_stub_reset();
        en_MCP_DISPATCH_RESULT rc;
        JsonParser *parser = NULL;
        char *resp = NULL;
        gchar *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9410,"
                                      "\"cmd\":\"coverage_start\",\"data\":%s}", bad[i]);
        (void)_rc_call(line, &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, rc, bad[i]);
        TEST_ASSERT_EQUAL_INT(0, g_stub_state.cov_calls);
        free(resp);
        g_free(line);
    }
}


/** @brief Všechny řádky coverage_read jako JSON pole (stránkami po @p limit). */
static JsonArray *_cov_read_all(const char *columns, const char *filter, int limit) {
    JsonArray *all = json_array_new();
    gchar *cursor = NULL;
    for (int guard = 0; guard < 100; guard++) {
        en_MCP_DISPATCH_RESULT rc;
        JsonParser *parser = NULL;
        char *resp = NULL;
        gchar *cur = cursor ? g_strdup_printf(",\"cursor\":\"%s\"", cursor) : g_strdup("");
        gchar *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9420,\"cmd\":\"coverage_read\","
                                      "\"data\":{\"limit\":%d%s%s%s%s%s}}", limit,
                                      columns ? ",\"columns\":" : "", columns ? columns : "",
                                      filter ? ",\"filter\":" : "", filter ? filter : "", cur);
        JsonObject *d = _rc_call(line, &rc, &parser, &resp);
        g_free(line);
        g_free(cur);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_OK, rc, resp);
        JsonArray *rows = json_object_get_array_member(d, "rows");
        for (guint i = 0; i < json_array_get_length(rows); i++) {
            json_array_add_element(all, json_node_copy(json_array_get_element(rows, i)));
        }
        g_free(cursor);
        cursor = g_strdup(json_object_get_string_member(d, "next_cursor"));
        gboolean end = json_object_get_boolean_member(d, "end_of_data");
        g_object_unref(parser);
        free(resp);
        if (end) break;
    }
    g_free(cursor);
    return all;
}


/** @brief Hodnota sloupce @p col řádku @p row (pole). */
static JsonNode *_cov_cell(JsonArray *rows, guint row, guint col) {
    return json_array_get_element(json_array_get_array_element(rows, row), col);
}


/**
 * @brief coverage_read úrovně branch: stavy, epocha, filtry, stránkování po 1.
 */
void test_coverage_read_branch(void) {
    dispatch_stub_reset();
    g_stub_state.cov_level = COVERAGE_LEVEL_BRANCH;
    JsonArray *rows = _cov_read_all(NULL, NULL, 1);
    TEST_ASSERT_EQUAL_UINT(4, json_array_get_length(rows));
    /* výchozí sloupce: pc, state, int_entry, first_epoch */
    TEST_ASSERT_EQUAL_INT(0x1000, json_node_get_int(_cov_cell(rows, 0, 0)));
    TEST_ASSERT_EQUAL_STRING("none", json_node_get_string(_cov_cell(rows, 0, 1)));
    TEST_ASSERT_EQUAL_STRING("taken_only", json_node_get_string(_cov_cell(rows, 1, 1)));
    TEST_ASSERT_EQUAL_INT(2, json_node_get_int(_cov_cell(rows, 2, 3)));
    TEST_ASSERT_EQUAL_STRING("both", json_node_get_string(_cov_cell(rows, 3, 1)));
    json_array_unref(rows);

    rows = _cov_read_all("[\"pc\"]", "{\"state\":\"partial\"}", 100);
    TEST_ASSERT_EQUAL_UINT(1, json_array_get_length(rows));
    TEST_ASSERT_EQUAL_INT(0x1001, json_node_get_int(_cov_cell(rows, 0, 0)));
    json_array_unref(rows);

    rows = _cov_read_all("[\"pc\"]", "{\"new_since_epoch\":2}", 100);
    TEST_ASSERT_EQUAL_UINT(1, json_array_get_length(rows));
    TEST_ASSERT_EQUAL_INT(0x1002, json_node_get_int(_cov_cell(rows, 0, 0)));
    json_array_unref(rows);
}


/**
 * @brief coverage_read úrovně detail: řazení (PC, buňka), filtr prostoru,
 *        operand, příznaky; stránkování po 1 uvnitř téže adresy.
 */
void test_coverage_read_detail(void) {
    dispatch_stub_reset();
    g_stub_state.cov_level = COVERAGE_LEVEL_DETAIL;
    JsonArray *rows = _cov_read_all("[\"pc\",\"space\",\"key\",\"exec\",\"state\",\"operand\",\"flags\"]", NULL, 1);
    TEST_ASSERT_EQUAL_UINT(4, json_array_get_length(rows));
    TEST_ASSERT_EQUAL_INT(0x1000, json_node_get_int(_cov_cell(rows, 0, 0)));
    TEST_ASSERT_EQUAL_STRING("3E", json_node_get_string(_cov_cell(rows, 0, 2)));
    TEST_ASSERT_EQUAL_INT(5, json_node_get_int(_cov_cell(rows, 0, 5)));
    TEST_ASSERT_EQUAL_STRING("operand_varied",
        json_array_get_string_element(json_node_get_array(_cov_cell(rows, 0, 6)), 0));
    TEST_ASSERT_EQUAL_INT(0x1003, json_node_get_int(_cov_cell(rows, 2, 0)));
    TEST_ASSERT_EQUAL_STRING("ram", json_node_get_string(_cov_cell(rows, 2, 1)));
    TEST_ASSERT_EQUAL_INT(4, json_node_get_int(_cov_cell(rows, 2, 3)));
    TEST_ASSERT_EQUAL_STRING("rom", json_node_get_string(_cov_cell(rows, 3, 1)));
    TEST_ASSERT_EQUAL_STRING("both", json_node_get_string(_cov_cell(rows, 3, 4)));
    json_array_unref(rows);

    rows = _cov_read_all("[\"pc\",\"exec\"]", "{\"space\":\"rom\"}", 100);
    TEST_ASSERT_EQUAL_UINT(1, json_array_get_length(rows));
    TEST_ASSERT_EQUAL_INT(2, json_node_get_int(_cov_cell(rows, 0, 1)));
    json_array_unref(rows);
}


/** @brief coverage_read: neznámý sloupec, filtr space u branch, kurzor jiné generace. */
void test_coverage_read_errors(void) {
    struct { en_COVERAGE_LEVEL level; const char *data; const char *want; } cases[] = {
        { COVERAGE_LEVEL_BRANCH, "{\"columns\":[\"key\"]}", "unknown column 'key'" },
        { COVERAGE_LEVEL_BRANCH, "{\"filter\":{\"space\":\"ram\"}}", "filter space needs level detail" },
        { COVERAGE_LEVEL_DETAIL, "{\"filter\":{\"state\":\"half\"}}", "unknown state" },
        { COVERAGE_LEVEL_DETAIL, "{\"limit\":0}", "limit must be 1..5000" },
        { COVERAGE_LEVEL_DETAIL, "{\"cursor\":\"x\"}", "invalid cursor" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        dispatch_stub_reset();
        g_stub_state.cov_level = cases[i].level;
        en_MCP_DISPATCH_RESULT rc;
        JsonParser *parser = NULL;
        char *resp = NULL;
        gchar *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9430,"
                                      "\"cmd\":\"coverage_read\",\"data\":%s}", cases[i].data);
        (void)_rc_call(line, &rc, &parser, &resp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MCP_DISPATCH_INVALID_PARAMS, rc, cases[i].data);
        gchar *e = _cov_error_text(resp);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e, cases[i].want), e);
        g_free(e);
        free(resp);
        g_free(line);
    }

    /* Kurzor jiné generace dat (stub hlásí 5). */
    dispatch_stub_reset();
    char *cur = trace_cursor_encode("coverage", 4, 0);
    gchar *line = g_strdup_printf("{\"type\":\"request\",\"req_id\":9431,"
                                  "\"cmd\":\"coverage_read\",\"data\":{\"cursor\":\"%s\"}}", cur);
    en_MCP_DISPATCH_RESULT rc;
    JsonParser *parser = NULL;
    char *resp = NULL;
    (void)_rc_call(line, &rc, &parser, &resp);
    TEST_ASSERT_EQUAL_INT(MCP_DISPATCH_INVALID_PARAMS, rc);
    gchar *e = _cov_error_text(resp);
    TEST_ASSERT_TRUE_MESSAGE(g_str_has_prefix(e, "segment_replaced:"), e);
    g_free(e);
    free(resp);
    g_free(line);
    g_free(cur);
}


int main(void) {
    UNITY_BEGIN();

    /* Success path - 10 dbgapi handlerů (ping a shutdown jsou lokální) */
    RUN_TEST(test_ping_handler_returns_pong);
    RUN_TEST(test_bp_remove_timeout_reports_not_executed);
    RUN_TEST(test_bp_remove_handler_failure_keeps_message);
    RUN_TEST(test_queue_full_and_ending_messages);
    RUN_TEST(test_partial_execution_reported);
    RUN_TEST(test_submit_track_reset_per_request);
    RUN_TEST(test_get_state_running_true);
    RUN_TEST(test_pause_handler_uses_origin_mcp);
    RUN_TEST(test_run_handler_uses_origin_mcp);
    RUN_TEST(test_reset_handler_uses_origin_mcp);
    RUN_TEST(test_get_registers_returns_14_fields);
    RUN_TEST(test_mem_read_returns_data_b64);
    RUN_TEST(test_mem_write_writes_bytes_and_returns_length);
    RUN_TEST(test_bp_add_returns_assigned_id);
    RUN_TEST(test_bp_list_returns_array);
    RUN_TEST(test_bp_list_serializes_addr_match_fields);
    RUN_TEST(test_bp_create_addr_end_without_range_warns);

    /* F015b - typed BP create + bp_list serializace + leak smoke */
    RUN_TEST(test_bp_create_typed_passes_type_to_dbgapi);
    RUN_TEST(test_bp_list_serializes_typed_fields);
    RUN_TEST(test_bp_create_unknown_field_invalid_params);
    RUN_TEST(test_bp_create_type_zone_string_parsed);
    RUN_TEST(test_bp_create_event_trigger_string_parsed);
    RUN_TEST(test_bp_create_type_number_still_works);
    RUN_TEST(test_bp_create_unknown_type_string_invalid_params);
    RUN_TEST(test_bp_update_unknown_zone_string_invalid_params);
    RUN_TEST(test_bp_clear_with_condition_smoke);

    /* Error paths */
    RUN_TEST(test_unknown_cmd_returns_error_response);
    RUN_TEST(test_mem_read_invalid_params_rejected);
    RUN_TEST(test_mem_write_region_check_fail);
    RUN_TEST(test_mem_write_invalid_hex_rejected);
    RUN_TEST(test_mem_write_overflow_rejected);
    RUN_TEST(test_mem_write_full_64k_length);
    RUN_TEST(test_bp_add_invalid_addr_rejected);
    RUN_TEST(test_pause_emu_failure_reports_error);
    RUN_TEST(test_non_request_message_rejected);
    RUN_TEST(test_null_args_not_crash);

    /* Hello + commands */
    RUN_TEST(test_build_hello_contains_19_commands);
    RUN_TEST(test_supported_commands_list);
    RUN_TEST(test_stall_limit_table);
    RUN_TEST(test_supported_cmd_names_generated_from_cmd_map);
    RUN_TEST(test_transport_kind_default_and_setter);

    /* Shutdown handler + callback (V0.A.4) */
    RUN_TEST(test_shutdown_handler_returns_success);
    RUN_TEST(test_shutdown_handler_invokes_callback);

    /* V1.B.3 - emu_stop (hot-swap workflow) */
    RUN_TEST(test_emu_stop_pipe_success);
    RUN_TEST(test_emu_stop_tcp_returns_error);
    RUN_TEST(test_emu_stop_no_callback_returns_error);
    RUN_TEST(test_emu_stop_transport_none_returns_error);

    /* V0.B.6 - 7 chybějících V0 Tools */
    RUN_TEST(test_bp_remove_happy);
    RUN_TEST(test_bp_remove_missing_id);
    RUN_TEST(test_bp_clear_iterates_and_removes);
    RUN_TEST(test_bp_enable_happy);
    RUN_TEST(test_bp_enable_missing_param);
    RUN_TEST(test_step_into_increments_counter);
    RUN_TEST(test_step_over_increments_counter);
    RUN_TEST(test_step_n_loops_count_times);
    RUN_TEST(test_step_n_partial_on_failure);
    RUN_TEST(test_step_n_invalid_range_rejected);
    RUN_TEST(test_run_until_addr_happy);
    RUN_TEST(test_run_until_addr_invalid_addr);

    /* V0.B.7 - get_mcp_config Resource backing */
    RUN_TEST(test_get_mcp_config_returns_payload);

    /* V1.A.1 - Snapshot Tools + cooperation hint */
    RUN_TEST(test_snapshot_save_happy);
    RUN_TEST(test_snapshot_save_invalid_path);
    RUN_TEST(test_snapshot_save_buffer_returns_b64);
    RUN_TEST(test_snapshot_load_buffer_roundtrip);
    RUN_TEST(test_snapshot_load_buffer_invalid_b64);
    RUN_TEST(test_run_until_addr_running_is_error);
    RUN_TEST(test_wait_stop_done_and_timeout);
    RUN_TEST(test_get_stop_history_limit);
    RUN_TEST(test_pause_if_run_seq);
    RUN_TEST(test_run_until_addr_deadline);
    RUN_TEST(test_step_completed_flags);
    RUN_TEST(test_mem_read_sha256_and_want_data);
    RUN_TEST(test_mem_read_full_64k);
    RUN_TEST(test_coverage_start_params);
    RUN_TEST(test_coverage_start_invalid);
    RUN_TEST(test_coverage_read_branch);
    RUN_TEST(test_coverage_read_detail);
    RUN_TEST(test_coverage_read_errors);
    RUN_TEST(test_frame_hash_submits);
    RUN_TEST(test_replay_record_start_params);
    RUN_TEST(test_replay_play_and_stop_modes);
    RUN_TEST(test_replay_status_fields);
    RUN_TEST(test_replay_rewind_seek_params);
    RUN_TEST(test_replay_goto_params);
    RUN_TEST(test_state_rev_fields_and_conditions);
    RUN_TEST(test_snapshot_load_not_paused_reason);
    RUN_TEST(test_cooperation_hint_set_modes);

    /* V1.A.2 - Symbol management Tools */
    RUN_TEST(test_symbol_add_happy);
    RUN_TEST(test_symbol_add_invalid_name);
    RUN_TEST(test_symbol_add_invalid_addr);
    RUN_TEST(test_symbol_remove_by_name);
    RUN_TEST(test_symbol_remove_by_addr);
    RUN_TEST(test_symbol_remove_exclusive);
    RUN_TEST(test_symbol_lookup_hex_address);
    RUN_TEST(test_symbol_lookup_by_name);
    RUN_TEST(test_symbol_lookup_not_found);
    RUN_TEST(test_symbol_list_prefix_and_items);
    RUN_TEST(test_symbol_list_invalid_limit);

    /* V1.A.3 - step out + run_until_* Tools */
    RUN_TEST(test_step_out_happy);
    RUN_TEST(test_step_out_no_callstack_fallback);
    RUN_TEST(test_run_until_raster_invalid_line);
    RUN_TEST(test_run_until_raster_reached);
    RUN_TEST(test_run_until_tstate_target_in_past);
    RUN_TEST(test_run_until_tstate_reached);
    RUN_TEST(test_run_until_tstate_ticks_reached_now);
    RUN_TEST(test_run_until_tstate_ticks_invalid);
    RUN_TEST(test_run_until_event_unsupported_kind);
    RUN_TEST(test_run_until_event_io_write_not_implemented);
    RUN_TEST(test_run_until_event_frame_done);

    /* V1.A.4 - EVENT subscribe + TRAP forwarding */
    RUN_TEST(test_event_subscribe_topics);
    RUN_TEST(test_event_subscribe_empty_rejected);
    RUN_TEST(test_event_poll_empty_queue);
    RUN_TEST(test_event_emit_pushes_to_subscriber);
    RUN_TEST(test_trap_respond_unknown_id);
    RUN_TEST(test_trap_respond_continue_submits_run);
    RUN_TEST(test_trap_respond_invalid_action);

    /* V1.A.5 - chip-level fault injection */
    RUN_TEST(test_io_read_happy);
    RUN_TEST(test_io_read_invalid_port);
    RUN_TEST(test_io_write_happy);
    RUN_TEST(test_io_write_invalid_value);
    RUN_TEST(test_irq_inject_with_vector);
    RUN_TEST(test_irq_inject_default);
    RUN_TEST(test_nmi_inject_happy);
    RUN_TEST(test_mem_write_force_happy);
    RUN_TEST(test_mem_write_force_odd_hex);

    /* V1.A.6 - Watch + Callstack + CDL Tools (13 testů) */
    RUN_TEST(test_watch_add_address_happy);
    RUN_TEST(test_watch_add_expr_scalar);
    RUN_TEST(test_watch_add_expr_missing_text);
    RUN_TEST(test_watch_remove_by_name_happy);
    RUN_TEST(test_watch_remove_missing_params);
    RUN_TEST(test_watch_list_returns_items);
    RUN_TEST(test_watch_eval_inline_expr);
    RUN_TEST(test_watch_eval_missing_params);
    RUN_TEST(test_callstack_get_max_depth_limit);
    RUN_TEST(test_callstack_get_invalid_depth);
    RUN_TEST(test_cdl_start_stop_reset_lifecycle);
    RUN_TEST(test_cdl_export_happy);
    RUN_TEST(test_cdl_export_missing_path);
    /* 0017 FÁZE 1 - Tracking lifecycle Tools */
    RUN_TEST(test_videorec_start_with_path_and_frames);
    RUN_TEST(test_videorec_start_without_path);
    RUN_TEST(test_videorec_start_invalid_params);
    RUN_TEST(test_videorec_start_core_error);
    RUN_TEST(test_videorec_stop);
    RUN_TEST(test_videorec_pause_variants);
    RUN_TEST(test_videorec_marker_label);
    RUN_TEST(test_videorec_marker_not_running);
    RUN_TEST(test_videorec_timebase);
    RUN_TEST(test_videorec_status_payload);
    RUN_TEST(test_videorec_status_no_event);
    RUN_TEST(test_trace_start_stop_reset_lifecycle);
    RUN_TEST(test_trace_save_with_path);
    RUN_TEST(test_trace_save_without_path);
    RUN_TEST(test_trace_missing_channel);
    RUN_TEST(test_trace_unknown_channel);

    /* V1.A.7 - Profiler Tools (7 testů) */
    RUN_TEST(test_profiler_start_stop_lifecycle);
    RUN_TEST(test_profiler_reset_clears_data);
    RUN_TEST(test_profiler_export_happy_csv);
    RUN_TEST(test_profiler_export_invalid_format);
    RUN_TEST(test_profiler_export_missing_path);
    RUN_TEST(test_profiler_get_limit_range);
    RUN_TEST(test_profiler_get_default_returns_stats);

    /* V1.B.1 - Media Tools (6 testů) */
    RUN_TEST(test_media_load_mzf_path_happy);
    RUN_TEST(test_media_load_mzf_both_path_and_b64_rejected);
    RUN_TEST(test_media_run_mzf_path_happy);
    RUN_TEST(test_media_run_mzf_errors);
    RUN_TEST(test_cmt_set_property_custom_pulses);
    RUN_TEST(test_cmt_tape_block_speed_pulses);
    RUN_TEST(test_media_load_binary_addr_validation);
    RUN_TEST(test_media_insert_each_slot);
    RUN_TEST(test_media_insert_invalid_slot);
    RUN_TEST(test_media_eject_lifecycle);
    RUN_TEST(test_media_state_returns_all_slots);

    /* V1.B.2 - Platform + Config Tools (10 testů) */
    RUN_TEST(test_settings_set_live_key_happy);
    RUN_TEST(test_settings_set_boot_only_rejected);
    RUN_TEST(test_settings_set_qdisk_keys_rejected);
    RUN_TEST(test_settings_set_invalid_key_format_rejected);
    RUN_TEST(test_settings_get_returns_value_and_type);
    RUN_TEST(test_settings_get_module_not_found);
    RUN_TEST(test_settings_get_live_value);
    RUN_TEST(test_settings_set_custom_fps_range);
    RUN_TEST(test_platform_set_invalid_kind);
    RUN_TEST(test_platform_set_requires_restart);
    RUN_TEST(test_periph_attach_each_kind);
    RUN_TEST(test_periph_attach_memext_options_type);
    RUN_TEST(test_periph_detach_not_in_arch);
    RUN_TEST(test_periph_detach_passes_options_type);
    RUN_TEST(test_periph_attach_invalid_option_type);
    RUN_TEST(test_periph_attach_device_refused);
    RUN_TEST(test_periph_attach_invalid_kind);

    /* V1.C.1 - HID Tools */
    RUN_TEST(test_input_send_key_lifecycle);
    RUN_TEST(test_input_send_key_landing_caveat);
    RUN_TEST(test_input_send_key_unknown_rejected);
    RUN_TEST(test_input_send_keys_ascii_mapping);
    RUN_TEST(test_input_send_keys_key_names_parsing);
    RUN_TEST(test_input_send_keys_bad_encoding);
    RUN_TEST(test_input_press_release_hold_pattern);
    RUN_TEST(test_input_release_all_when_no_key);
    RUN_TEST(test_input_send_joystick_bit_mask);
    RUN_TEST(test_input_send_joystick_invalid_port);
    RUN_TEST(test_input_press_key_aliases);
    RUN_TEST(test_input_press_key_old_names_still_work);
    RUN_TEST(test_input_release_key_alias);
    RUN_TEST(test_input_press_key_unknown_suggests);
    RUN_TEST(test_input_send_keys_key_names_alias_and_unknown);
    RUN_TEST(test_input_send_keys_with_delays_event_list);
    RUN_TEST(test_input_send_keys_with_delays_unknown_key_sends_nothing);
    RUN_TEST(test_input_send_keys_with_delays_empty_rejected);

    /* V1.D.1 - Core + CPU extras Resource backings (9 testů) */
    RUN_TEST(test_get_config_settings_returns_filtered_or_sections);
    RUN_TEST(test_set_cpu_flags_r_writes_full_byte);
    RUN_TEST(test_get_media_state_returns_slots_array_and_note);
    RUN_TEST(test_get_cpu_im2_vector_available_with_stub);
    RUN_TEST(test_get_cpu_interrupt_bus_z80_state_and_placeholders);
    RUN_TEST(test_get_cooperation_policy_returns_default_free);
    RUN_TEST(test_get_security_profile_capabilities_present);
    RUN_TEST(test_get_memory_map_returns_16_slots);
    RUN_TEST(test_get_memext_info_none_when_disconnected);
    RUN_TEST(test_get_state_includes_last_user_action_when_present);

    /* V1.D.2.A - 7 testů pro easy reuse Resources */
    RUN_TEST(test_get_callstack_empty_when_no_frames);
    RUN_TEST(test_get_callstack_populated_frames);
    RUN_TEST(test_get_profiler_returns_stats_and_entries);
    RUN_TEST(test_get_symbols_returns_user_source_mapping);
    RUN_TEST(test_get_stack_history_disabled_returns_empty_samples);
    RUN_TEST(test_get_stack_history_enabled_serializes_samples);
    RUN_TEST(test_get_stack_regions_returns_two_named_regions);

    /* V1.D.2.B - Medium debug Resources */
    RUN_TEST(test_get_watch_empty);
    RUN_TEST(test_get_watch_populated);
    RUN_TEST(test_get_stack_basic);
    RUN_TEST(test_get_stack_sp_odd);
    RUN_TEST(test_get_vars_empty);
    RUN_TEST(test_get_vars_populated);
    RUN_TEST(test_get_bookmarks_empty);
    RUN_TEST(test_get_bookmarks_populated);

    /* V1.D.3.A - IRQ chip Resources */
    RUN_TEST(test_get_periph_i8255_layout);
    RUN_TEST(test_get_periph_i8255_cw_not_decoded);
    RUN_TEST(test_get_periph_i8253_layout);
    RUN_TEST(test_get_periph_z80_pio_available);
    RUN_TEST(test_get_periph_z80_pio_unavailable_mz700);

    /* V1.D.3.B - audio chip Resources */
    RUN_TEST(test_get_periph_sn76489_mono);
    RUN_TEST(test_get_periph_sn76489_stereo);
    RUN_TEST(test_get_periph_sn76489_unavailable_mz700);
    RUN_TEST(test_get_periph_ay3_8910_placeholder);
    RUN_TEST(test_get_periph_beeper_audible);
    RUN_TEST(test_get_periph_beeper_silent_pc0_low);

    /* V1.D.3.C - storage + display Resources */
    RUN_TEST(test_get_periph_gdg_mz800_layout);
    RUN_TEST(test_get_periph_gdg_mz1500_layout);
    RUN_TEST(test_get_periph_wd1793_default);
    RUN_TEST(test_get_periph_wd1793_drive0_mounted);
    RUN_TEST(test_get_periph_wd1793_detached);
    RUN_TEST(test_get_periph_cmt_stop);
    RUN_TEST(test_get_periph_cmt_play);
    RUN_TEST(test_get_periph_qd_image);
    RUN_TEST(test_get_periph_qd_detached);

    /* V1.D.4 - input + frame Resources */
    RUN_TEST(test_get_input_keyboard_state_idle);
    RUN_TEST(test_get_input_keyboard_state_one_pressed);
    RUN_TEST(test_get_input_keyboard_matrix_info);
    RUN_TEST(test_get_input_joystick_state_port0_connected);
    RUN_TEST(test_get_input_joystick_state_disconnected);
    RUN_TEST(test_get_frame_framebuffer_info);
    RUN_TEST(test_get_frame_screenshot_raw_available);
    RUN_TEST(test_get_frame_screenshot_raw_unavailable);
    RUN_TEST(test_get_frame_screenshot_png_unavailable);
    RUN_TEST(test_screenshot_save_to_file_missing_path);
    RUN_TEST(test_screenshot_save_to_file_bad_format);
    RUN_TEST(test_screenshot_save_to_file_png_unavailable);
    RUN_TEST(test_screenshot_save_to_file_options_passed);
    RUN_TEST(test_screenshot_save_to_file_invalid_options);
    RUN_TEST(test_get_frame_screenshot_options);
    RUN_TEST(test_screenshot_series_start_params);
    RUN_TEST(test_screenshot_series_errors);
    RUN_TEST(test_screenshot_series_status_stop);
    RUN_TEST(test_profiler_pc_lifecycle);
    RUN_TEST(test_profiler_pc_get_params);
    RUN_TEST(test_profiler_pc_errors_export);
    RUN_TEST(test_profiler_pc_bitmap);
    RUN_TEST(test_trace_status_all_channels);
    RUN_TEST(test_trace_read_rows_cursor_and_segment);
    RUN_TEST(test_trace_read_cputrack_symbols_columns_raw);
    RUN_TEST(test_trace_read_invalid_params);
    RUN_TEST(test_trace_read_audio_channels);
    RUN_TEST(test_ramlog_start_params);
    RUN_TEST(test_ramlog_queries);
    RUN_TEST(test_freeze_mcp);
    RUN_TEST(test_memsearch_mcp_flow);
    RUN_TEST(test_memsearch_mcp_pehu_hint);
    RUN_TEST(test_memsearch_mcp_errors);
    RUN_TEST(test_trace_read_ramlog);
    RUN_TEST(test_trace_read_bplog);
    RUN_TEST(test_trace_read_eventlog);
    RUN_TEST(test_audio_capture_start_params);
    RUN_TEST(test_audio_capture_errors);
    RUN_TEST(test_audio_capture_start_memory_params);
    RUN_TEST(test_audio_capture_summary);
    RUN_TEST(test_audio_capture_pcm);
    RUN_TEST(test_get_video_text_dump_available);
    RUN_TEST(test_get_video_text_dump_unavailable);

    /* V1.D.2.C - per-watch snapshot Resource (3 testy) */
    RUN_TEST(test_get_watch_snapshot_found_payload);
    RUN_TEST(test_get_watch_snapshot_not_found_payload);
    RUN_TEST(test_get_watch_snapshot_missing_name_rejected);

    /* observe-cas (M10) - state_observe (3 testy) */
    RUN_TEST(test_state_observe_windows_and_core);
    RUN_TEST(test_state_observe_sections_hashes_unavailable);
    RUN_TEST(test_state_observe_invalid_params);
    RUN_TEST(test_machine_hash_parts_and_response);
    RUN_TEST(test_machine_hash_invalid_params);

    /* observe-cas (M10) - podmíněné zápisy (3 testy) */
    RUN_TEST(test_cond_memory_writes);
    RUN_TEST(test_cond_set_register);
    RUN_TEST(test_cond_set_cpu_flags);

    return UNITY_END();
}
