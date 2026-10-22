/**
 * @file   automation.c
 * @brief  Běh ukončený podmínkou, návratové kódy a `result.json` - viz automation.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "automation.h"
#include "mzarch/mzarch_config.h"
#include "emulator.h"
#include "cfgmain.h"
#include "state_hash.h"
#include "replay.h"
#include "build_revision/build_revision.h"
#include "emulator/mzarch/mzarch_platform.h"
#include "mzarch/mzarch.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/gdg/framebuffer.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "screenshot/screenshot.h"
#include "screenshot/screenshot_capture.h"
#include "snapshot/snapshot.h"
#include "debugger/coverage/coverage.h"
#include "debugger/coverage/coverage_export.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/breakpoints.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/run_control/run_control.h"
#endif

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include "mcp/dispatch.h"
#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
#include "mcp/tcp_server.h"
#endif
#endif


/** @brief Hodnota pole `schema` v `result.json`. */
#define AUTOMATION_RESULT_SCHEMA "mz800emu-result/1"

/** @brief Jméno výsledku v `--automation-dir`. */
#define AUTOMATION_DIR_RESULT "result.json"

/** @brief Jméno screenshotu v `--automation-dir` (bez `--exit-screenshot`). */
#define AUTOMATION_DIR_SCREENSHOT "screen.png"


bool g_automation_frame_hook = false;


/**
 * @brief Druh breakpointu, který automatizace založí z příkazové řádky.
 */
typedef enum en_AM_CLI_BP
{
    AM_CLI_EXIT_ON_PC = 0, /**< `--exit-on-pc ADDR[,skip=N][,code=C]`, PC_EXEC, výchozí kód 0. */
    AM_CLI_FAIL_ON_PC,     /**< `--fail-on-pc ADDR[,skip=N]`, PC_EXEC, kód 1. */
    AM_CLI_EXIT_ON_WRITE,  /**< `--exit-on-write ADDR[=VAL][,skip=N][,code=C]`, MEM_W, výchozí kód = zapsaná hodnota. */
    AM_CLI_EXIT_ON_OUT,    /**< `--exit-on-out PORT[,skip=N][,code=C]`, IORQ_W, výchozí kód = zapsaná hodnota. */
    AM_CLI_BP_COUNT        /**< Počet druhů (není to druh). */
} en_AM_CLI_BP;

/** @brief Jména voleb podle en_AM_CLI_BP. */
static const char *const s_am_cli_bp_option[AM_CLI_BP_COUNT] = {
    "--exit-on-pc", "--fail-on-pc", "--exit-on-write", "--exit-on-out",
};

/**
 * @brief Jeden breakpoint ze specifikace na příkazové řádce.
 *
 * @invariant code je -1 (kód = `Value` zápisu) nebo 0-255.
 */
typedef struct st_AM_CLI_BP
{
    en_AM_CLI_BP kind;  /**< Druh (volba). */
    char *spec;         /**< Původní text specifikace (pro zprávu, vlastněný). */
    uint16_t addr;      /**< Adresa (PC, MEM_W) nebo port (IORQ_W, 0-255). */
    bool has_value;     /**< `--exit-on-write ADDR=VAL`: podmínka na hodnotu. */
    uint8_t value;      /**< Hodnota podmínky. */
    uint32_t skip;      /**< Kolik prvních zásahů přeskočit (skip_count breakpointu). */
    int code;           /**< Návratový kód, -1 = zapsaná hodnota (`exit Value`). */
    int bp_id;          /**< ID založeného breakpointu, -1 = zatím nezaložen. */
} st_AM_CLI_BP;

/**
 * @brief Stav automatizace (jeden na proces).
 *
 * Volby plní automation_cli_validate() v hlavním vlákně před startem emu
 * vlákna, běhová část patří emu vláknu. Řetězce jsou vlastněné (g_free).
 *
 * @invariant pending => code, reason platné; result_written se nastaví po
 *            prvním zapsání výsledku.
 */
typedef struct st_AM
{
    /* Volby (hlavní vlákno) */
    bool requested;             /**< Zadaná některá automatizační volba. */
    bool validated;             /**< automation_cli_validate() prošel. */
    char *result_path;          /**< Absolutní cesta `result.json` nebo NULL. */
    char *dir;                  /**< Absolutní `--automation-dir` nebo NULL. */
    char *screenshot_path;      /**< Absolutní cesta screenshotu na konci nebo NULL. */
    char *snapshot_path;        /**< Absolutní cesta snapshotu na konci nebo NULL. */
    char *bpt_path;             /**< Absolutní `--bpt` nebo NULL. */
    char *run_mzf;              /**< Absolutní `--run-mzf` (jen pro `inputs`) nebo NULL. */
    uint64_t exit_after_frames; /**< `--exit-after-frames`, 0 = vypnuto. */
    uint64_t timeout_frames;    /**< `--timeout-*` přepočtený na snímky, 0 = vypnuto. */
    GArray *cli_bps;            /**< Pole st_AM_CLI_BP. */

    /* Běh (emu vlákno) */
    bool started;               /**< automation_start() proběhl. */
    uint64_t frames;            /**< Snímky od automation_start(). */
    uint64_t elapsed_ticks;     /**< Takty GDG od automation_start() k poslednímu konci snímku. */
    uint64_t last_total_ticks;  /**< gdg_get_total_ticks() při posledním konci snímku. */
    gint64 wall_start_us;       /**< g_get_monotonic_time() při startu. */
    bool pending;               /**< Čeká žádost o konec na vyřízení v pauze. */
    bool result_written;        /**< `result.json` už zapsán (řízeným koncem nebo žádostí). */
    int code;                   /**< Kód žádosti. */
    en_AUTOMATION_REASON reason; /**< Důvod žádosti. */
    char *message;              /**< Popis žádosti nebo NULL. */
    int trig_bp_id;             /**< Breakpoint žádosti, -1 = žádný. */
    char *trig_bp_name;         /**< Jméno breakpointu nebo NULL. */
    char *trig_bp_type;         /**< Typ breakpointu nebo NULL. */
    uint64_t trig_hits;         /**< Zásahy breakpointu. */
    GPtrArray *warnings;        /**< Varování do `result.json` (char *, g_free). */
} st_AM;

