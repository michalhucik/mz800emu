/*
 * dbgapi_helpers.cpp - implementace UI helperů nad dbgapi CMDRQ
 *
 * Každý helper je tenký wrapper nad dbgapi_ui_submit_cmd_sync() s
 * default timeoutem DBG_UI_DEFAULT_TIMEOUT_MS. Parametrické struktury
 * jsou alokované na stacku - submit_cmd_sync je synchronní, takže do
 * návratu helperu jsou data v platnosti pro EMU stranu.
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "debugger/dbgapi_ui.h"
#include "debugger/dbgapi_cmdrq.h"
#include "debugger/debugger.h"
#include "debugger/mhmap.h"
#include "debugger/eventlog_filter.h"

#include <string.h>
#include "dbgapi_helpers.h"


bool dbg_ui_pause(void)
{
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_PAUSE,
                                     NULL, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_run(void)
{
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_RUN,
                                     NULL, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_pause_toggle(void)
{
    /* Aktuální stav zjistíme přes CMD_IS_RUNNING. Alternativa: číst
     * přímo EMULATOR_TEST_PAUSED z UI vlákna - to je atomické čtení
     * jednoho bitu, ale obchází to abstrakci dbgapi. Pro V1 použijeme
     * dvě CMDRQ aby UI vůbec nemuselo sahat do g_emulator stavu. */
    bool is_running = false;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_IS_RUNNING,
                                        NULL, &is_running,
                                        DBG_UI_DEFAULT_TIMEOUT_MS);
    if (!ok)
        return false;

    if (is_running)
        return dbg_ui_pause();
    return dbg_ui_run();
}


