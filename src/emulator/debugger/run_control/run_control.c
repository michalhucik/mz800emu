/**
 * @file run_control.c
 * @brief Řízení běhu z pohledu emu vlákna - implementace (viz run_control.h).
 *
 * Stav modulu (s_rc) čte a zapisuje jen emu vlákno, proto nemá zámek.
 * Ven se publikuje jen přes run_log (vlastní zámek) a event_bus.
 */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <string.h>
#include <glib.h>

#include "run_control.h"
#include "run_log.h"
#include "emulator.h"
#include "debugger/debugger.h"
#include "debugger/bptmap.h"
#include "debugger/dbgapi_cmdrq.h"
#include "mzarch/mzarch.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include <json-glib/json-glib.h>
#include "../mcp/event_bus.h"
#endif


/**
 * @brief Stav řízení běhu (jen emu vlákno).
 *
 * `note_*` jsou detaily od zdroje pauzy nebo rozběhu, platné do nejbližšího
 * run_control_on_stop() / run_control_on_resume(), které je spotřebuje.
 * Invariant: řetězcové ukazatele jsou statické literály nebo NULL.
 */
static struct
{
    uint64_t start_mono;           /**< Monotónní čas při spuštění aktuálního běhu. */
    uint64_t start_ticks;          /**< gdg_get_total_ticks() při spuštění aktuálního běhu. */
    const char *started_by;        /**< Původ, který spustil aktuální běh. */
    const char *start_cmd;         /**< Příkaz, který spustil aktuální běh. */
    bool step;                     /**< Aktuální běh je krok jedné instrukce. */

    const char *note_start_by;     /**< Původ příkazu, který právě rozběhl emulaci (NULL = mimo frontu). */
    const char *note_start_cmd;    /**< Ten příkaz. */
    const char *note_stopped_by;   /**< Původ příkazu, který emulaci pozastavil (NULL = žádný). */
    int note_bp_id;                /**< ID breakpointu zastavení, -1 = žádný. */
    uint64_t note_bytes;           /**< Objem zápisů u saturace akcí. */
    int note_exit_code;            /**< Návratový kód u "exit", -1 = žádný. */
    const char *note_exit_reason;  /**< Důvod žádosti o konec, NULL = žádný. */
    char note_detail[RUN_LOG_DETAIL_SIZE]; /**< Text detailu, "" = žádný. */
} s_rc = {
    .started_by = "internal",
    .start_cmd = "startup",
    .note_bp_id = -1,
    .note_exit_code = -1,
};


/**
 * @brief Jméno původu příkazu dbgapi pro záznamy a události.
 *
 * @param origin Hodnota en_DBGAPI_CMD_ORIGIN.
 * @return Statický řetězec.
 */
static const char *run_control_origin_name(int origin)
{
    switch (origin)
    {
        case DBGAPI_CMD_ORIGIN_USER: return "user";
        case DBGAPI_CMD_ORIGIN_MCP: return "mcp";
        case DBGAPI_CMD_ORIGIN_TEST: return "test";
        case DBGAPI_CMD_ORIGIN_INTERNAL: return "internal";
        default: break;
    };
    return "unknown";
}


/**
 * @brief Jméno důvodu zastavení pro záznam a událost `paused`.
 *
 * @param reason Důvod pauzy.
 * @return Statický řetězec (stejná jména jako dřívější události `paused`).
 */