static st_AM s_am = {
    .trig_bp_id = -1,
};


/** @brief Všechny automatizační volby (pro automation_cli_requested()). */
static const char *const s_am_options[] = {
    "--result-json", "--automation-dir", "--exit-after-frames", "--timeout-frames",
    "--timeout-seconds", "--exit-on-pc", "--fail-on-pc", "--exit-on-write", "--exit-on-out",
    "--bpt", "--exit-screenshot", "--exit-snapshot", "--exit-at-replay-end", "--replay-verify", NULL,
};


/*******************************************************************************
 *
 *                  Pomocné funkce
 *
 ******************************************************************************/

/**
 * @brief Přidá varování do `result.json` a vypíše ho na stderr.
 */
static void am_warn(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void am_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    fprintf(stderr, "[EXIT] warning: %s\n", msg);
    if (!s_am.warnings) s_am.warnings = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(s_am.warnings, msg);
}


/**
 * @brief Přečte nezáporné celé číslo: desítkově, `0x` hex nebo hex s příponou `h`.
 *
 * @param s Text (celý musí být číslo).
 * @param max Největší povolená hodnota.
 * @param[out] out Hodnota.
 * @return true = platné číslo v rozsahu 0..max.
 */
static bool am_parse_uint(const char *s, uint64_t max, uint64_t *out)
{
    if (!s || !*s) return false;
    size_t len = strlen(s);
    const char *digits = s;
    size_t n = len;
    unsigned base = 10;
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        digits = s + 2;
        n = len - 2;
    }
    else if (len > 1 && (s[len - 1] == 'h' || s[len - 1] == 'H'))
    {
        base = 16;
        n = len - 1;
    };
    if (n == 0 || n > 16) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++)
    {
        int d = g_ascii_xdigit_value(digits[i]);
        if (d < 0 || (unsigned)d >= base) return false;
        if (v > (max - (uint64_t)d) / base) return false;
        v = v * base + (uint64_t)d;
    };
    *out = v;
    return true;
}


/**
 * @brief Absolutní cesta vůči aktuálnímu adresáři (vlastněná, g_free).
 */
static char *am_abs_path(const char *path)
{
    return g_canonicalize_filename(path, NULL);
}


/**
 * @brief Přečte specifikaci breakpointu z příkazové řádky.
 *
 * Tvar: první položka adresa (`--exit-on-write` volitelně `ADDR=VAL`, u
 * `--exit-on-out` port 0-255), za ní čárkou oddělené `skip=N` a `code=C`
 * (ne u `--fail-on-pc`).
 *
 * @return true = platná specifikace; false = chyba vypsaná na stderr.
 */
static bool am_parse_cli_bp(en_AM_CLI_BP kind, const char *spec, st_AM_CLI_BP *out)
{
    const char *opt = s_am_cli_bp_option[kind];
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->bp_id = -1;
    out->code = (kind == AM_CLI_EXIT_ON_PC) ? AUTOMATION_EXIT_SUCCESS
              : (kind == AM_CLI_FAIL_ON_PC) ? AUTOMATION_EXIT_FAILURE : -1;

    bool ok = true;
    char **parts = g_strsplit(spec, ",", -1);
    for (int i = 0; ok && parts[i]; i++)
    {
        char *p = g_strstrip(parts[i]);
        uint64_t v;
        if (i == 0)
        {
            char *eq = (kind == AM_CLI_EXIT_ON_WRITE) ? strchr(p, '=') : NULL;
            if (eq)
            {
                *eq = '\0';
                if (!am_parse_uint(eq + 1, 0xFF, &v))
                {
                    fprintf(stderr, "Invalid value for %s: '%s' (value after '=' must be 0-255)\n", opt, spec);
                    ok = false;
                    break;
                };
                out->has_value = true;
                out->value = (uint8_t)v;
            };
            uint64_t max = (kind == AM_CLI_EXIT_ON_OUT) ? 0xFF : 0xFFFF;
            if (!am_parse_uint(p, max, &v))
            {
                fprintf(stderr, "Invalid value for %s: '%s' (expected %s 0-%" G_GUINT64_FORMAT
                        ", decimal, 0x hex or hex with h suffix)\n",
                        opt, spec, (kind == AM_CLI_EXIT_ON_OUT) ? "port" : "address", max);
                ok = false;
                break;
            };
            out->addr = (uint16_t)v;
        }
        else if (g_str_has_prefix(p, "skip="))
        {
            if (!am_parse_uint(p + 5, 0xFFFFFFFFu, &v))
            {
                fprintf(stderr, "Invalid value for %s: '%s' (skip must be a non-negative integer)\n", opt, spec);
                ok = false;
                break;
            };
            out->skip = (uint32_t)v;
        }
        else if (g_str_has_prefix(p, "code=") && kind != AM_CLI_FAIL_ON_PC)
        {
            if (!am_parse_uint(p + 5, 0xFF, &v))
            {
                fprintf(stderr, "Invalid value for %s: '%s' (code must be 0-255)\n", opt, spec);
                ok = false;
                break;
            };
            out->code = (int)v;
        }
        else
        {
            fprintf(stderr, "Invalid value for %s: '%s' (unknown item '%s'; allowed: skip=N%s)\n",
                    opt, spec, p, (kind == AM_CLI_FAIL_ON_PC) ? "" : ", code=C");
            ok = false;
        };
    };
    g_strfreev(parts);
    if (ok) out->spec = g_strdup(spec);
    return ok;
}