bool dbg_ui_reset(void)
{
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_RESET,
                                     NULL, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_debugger_state_recompute(void)
{
    /* Deleguje mzarch_platform_fn_debugger_state_changed na EMU vlákno
     * (DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE) místo přímého volání z UI vlákna.
     * Nutné pro trace-suite: stop kanálu uvolní writer buffer (tlog_writer_close)
     * - kdyby to běželo na UI vlákně souběžně s emu-thread tlog_writer_append,
     * vznikne use-after-free race. Volající si PŘEDEM nastaví mode/flag
     * (cfg.mode = ... apod., atomický int zápis), pak zavolá tuto funkci. */
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE,
                                     NULL, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_step_into(void)
{
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_STEP_INTO,
                                     NULL, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_step_over(void)
{
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_STEP_OVER,
                                     NULL, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_run_to(uint16_t addr)
{
    /* data_ptr ukazuje na lokální uint16_t. Po návratu submit_cmd_sync()
     * EMU strana data už nečte - synchronní kontrakt CMDRQ. */
    uint16_t target = addr;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_RUN_TO,
                                     &target, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_set_reg(uint8_t reg_id, uint16_t value)
{
    st_DBGAPI_REG_PARAM p;
    p.reg_id = reg_id;
    p.value = value;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_SET_REG,
                                     &p, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_mem_write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    if (!buf || len == 0)
        return false;

    st_DBGAPI_MEM_PARAM p;
    p.addr = addr;
    p.len = len;
    /* Cast const-away: dispatch v dbgapi.c jen čte z p.buf, neměnit. */
    p.buf = (uint8_t *)buf;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_MEM_WRITE,
                                     &p, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_bp_add(uint16_t addr, int *out_id)
{
    st_DBGAPI_BP_PARAM p;
    p.addr = addr;
    p.id = -1;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_BP_ADD,
                                        &p, NULL,
                                        DBG_UI_DEFAULT_TIMEOUT_MS);
    if (ok && out_id)
        *out_id = p.id;
    return ok;
}


bool dbg_ui_bp_remove(int id)
{
    st_DBGAPI_BP_PARAM p;
    p.addr = 0;
    p.id = id;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_BP_REMOVE,
                                     &p, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_bp_update(const st_DBGAPI_BP_UPDATE_PARAM *p)
{
    if (!p) return false;
    /* Const-cast: dispatch handler v UPDATE cestě (allow_create=false)
     * payload jen čte. CREATE cesta (allow_create=true) zapisuje do
     * p->id - tam volá caller dbg_ui_bp_create_with_init(), který
     * dostává non-const pointer. */
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_BP_UPDATE,
                                     (void *)p, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_bp_set_enabled(int id, bool enabled)
{
    st_DBGAPI_BP_SET_ENABLED_PARAM p;
    p.id = id;
    p.enabled = enabled;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_BP_SET_ENABLED,
                                     &p, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_bp_set_parent(int id, int parent_id)
{
    st_DBGAPI_BP_SET_PARENT_PARAM p;
    p.id = id;
    p.parent_id = parent_id;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_BP_SET_PARENT,
                                     &p, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_bp_create_with_init(st_DBGAPI_BP_UPDATE_PARAM *p, int *out_id)
{
    if (!p) return false;
    p->id = -1;  /* CREATE invariant - handler odmítne jinou hodnotu */
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_BP_CREATE_WITH_INIT,
                                        p, NULL,
                                        DBG_UI_DEFAULT_TIMEOUT_MS);
    if (ok && out_id)
        *out_id = p->id;
    return ok;
}


bool dbg_ui_bpgrp_add(const char *name, int parent, int *out_id)
{
    st_DBGAPI_BPGRP_ADD_PARAM p;
    p.name = name;
    p.parent = parent;
    p.id = -1;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_BPGRP_ADD,
                                        &p, NULL,
                                        DBG_UI_DEFAULT_TIMEOUT_MS);
    if (ok && out_id)
        *out_id = p.id;
    return ok;
}


bool dbg_ui_bpgrp_remove(int id)
{
    st_DBGAPI_BPGRP_REMOVE_PARAM p;
    p.id = id;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_BPGRP_REMOVE,
                                     &p, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}


bool dbg_ui_bpgrp_update(const st_DBGAPI_BPGRP_UPDATE_PARAM *p)
{
    if (!p) return false;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_BPGRP_UPDATE,
                                     (void *)p, NULL,
                                     DBG_UI_DEFAULT_TIMEOUT_MS);
}

bool dbg_ui_io_history_set_capacity(uint32_t capacity, uint32_t *out_capacity_after)
{
    /* Realokace ringu (free + calloc) musí běžet na emu vlákně: to do ringu
     * zapisuje v io_history_record(). Param žije na zásobníku, synchronní
     * submit ho po dokončení příkazu už nepoužívá. */
    st_DBGAPI_IO_HISTORY_CAPACITY_PARAM p;
    p.capacity = capacity;
    p.capacity_after = 0;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_IO_HISTORY_SET_CAPACITY,
                                        &p, NULL,
                                        DBG_UI_IO_CMD_TIMEOUT_MS);
    if (ok && out_capacity_after)
        *out_capacity_after = p.capacity_after;
    return ok;
}


bool dbg_ui_io_history_clear(void)
{
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_IO_HISTORY_CLEAR,
                                     NULL, NULL,
                                     DBG_UI_IO_CMD_TIMEOUT_MS);
}


bool dbg_ui_io_activity_reset_all(void)
{
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_IO_ACTIVITY_RESET,
                                     NULL, NULL,
                                     DBG_UI_IO_CMD_TIMEOUT_MS);
}


bool dbg_ui_io_activity_reset_port(uint16_t port, bool is_8bit)
{
    /* memset slotů g_io_activity musí běžet na emu vlákně (souběh
     * s io_activity_record_hit / io_activity_advance_frame). Param žije
     * na zásobníku, synchronní submit ho po dokončení už nepoužívá. */
    st_DBGAPI_IO_ACTIVITY_RESET_PORT_PARAM p;
    p.port = port;
    p.is_8bit = is_8bit ? 1u : 0u;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_IO_ACTIVITY_RESET_PORT,
                                     &p, NULL,
                                     DBG_UI_IO_CMD_TIMEOUT_MS);
}


bool dbg_ui_bp_clear_all(void)
{
    /* Uvolnění stringů/AST BP a vyčištění bptmap musí běžet na emu
     * vlákně: to obojí čte při vyhodnocení BP v CPU smyčce. */
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_BP_CLEAR_ALL,
                                     NULL, NULL,
                                     DBG_UI_BP_BULK_CMD_TIMEOUT_MS);
}


bool dbg_ui_bp_load_from_file(const char *filepath)
{
    /* Param i řetězec cesty žijí u volajícího; synchronní submit je po
     * dokončení příkazu už nepoužívá. */
    st_DBGAPI_BP_LOAD_FILE_PARAM p;
    p.filepath = filepath;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_BP_LOAD_FILE,
                                     &p, NULL,
                                     DBG_UI_BP_BULK_CMD_TIMEOUT_MS);
}


bool dbg_ui_bp_reset_hits(int id)
{
    /* ID žije na zásobníku; synchronní submit ho po dokončení nepoužívá. */
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_BP_RESET_HITS,
                                     &id, NULL,
                                     DBG_UI_BP_BULK_CMD_TIMEOUT_MS);
}