static const char *run_control_reason_name(en_EMU_PAUSE_REASON reason)
{
    switch (reason)
    {
        case EMU_PAUSE_REASON_FRAMES: return "frames";
        case EMU_PAUSE_REASON_BREAKPOINT: return "breakpoint";
        case EMU_PAUSE_REASON_MANUAL: return "manual";
        case EMU_PAUSE_REASON_RUN_TO: return "run_to";
        case EMU_PAUSE_REASON_EXIT: return "exit";
        case EMU_PAUSE_REASON_DEADLINE: return "deadline";
        case EMU_PAUSE_REASON_STEP: return "step";
        case EMU_PAUSE_REASON_BP_SATURATION: return "bp_action_saturation";
        case EMU_PAUSE_REASON_EVENT_TRIGGER: return "event_trigger";
        case EMU_PAUSE_REASON_REPLAY: return "replay";
        case EMU_PAUSE_REASON_REPLAY_END: return "replay_end";
        case EMU_PAUSE_REASON_REPLAY_DESYNC: return "replay_desync";
        case EMU_PAUSE_REASON_REPLAY_SEEK: return "replay_seek";
        case EMU_PAUSE_REASON_RUN_UNTIL_TICKS: return "run_until_ticks";
        case EMU_PAUSE_REASON_REPLAY_GOTO: return "replay_goto";
        case EMU_PAUSE_REASON_NONE:
        default: break;
    };
    return "manual";
}


/** @brief Zahodí detaily zastavení předané run_control_note_*(). */
static void run_control_clear_stop_notes(void)
{
    s_rc.note_stopped_by = NULL;
    s_rc.note_bp_id = -1;
    s_rc.note_bytes = 0;
    s_rc.note_exit_code = -1;
    s_rc.note_exit_reason = NULL;
    s_rc.note_detail[0] = '\0';
}


uint64_t run_control_mono_ticks(void)
{
    /* Čítač času GDG reset stroje nenuluje (nuluje ho jen gdg_init() při
     * startu, snapshot ho nastaví na uloženou hodnotu). */
    return gdg_get_total_ticks();
}


#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
/**
 * @brief Vyšle událost `paused` ze záznamu zastavení.
 *
 * Pole `reason` a `pc` mají stejný tvar jako dřívější události; u důvodu
 * "exit" navíc `exit_code`, `exit_reason`, `message`, u saturace `bp_id`,
 * `bytes`, `detail`.
 *
 * @param rec Záznam po run_log_end() (s platným run_seq).
 */
static void run_control_emit_paused(const st_RUN_LOG_RECORD *rec)
{
    if (!event_bus_has_subscriber("paused")) return;
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "reason", rec->reason);
    json_object_set_int_member(p, "pc", rec->pc);
    json_object_set_int_member(p, "run_seq", (gint64)rec->run_seq);
    json_object_set_boolean_member(p, "step", rec->step ? TRUE : FALSE);
    json_object_set_int_member(p, "stop_ticks", (gint64)rec->stop_ticks);
    json_object_set_int_member(p, "start_ticks", (gint64)rec->start_ticks);
    json_object_set_int_member(p, "elapsed_ticks", (gint64)rec->elapsed_ticks);
    json_object_set_int_member(p, "frame", (gint64)rec->stop_frame);
    json_object_set_string_member(p, "started_by", rec->started_by);
    json_object_set_string_member(p, "start_cmd", rec->start_cmd);
    json_object_set_string_member(p, "stopped_by", rec->stopped_by);
    if (rec->bp_id >= 0) json_object_set_int_member(p, "bp_id", rec->bp_id);
    if (strcmp(rec->reason, "exit") == 0)
    {
        json_object_set_int_member(p, "exit_code", rec->exit_code);
        json_object_set_string_member(p, "exit_reason", rec->exit_reason);
        if (rec->detail[0]) json_object_set_string_member(p, "message", rec->detail);
    }
    else if (strcmp(rec->reason, "bp_action_saturation") == 0)
    {
        json_object_set_int_member(p, "bytes", (gint64)rec->bytes);
        json_object_set_string_member(p, "detail", rec->detail);
    };
    event_bus_emit("paused", p);
}


/**
 * @brief Vyšle událost `resumed` o právě spuštěném běhu.
 *
 * @param run_seq Číslo běhu.
 */
static void run_control_emit_resumed(uint64_t run_seq)
{
    if (!event_bus_has_subscriber("resumed")) return;
    JsonObject *p = json_object_new();
    json_object_set_int_member(p, "run_seq", (gint64)run_seq);
    json_object_set_int_member(p, "pc", g_mzarch_main.cpu ? g_mzarch_main.cpu->pc : 0);
    json_object_set_string_member(p, "started_by", s_rc.started_by);
    json_object_set_string_member(p, "start_cmd", s_rc.start_cmd);
    json_object_set_int_member(p, "start_ticks", (gint64)s_rc.start_ticks);
    json_object_set_boolean_member(p, "deadline", g_debugger.run_deadline_active ? TRUE : FALSE);
    event_bus_emit("resumed", p);
}
#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */


void run_control_on_stop(void)
{
    en_EMU_PAUSE_REASON why = g_emulator.pause_reason;
    if (why == EMU_PAUSE_REASON_NONE)
    {
        why = TEST_DEBUGGER_STEP_CALL ? EMU_PAUSE_REASON_STEP : EMU_PAUSE_REASON_MANUAL;
    };

    /* Deadline i frame-bounded run platí jen do pauzy: běh přerušený
     * breakpointem nebo uživatelem by jinak další, nesouvisející běh
     * zastavil na starém cíli. */
    g_debugger.run_deadline_active = 0;
    g_debugger.run_frames_active = 0;

    st_RUN_LOG_RECORD rec;
    run_log_record_clear(&rec);
    rec.reason = run_control_reason_name(why);
    rec.started_by = s_rc.started_by;
    rec.start_cmd = s_rc.start_cmd;
    rec.stopped_by = (why == EMU_PAUSE_REASON_MANUAL)
                     ? (s_rc.note_stopped_by ? s_rc.note_stopped_by : "user")
                     : "emulator";
    rec.step = TEST_DEBUGGER_STEP_CALL ? true : false;
    rec.pc = g_mzarch_main.cpu ? (uint16_t)g_mzarch_main.cpu->pc : 0;
    rec.bp_id = s_rc.note_bp_id;
    rec.start_ticks = s_rc.start_ticks;
    rec.stop_ticks = gdg_get_total_ticks();
    uint64_t mono = run_control_mono_ticks();
    rec.elapsed_ticks = (mono >= s_rc.start_mono) ? (mono - s_rc.start_mono) : 0;
    rec.stop_frame = (uint32_t)g_gdg.total_elapsed.screens;
    if (why == EMU_PAUSE_REASON_EXIT)
    {
        rec.exit_code = s_rc.note_exit_code;
        rec.exit_reason = s_rc.note_exit_reason ? s_rc.note_exit_reason : "";
    };
    rec.bytes = s_rc.note_bytes;
    g_strlcpy(rec.detail, s_rc.note_detail, sizeof(rec.detail));
    run_control_clear_stop_notes();

    run_log_end(&rec);

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    rec.run_seq = run_log_current(NULL);
    run_control_emit_paused(&rec);
#endif
}


void run_control_on_resume(void)
{
    uint64_t seq = run_log_begin();
    g_emulator.pause_reason = EMU_PAUSE_REASON_NONE;
    /* Rozběh mimo frontu dbgapi (klávesa Pause v GUI) jde jen z UI vlákna. */
    s_rc.started_by = s_rc.note_start_by ? s_rc.note_start_by : "user";
    s_rc.start_cmd = s_rc.note_start_cmd ? s_rc.note_start_cmd : "external";
    s_rc.note_start_by = NULL;
    s_rc.note_start_cmd = NULL;
    s_rc.step = TEST_DEBUGGER_STEP_CALL ? true : false;
    s_rc.start_mono = run_control_mono_ticks();
    s_rc.start_ticks = gdg_get_total_ticks();
    /* Zbytky detailů zastavení z doby pauzy (např. příkaz, který nic
     * nezastavil) do nového běhu nepatří. */
    run_control_clear_stop_notes();

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    if (!s_rc.step) run_control_emit_resumed(seq);
#else
    (void)seq;
#endif
}


void run_control_after_cmd(unsigned cmd, int origin, bool was_paused, bool was_step)
{
    bool now_paused = EMULATOR_TEST_PAUSED ? true : false;
    bool now_step = TEST_DEBUGGER_STEP_CALL ? true : false;

    if (was_paused && (!now_paused || (now_step && !was_step)))
    {
        s_rc.note_start_by = run_control_origin_name(origin);
        s_rc.note_start_cmd = dbgapi_cmd_to_str((en_DBGAPI_CMD)cmd);
    }
    else if (!was_paused && now_paused)
    {
        if (g_emulator.pause_reason == EMU_PAUSE_REASON_NONE)
        {
            g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
        };
        s_rc.note_stopped_by = run_control_origin_name(origin);
    };
}