/**
 * @brief Volba s hodnotou: hodnota, nebo NULL, když volba chybí.
 */
static const char *am_opt(const char *name)
{
    return sdlapp_option_present(name) ? sdlapp_option_value(name) : NULL;
}


/**
 * @brief Smí žádost o konec ukončit proces? (rozhodnutí M4 č. 9, R5)
 *
 * Jen v `--headless`. V `--mcp-pipe` ne (klient pipe čeká na odpovědi).
 * S běžícím MCP TCP serverem jen tehdy, když je zadaná automatizační volba.
 */
static bool am_may_terminate(void)
{
    if (!sdlapp_option_present("--headless")) return false;
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    if (mcp_dispatch_get_transport_kind() == MCP_DISPATCH_TRANSPORT_PIPE) return false;
#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
    if (!s_am.requested && g_mcp_tcp_server && mcp_tcp_server_is_running(g_mcp_tcp_server)) return false;
#endif
#endif
    return true;
}


/**
 * @brief Text důvodu do `result.json`.
 */
static const char *am_reason_name(en_AUTOMATION_REASON reason)
{
    switch (reason)
    {
        case AUTOMATION_REASON_FRAMES: return "frames";
        case AUTOMATION_REASON_TIMEOUT: return "timeout";
        case AUTOMATION_REASON_EXIT_ACTION: return "exit_action";
        case AUTOMATION_REASON_ERROR: return "error";
        case AUTOMATION_REASON_QUIT: return "quit";
        case AUTOMATION_REASON_REPLAY_END: return "replay_end";
        case AUTOMATION_REASON_REPLAY_DESYNC: return "replay_desync";
        default: break;
    };
    return "none";
}


/**
 * @brief Volba příkazové řádky, ze které vznikl breakpoint, nebo NULL.
 */
static const char *am_cli_option_of_bp(int bp_id)
{
    if (bp_id < 0 || !s_am.cli_bps) return NULL;
    for (guint i = 0; i < s_am.cli_bps->len; i++)
    {
        const st_AM_CLI_BP *b = &g_array_index(s_am.cli_bps, st_AM_CLI_BP, i);
        if (b->bp_id == bp_id) return s_am_cli_bp_option[b->kind];
    };
    return NULL;
}


/*******************************************************************************
 *
 *                  result.json
 *
 ******************************************************************************/

/**
 * @brief Přidá řetězec, nebo null pro NULL.
 */
static void am_json_str_or_null(JsonBuilder *b, const char *name, const char *value)
{
    json_builder_set_member_name(b, name);
    if (value) json_builder_add_string_value(b, value);
    else json_builder_add_null_value(b);
}


/**
 * @brief Objekt `emulator`: verze, revize, platforma.
 */
static void am_json_emulator(JsonBuilder *b)
{
    json_builder_set_member_name(b, "emulator");
    json_builder_begin_object(b);
    am_json_str_or_null(b, "version", EMULATOR_VERSION);
    json_builder_set_member_name(b, "build_revision");
    json_builder_add_int_value(b, build_revision_get_int());
    am_json_str_or_null(b, "commit", build_revision_get_commit_short());
    am_json_str_or_null(b, "platform", g_mzarch_full_name);
    json_builder_set_member_name(b, "screens_per_second");
    json_builder_add_int_value(b, VIDEO_SCREENS_PER_SEC);
    json_builder_set_member_name(b, "debugger");
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    json_builder_add_boolean_value(b, TRUE);
#else
    json_builder_add_boolean_value(b, FALSE);
#endif
    json_builder_end_object(b);
}


/**
 * @brief Objekt `trigger`: co konec spustilo.
 */
static void am_json_trigger(JsonBuilder *b)
{
    json_builder_set_member_name(b, "trigger");
    json_builder_begin_object(b);
    const char *kind = "none";
    if (s_am.trig_bp_id >= 0) kind = "breakpoint";
    else if (s_am.reason == AUTOMATION_REASON_FRAMES) kind = "frames";
    else if (s_am.reason == AUTOMATION_REASON_TIMEOUT) kind = "timeout";
    else if (s_am.reason == AUTOMATION_REASON_ERROR) kind = "error";
    else if (s_am.reason == AUTOMATION_REASON_QUIT) kind = "quit";
    else if (s_am.reason == AUTOMATION_REASON_REPLAY_END || s_am.reason == AUTOMATION_REASON_REPLAY_DESYNC) kind = "replay";
    am_json_str_or_null(b, "kind", kind);
    if (s_am.trig_bp_id >= 0)
    {
        am_json_str_or_null(b, "option", am_cli_option_of_bp(s_am.trig_bp_id));
        json_builder_set_member_name(b, "bp_id");
        json_builder_add_int_value(b, s_am.trig_bp_id);
        am_json_str_or_null(b, "bp_name", s_am.trig_bp_name);
        am_json_str_or_null(b, "bp_type", s_am.trig_bp_type);
        json_builder_set_member_name(b, "hits");
        json_builder_add_int_value(b, (gint64)s_am.trig_hits);
    }
    else if (s_am.reason == AUTOMATION_REASON_FRAMES)
    {
        am_json_str_or_null(b, "option", "--exit-after-frames");
    }
    else if (s_am.reason == AUTOMATION_REASON_TIMEOUT)
    {
        am_json_str_or_null(b, "option", sdlapp_option_present("--timeout-seconds")
                                             ? "--timeout-seconds" : "--timeout-frames");
    }
    else if (s_am.reason == AUTOMATION_REASON_REPLAY_END || s_am.reason == AUTOMATION_REASON_REPLAY_DESYNC)
    {
        am_json_str_or_null(b, "option", sdlapp_option_present("--replay-verify")
                                             ? "--replay-verify" : "--exit-at-replay-end");
    };
    json_builder_end_object(b);
}