bool dbg_ui_mhmap_set_mode(int mode)
{
    /* Vzor CPU Instruction History: flag zapíše UI (int zápis), swap
     * callbacků (mzarch_platform_fn_debugger_state_changed) běží na emu
     * vlákně. mhmap_set_mode se z UI nevolá - swapoval by souběžně s CPU. */
    g_debugger.mhmap_mode = (en_DEBUGGER_MHMAP_MODE)mode;
    return dbg_ui_debugger_state_recompute();
}


bool dbg_ui_mhmap_reset(void)
{
    /* Existující příkaz MCP cdl_reset = přesně mhmap_reset() na emu vlákně. */
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_CDL_RESET,
                                     NULL, NULL,
                                     DBG_UI_MHMAP_CMD_TIMEOUT_MS);
}


bool dbg_ui_mhmap_reset_region(uint32_t region_index)
{
    st_DBGAPI_MHMAP_RESET_REGION_PARAM p;
    p.region_index = region_index;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_MHMAP_RESET_REGION,
                                     &p, NULL,
                                     DBG_UI_MHMAP_CMD_TIMEOUT_MS);
}


bool dbg_ui_mhmap_merge(const struct st_MHMAP *src, unsigned op)
{
    if (!src) return false;
    /* Zdrojová mapa zůstává u volajícího; emu vlákno ji čte jen do
     * dokončení synchronního příkazu. */
    st_DBGAPI_MHMAP_MERGE_PARAM p;
    p.src = src;
    p.src_size = sizeof(st_MHMAP);
    p.op = op;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_MHMAP_MERGE,
                                     &p, NULL,
                                     DBG_UI_MHMAP_CMD_TIMEOUT_MS);
}


bool dbg_ui_memmap_change_map(uint8_t clear_mask, uint8_t set_mask,
                              uint8_t *map_after)
{
    st_DBGAPI_MEMMAP_SET_PARAM p;
    memset(&p, 0, sizeof(p));
    p.map_clear_mask = clear_mask;
    p.map_set_mask = set_mask;
    p.dmd_write = false;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_MEMMAP_SET,
                                        &p, NULL,
                                        DBG_UI_MEMMAP_CMD_TIMEOUT_MS);
    if (ok && map_after) *map_after = p.map_after;
    return ok;
}


bool dbg_ui_memmap_set_dmd(uint8_t dmd)
{
    /* Masky 0 = banking bity beze změny; handler na ne-MZ-800 odmítne. */
    st_DBGAPI_MEMMAP_SET_PARAM p;
    memset(&p, 0, sizeof(p));
    p.dmd_write = true;
    p.dmd_value = dmd;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_MEMMAP_SET,
                                     &p, NULL,
                                     DBG_UI_MEMMAP_CMD_TIMEOUT_MS);
}


bool dbg_ui_eventlog_set_mode(uint32_t mode, bool *out_active)
{
    /* Režim + eventlog_recompute_active() na emu vlákně (gate záznamu čte
     * eventlog_record). Neplatný režim odmítne handler. */
    st_DBGAPI_EVENTLOG_MODE_PARAM p;
    p.mode = mode;
    p.active_after = 0;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_EVENTLOG_SET_MODE,
                                        &p, NULL,
                                        DBG_UI_EVENTLOG_CMD_TIMEOUT_MS);
    if (ok && out_active) *out_active = (p.active_after != 0);
    return ok;
}


bool dbg_ui_eventlog_set_mask(uint64_t mask)
{
    st_DBGAPI_EVENTLOG_MASK_PARAM p;
    p.mask = mask;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_EVENTLOG_SET_MASK,
                                     &p, NULL,
                                     DBG_UI_EVENTLOG_CMD_TIMEOUT_MS);
}


bool dbg_ui_eventlog_import_file(const char *path, int *out_rc,
                                 uint32_t *out_count_after)
{
    if (!path || !path[0]) return false;
    /* Sentinel rc: handler ho přepíše, jen když import opravdu proběhl.
     * Tím se odliší chyba importu (rc = -1) od neprovedeného příkazu
     * (timeout, plná fronta, ukončování). */
    st_DBGAPI_EVENTLOG_IMPORT_PARAM p;
    p.path = path;
    p.rc = INT32_MIN;
    p.count_after = 0;
    p.capacity_after = 0;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_EVENTLOG_IMPORT_FILE,
                                        &p, NULL,
                                        DBG_UI_EVENTLOG_CMD_TIMEOUT_MS);
    if (p.rc != INT32_MIN)
    {
        if (out_rc) *out_rc = (int)p.rc;
        if (out_count_after) *out_count_after = p.count_after;
    };
    return ok;
}


