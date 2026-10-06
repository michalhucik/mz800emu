/**
 * @file   test_dispatch_dbgapi_stub.h
 * @brief  Sdílený header pro stub dbgapi v test_dispatch (V0.A.3).
 *
 * Drží strukturu `st_DISPATCH_STUB_STATE` kterou testy nastavují pro
 * jednotlivé scénáře (return-fail, data fill, atd.).
 *
 * @par Licence:
 * GPL-3.0-or-later.
 */

#ifndef MZ800EMU_TESTS_MCP_DISPATCH_STUB_H
#define MZ800EMU_TESTS_MCP_DISPATCH_STUB_H

#include <stdbool.h>
#include <stdint.h>

#include "emulator/debugger/dbgapi_cmdrq.h"


/* Forward deklarace dbgapi UI submit API.
 *
 * Tady neincludujeme `dbgapi_ui.h` aby se v testovacím buildu neřetězil
 * pull main.h -> mzarch_config.h (= redefinition warning na
 * MZ800EMU_CFG_MCP_SERVER_ENABLED). Reálná definice v stub .c file. */
bool dbgapi_ui_submit_cmd_sync_with_origin(st_DBGAPI_CMDRQ_QUEUE *queue,
                                            en_DBGAPI_CMD cmd,
                                            en_DBGAPI_CMD_ORIGIN origin,
                                            void *data_ptr,
                                            void *result_ptr,
                                            int timeout_ms);

bool dbgapi_ui_submit_cmd_sync(st_DBGAPI_CMDRQ_QUEUE *queue,
                                en_DBGAPI_CMD cmd,
                                void *data_ptr,
                                void *result_ptr,
                                int timeout_ms);

en_DBGAPI_SUBMIT_STATUS dbgapi_ui_submit_cmd_sync_ex(st_DBGAPI_CMDRQ_QUEUE *queue,
                                                     en_DBGAPI_CMD cmd,
                                                     en_DBGAPI_CMD_ORIGIN origin,
                                                     void *data_ptr,
                                                     void *result_ptr,
                                                     int timeout_ms);

extern st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


/**
 * @brief Stav mocku submit funkce.
 *
 * Test si může před voláním `mcp_dispatch_request` nastavit fail
 * scenario nebo data fill flag. Po návratu může číst, jaký cmd byl
 * poslán, s jakým origin atd.
 */