/**
 * @brief Objekt `coverage`: stav pokrytí kódu (coverage_status_to_json()).
 *
 * `null`, pokud v běhu žádná data pokrytí nevznikla (i v buildu bez
 * debuggeru). Jinak úroveň, úplnost (`complete`, `dropped`, ...), počty
 * jednotek a instrukcí a souhrn větví `branches`.
 */
static void am_json_coverage(JsonBuilder *b)
{
    st_COVERAGE_STATUS st;
    coverage_get_status(&st);
    json_builder_set_member_name(b, "coverage");
    if (!st.has_data)
    {
        json_builder_add_null_value(b);
        return;
    };
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, coverage_status_to_json(&st));
    json_builder_add_value(b, n);
}


/**
 * @brief Objekt `replay`: výsledek přehrávání / záznamu vstupů (replay.h).
 *
 * `null`, pokud se v běhu záznam ani přehrávání nepoužily. Jinak `mode`
 * (stav při zápisu), `path`, `outcome`, `outcome_frame`, `frames`
 * (délka přehrávaného záznamu, 0 jinak), `deviated`, `deviation`,
 * `desync_parts` (pole jmen částí) a `message`.
 */
static void am_json_replay(JsonBuilder *b)
{
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    json_builder_set_member_name(b, "replay");
    if (!st.path[0] && st.mode == REPLAY_MODE_OFF)
    {
        json_builder_add_null_value(b);
        return;
    };
    json_builder_begin_object(b);
    am_json_str_or_null(b, "mode", replay_mode_name(st.mode));
    am_json_str_or_null(b, "path", st.path);
    am_json_str_or_null(b, "outcome", replay_outcome_name(st.outcome));
    json_builder_set_member_name(b, "outcome_frame");
    json_builder_add_int_value(b, st.outcome_frame);
    json_builder_set_member_name(b, "frames");
    json_builder_add_int_value(b, st.frames);
    json_builder_set_member_name(b, "deviated");
    json_builder_add_boolean_value(b, st.deviated);
    am_json_str_or_null(b, "deviation", st.deviation[0] ? st.deviation : NULL);
    json_builder_set_member_name(b, "desync_parts");
    json_builder_begin_array(b);
    if (st.desync_parts[0])
    {
        char **v = g_strsplit(st.desync_parts, ",", -1);
        for (int i = 0; v[i]; i++) json_builder_add_string_value(b, v[i]);
        g_strfreev(v);
    };
    json_builder_end_array(b);
    am_json_str_or_null(b, "message", st.outcome_message[0] ? st.outcome_message : NULL);
    json_builder_end_object(b);
}


/**
 * @brief Takty GDG od startu automatizace do teď.
 *
 * Součet po koncích snímků (odolný vůči resetu, který nuluje čítač snímků
 * GDG) plus rozpracovaný snímek. Po resetu nebo načtení snapshotu za běhu
 * je hodnota jen přibližná; počet snímků (`frames`) je přesný vždy.
 */
static uint64_t am_elapsed_ticks_now(void)
{
    uint64_t now = gdg_get_total_ticks();
    uint64_t part = (now >= s_am.last_total_ticks) ? (now - s_am.last_total_ticks) : 0;
    return s_am.elapsed_ticks + part;
}


/**
 * @brief Objekty `time`, `cpu`, `raster`, `hashes` (stav stroje).
 */