bool dbg_ui_eventlog_trigger_set(uint32_t kind,
                                 struct st_EVENTLOG_FILTER *filter,
                                 const char *name)
{
    st_DBGAPI_EVENTLOG_TRIGGER_PARAM p;
    p.kind = kind;
    p.filter = filter;
    p.name = name;
    p.old_filter = NULL;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_EVENTLOG_TRIGGER_SET,
                                        &p, NULL,
                                        DBG_UI_EVENTLOG_CMD_TIMEOUT_MS);
    if (ok)
    {
        /* Emu vlákno nový filtr převzalo a starý už nepoužívá. */
        eventlog_filter_free((st_EVENTLOG_FILTER *)p.old_filter);
    }
    else
    {
        /* Neprovedeno nebo odmítnuto: filtr zůstal nám, stav beze změny. */
        eventlog_filter_free(filter);
    };
    return ok;
}


bool dbg_ui_eventlog_trigger_clear_matches(uint32_t kind)
{
    uint32_t k = kind;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_EVENTLOG_TRIGGER_CLEAR_MATCHES,
                                     &k, NULL,
                                     DBG_UI_EVENTLOG_CMD_TIMEOUT_MS);
}


/**
 * @brief Společné jádro dbg_ui_freeze_add / dbg_ui_freeze_remove.
 *
 * @param cmd         DBGAPI_CMD_FREEZE_ADD nebo DBGAPI_CMD_FREEZE_REMOVE.
 * @param region_kind en_REGION_KIND regionu.
 * @param sub_id      Disambiguator banku / plane.
 * @param offset      Offset v rámci regionu.
 * @param value       Hodnota (jen ADD).
 * @return true pokud emu vlákno příkaz provedlo a operace uspěla.
 */
static bool dbg_ui_freeze_submit(en_DBGAPI_CMD cmd, int region_kind,
                                 int sub_id, uint32_t offset, uint8_t value)
{
    st_DBGAPI_FREEZE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.region_kind = (int32_t)region_kind;
    p.sub_id = (int32_t)sub_id;
    p.offset = offset;
    p.value = value;
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue, cmd, &p, NULL,
                                     DBG_UI_FREEZE_CMD_TIMEOUT_MS);
}


bool dbg_ui_freeze_add(int region_kind, int sub_id, uint32_t offset,
                       uint8_t value)
{
    return dbg_ui_freeze_submit(DBGAPI_CMD_FREEZE_ADD, region_kind, sub_id,
                                offset, value);
}


bool dbg_ui_freeze_remove(int region_kind, int sub_id, uint32_t offset)
{
    return dbg_ui_freeze_submit(DBGAPI_CMD_FREEZE_REMOVE, region_kind, sub_id,
                                offset, 0);
}


bool dbg_ui_callstack_set_active(bool active, bool *out_active)
{
    st_DBGAPI_CALLSTACK_SET_ACTIVE_PARAM p;
    p.active = active ? 1u : 0u;
    p.active_after = 0;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_CALLSTACK_SET_ACTIVE,
                                        &p, NULL,
                                        DBG_UI_CALLSTACK_CMD_TIMEOUT_MS);
    if (ok && out_active) *out_active = (p.active_after != 0);
    return ok;
}


bool dbg_ui_callstack_reset(void)
{
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_CALLSTACK_RESET,
                                     NULL, NULL,
                                     DBG_UI_CALLSTACK_CMD_TIMEOUT_MS);
}


bool dbg_ui_screen_refresh(void)
{
    return dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
                                     DBGAPI_CMD_SCREEN_REFRESH,
                                     NULL, NULL,
                                     DBG_UI_SCREEN_REFRESH_TIMEOUT_MS);
}

#else /* !MZ800EMU_CFG_DEBUGGER_ENABLED */

/* No-debugger build: pause/run helpery se volají z non-debug UI
 * (snapshot dialogy, topmenu Alt+P). CMDRQ fronta v tomto buildu
 * neexistuje, takže jdeme přímo přes emulator_pause() z UI vlákna -
 * stejný zavedený vzor jako emulator_max_speed() volaný z menu Speed /
 * Alt+M. EMU vlákno respektuje g_emulator.paused i bez debuggeru
 * (mzarch.c má pause wait-loop ve větvi #else). */
#include "dbgapi_helpers.h"
#include "emulator.h"
bool dbg_ui_pause(void)        { emulator_pause(true);  return true; }
bool dbg_ui_run(void)          { emulator_pause(false); return true; }
bool dbg_ui_pause_toggle(void) { emulator_pause(!EMULATOR_TEST_PAUSED); return true; }

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