void run_control_note_breakpoint(int bp_id)
{
    s_rc.note_bp_id = bp_id;
}


void run_control_note_saturation(int bp_id, uint64_t bytes, const char *detail)
{
    s_rc.note_bp_id = bp_id;
    s_rc.note_bytes = bytes;
    g_strlcpy(s_rc.note_detail, detail ? detail : "", sizeof(s_rc.note_detail));
}


void run_control_note_exit(int code, const char *exit_reason, const char *message)
{
    s_rc.note_exit_code = code;
    s_rc.note_exit_reason = exit_reason;
    g_strlcpy(s_rc.note_detail, message ? message : "", sizeof(s_rc.note_detail));
}


bool run_control_deadline_arm(en_RUN_DEADLINE_KIND kind, uint64_t value, uint64_t *out_ticks)
{
    uint64_t now = run_control_mono_ticks();
    uint64_t target;
    switch (kind)
    {
        case RUN_DEADLINE_FRAMES:
            if (value < 1 || value > RUN_DEADLINE_MAX_FRAMES) return false;
            /* Hranice snímku: čas screens * VIDEO_SCREEN_TICKS. */
            target = ((uint64_t)g_gdg.total_elapsed.screens + value) * (uint64_t)VIDEO_SCREEN_TICKS;
            break;
        case RUN_DEADLINE_EMULATED_MS:
            if (value < 1 || value > RUN_DEADLINE_MAX_MS) return false;
            target = now + value * (uint64_t)GDGCLK_BASE / 1000u;
            break;
        case RUN_DEADLINE_TICKS:
            if (value < 1 || value > (UINT64_MAX / 4)) return false;
            target = now + value;
            break;
        default:
            return false;
    };
    g_debugger.run_deadline_ticks = target;
    g_debugger.run_deadline_active = 1;
    if (out_ticks) *out_ticks = (target > now) ? (target - now) : 0;
    return true;
}


void run_control_deadline_clear(void)
{
    g_debugger.run_deadline_active = 0;
}


void run_control_stop_at_arm(uint64_t ticks, int reason)
{
    g_debugger.stop_at_ticks = ticks;
    g_debugger.stop_at_reason = reason;
    g_debugger.stop_at_active = 1;
    bptmap_set_global_hook(true);
}


void run_control_stop_at_clear(void)
{
    if (!g_debugger.stop_at_active) return;
    g_debugger.stop_at_active = 0;
    bptmap_set_global_hook(false);
}


bool run_control_stop_at_check(void)
{
    if (!g_debugger.stop_at_active || gdg_get_total_ticks() < g_debugger.stop_at_ticks) return false;
    en_EMU_PAUSE_REASON why = (en_EMU_PAUSE_REASON)g_debugger.stop_at_reason;
    run_control_stop_at_clear();
    g_emulator.pause_reason = why;
    emulator_pause(true);
    return true;
}


void run_control_check_run_bounds(void)
{
    if (g_debugger.run_frames_active
        && g_gdg.total_elapsed.screens >= g_debugger.run_frames_target)
    {
        g_debugger.run_frames_active = 0;
        g_emulator.pause_reason = EMU_PAUSE_REASON_FRAMES;
        emulator_pause(true);
    }
    else if (g_debugger.run_deadline_active
             && run_control_mono_ticks() >= g_debugger.run_deadline_ticks)
    {
        g_debugger.run_deadline_active = 0;
        g_emulator.pause_reason = EMU_PAUSE_REASON_DEADLINE;
        emulator_pause(true);
    };
}


uint64_t run_control_seq_for_start(void)
{
    bool running = false;
    uint64_t seq = run_log_current(&running);
    return running ? seq : seq + 1;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