static void am_json_machine(JsonBuilder *b)
{
    uint64_t elapsed = am_elapsed_ticks_now();
    json_builder_set_member_name(b, "time");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "frames");
    json_builder_add_int_value(b, (gint64)s_am.frames);
    json_builder_set_member_name(b, "frames_total");
    json_builder_add_int_value(b, (gint64)g_gdg.total_elapsed.screens);
    json_builder_set_member_name(b, "gdg_ticks");
    json_builder_add_int_value(b, (gint64)elapsed);
    json_builder_set_member_name(b, "gdg_ticks_total");
    json_builder_add_int_value(b, (gint64)gdg_get_total_ticks());
    json_builder_set_member_name(b, "emulated_seconds");
    json_builder_add_double_value(b, (double)elapsed / (double)GDGCLK_BASE);
    json_builder_set_member_name(b, "wall_seconds");
    json_builder_add_double_value(b, s_am.started
                                         ? (double)(g_get_monotonic_time() - s_am.wall_start_us) / 1e6
                                         : 0.0);
    json_builder_end_object(b);

    const z80_t *cpu = g_mzarch_main.cpu;
    json_builder_set_member_name(b, "cpu");
    if (!cpu)
    {
        json_builder_add_null_value(b);
    }
    else
    {
        json_builder_begin_object(b);
        const struct { const char *name; unsigned value; } regs[] = {
            { "pc", cpu->pc }, { "sp", cpu->sp }, { "af", cpu->af.w }, { "bc", cpu->bc.w },
            { "de", cpu->de.w }, { "hl", cpu->hl.w }, { "ix", cpu->ix.w }, { "iy", cpu->iy.w },
            { "af2", cpu->af2.w }, { "bc2", cpu->bc2.w }, { "de2", cpu->de2.w }, { "hl2", cpu->hl2.w },
            { "i", cpu->i }, { "r", cpu->r }, { "im", cpu->im }, { "iff1", cpu->iff1 }, { "iff2", cpu->iff2 },
        };
        for (size_t i = 0; i < G_N_ELEMENTS(regs); i++)
        {
            json_builder_set_member_name(b, regs[i].name);
            json_builder_add_int_value(b, regs[i].value);
        };
        json_builder_set_member_name(b, "halted");
        json_builder_add_boolean_value(b, cpu->halted);
        json_builder_end_object(b);
    };

    json_builder_set_member_name(b, "raster");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "beam_row");
    json_builder_add_int_value(b, g_gdg.beam_row);
    json_builder_set_member_name(b, "frame_ticks");
    json_builder_add_int_value(b, g_gdg.total_elapsed.ticks);
    json_builder_end_object(b);

    json_builder_set_member_name(b, "hashes");
    json_builder_begin_object(b);
    am_json_str_or_null(b, "algorithm", "sha256");
    for (int p = 0; p < STATE_HASH_PART_COUNT; p++)
    {
        char hex[STATE_HASH_HEX_LEN + 1];
        if (state_hash_part((en_STATE_HASH_PART)p, hex))
        {
            am_json_str_or_null(b, state_hash_part_name((en_STATE_HASH_PART)p), hex);
        };
    };
    json_builder_end_object(b);
}


/**
 * @brief Cesta z volby jako absolutní (vlastněná) nebo NULL.
 */
static char *am_opt_abs(const char *name)
{
    const char *v = am_opt(name);
    return v ? am_abs_path(v) : NULL;
}


/**
 * @brief Objekty `artifacts`, `inputs` a pole `warnings`.
 *
 * @param screenshot_ok Screenshot se na konci opravdu zapsal.
 * @param snapshot_ok Snapshot se na konci opravdu zapsal.
 */
static void am_json_files(JsonBuilder *b, bool screenshot_ok, bool snapshot_ok)
{
    json_builder_set_member_name(b, "artifacts");
    json_builder_begin_object(b);
    am_json_str_or_null(b, "screenshot", screenshot_ok ? s_am.screenshot_path : NULL);
    am_json_str_or_null(b, "snapshot", snapshot_ok ? s_am.snapshot_path : NULL);
    char *audio = am_opt_abs("--audio-capture");
    am_json_str_or_null(b, "audio_capture", audio);
    g_free(audio);
    char *video = am_opt_abs("--record");
    am_json_str_or_null(b, "video", video);
    g_free(video);
    json_builder_end_object(b);

    json_builder_set_member_name(b, "inputs");
    json_builder_begin_object(b);
    am_json_str_or_null(b, "run_mzf", s_am.run_mzf);
    am_json_str_or_null(b, "bpt", s_am.bpt_path);
    json_builder_end_object(b);

    json_builder_set_member_name(b, "warnings");
    json_builder_begin_array(b);
    if (s_am.warnings)
    {
        for (guint i = 0; i < s_am.warnings->len; i++)
        {
            json_builder_add_string_value(b, (const char *)g_ptr_array_index(s_am.warnings, i));
        };
    };
    json_builder_end_array(b);
}


/**
 * @brief Zapíše `result.json` atomicky (dočasný soubor + přejmenování).
 *
 * @param path Cesta.
 * @param machine true = se stavem stroje (`time`, `cpu`, `raster`, `hashes`,
 *        `coverage`);
 *        false = jen základ (chyba před startem emulace). Objekt `replay`
 *        se zapisuje v obou případech (`null` bez záznamu i přehrávání).
 * @param screenshot_ok, snapshot_ok Viz am_json_files().
 * @return true = zapsáno.
 */
static bool am_write_result_file(const char *path, bool machine, bool screenshot_ok, bool snapshot_ok)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    am_json_str_or_null(b, "schema", AUTOMATION_RESULT_SCHEMA);
    am_json_emulator(b);
    am_json_str_or_null(b, "reason", am_reason_name(s_am.reason));
    json_builder_set_member_name(b, "exit_code");
    json_builder_add_int_value(b, s_am.code);
    am_json_str_or_null(b, "message", s_am.message);
    am_json_trigger(b);
    am_json_replay(b);
    if (machine)
    {
        am_json_machine(b);
        am_json_coverage(b);
    };
    am_json_files(b, screenshot_ok, snapshot_ok);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, TRUE);
    gsize len = 0;
    gchar *data = json_generator_to_data(gen, &len);

    GError *err = NULL;
    bool ok = g_file_set_contents(path, data, (gssize)len, &err);
    if (!ok)
    {
        fprintf(stderr, "[EXIT] cannot write result file '%s': %s\n", path, err ? err->message : "?");
        g_clear_error(&err);
    };
    g_free(data);
    json_node_unref(root);
    g_object_unref(gen);
    g_object_unref(b);
    return ok;
}


/**
 * @brief Zapíše screenshot na konci (`--exit-screenshot` / `--automation-dir`).
 *
 * @return true = PNG zapsán.
 */