typedef struct st_DISPATCH_STUB_STATE {
    en_DBGAPI_CMD        last_cmd;     /**< Poslední cmd předaný submitu. */
    en_DBGAPI_CMD_ORIGIN last_origin;  /**< Origin field z posledního volání. */
    void                *last_data;    /**< data_ptr poslední call. */
    void                *last_result;  /**< result_ptr poslední call. */
    int                  call_count;   /**< Počet volání mocku. */
    bool                 fail_next;    /**< Pokud true, příští call vrátí false. */
    /** Stav, který při neúspěchu vrátí `dbgapi_ui_submit_cmd_sync_ex`.
     *  0 (= DBGAPI_SUBMIT_OK po resetu) znamená výchozí DBGAPI_SUBMIT_FAILED
     *  (= handler neuspěl). TIMEOUT / QUEUE_FULL / ENDING simulují příkaz,
     *  který emu neprovedlo. Platí pro každý neúspěšný call, dokud ho test
     *  nezmění. */
    en_DBGAPI_SUBMIT_STATUS fail_status;
    /** Pokud > 0, call s pořadovým číslem `fail_on_call` (1 = první call
     *  po resetu) vrátí false - pro scénáře, kde handler selže až na
     *  pozdějším submitu. */
    int                  fail_on_call;
    bool                 is_running;   /**< Pro IS_RUNNING - jaké running vrátit. */
    bool                 fill_regs;    /**< Pro GET_ALL_REGS - naplnit deterministický pattern. */
    int                  fill_bp_id;   /**< Pro BP_ADD - jaké id přidělit. */
    int                  fill_bp_list_count; /**< Pro BP_LIST - kolik BP vrátit. */

    /* F015b - typed BP + condition round-trip + leak smoke:
     *
     * BP_LIST rozšířené naplnění:
     *   bp_list_fake_type      - hodnota do bp[0].type (default 0 = PC_EXEC).
     *   bp_list_fake_zone      - hodnota do bp[0].zone (default 0 = CPU_VIEW).
     *   bp_list_fake_bank_id   - hodnota do bp[0].bank_id.
     *   bp_list_fake_hits      - hodnota do bp[0].hits.
     *   bp_list_fake_condition - pokud != NULL, stub do bp[0].condition zapíše
     *                            g_strdup() této hodnoty (= simulace dbgapi
     *                            ownership kontraktu; caller MUSÍ uvolnit).
     *                            Pole bp[1..] zůstanou bez condition (NULL).
     *
     * BP_CREATE_WITH_INIT zachycení (typed create path):
     *   bp_create_calls        - kolikrát stub viděl BP_CREATE_WITH_INIT.
     *   bp_create_last_type    - kopie p->type z posledního volání.
     *   bp_create_last_addr    - kopie p->addr.
     *   bp_create_last_mask    - kopie p->update_mask.
     *   bp_create_last_expr    - kopie p->expr (g_strdup, NULL pokud nebyl;
     *                            uvolňuje dispatch_stub_reset).
     *   bp_create_fake_id      - hodnota kterou stub zapíše do p->id
     *                            (= simulace přiděleného ID; default 7). */
    uint8_t              bp_list_fake_type;
    uint8_t              bp_list_fake_zone;
    uint8_t              bp_list_fake_bank_id;
    uint64_t             bp_list_fake_hits;
    const char          *bp_list_fake_condition;
    /* bp_list: addr_end / addr_match_mode (en_BP_MATCH_MODE) / addr_mask
     * do bp[0] (default 0 = SINGLE). */
    uint16_t             bp_list_fake_addr_end;
    uint8_t              bp_list_fake_addr_match_mode;
    uint16_t             bp_list_fake_addr_mask;
    /* bp_list: bank_id_end / bank_match_mode / bank_id_mask do bp[0]. */
    uint8_t              bp_list_fake_bank_id_end;
    uint8_t              bp_list_fake_bank_match_mode;
    uint8_t              bp_list_fake_bank_id_mask;
    int                  bp_create_calls;
    uint8_t              bp_create_last_type;
    uint16_t             bp_create_last_addr;
    uint64_t             bp_create_last_mask;
    char                *bp_create_last_expr;
    int                  bp_create_fake_id;
    /* H2 - enum string parse ověření (string -> kanonický enum index):
     *   bp_create_last_zone           - kopie p->zone z posledního create.
     *   bp_create_last_event_trigger  - kopie p->event_trigger. */
    uint8_t              bp_create_last_zone;
    uint8_t              bp_create_last_event_trigger;
    /* V0.B.3 - MEM_WRITE_CHECKED scenario:
     *   mem_write_fail = true => stub vrátí success=0 (region check fail)
     *                            s adresou first_failed_addr a kind
     *                            first_failed_kind.
     *
     * Kopie dat:
     *   Dispatch.c uvolní bytes hned po _submit_dbgapi, takže pointer
     *   p->data po návratu z mcp_dispatch_request ukazuje na uvolněnou
     *   paměť. Aby test mohl kontrolovat obsah toho co handler dekódoval,
     *   stub si při zachycení CMD_MEM_WRITE_CHECKED zkopíruje data do
     *   vlastního interního bufferu mem_write_buf, accessible přes
     *   mem_write_buf + mem_write_buf_len. */
    bool                 mem_write_fail;       /**< Pro MEM_WRITE_CHECKED - simulovat region fail. */
    uint16_t             mem_write_fail_addr;  /**< Adresa kde region check selhal. */
    uint8_t              mem_write_fail_kind;  /**< Druh failed regionu. */
    uint8_t              mem_write_buf[256];   /**< Zachycená kopie dat z MEM_WRITE_CHECKED.data */
    uint16_t             mem_write_buf_len;    /**< Počet zachycených bajtů. */
    uint16_t             mem_write_addr;       /**< Zachycená adresa z param.addr. */

    /* V0.B.6 - chybející Tools:
     *
     * BP_REMOVE - zachycený id (z st_DBGAPI_BP_PARAM):
     *   bp_remove_last_id = ID poslední BP_REMOVE call. -1 = ještě nevolán.
     *
     * BP_SET_ENABLED - zachycené id + enabled:
     *   bp_set_enabled_last_id / last_enabled - z poslední call.
     *
     * BP_LIST loop (bp_clear scenario):
     *   bp_list_call_count_in_clear = počet BP_LIST volání (bp_clear
     *     volá BP_LIST + N x BP_REMOVE). Reset standardně přes
     *     dispatch_stub_reset.
     *
     * STEP_INTO / STEP_OVER counters:
     *   step_into_calls / step_over_calls - kolikrát stub viděl
     *     odpovídající cmd. Užitečné pro step_n test (musí vidět N volání).
     *
     * RUN_TO - zachycená target adresa:
     *   run_to_last_addr = z uint16_t* data_ptr.
     */
    int                  bp_remove_last_id;
    int                  bp_set_enabled_last_id;
    bool                 bp_set_enabled_last_enabled;
    int                  step_into_calls;
    int                  step_over_calls;
    uint16_t             run_to_last_addr;
    /* Per-call fail control - pro step_n partial test:
     *   step_into_fail_after_n = pokud > 0, stub vrátí false na N+1. volání
     *   STEP_INTO. 0 = vždy success. Reset přes dispatch_stub_reset. */
    int                  step_into_fail_after_n;

    /* V1.A.1 - Snapshot scenario:
     *
     *   snapshot_last_filepath    - kopie filepath z posledního SAVE_FILE
     *                                / LOAD_FILE volání (heap, g_strdup,
     *                                stub uvolní v dispatch_stub_reset
     *                                přes g_free).
     *   snapshot_last_description - kopie description z posledního SAVE_*
     *                                volání. NULL pokud description == NULL.
     *   snapshot_load_buf_size    - velikost bufferu z posledního
     *                                LOAD_BUFFER volání.
     *   snapshot_load_buf_first   - první bajt předaného bufferu (= sanity
     *                                check že base64 dekódování funguje).
     *   snapshot_save_buf_size    - velikost kterou má stub vyplnit pro
     *                                SAVE_BUFFER. Pokud 0, použije se
     *                                default 8 (pro test). Stub alokuje
     *                                buffer přes g_malloc a vyplní pattern.
     *   snapshot_save_buf_pattern - pattern byte vyplněný do alokovaného
     *                                bufferu (default 0xAB). Test ho
     *                                ověří přes base64 dekódování. */
    char                *snapshot_last_filepath;
    char                *snapshot_last_description;
    size_t               snapshot_load_buf_size;
    uint8_t              snapshot_load_buf_first;
    size_t               snapshot_save_buf_size;
    uint8_t              snapshot_save_buf_pattern;

    /* V1.A.1 - Cooperation hint scenario - jen counter, stav verifikujeme
     * přímo přes g_cooperation_hint (= dispatch handler je lokální).
     *   cooperation_call_count - kolikrát byl handler zavolán.
     *
     * Pozn.: Lokální handler nevolá _submit_dbgapi - tj. stub se sem
     * nedostane. Counter zvyšujeme jen pro fail scenario (nebude se
     * inkrementovat aktuálně, ale máme připraveno). */
    int                  cooperation_call_count;

    /* V1.A.2 - Symbol management scenario:
     *
     *   sym_last_name      - kopie name z posledního SYMBOL_* volání,
     *                        g_strdup, uvolňuje dispatch_stub_reset.
     *   sym_last_addr      - addr field z posledního volání.
     *   sym_last_comment   - kopie comment (SYMBOL_ADD), g_strdup.
     *   sym_last_prefix    - kopie prefix (SYMBOL_LIST), g_strdup.
     *   sym_lookup_fake_*  - data, kterými stub vyplní out_entries[0]
     *                        při SYMBOL_LOOKUP. Pokud sym_lookup_found=true,
     *                        out_count nastaví na 1, jinak na 0.
     *   sym_list_fake_count - kolik fake záznamů vyplnit pro SYMBOL_LIST
     *                          (omezeno out_max). Záznamy mají
     *                          addr=0x4000+i, name="SYM_i", comment="cmt_i",
     *                          source=3 (LBL).
     */
    char                *sym_last_name;
    uint16_t             sym_last_addr;
    char                *sym_last_comment;
    char                *sym_last_prefix;
    bool                 sym_lookup_found;
    uint16_t             sym_lookup_fake_addr;
    const char          *sym_lookup_fake_name;
    const char          *sym_lookup_fake_comment;
    uint8_t              sym_lookup_fake_source;
    int                  sym_list_fake_count;

    /* V1.A.3 - step out + run_until_* scenario:
     *
     * STEP_OUT (callstack pop):
     *   step_out_fake_status     - hodnota kterou stub zapíše do param.status
     *                              (0=OK, 1..4=chyba). Pokud != 0, stub
     *                              vrátí false (= submit failed) + status
     *                              fixed v param.
     *   step_out_fake_return_addr- adresa kterou stub zapíše do
     *                              param.return_addr při status=0.
     *
     * GET_RASTER_POS polling:
     *   raster_seq_idx       - počítadlo volání GET_RASTER_POS (= jak
     *                          daleko jsme v simulované timeline).
     *   raster_seq_step_lines- po každém GET_RASTER_POS přidá tuto
     *                          hodnotu k scanline (= simulace běhu
     *                          rasteru během polling smyčky).
     *   raster_seq_step_cycles- to samé pro total_cycles.
     *   raster_initial_*      - výchozí hodnoty pro první snapshot.
     */
    uint8_t              step_out_fake_status;
    uint16_t             step_out_fake_return_addr;
    int                  raster_seq_idx;
    int                  raster_seq_step_lines;
    int                  raster_seq_step_cycles;
    uint32_t             raster_initial_frame;
    uint16_t             raster_initial_scanline;
    uint16_t             raster_initial_column;
    uint32_t             raster_initial_total_cycles;
    uint32_t             raster_initial_frame_cycles;

    /* V1.A.6 - Watch + Callstack + CDL scenario:
     *
     * Watch:
     *   watch_add_last_mode / _last_name / _last_addr / _last_expr_text / _last_type
     *     - kopie polí param. Heap stringy g_strdup, uvolňuje
     *       dispatch_stub_reset.
     *   watch_add_fake_index - hodnota kterou stub zapíše do out_index.
     *
     *   watch_remove_last_name / _last_index - z poslední WATCH_REMOVE call.
     *   watch_remove_found - 1 = stub zapíše out_removed=1, jinak 0 +
     *                        param.index=-1 + rq->success=false.
     *
     *   watch_list_fake_count - kolik fake záznamů vyplnit pro WATCH_LIST.
     *     Záznamy mají index=i, name="W_i", mode=0, type=0, addr=0x100+i,
     *     value_str="v_i".
     *
     *   watch_eval_fake_value_int / _fake_value_str / _fake_error
     *     - hodnoty kterými stub vyplní param při WATCH_EVAL. Pokud
     *       fake_error != NULL, stub vrátí false a vyplní out_error.
     *   watch_eval_last_name / _last_index / _last_expr_text
     *     - kopie polí z posledního volání.
     *
     * Callstack:
     *   callstack_fake_count - počet entries které stub vrátí v param.entries.
     *     Heap-alokované pole st_CALLSTACK_ENTRY. Stub naplní deterministicky
     *     (entries[i].return_addr=0x8000+i*4, kind=CS_KIND_CALL, ...).
     *   callstack_fake_active / _fake_current_depth / _fake_cycles_now
     *
     * CDL:
     *   cdl_start_calls / cdl_stop_calls / cdl_reset_calls - countery volání.
     *   cdl_export_last_path - kopie path z posledního CDL_EXPORT volání.
     *   cdl_export_fake_result - hodnota kterou stub zapíše do out_result.
     *   cdl_export_fake_region_count - hodnota do out_region_count.
     */
    en_DBGAPI_WATCH_MODE watch_add_last_mode;
    char                *watch_add_last_name;
    uint16_t             watch_add_last_addr;
    char                *watch_add_last_expr_text;
    en_DBGAPI_WATCH_TYPE watch_add_last_type;
    int                  watch_add_fake_index;
    char                *watch_remove_last_name;
    int                  watch_remove_last_index;
    int                  watch_remove_found;
    int                  watch_list_fake_count;
    int32_t              watch_eval_fake_value_int;
    const char          *watch_eval_fake_value_str;
    const char          *watch_eval_fake_error;
    char                *watch_eval_last_name;
    int                  watch_eval_last_index;
    char                *watch_eval_last_expr_text;
    int                  callstack_fake_count;
    uint8_t              callstack_fake_active;
    int                  callstack_fake_current_depth;
    uint64_t             callstack_fake_cycles_now;
    int                  cdl_start_calls;
    int                  cdl_stop_calls;
    int                  cdl_reset_calls;
    char                *cdl_export_last_path;
    int                  cdl_export_fake_result;
    int                  cdl_export_fake_region_count;

    /* 0017 FÁZE 1 - Tracking lifecycle (trace-suite):
     *   trace_start/stop/reset/save_calls - countery volání.
     *   trace_last_channel - en_DBGAPI_TRACE_CHANNEL z posledního volání.
     *   trace_save_last_path - kopie path z posledního TRACE_SAVE.
     *   trace_fake_result - hodnota kterou stub zapíše do out_result. */
    int                  trace_start_calls;
    int                  trace_stop_calls;
    int                  trace_reset_calls;
    int                  trace_save_calls;
    int                  trace_last_channel;
    char                *trace_save_last_path;
    int                  trace_fake_result;

    /* V1.A.5 - chip-level fault injection scenario:
     *
     * io_read / io_write: stub si zachytí poslední parametry pro inspekci.
     *   io_last_port / io_last_value - z posledního IO_READ nebo IO_WRITE
     *   io_read_fake_value - hodnotu kterou vyplníme do param.value
     *                        při IO_READ.
     *   io_read_calls / io_write_calls - kolikrát stub zachytil daný cmd.
     *
     * irq_inject / nmi_inject:
     *   irq_inject_last_vector_valid + irq_inject_last_vector + irq_inject_last_source
     *     - kopie polí param. last_source heap-alokovaný stringem g_strdup,
     *       uvolňuje dispatch_stub_reset.
     *   nmi_inject_calls - kolikrát byl NMI_INJECT vyslán.
     *
     * mem_write_force:
     *   mwf_addr / mwf_buf / mwf_len - zachycený obsah parametru.
     */
    uint16_t             io_last_port;
    uint8_t              io_last_value;
    uint8_t              io_read_fake_value;
    int                  io_read_calls;
    int                  io_write_calls;
    char                *irq_inject_last_source;
    uint8_t              irq_inject_last_vector;
    uint8_t              irq_inject_last_vector_valid;
    int                  irq_inject_calls;
    int                  nmi_inject_calls;
    uint16_t             mwf_addr;
    uint16_t             mwf_len;
    uint8_t              mwf_buf[ 256 ];

    /* V1.A.7 - Profiler Tools scenario:
     *
     * profiler_start / profiler_stop / profiler_reset:
     *   profiler_set_active_calls - kolikrát byl SET_ACTIVE volán.
     *   profiler_last_active - poslední hodnota param.active (0/1).
     *   profiler_reset_calls - kolikrát byl RESET volán.
     *
     * profiler_export:
     *   profiler_export_last_path - heap-alokovaný posledn filepath.
     *   profiler_export_last_format - 0=CSV, 1=JSON.
     *   profiler_export_fake_result - hodnota zapsaná do param.result.
     *   profiler_export_fake_entry_count - hodnota zapsaná do
     *                                       param.entry_count.
     *
     * profiler_get:
     *   profiler_get_calls - kolikrát byl GET_PROFILER volán.
     *   profiler_get_fake_active / _total_cycles_64 / _total_calls /
     *      _irq_entries / _unmatched_returns / _max_depth_reached /
     *      _overflow_count - vyplněné do param při GET.
     *   profiler_get_fake_count - počet fake entries (pole alokuje stub).
     */
    int                  profiler_set_active_calls;
    uint8_t              profiler_last_active;
    int                  profiler_reset_calls;
    char                *profiler_export_last_path;
    int                  profiler_export_last_format;
    int                  profiler_export_fake_result;
    int                  profiler_export_fake_entry_count;
    int                  profiler_get_calls;
    uint8_t              profiler_get_fake_active;
    uint64_t             profiler_get_fake_total_cycles_64;
    uint64_t             profiler_get_fake_total_calls;
    uint32_t             profiler_get_fake_irq_entries;
    uint32_t             profiler_get_fake_unmatched_returns;
    uint32_t             profiler_get_fake_max_depth_reached;
    uint32_t             profiler_get_fake_overflow_count;
    int                  profiler_get_fake_count;

    /* V1.B.1 - Media Tools scenario:
     *
     * media_load_mzf / load_binary / insert / eject / state:
     *   media_last_cmd       - DBGAPI_CMD_MEDIA_* z posledního volání.
     *   media_last_slot      - en_DBGAPI_MEDIA_SLOT z posledního volání.
     *   media_last_filepath  - heap-alokovaný g_strdup, uvolňuje
     *                          dispatch_stub_reset.
     *   media_last_load_addr - z load_binary.
     *   media_last_ro        - read_only flag (insert).
     *   media_fake_result    - hodnota do param.out_result (default 0).
     *   media_fake_out_size  - hodnota do param.out_size (load_binary).
     *
     * media_state:
     *   media_state_fake_count - kolik fake slot info naplnit (max 5).
     *   media_state_calls      - kolikrát byl STATE volán.
     */
    int                  media_last_cmd;
    int                  media_last_slot;
    char                *media_last_filepath;
    uint16_t             media_last_load_addr;
    uint8_t              media_last_ro;
    int                  media_fake_result;
    uint32_t             media_fake_out_size;
    int                  media_state_fake_count;
    int                  media_state_calls;

    /* V1.B.2 - Platform + Config Tools scenario:
     *
     * settings_get / settings_set:
     *   settings_last_cmd     - DBGAPI_CMD_SETTINGS_* (GET / SET).
     *   settings_last_module  - heap-alokovaný g_strdup vstupního pole.
     *   settings_last_element - heap-alokovaný g_strdup.
     *   settings_last_new_value - g_strdup vstupní hodnoty (SET).
     *   settings_fake_value   - hodnota co stub vrátí v out_value
     *                           (g_strdup, stub uvolní; default NULL).
     *   settings_fake_type    - hodnota co stub vrátí v out_type.
     *   settings_fake_result  - hodnota co stub zapíše do out_result
     *                           (default 0; -2/-3/-4 podle scénáře).
     *
     * platform_set:
     *   platform_last_target_kind - vstupní target_kind.
     *   platform_fake_active_kind - hodnota co stub vrátí v
     *                               out_active_kind (default 2 = mz800).
     *   platform_fake_result  - default -10 (= nepodporováno).
     *
     * periph_attach / periph_detach:
     *   periph_last_cmd        - DBGAPI_CMD_PERIPH_* (ATTACH / DETACH).
     *   periph_last_kind       - en_DBGAPI_PERIPH_KIND.
     *   periph_last_option_value - g_strdup vstupního option.
     *   periph_fake_result     - hodnota do out_result (default 0).
     *   periph_fake_requires_restart - hodnota do
     *                                  out_requires_restart (default 1).
     */
    int                  settings_last_cmd;
    char                *settings_last_module;
    char                *settings_last_element;
    char                *settings_last_new_value;
    char                *settings_fake_value;
    int                  settings_fake_type;
    int                  settings_fake_result;
    int                  platform_last_target_kind;
    int                  platform_fake_active_kind;
    int                  platform_fake_result;
    int                  periph_last_cmd;
    int                  periph_last_kind;
    char                *periph_last_option_value;
    int                  periph_fake_result;
    int                  periph_fake_requires_restart;

    /* V1.C.1 - HID Tools scenario:
     *
     * input_press_key / release_key / release_all:
     *   hid_press_calls / hid_release_calls / hid_release_all_calls -
     *     kolikrát stub zachytil daný typ volání. Zachycují se v
     *     test_hid_keymap_stub.c (= dispatch.c volá _hid_submit_key
     *     -> dbgapi handler -> hid_keymap_press/release, který je
     *     v test buildu poskytnut stubem).
     *   hid_last_col / hid_last_bit / hid_last_shift - posledně
     *     předané parametry.
     *
     * input_joy_set / joy_clear:
     *   hid_joy_set_calls / hid_joy_clear_calls.
     *   hid_joy_last_port / hid_joy_last_mask.
     *
     * Pole se nulují přes dispatch_stub_reset (= v setUp).
     */
    int                  hid_press_calls;
    int                  hid_release_calls;
    int                  hid_release_all_calls;
    int                  hid_last_col;
    int                  hid_last_bit;
    int                  hid_last_shift;
    int                  hid_joy_set_calls;
    int                  hid_joy_clear_calls;
    int                  hid_joy_last_port;
    int                  hid_joy_last_mask;

    /* V1.D.2.A - Stack history Resource backing scenario:
     *
     * Stub odpovídá na DBGAPI_CMD_STACK_HISTORY_GET. Naplní samples pole
     * (alokuje caller - dispatch handler), `count`, `active`, `slope`.
     *
     *   stack_history_fake_active - hodnota do param.active (0/1).
     *   stack_history_fake_count  - kolik fake samplů zapsat
     *                                (omezeno na param.max_count).
     *   stack_history_fake_slope  - hodnota do param.slope.
     *   stack_history_calls       - kolikrát stub viděl GET.
     *
     * Vzorky generujeme deterministicky:
     *   samples[i].cycles = 1000 + i*10
     *   samples[i].sp     = 0xF000 - i
     */
    uint8_t              stack_history_fake_active;
    uint32_t             stack_history_fake_count;
    float                stack_history_fake_slope;
    int                  stack_history_calls;

    /* V1.D.2.A - Stack regions Resource backing scenario:
     *
     * Stub odpovídá na DBGAPI_CMD_STACK_REGIONS_LIST. Naplní regions[]
     * pole + count + sp_now.
     *
     *   stack_regions_fake_count  - kolik fake regions zapsat
     *                                (max DBGAPI_STACK_REGIONS_MAX = 8).
     *   stack_regions_fake_sp_now - hodnota do param.sp_now.
     *   stack_regions_calls       - kolikrát stub viděl LIST.
     *
     * Regions generujeme deterministicky:
     *   regions[i].name      = "stack_<i>"
     *   regions[i].base      = 0xF000 - i*0x100
     *   regions[i].limit     = base - 0x80
     *   regions[i].watermark = limit + 0x10
     *   regions[i].push_count = (i+1) * 100
     *   regions[i].pop_count  = (i+1) * 90
     *   regions[i].current_sp_in_region = (i == 0) ? 1 : 0
     */
    int                  stack_regions_fake_count;
    uint16_t             stack_regions_fake_sp_now;
    int                  stack_regions_calls;

    /* V1.D.2.B - Stack dump Resource backing scenario:
     *
     * Stub odpovídá na DBGAPI_CMD_STACK_DUMP. Naplní buf[], sp_now, sp_odd
     * podle nastavených fields. Generuje deterministický pattern:
     *
     *   sp_now    = stack_dump_fake_sp_now (default 0xF000)
     *   sp_odd    = stack_dump_fake_sp_odd (default 0)
     *   buf[i]    = stack_dump_fake_pattern_base + (uint8_t)i (default base 0x10)
     *
     * Pokud dispatch handler volá SP-anchored mode (lines_above > 0),
     * stub do `addr` zapíše vypočtenou base = sp_now + lines_above*2.
     *
     *   stack_dump_calls          - kolikrát stub viděl STACK_DUMP.
     */
    uint16_t             stack_dump_fake_sp_now;
    uint8_t              stack_dump_fake_sp_odd;
    uint8_t              stack_dump_fake_pattern_base;
    int                  stack_dump_calls;

    /* V1.D.2.B - bp_vars Resource backing scenario:
     *
     * Stub odpovídá na novém DBGAPI_CMD_BP_VARS_LIST. Naplní entries[]
     * pole, out_count, truncated podle fields. Pattern:
     *
     *   entries[i].name     = "VAR_<i>"
     *   entries[i].value    = (int32_t)(0x1000 + i)
     *   entries[i].has_comment = (i % 2 == 0) ? 1 : 0
     *   entries[i].comment  = "cmt_<i>" pokud has_comment, jinak ""
     *   entries[i].persist_value = (i % 2 == 0) ? 1 : 0
     *
     *   bp_vars_fake_count - kolik fake var zapsat (omezeno capacity).
     *   bp_vars_calls      - kolikrát stub viděl BP_VARS_LIST.
     */
    int                  bp_vars_fake_count;
    int                  bp_vars_calls;

    /* V1.D.2.B - bookmarks Resource backing scenario:
     *
     * Stub odpovídá na novém DBGAPI_CMD_BOOKMARKS_LIST. Naplní entries[]
     * pole, out_count, truncated podle fields. Pattern:
     *
     *   entries[i].id          = (uint32_t)(i + 1)
     *   entries[i].user_input  = "BM_<i>"
     *   entries[i].comment     = "bcmt_<i>"
     *   entries[i].cmd_origin  = (i == 0) ? DBGAPI_CMD_ORIGIN_USER
     *                                     : DBGAPI_CMD_ORIGIN_MCP
     *
     *   bookmarks_fake_count - kolik fake záložek zapsat.
     *   bookmarks_calls      - kolikrát stub viděl BOOKMARKS_LIST.
     */
    int                  bookmarks_fake_count;
    int                  bookmarks_calls;

    /* V1.D.3.A - IRQ chip Resource backing scenario:
     *
     * Stub odpovídá na DBGAPI_CMD_GET_PERIPH_I8255 / I8253 / Z80_PIO.
     * Naplní fixed-size payload deterministickými hodnotami; testy
     * verifikují že dispatch handler JSON serializuje pole správně
     * (= klíče, typy, enum-to-string mapping).
     *
     * i8255:
     *   periph_i8255_calls          - kolikrát stub viděl GET_PERIPH_I8255.
     *   periph_i8255_fake_cw        - hodnota do param.control_word
     *                                  (default 0x90 = Mode Set, mode A=0,
     *                                  PA=input).
     *
     * i8253:
     *   periph_i8253_calls          - kolikrát stub viděl GET_PERIPH_I8253.
     *
     * z80_pio:
     *   periph_z80_pio_calls        - kolikrát stub viděl GET_PERIPH_Z80_PIO.
     *   periph_z80_pio_fake_available - hodnota do param.available
     *                                    (default 1 - MZ-800/1500; testy
     *                                    nastaví na 0 pro simulaci MZ-700).
     *
     * V1.D.3.B audio chips:
     *
     * sn76489:
     *   periph_sn76489_calls          - kolikrát stub viděl GET_PERIPH_SN76489.
     *   periph_sn76489_fake_available - hodnota do param.available
     *                                    (default 1; testy nastaví 0 pro
     *                                    simulaci MZ-700).
     *   periph_sn76489_fake_stereo    - hodnota do param.stereo
     *                                    (default 0 = mono; 1 = stereo,
     *                                    psg_count=2).
     *
     * ay3_8910:
     *   periph_ay3_8910_calls         - kolikrát stub viděl GET_PERIPH_AY3_8910.
     *   (Není fake_available - placeholder, vždy 0.)
     *
     * beeper:
     *   periph_beeper_calls           - kolikrát stub viděl GET_PERIPH_BEEPER.
     *   periph_beeper_fake_ctc0_out   - hodnota raw CTC0 OUT (0/1).
     *   periph_beeper_fake_gate0      - hodnota GATE0 (0/1).
     *   periph_beeper_fake_pc0        - hodnota PC0 (0/1).
     */
    int                  periph_i8255_calls;
    uint8_t              periph_i8255_fake_cw;
    int                  periph_i8253_calls;
    int                  periph_z80_pio_calls;
    uint8_t              periph_z80_pio_fake_available;
    int                  periph_sn76489_calls;
    uint8_t              periph_sn76489_fake_available;
    uint8_t              periph_sn76489_fake_stereo;
    int                  periph_ay3_8910_calls;
    int                  periph_beeper_calls;
    uint8_t              periph_beeper_fake_ctc0_out;
    uint8_t              periph_beeper_fake_gate0;
    uint8_t              periph_beeper_fake_pc0;
    /* V1.D.3.C - storage + display Resources fake state.
     *
     * gdg:
     *   periph_gdg_calls            - kolikrát stub viděl GET_PERIPH_GDG.
     *   periph_gdg_fake_platform    - "mz800" / "mz700" / "mz1500" (vč. NUL).
     *   periph_gdg_fake_palette_count - 16 (MZ-800) nebo 8 (jinde).
     *   periph_gdg_fake_has_*       - 0/1 flagy per-platforma fields.
     *   periph_gdg_fake_regDMD      - DMD bajt (default 0).
     *
     * wd1793:
     *   periph_wd1793_calls         - kolikrát stub viděl GET_PERIPH_WD1793.
     *   periph_wd1793_fake_available - 0/1 (1 = chip dostupný).
     *   periph_wd1793_fake_status   - reg_status bajt.
     *   periph_wd1793_fake_drive0_present - 0/1 mounted flag drive 0.
     *
     * cmt:
     *   periph_cmt_calls            - kolikrát stub viděl GET_PERIPH_CMT.
     *   periph_cmt_fake_state       - 0=STOP, 1=PLAY, 2=RECORD.
     *
     * qd:
     *   periph_qd_calls             - kolikrát stub viděl GET_PERIPH_QD.
     *   periph_qd_fake_available    - 0/1.
     *   periph_qd_fake_type         - 0=IMAGE, 1=VIRTUAL, 2=UNICARD.
     */
    int                  periph_gdg_calls;
    char                 periph_gdg_fake_platform[12];
    uint8_t              periph_gdg_fake_palette_count;
    uint8_t              periph_gdg_fake_has_border_reg;
    uint8_t              periph_gdg_fake_has_pal_group;
    uint8_t              periph_gdg_fake_has_cksw;
    uint8_t              periph_gdg_fake_regDMD;
    int                  periph_wd1793_calls;
    uint8_t              periph_wd1793_fake_available;
    uint8_t              periph_wd1793_fake_status;
    uint8_t              periph_wd1793_fake_drive0_present;
    int                  periph_cmt_calls;
    uint8_t              periph_cmt_fake_state;
    int                  periph_qd_calls;
    uint8_t              periph_qd_fake_available;
    uint8_t              periph_qd_fake_type;

    /* V1.D.4 - input + frame Resources fake state.
     *
     * input_kbd_state_calls         - kolikrát stub viděl GET_INPUT_KEYBOARD_STATE.
     * kbd_real_matrix[10]           - HW scan matrix bytes (default 0xff = idle).
     * kbd_virtual_matrix[10]        - VKBD matrix bytes (default 0xff = idle).
     *
     * input_kbd_matrix_info_calls   - kolikrát stub viděl GET_INPUT_KBD_MATRIX_INFO.
     *
     * input_joy_state_calls         - kolikrát stub viděl GET_INPUT_JOYSTICK_STATE.
     * joy_port0_connected           - 0/1, default 0.
     * joy_port0_state_bits          - active-HIGH bitmask.
     *
     * frame_fb_info_calls           - kolikrát stub viděl GET_FRAME_FRAMEBUFFER_INFO.
     * fb_screen_id                  - reportovaný frame counter.
     * fb_state                      - en_FBSTATE bitmask (0=clean, 1=screen_changed).
     *
     * frame_screenshot_raw_calls    - kolikrát stub viděl GET_FRAME_SCREENSHOT_RAW.
     * fb_available                  - 0/1, default 1 = stub má framebuffer.
     *
     * frame_screenshot_png_calls    - kolikrát stub viděl GET_FRAME_SCREENSHOT_PNG.
     *
     * video_text_dump_calls         - kolikrát stub viděl GET_VIDEO_TEXT_DUMP.
     * text_dump_available           - 0/1, default 1 = stub má text dump.
     */
    int                  input_kbd_state_calls;
    uint8_t              kbd_real_matrix[10];
    uint8_t              kbd_virtual_matrix[10];
    int                  input_kbd_matrix_info_calls;
    int                  input_joy_state_calls;
    uint8_t              joy_port0_connected;
    uint8_t              joy_port0_state_bits;
    int                  frame_fb_info_calls;
    uint32_t             fb_screen_id;
    uint8_t              fb_state;
    int                  frame_screenshot_raw_calls;
    uint8_t              fb_available;
    int                  frame_screenshot_png_calls;
    int                  video_text_dump_calls;
    uint8_t              text_dump_available;

    /* V1.D.2.C - per-watch snapshot Resource backing scenario:
     *
     * Stub odpovídá na DBGAPI_CMD_GET_WATCH_SNAPSHOT. Naplní pole z
     * následujících fake_ fields nebo vrátí found=0 (= řádek neexistuje).
     *
     *   watch_snapshot_calls          - kolikrát stub viděl GET_WATCH_SNAPSHOT.
     *   watch_snapshot_last_name      - heap g_strdup vstupního jména
     *                                    (= uvolňuje dispatch_stub_reset).
     *   watch_snapshot_fake_found     - 0/1; pokud 0, ostatní fields ignorovány.
     *   watch_snapshot_fake_snapshot_active - hodnota do snapshot_active.
     *   watch_snapshot_fake_min_max_valid   - hodnota do min_max_valid.
     *   watch_snapshot_fake_row_id    - hodnota do row_id.
     *   watch_snapshot_fake_type_snap - hodnota do type_snap.
     *   watch_snapshot_fake_snap_int  - hodnota do snap_int.
     *   watch_snapshot_fake_cur_int   - hodnota do cur_int.
     *   watch_snapshot_fake_delta_int - hodnota do delta_int.
     *   watch_snapshot_fake_min_int   - hodnota do min_int.
     *   watch_snapshot_fake_max_int   - hodnota do max_int.
     *   watch_snapshot_fake_change_count - hodnota do change_count.
     */
    int                  watch_snapshot_calls;
    char                *watch_snapshot_last_name;
    uint8_t              watch_snapshot_fake_found;
    uint8_t              watch_snapshot_fake_snapshot_active;
    uint8_t              watch_snapshot_fake_min_max_valid;
    int32_t              watch_snapshot_fake_row_id;
    int32_t              watch_snapshot_fake_type_snap;
    int64_t              watch_snapshot_fake_snap_int;
    int64_t              watch_snapshot_fake_cur_int;
    int64_t              watch_snapshot_fake_delta_int;
    int64_t              watch_snapshot_fake_min_int;
    int64_t              watch_snapshot_fake_max_int;
    uint64_t             watch_snapshot_fake_change_count;

    /* video-capture Task 15: DBGAPI_CMD_VIDEOREC (videorec_* MCP příkazy).
     *
     *   videorec_calls       - kolikrát stub viděl DBGAPI_CMD_VIDEOREC.
     *   videorec_last_op     - en_DBGAPI_VIDEOREC_OP posledního volání.
     *   videorec_last_path   - heap g_strdup vstupní cesty (NULL = nezadaná);
     *                          uvolňuje dispatch_stub_reset.
     *   videorec_last_frames - vstupní frames (START).
     *   videorec_last_paused - vstupní paused (-1/0/1, PAUSE).
     *   videorec_last_label  - heap g_strdup vstupního popisku (NULL = nezadaný).
     *   videorec_last_timebase - vstupní timebase (0/1, TIMEBASE; Task 18).
     *   videorec_fake        - výstupní pole (out_*) zkopírovaná do parametru;
     *                          out_result != 0 => submit vrátí false (chyba).
     */
    int                      videorec_calls;
    int                      videorec_last_op;
    char                    *videorec_last_path;
    uint64_t                 videorec_last_frames;
    int                      videorec_last_paused;
    char                    *videorec_last_label;
    int                      videorec_last_timebase;
    st_DBGAPI_VIDEOREC_PARAM videorec_fake;
} st_DISPATCH_STUB_STATE;


extern st_DISPATCH_STUB_STATE g_stub_state;


/** @brief Reset stub state před testem. */
void dispatch_stub_reset(void);


/**
 * @brief Nastaví last_user_action stub state (V1.D.1).
 *
 * Pro testy ověřující že emulator://state Resource správně serializuje
 * last_user_action. Pokud valid=false, dbgapi_get_last_user_action stub
 * vrátí false a dispatch handler vystaví JSON null.
 *
 * @param valid         true = pretend USER akce zaznamenána.
 * @param cmd           Zaznamenaný CMD.
 * @param timestamp_us  Monotonic timestamp v mikrosekundách.
 */
void dispatch_stub_set_last_user_action ( bool valid,
                                            en_DBGAPI_CMD cmd,
                                            uint64_t timestamp_us );


#endif /* MZ800EMU_TESTS_MCP_DISPATCH_STUB_H */