static bool am_write_screenshot(void)
{
    if (!s_am.screenshot_path) return false;
    if (!g_framebuffer.pixels)
    {
        am_warn("screenshot not written: framebuffer not rendered yet");
        return false;
    };
    st_SCREENSHOT_OPTS opts;
    screenshot_opts_init(&opts);
    st_SCREENSHOT_PNG png;
    char reason[128] = "";
    if (!screenshot_encode_png(g_framebuffer.pixels, VIDEO_DISPLAY_WIDTH, &opts, &png, reason, sizeof(reason)))
    {
        am_warn("screenshot not written: %s", reason);
        return false;
    };
    GError *err = NULL;
    bool ok = g_file_set_contents(s_am.screenshot_path, (const gchar *)png.png, (gssize)png.png_len, &err);
    if (!ok)
    {
        am_warn("screenshot not written to '%s': %s", s_am.screenshot_path, err ? err->message : "?");
        g_clear_error(&err);
    };
    g_free(png.png);
    return ok;
}


/**
 * @brief Zapíše snapshot na konci (`--exit-snapshot`); jen v pauze.
 *
 * @return true = snapshot uložen.
 */
static bool am_write_snapshot(void)
{
    if (!s_am.snapshot_path) return false;
    en_SNAPSHOT_RESULT r = snapshot_save(s_am.snapshot_path, "Automation exit");
    if (r != SNAPSHOT_OK)
    {
        am_warn("snapshot not written to '%s' (error %d)", s_am.snapshot_path, (int)r);
        return false;
    };
    return true;
}


/**
 * @brief Zapíše artefakty a `result.json` podle uložené žádosti.
 *
 * @param in_pause true = emulace stojí v pauze (snapshot smí vzniknout).
 */
static void am_write_outputs(bool in_pause)
{
    bool shot = am_write_screenshot();
    bool snap = false;
    if (s_am.snapshot_path)
    {
        if (in_pause) snap = am_write_snapshot();
        else am_warn("snapshot not written: the run ended outside a pause");
    };
    if (s_am.result_path)
    {
        if (am_write_result_file(s_am.result_path, true, shot, snap))
        {
            s_am.result_written = true;
        };
    };
}


/**
 * @brief Uloží parametry žádosti do s_am (kopie řetězců).
 */
static void am_store_request(int code, en_AUTOMATION_REASON reason, const char *message,
                             const st_AUTOMATION_TRIGGER *trigger)
{
    s_am.code = code;
    s_am.reason = reason;
    g_free(s_am.message);
    s_am.message = g_strdup(message);
    g_free(s_am.trig_bp_name);
    g_free(s_am.trig_bp_type);
    s_am.trig_bp_name = NULL;
    s_am.trig_bp_type = NULL;
    s_am.trig_bp_id = -1;
    s_am.trig_hits = 0;
    if (trigger && trigger->bp_id >= 0)
    {
        s_am.trig_bp_id = trigger->bp_id;
        s_am.trig_bp_name = g_strdup(trigger->bp_name);
        s_am.trig_bp_type = g_strdup(trigger->bp_type);
        s_am.trig_hits = trigger->hits;
    };
}


/*******************************************************************************
 *
 *                  Hlavní vlákno: volby
 *
 ******************************************************************************/

bool automation_cli_requested(void)
{
    for (int i = 0; s_am_options[i]; i++)
    {
        if (sdlapp_option_present(s_am_options[i])) return true;
    };
    return false;
}


bool automation_cli_validate(void)
{
    s_am.requested = automation_cli_requested();
    if (!s_am.requested) return true;

    bool ok = true;
    const char *v;
    uint64_t n;

    if ((v = am_opt("--exit-after-frames")))
    {
        if (!am_parse_uint(v, G_MAXINT64, &n) || n == 0)
        {
            fprintf(stderr, "Invalid value for --exit-after-frames: '%s' (expected a positive integer)\n", v);
            ok = false;
        };
        s_am.exit_after_frames = n;
    };

    const char *tf = am_opt("--timeout-frames");
    const char *ts = am_opt("--timeout-seconds");
    if (tf && ts)
    {
        fprintf(stderr, "Use only one of --timeout-frames and --timeout-seconds\n");
        ok = false;
    }
    else if (tf)
    {
        if (!am_parse_uint(tf, G_MAXINT64, &n) || n == 0)
        {
            fprintf(stderr, "Invalid value for --timeout-frames: '%s' (expected a positive integer)\n", tf);
            ok = false;
        };
        s_am.timeout_frames = n;
    }
    else if (ts)
    {
        char *end = NULL;
        double sec = g_ascii_strtod(ts, &end);
        if (!end || *end || !isfinite(sec) || sec <= 0.0 || sec > 1e9)
        {
            fprintf(stderr, "Invalid value for --timeout-seconds: '%s' (expected a positive number of emulated seconds)\n", ts);
            ok = false;
        }
        else
        {
            /* Emulovaný čas zaokrouhlený nahoru na celé snímky. */
            s_am.timeout_frames = (uint64_t)ceil(sec * (double)VIDEO_SCREENS_PER_SEC);
        };
    };

    if ((v = am_opt("--automation-dir")))
    {
        s_am.dir = am_abs_path(v);
        if (g_mkdir_with_parents(s_am.dir, 0755) != 0)
        {
            fprintf(stderr, "Cannot create --automation-dir '%s'\n", s_am.dir);
            ok = false;
        };
    };
    if ((v = am_opt("--result-json"))) s_am.result_path = am_abs_path(v);
    else if (s_am.dir) s_am.result_path = g_build_filename(s_am.dir, AUTOMATION_DIR_RESULT, NULL);
    if ((v = am_opt("--exit-screenshot"))) s_am.screenshot_path = am_abs_path(v);
    else if (s_am.dir) s_am.screenshot_path = g_build_filename(s_am.dir, AUTOMATION_DIR_SCREENSHOT, NULL);
    if ((v = am_opt("--exit-snapshot"))) s_am.snapshot_path = am_abs_path(v);

    if ((v = am_opt("--run-mzf")))
    {
        s_am.run_mzf = am_abs_path(v);
        if (!g_file_test(s_am.run_mzf, G_FILE_TEST_IS_REGULAR))
        {
            fprintf(stderr, "Cannot open --run-mzf file '%s'\n", v);
            ok = false;
        };
    };

    if ((v = am_opt("--bpt")))
    {
        s_am.bpt_path = am_abs_path(v);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        if (!g_file_test(s_am.bpt_path, G_FILE_TEST_IS_REGULAR))
        {
            fprintf(stderr, "Cannot open --bpt file '%s'\n", v);
            ok = false;
        };
#else
        fprintf(stderr, "--bpt requires a build with the debugger\n");
        ok = false;
#endif
    };

    s_am.cli_bps = g_array_new(FALSE, TRUE, sizeof(st_AM_CLI_BP));
    for (int k = 0; k < AM_CLI_BP_COUNT; k++)
    {
        const char *opt = s_am_cli_bp_option[k];
        int cnt = sdlapp_option_count(opt);
        for (int i = 0; i < cnt; i++)
        {
#ifndef MZ800EMU_CFG_DEBUGGER_ENABLED
            fprintf(stderr, "%s requires a build with the debugger\n", opt);
            ok = false;
            break;
#else
            const char *spec = sdlapp_option_value_nth(opt, i);
            st_AM_CLI_BP bp;
            if (!spec || !am_parse_cli_bp((en_AM_CLI_BP)k, spec, &bp))
            {
                if (!spec) fprintf(stderr, "Option %s requires a value\n", opt);
                ok = false;
                continue;
            };
            g_array_append_val(s_am.cli_bps, bp);
#endif
        };
    };

    s_am.validated = ok;
    return ok;
}


bool automation_is_active(void)
{
    return s_am.requested;
}


int automation_cli_error(int default_code, const char *message)
{
    if (!automation_cli_requested()) return default_code;
    s_am.requested = true;

    /* Cesta výsledku: z validace, nebo přímo z voleb (validace mohla
     * skončit dřív, než ji určila). */
    char *path = s_am.result_path ? g_strdup(s_am.result_path) : NULL;
    if (!path)
    {
        const char *v = am_opt("--result-json");
        const char *d = am_opt("--automation-dir");
        if (v && *v) path = am_abs_path(v);
        else if (d && *d)
        {
            char *dir = am_abs_path(d);
            if (g_mkdir_with_parents(dir, 0755) == 0) path = g_build_filename(dir, AUTOMATION_DIR_RESULT, NULL);
            g_free(dir);
        };
    };
    am_store_request(AUTOMATION_EXIT_ERROR, AUTOMATION_REASON_ERROR, message, NULL);
    if (path)
    {
        if (am_write_result_file(path, false, false, false)) s_am.result_written = true;
        g_free(path);
    };
    return AUTOMATION_EXIT_ERROR;
}


/*******************************************************************************
 *
 *                  Emu vlákno: běh
 *
 ******************************************************************************/

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

/**
 * @brief Založí breakpoint pro jednu specifikaci z příkazové řádky.
 *
 * @return true = breakpoint založen a zapnutý; false = chyba (vypsaná).
 */
static bool am_create_cli_bp(st_AM_CLI_BP *b, int index)
{
    const char *opt = s_am_cli_bp_option[b->kind];
    char name[48];
    g_snprintf(name, sizeof(name), "cli%s-%d", opt + 1, index + 1);

    char *expr = NULL;
    char *action;
    if (b->kind == AM_CLI_EXIT_ON_WRITE && b->has_value)
    {
        expr = g_strdup_printf("Value == 0x%02X", b->value);
    };
    char *code = (b->code < 0) ? g_strdup("Value") : g_strdup_printf("%d", b->code);
    action = g_strdup_printf("exit %s, \"%s %s\"", code, opt, b->spec);
    g_free(code);

    st_DBGAPI_BP_UPDATE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.id = -1;
    p.update_mask = DBGAPI_BP_UM_AUTO_NAME | DBGAPI_BP_UM_NAME | DBGAPI_BP_UM_TYPE
                  | DBGAPI_BP_UM_ACTION | DBGAPI_BP_UM_SKIP_COUNT;
    p.auto_name = false; /* jinak by jméno přepsalo automatické "Addr: ..." */
    p.name = name;
    p.action = action;
    p.skip_count = b->skip;
    switch (b->kind)
    {
        case AM_CLI_EXIT_ON_PC:
        case AM_CLI_FAIL_ON_PC:
            p.type = BPT_TYPE_PC_EXEC;
            p.update_mask |= DBGAPI_BP_UM_ADDR;
            p.addr = b->addr;
            break;
        case AM_CLI_EXIT_ON_WRITE:
            p.type = BPT_TYPE_MEM_W;
            p.update_mask |= DBGAPI_BP_UM_ADDR;
            p.addr = b->addr;
            break;
        case AM_CLI_EXIT_ON_OUT:
        default:
            p.type = BPT_TYPE_IORQ_W;
            p.update_mask |= DBGAPI_BP_UM_PORT;
            p.port = b->addr;
            break;
    };
    if (expr)
    {
        p.update_mask |= DBGAPI_BP_UM_EXPR;
        p.expr = expr;
    };

    bool ok = dbgapi_emu_bp_create(&p) && p.id >= 0;
    if (ok)
    {
        /* Založení na obsazené adrese nechá breakpoint vypnutý; zapnutí
         * pak konflikt odhalí. */
        breakpoints_set_enabled(p.id, true);
        st_BPT *bpt = breakpoints_find_by_id(p.id);
        ok = bpt && bpt->enabled;
        b->bp_id = p.id;
    };
    if (!ok)
    {
        fprintf(stderr, "[EXIT] %s %s: cannot create the breakpoint (another breakpoint at the same place?)\n",
                opt, b->spec);
    };
    g_free(expr);
    g_free(action);
    return ok;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */


void automation_start(void)
{
    if (!s_am.requested) return;
    s_am.started = true;
    s_am.frames = 0;
    s_am.elapsed_ticks = 0;
    s_am.last_total_ticks = gdg_get_total_ticks();
    s_am.wall_start_us = g_get_monotonic_time();
    g_automation_frame_hook = true;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Výchozí sada se v automatickém běhu nenačetla (breakpoints_init,
     * automation_is_active) a při konci se nesmí uložit přes uživatelovu. */
    breakpoints_suppress_auto_save();
    if (s_am.bpt_path || (s_am.cli_bps && s_am.cli_bps->len))
    {
        if (s_am.bpt_path)
        {
            breakpoints_load_from_filepath(s_am.bpt_path);
        };
        for (guint i = 0; i < s_am.cli_bps->len; i++)
        {
            st_AM_CLI_BP *b = &g_array_index(s_am.cli_bps, st_AM_CLI_BP, i);
            if (!am_create_cli_bp(b, (int)i))
            {
                char *msg = g_strdup_printf("cannot create breakpoint for %s %s",
                                            s_am_cli_bp_option[b->kind], b->spec);
                automation_request_exit(AUTOMATION_EXIT_ERROR, AUTOMATION_REASON_ERROR, msg, NULL);
                g_free(msg);
                break;
            };
        };
        /* MEM_W a IORQ_W breakpointy potřebují logovací callbacky. */
        dbgapi_emu_bp_recompute_gating();
    };
#endif
}


void automation_on_screen_done(void)
{
    uint64_t now = gdg_get_total_ticks();
    /* Reset nebo načtení snapshotu čas GDG vrátí: rozpracovaný úsek pak
     * počítáme od nuly (viz am_elapsed_ticks_now()). */
    s_am.elapsed_ticks += (now >= s_am.last_total_ticks) ? (now - s_am.last_total_ticks) : now;
    s_am.last_total_ticks = now;
    s_am.frames++;

    if (s_am.exit_after_frames && s_am.frames >= s_am.exit_after_frames)
    {
        s_am.exit_after_frames = 0; /* jen jednou (v GUI se dá pokračovat) */
        char msg[96];
        g_snprintf(msg, sizeof(msg), "%" G_GUINT64_FORMAT " frames done", s_am.frames);
        automation_request_exit(AUTOMATION_EXIT_SUCCESS, AUTOMATION_REASON_FRAMES, msg, NULL);
    }
    else if (s_am.timeout_frames && s_am.frames >= s_am.timeout_frames)
    {
        s_am.timeout_frames = 0;
        char msg[96];
        g_snprintf(msg, sizeof(msg), "timeout after %" G_GUINT64_FORMAT " frames", s_am.frames);
        automation_request_exit(AUTOMATION_EXIT_TIMEOUT, AUTOMATION_REASON_TIMEOUT, msg, NULL);
    };
}


void automation_request_exit(int code, en_AUTOMATION_REASON reason, const char *message,
                             const st_AUTOMATION_TRIGGER *trigger)
{
    if (s_am.pending) return; /* vyhrává první žádost */
    am_store_request(code & 0xFF, reason, message, trigger);
    s_am.pending = true;

    fprintf(stderr, "[EXIT] exit code %d (%s)%s%s\n", s_am.code, am_reason_name(reason),
            message ? ": " : "", message ? message : "");

    g_emulator.pause_reason = EMU_PAUSE_REASON_EXIT;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Událost "paused" s reason "exit", exit_code, exit_reason a message
     * vyšle záznam zastavení na vstupu do pauzy (run_control_on_stop()). */
    run_control_note_exit(s_am.code, am_reason_name(reason), message);
#endif
    emulator_pause(true);
}


void automation_on_paused(void)
{
    if (!s_am.pending) return;
    s_am.pending = false;

    am_write_outputs(true);
    if (am_may_terminate())
    {
        fprintf(stderr, "[EXIT] exiting with code %d\n", s_am.code);
        emulator_quit_with_code(s_am.code);
    };
    fprintf(stderr, "[EXIT] the process keeps running (GUI or MCP session); emulation paused\n");
}


void automation_on_quit(void)
{
    if (!s_am.requested || s_am.result_written || !s_am.result_path) return;
    am_store_request(AUTOMATION_EXIT_SUCCESS, AUTOMATION_REASON_QUIT,
                     "the process was ended before an exit condition", NULL);
    am_write_outputs(false);
}


int automation_on_crash(int exit_value)
{
    if (!s_am.requested) return exit_value;
    am_store_request(AUTOMATION_EXIT_ERROR, AUTOMATION_REASON_ERROR, "emulator error (abnormal exit)", NULL);
    if (s_am.result_path && !s_am.result_written)
    {
        if (am_write_result_file(s_am.result_path, s_am.started, false, false)) s_am.result_written = true;
    };
    return AUTOMATION_EXIT_ERROR;
}
