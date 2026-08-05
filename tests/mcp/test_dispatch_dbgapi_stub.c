/**
 * @file   test_dispatch_dbgapi_stub.c
 * @brief  Stub implementace dbgapi UI submit API pro standalone test
 *         dispatch vrstvy (V0.A.3).
 *
 * Dispatch.c volá `dbgapi_ui_submit_cmd_sync_with_origin(...)` a používá
 * global `g_dbgapi_cmdrq_queue` symbol. V STANDALONE testu žádný emu
 * neběží - tady poskytneme prázdný queue symbol + parametrizovatelný
 * mock který testy nastaví podle scénáře.
 *
 * Test runner kontroluje výsledek přes globální `g_stub_state` strukturu:
 *  - `g_stub_state.last_cmd`     - které dbgapi cmd handler poslal
 *  - `g_stub_state.last_origin`  - origin field (musí být MCP)
 *  - `g_stub_state.last_data`    - pointer na data_ptr (pro inspekci
 *                                   parametrů)
 *  - `g_stub_state.last_result`  - pointer na result_ptr (test může
 *                                   zapsat fake data před voláním)
 *  - `g_stub_state.fail_next`    - pokud true, submit vrátí false
 *  - `g_stub_state.fill_regs`    - pro GET_ALL_REGS - pokud true, mock
 *                                   naplní registry deterministickým
 *                                   patternem
 *  - `g_stub_state.fill_bp_id`   - pro BP_ADD - vyplní param->id
 *  - `g_stub_state.fill_bp_list_count` - pro BP_LIST
 *  - `g_stub_state.is_running`   - pro IS_RUNNING (default false)
 *
 * Komentáře v kódu jsou česky s diakritikou (per CLAUDE.md).
 *
 * @par Licence:
 * GPL-3.0-or-later. Viz licence header v dispatch.c.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>

#include "emulator/debugger/dbgapi_cmdrq.h"
/* Pozn.: dbgapi_ui.h NEincludujeme - viz komentář ve stub headeru. */
/* F015a - dispatch.c v _handle_bp_list volá bpt_type_to_string /
 * bp_zone_to_string. V standalone testu se breakpoints.c nelinkuje,
 * proto níže shim. Header poskytuje typy en_BPT_TYPE / en_BP_ZONE a
 * je self-contained (jen stdint/stdbool/glib/dbgapi_cmdrq.h/bp_event.h). */
#include "emulator/debugger/breakpoints.h"

#include "test_dispatch_dbgapi_stub.h"

/* V1.C.1 - hid_keymap stub funkce (definované v test_hid_keymap_stub.c). */
extern void hid_keymap_press(int col, int bit, bool also_shift);
extern void hid_keymap_release(int col, int bit, bool also_shift);
extern void hid_keymap_release_all(void);
extern bool hid_keymap_joystick_set(int port, uint8_t mcp_mask);
extern bool hid_keymap_joystick_clear(int port);


/* Globální symbol na který dispatch.c odkazuje. V stubu má placeholder
 * obsah - nikdy se nepoužije reálně (mock submit nic z queue neřeší). */
st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;

/* history_get handler v dispatch.c čte position field z této struktury.
 * V testovacím buildu stub poskytuje placeholder bez plné definice
 * (debugger.h taháne příliš mnoho transitivních závislostí). Layout
 * MUSÍ začínat 32-bit position field aby dispatch.c čtení nezpůsobilo
 * memory error - viz st_DEBUGGER_HISTORY v debugger.h:226. */
struct test_stub_history_layout {
    unsigned position;
    unsigned byte_position;
    struct { uint16_t addr; uint8_t byte[4]; } row[32];
};
struct test_stub_history_layout g_debugger_history;


/* V1.E.6.C - fake symboly pro link satisfaction.
 *
 * dispatch.c includuje mzarch_platform.h a používá g_mzarch_* globály
 * v platform/info handlerech (_dispatch_detect_platform). V test buildu
 * se nelinkuje mzarch_platform.c, takže musíme symboly dodat sami.
 *
 * dispatch.c také v _dispatch_detect_platform čte g_gdg.regDMD podle
 * compile-time MZARCH guard (MZARCH=800 v CMake DEFINES). Pro link
 * stačí poskytnout symbol s dostatečnou velikostí (offset regDMD ve
 * skutečném st_GDG je menší než 4096); test obsah nečte semanticky,
 * jen ověřuje že dispatch JSON build prošel.
 *
 * Pozn.: Žádný test sémanticky neopírá hodnoty fake symbolů - jsou
 * jen pro link / pre-existing baseline ladění. Pokud někdo přidá test
 * který ověřuje hodnoty z platform/info, musí buď přesměrovat symboly
 * sem nebo testovat full emu binárku přes e2e Python testy. */
const char    *g_mzarch_full_name        = "MZ-stub";
const char    *g_mzarch_platform_name    = "stub";
int            g_mzarch_platform_numeric = 0;
const uint32_t g_mzarch_platform_pxclk   = 0;
struct {
    uint8_t _pad[4096];
} g_gdg;


/* V1.A.6 - shim implementace callstack_snapshot_free pro test build.
 *
 * Dispatch.c v _handle_callstack_get volá callstack_snapshot_free() na
 * pole, které stub alokoval přes g_malloc0. callstack_snapshot_get v
 * reálném buildu používá g_malloc, takže g_free je správný způsob
 * uvolnění. Forward-declarace je `void callstack_snapshot_free(void *)`,
 * v reálném callstack.h má parametr `st_CALLSTACK_ENTRY *`, ale C linkage
 * je stejná (= pointer). */
void callstack_snapshot_free(void *entries) {
    g_free(entries);
}

/** @brief Sdílený stav mock submitu (deklarovaný v headeru). */
st_DISPATCH_STUB_STATE g_stub_state;


/**
 * @brief Reset stub state před každým testem.
 *
 * V0.B.6 také resetuje counters / last-call záznamy pro step_into,
 * step_over, bp_remove, bp_set_enabled a run_to (= viz header doc).
 * `bp_remove_last_id` se nastavuje na -1, aby test mohl rozpoznat
 * "nikdy nevolán" od "volán s id=0".
 */
void dispatch_stub_reset(void) {
    /* V1.A.1: uvolnit případné heap stringy z předchozího testu před
     * memsetem (jinak by se ztratily). */
    if (g_stub_state.snapshot_last_filepath) {
        g_free(g_stub_state.snapshot_last_filepath);
    }
    if (g_stub_state.snapshot_last_description) {
        g_free(g_stub_state.snapshot_last_description);
    }
    /* V1.A.2: uvolnit symbol heap stringy. */
    if (g_stub_state.sym_last_name) {
        g_free(g_stub_state.sym_last_name);
    }
    if (g_stub_state.sym_last_comment) {
        g_free(g_stub_state.sym_last_comment);
    }
    if (g_stub_state.sym_last_prefix) {
        g_free(g_stub_state.sym_last_prefix);
    }
    /* V1.A.5: uvolnit heap stringy z IRQ injection. */
    if (g_stub_state.irq_inject_last_source) {
        g_free(g_stub_state.irq_inject_last_source);
    }
    /* V1.A.6: uvolnit heap stringy z Watch + CDL. */
    if (g_stub_state.watch_add_last_name) {
        g_free(g_stub_state.watch_add_last_name);
    }
    if (g_stub_state.watch_add_last_expr_text) {
        g_free(g_stub_state.watch_add_last_expr_text);
    }
    if (g_stub_state.watch_remove_last_name) {
        g_free(g_stub_state.watch_remove_last_name);
    }
    if (g_stub_state.watch_eval_last_name) {
        g_free(g_stub_state.watch_eval_last_name);
    }
    if (g_stub_state.watch_eval_last_expr_text) {
        g_free(g_stub_state.watch_eval_last_expr_text);
    }
    if (g_stub_state.cdl_export_last_path) {
        g_free(g_stub_state.cdl_export_last_path);
    }
    /* 0017 FÁZE 1 - uvolnit trace save path heap string. */
    if (g_stub_state.trace_save_last_path) {
        g_free(g_stub_state.trace_save_last_path);
    }
    /* V1.A.7 - uvolnit profiler export heap string. */
    if (g_stub_state.profiler_export_last_path) {
        g_free(g_stub_state.profiler_export_last_path);
    }
    /* V1.B.1 - uvolnit media filepath heap string. */
    if (g_stub_state.media_last_filepath) {
        g_free(g_stub_state.media_last_filepath);
    }
    /* V1.B.2 - uvolnit settings + periph heap stringy. */
    if (g_stub_state.settings_last_module) {
        g_free(g_stub_state.settings_last_module);
    }
    if (g_stub_state.settings_last_element) {
        g_free(g_stub_state.settings_last_element);
    }
    if (g_stub_state.settings_last_new_value) {
        g_free(g_stub_state.settings_last_new_value);
    }
    if (g_stub_state.settings_fake_value) {
        g_free(g_stub_state.settings_fake_value);
    }
    if (g_stub_state.periph_last_option_value) {
        g_free(g_stub_state.periph_last_option_value);
    }
    /* F015b - uvolnit zachycený expr z BP_CREATE_WITH_INIT. */
    if (g_stub_state.bp_create_last_expr) {
        g_free(g_stub_state.bp_create_last_expr);
    }
    memset(&g_stub_state, 0, sizeof(g_stub_state));
    memset(&g_dbgapi_cmdrq_queue, 0, sizeof(g_dbgapi_cmdrq_queue));
    g_stub_state.bp_remove_last_id      = -1;
    g_stub_state.bp_set_enabled_last_id = -1;
    g_stub_state.snapshot_save_buf_size    = 8;
    g_stub_state.snapshot_save_buf_pattern = 0xAB;
    /* V1.A.3 raster sekvence: bez explicitního nastavení testem se chová
     * jako statický raster na (frame=10, line=100, col=200, cycles=1000),
     * žádný posun mezi voláními (= run_until_* polling skončí timeoutem
     * pokud test nenastaví step). */
    g_stub_state.raster_initial_frame        = 10;
    g_stub_state.raster_initial_scanline     = 100;
    g_stub_state.raster_initial_column       = 200;
    g_stub_state.raster_initial_total_cycles = 1000;
    g_stub_state.raster_initial_frame_cycles = 500;
    /* V1.A.6 - inicializovat watch_remove_last_index na -1 (= "nikdy
     * nevolán") + watch_eval_last_index na -1 obdobně. */
    g_stub_state.watch_remove_last_index = -1;
    g_stub_state.watch_eval_last_index   = -1;
    g_stub_state.watch_add_fake_index    = 0;
    /* V1.B.1 - media. media_last_slot/cmd default 0 (= NONE). */
    /* V1.B.2 - default: platform fake active = 2 (mz800), result = -10
     * (= runtime switch nepodporován). Periph default requires_restart
     * = 1, result = 0. Settings fake_result = 0 (= OK). */
    g_stub_state.platform_fake_active_kind   = 2;
    g_stub_state.platform_fake_result        = -10;
    g_stub_state.periph_fake_requires_restart = 1;
    /* V1.D.3.A - Z80 PIO default available=1 (= MZ-800/1500 build);
     * test pro MZ-700 simulaci nastaví fake_available=0 explicitně. */
    g_stub_state.periph_z80_pio_fake_available = 1;
    /* V1.D.3.B - SN76489 default available=1, mono (= MZ-800 mono initial).
     * MZ-700 simulace: fake_available=0. MZ-1500 / stereo simulace:
     * fake_stereo=1. Beeper default ctc0_out=1, gate0=1, pc0=1 (= level=1
     * audible). */
    g_stub_state.periph_sn76489_fake_available = 1;
    g_stub_state.periph_sn76489_fake_stereo    = 0;
    g_stub_state.periph_beeper_fake_ctc0_out   = 1;
    g_stub_state.periph_beeper_fake_gate0      = 1;
    g_stub_state.periph_beeper_fake_pc0        = 1;
    /* V1.D.3.C - storage + display Resources defaults.
     *
     * GDG default: MZ-800 layout (16-color palette, has_border_reg=1,
     * has_pal_group=1, has_cksw=1). Test pro MZ-700/MZ-1500 přepíše
     * platform string + palette_count + flags na 0.
     *
     * WD1793 default: available=1 (FDC compiled + connected), drive0
     * mounted=0 (= no DSK image). Test pro detached state nastaví
     * fake_available=0.
     *
     * CMT default: state=STOP. Test může přepnout na PLAY/RECORD.
     *
     * QD default: available=1, type=IMAGE. Test pro detached state
     * nastaví fake_available=0.
     */
    memcpy(g_stub_state.periph_gdg_fake_platform, "mz800", 6);
    g_stub_state.periph_gdg_fake_palette_count  = 16;
    g_stub_state.periph_gdg_fake_has_border_reg = 1;
    g_stub_state.periph_gdg_fake_has_pal_group  = 1;
    g_stub_state.periph_gdg_fake_has_cksw       = 1;
    g_stub_state.periph_gdg_fake_regDMD         = 0;
    g_stub_state.periph_wd1793_fake_available   = 1;
    g_stub_state.periph_wd1793_fake_status      = 0;
    g_stub_state.periph_wd1793_fake_drive0_present = 0;
    g_stub_state.periph_cmt_fake_state          = 0;
    g_stub_state.periph_qd_fake_available       = 1;
    g_stub_state.periph_qd_fake_type            = 0;
    /* V1.D.4 - input + frame defaults. */
    g_stub_state.input_kbd_state_calls          = 0;
    memset(g_stub_state.kbd_real_matrix,    0xff, 10);
    memset(g_stub_state.kbd_virtual_matrix, 0xff, 10);
    g_stub_state.input_kbd_matrix_info_calls    = 0;
    g_stub_state.input_joy_state_calls          = 0;
    g_stub_state.joy_port0_connected            = 0;
    g_stub_state.joy_port0_state_bits           = 0;
    g_stub_state.frame_fb_info_calls            = 0;
    g_stub_state.fb_screen_id                   = 42;
    g_stub_state.fb_state                       = 0;
    g_stub_state.frame_screenshot_raw_calls     = 0;
    g_stub_state.fb_available                   = 1;
    g_stub_state.frame_screenshot_png_calls     = 0;
    g_stub_state.video_text_dump_calls          = 0;
    g_stub_state.text_dump_available            = 1;

    /* V1.D.2.C - per-watch snapshot Resource backing. */
    g_stub_state.watch_snapshot_calls           = 0;
    g_free(g_stub_state.watch_snapshot_last_name);
    g_stub_state.watch_snapshot_last_name       = NULL;
    g_stub_state.watch_snapshot_fake_found      = 0;
    g_stub_state.watch_snapshot_fake_snapshot_active = 0;
    g_stub_state.watch_snapshot_fake_min_max_valid   = 0;
    g_stub_state.watch_snapshot_fake_row_id     = 0;
    g_stub_state.watch_snapshot_fake_type_snap  = 0;
    g_stub_state.watch_snapshot_fake_snap_int   = 0;
    g_stub_state.watch_snapshot_fake_cur_int    = 0;
    g_stub_state.watch_snapshot_fake_delta_int  = 0;
    g_stub_state.watch_snapshot_fake_min_int    = 0;
    g_stub_state.watch_snapshot_fake_max_int    = 0;
    g_stub_state.watch_snapshot_fake_change_count = 0;
}


/**
 * @brief Mock implementace `dbgapi_ui_submit_cmd_sync_with_origin`.
 *
 * Zaznamenává parametry do `g_stub_state` a podle scénáře buď fake
 * naplní result_ptr nebo vrátí false (= fail_next).
 *
 * Pro CMD_GET_ALL_REGS: pokud `fill_regs == true`, naplní pole 14
 * uint16_t hodnotami 0x1000..0x1D00 (deterministický pattern).
 *
 * Pro CMD_BP_ADD: pokud data_ptr (st_DBGAPI_BP_PARAM) má pole `id`,
 * stub do něj zapíše `fill_bp_id` (= simulace přidělení handlerem).
 *
 * Pro CMD_BP_LIST: pokud result_ptr (st_DBGAPI_BP_LIST_RESULT), stub
 * naplní `count = fill_bp_list_count` a `bp[i]` deterministicky
 * (i, addr=0x1000+i, enabled=true). F015b: bp[0] navíc nese
 * type/zone/bank_id/hits z bp_list_fake_* a condition g_strdup()
 * z bp_list_fake_condition (caller je povinen uvolnit).
 *
 * Pro CMD_BP_CREATE_WITH_INIT (F015b): zachytí p->type / addr /
 * update_mask / expr do bp_create_last_* a naplní p->id =
 * bp_create_fake_id (= simulace typed create path).
 *
 * Pro CMD_IS_RUNNING: zapíše do *(bool *)result_ptr hodnotu `is_running`.
 */
bool dbgapi_ui_submit_cmd_sync_with_origin(st_DBGAPI_CMDRQ_QUEUE *queue,
                                            en_DBGAPI_CMD cmd,
                                            en_DBGAPI_CMD_ORIGIN origin,
                                            void *data_ptr,
                                            void *result_ptr,
                                            int timeout_ms) {
    (void)queue;
    (void)timeout_ms;

    g_stub_state.last_cmd     = cmd;
    g_stub_state.last_origin  = origin;
    g_stub_state.last_data    = data_ptr;
    g_stub_state.last_result  = result_ptr;
    g_stub_state.call_count++;

    if (g_stub_state.fail_next) {
        g_stub_state.fail_next = false;
        return false;
    }

    /* V0.B.6 - step_n fail-after-N scénář: pokud test nastavil
     * step_into_fail_after_n a aktuální cmd je STEP_INTO, počítáme
     * úspěšné volání; jakmile překročíme limit, vrátíme false. */
    if (cmd == DBGAPI_CMD_STEP_INTO &&
        g_stub_state.step_into_fail_after_n > 0 &&
        g_stub_state.step_into_calls >= g_stub_state.step_into_fail_after_n) {
        /* I tak započítáme do counteru jen úspěšné volání. Failed
         * call zaznamenáme do separátního stavu by extending state
         * pokud bude potřeba; pro V0.B.6 stačí prostý break. */
        return false;
    }

    /* Cmd-specific fake fill */
    switch (cmd) {
        case DBGAPI_CMD_IS_RUNNING:
            if (result_ptr) {
                *(bool *)result_ptr = g_stub_state.is_running;
            }
            break;

        case DBGAPI_CMD_GET_ALL_REGS:
            if (result_ptr && g_stub_state.fill_regs) {
                uint16_t *regs = (uint16_t *)result_ptr;
                for (int i = 0; i < DBGAPI_REG_COUNT; i++) {
                    regs[i] = (uint16_t)(0x1000 + (i << 8));
                }
            }
            break;

        case DBGAPI_CMD_MEM_READ:
            if (data_ptr) {
                st_DBGAPI_MEM_PARAM *p = (st_DBGAPI_MEM_PARAM *)data_ptr;
                /* Naplň deterministicky 0xAA + index */
                for (uint16_t i = 0; i < p->len && p->buf; i++) {
                    p->buf[i] = (uint8_t)(0xA0 + (i & 0x0F));
                }
            }
            break;

        case DBGAPI_CMD_BP_ADD:
            if (data_ptr) {
                st_DBGAPI_BP_PARAM *p = (st_DBGAPI_BP_PARAM *)data_ptr;
                p->id = g_stub_state.fill_bp_id;
            }
            break;

        case DBGAPI_CMD_BP_LIST:
            if (result_ptr) {
                st_DBGAPI_BP_LIST_RESULT *r =
                    (st_DBGAPI_BP_LIST_RESULT *)result_ptr;
                int n = g_stub_state.fill_bp_list_count;
                if (n > r->max_count) n = r->max_count;
                r->count = n;
                for (int i = 0; i < n; i++) {
                    r->bp[i].id      = 100 + i;
                    r->bp[i].addr    = (uint16_t)(0x1000 + i);
                    r->bp[i].enabled = true;
                    /* F015b - bp[0] nese typed atributy + condition (heap
                     * g_strdup = simulace dbgapi ownership; caller MUSÍ
                     * uvolnit). bp[1..] zůstanou výchozí (NULL condition). */
                    if (i == 0) {
                        r->bp[i].type    = g_stub_state.bp_list_fake_type;
                        r->bp[i].zone    = g_stub_state.bp_list_fake_zone;
                        r->bp[i].bank_id = g_stub_state.bp_list_fake_bank_id;
                        r->bp[i].hits    = g_stub_state.bp_list_fake_hits;
                        r->bp[i].condition =
                            g_stub_state.bp_list_fake_condition
                            ? g_strdup(g_stub_state.bp_list_fake_condition)
                            : NULL;
                    }
                }
            }
            break;

        case DBGAPI_CMD_BP_CREATE_WITH_INIT:
            /* F015b - zachytí typed create path. Dispatch handler předá
             * st_DBGAPI_BP_UPDATE_PARAM* s type/addr/update_mask/expr a
             * čeká, že handler naplní p->id přiděleným ID. */
            if (data_ptr) {
                st_DBGAPI_BP_UPDATE_PARAM *p =
                    (st_DBGAPI_BP_UPDATE_PARAM *)data_ptr;
                g_stub_state.bp_create_calls++;
                g_stub_state.bp_create_last_type = p->type;
                g_stub_state.bp_create_last_addr = p->addr;
                g_stub_state.bp_create_last_mask = p->update_mask;
                g_stub_state.bp_create_last_zone = p->zone;
                g_stub_state.bp_create_last_event_trigger = p->event_trigger;
                g_free(g_stub_state.bp_create_last_expr);
                g_stub_state.bp_create_last_expr =
                    p->expr ? g_strdup(p->expr) : NULL;
                p->id = g_stub_state.bp_create_fake_id;
            }
            break;

        case DBGAPI_CMD_MEM_WRITE_CHECKED:
            if (data_ptr) {
                st_DBGAPI_MEM_WRITE_CHECKED_PARAM *p =
                    (st_DBGAPI_MEM_WRITE_CHECKED_PARAM *)data_ptr;
                /* Zkopíruj data pro pozdější inspekci testem - dispatch.c
                 * po návratu uvolní původní buffer. */
                g_stub_state.mem_write_addr    = p->addr;
                g_stub_state.mem_write_buf_len = p->length;
                if (p->data && p->length > 0) {
                    uint16_t copy_len = p->length;
                    if (copy_len > sizeof(g_stub_state.mem_write_buf)) {
                        copy_len = sizeof(g_stub_state.mem_write_buf);
                    }
                    memcpy(g_stub_state.mem_write_buf, p->data, copy_len);
                    g_stub_state.mem_write_buf_len = copy_len;
                }
                if (g_stub_state.mem_write_fail) {
                    p->success           = 0;
                    p->first_failed_addr = g_stub_state.mem_write_fail_addr;
                    p->first_failed_kind = g_stub_state.mem_write_fail_kind;
                } else {
                    p->success           = 1;
                    p->first_failed_addr = 0;
                    p->first_failed_kind = 0;
                }
            }
            break;

        case DBGAPI_CMD_BP_REMOVE:
            /* V0.B.6 - zaznamenej ID poslední BP_REMOVE call. */
            if (data_ptr) {
                st_DBGAPI_BP_PARAM *p = (st_DBGAPI_BP_PARAM *)data_ptr;
                g_stub_state.bp_remove_last_id = p->id;
            }
            break;

        case DBGAPI_CMD_BP_SET_ENABLED:
            /* V0.B.6 - zaznamenej id + enabled. */
            if (data_ptr) {
                st_DBGAPI_BP_SET_ENABLED_PARAM *p =
                    (st_DBGAPI_BP_SET_ENABLED_PARAM *)data_ptr;
                g_stub_state.bp_set_enabled_last_id      = p->id;
                g_stub_state.bp_set_enabled_last_enabled = p->enabled;
            }
            break;

        case DBGAPI_CMD_STEP_INTO:
            /* V0.B.6 - inkrementuj counter (pro step_n + step_into testy). */
            g_stub_state.step_into_calls++;
            break;

        case DBGAPI_CMD_STEP_OVER:
            /* V0.B.6 - counter pro step_over test. */
            g_stub_state.step_over_calls++;
            break;

        case DBGAPI_CMD_RUN_TO:
            /* V0.B.6 - zaznamenej target adresu (data_ptr = uint16_t*). */
            if (data_ptr) {
                g_stub_state.run_to_last_addr = *(uint16_t *)data_ptr;
            }
            break;

        case DBGAPI_CMD_SNAPSHOT_SAVE_FILE:
        case DBGAPI_CMD_SNAPSHOT_LOAD_FILE:
            /* V1.A.1 - zachyť filepath + description pro test assert.
             * Stringy duplikujeme přes g_strdup, vlastní stub
             * (uvolňuje dispatch_stub_reset). */
            if (data_ptr) {
                st_DBGAPI_SNAPSHOT_PARAM *p =
                    (st_DBGAPI_SNAPSHOT_PARAM *)data_ptr;
                if (g_stub_state.snapshot_last_filepath) {
                    g_free(g_stub_state.snapshot_last_filepath);
                }
                g_stub_state.snapshot_last_filepath =
                    p->filepath ? g_strdup(p->filepath) : NULL;
                if (cmd == DBGAPI_CMD_SNAPSHOT_SAVE_FILE) {
                    if (g_stub_state.snapshot_last_description) {
                        g_free(g_stub_state.snapshot_last_description);
                    }
                    g_stub_state.snapshot_last_description =
                        p->description ? g_strdup(p->description) : NULL;
                }
                p->result = 0;
            }
            break;

        case DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER:
            /* V1.A.1 - alokuj fake buffer s deterministickým patternem;
             * vyplň description echo. Dispatch.c po návratu volá
             * g_base64_encode + g_free. */
            if (data_ptr) {
                st_DBGAPI_SNAPSHOT_PARAM *p =
                    (st_DBGAPI_SNAPSHOT_PARAM *)data_ptr;
                if (g_stub_state.snapshot_last_description) {
                    g_free(g_stub_state.snapshot_last_description);
                }
                g_stub_state.snapshot_last_description =
                    p->description ? g_strdup(p->description) : NULL;
                size_t n = g_stub_state.snapshot_save_buf_size;
                if (n == 0) n = 8;
                p->buffer = (uint8_t *)g_malloc(n);
                memset(p->buffer, g_stub_state.snapshot_save_buf_pattern, n);
                p->buffer_size = n;
                p->result      = 0;
            }
            break;

        case DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER:
            /* V1.A.1 - zachyť velikost + první byte (= test verifikuje
             * že base64 dekódování v dispatch.c proběhlo správně). */
            if (data_ptr) {
                st_DBGAPI_SNAPSHOT_PARAM *p =
                    (st_DBGAPI_SNAPSHOT_PARAM *)data_ptr;
                g_stub_state.snapshot_load_buf_size  = p->buffer_size;
                g_stub_state.snapshot_load_buf_first =
                    (p->buffer && p->buffer_size > 0) ? p->buffer[0] : 0;
                p->result = 0;
            }
            break;

        case DBGAPI_CMD_SYMBOL_ADD:
        case DBGAPI_CMD_SYMBOL_REMOVE:
            /* V1.A.2 - zachyť name + addr + (pro ADD) comment. Stringy
             * duplikujeme přes g_strdup, uvolňuje dispatch_stub_reset. */
            if (data_ptr) {
                st_DBGAPI_SYMBOL_PARAM *p =
                    (st_DBGAPI_SYMBOL_PARAM *)data_ptr;
                if (g_stub_state.sym_last_name) {
                    g_free(g_stub_state.sym_last_name);
                }
                g_stub_state.sym_last_name =
                    p->name ? g_strdup(p->name) : NULL;
                g_stub_state.sym_last_addr = p->addr;
                if (cmd == DBGAPI_CMD_SYMBOL_ADD) {
                    if (g_stub_state.sym_last_comment) {
                        g_free(g_stub_state.sym_last_comment);
                    }
                    g_stub_state.sym_last_comment =
                        p->comment ? g_strdup(p->comment) : NULL;
                }
            }
            break;

        case DBGAPI_CMD_SYMBOL_LOOKUP:
            /* V1.A.2 - zachyť kritéria, vyplň fake výsledek dle scénáře.
             * Pokud sym_lookup_found = false, nastavíme out_count = 0
             * (= nenalezeno). */
            if (data_ptr) {
                st_DBGAPI_SYMBOL_PARAM *p =
                    (st_DBGAPI_SYMBOL_PARAM *)data_ptr;
                if (g_stub_state.sym_last_name) {
                    g_free(g_stub_state.sym_last_name);
                }
                g_stub_state.sym_last_name =
                    p->name ? g_strdup(p->name) : NULL;
                g_stub_state.sym_last_addr = p->addr;
                if (g_stub_state.sym_lookup_found && p->out_entries &&
                    p->out_max >= 1) {
                    p->out_entries[0].addr =
                        g_stub_state.sym_lookup_fake_addr;
                    p->out_entries[0].name =
                        g_stub_state.sym_lookup_fake_name
                            ? g_strdup(g_stub_state.sym_lookup_fake_name)
                            : NULL;
                    p->out_entries[0].comment =
                        g_stub_state.sym_lookup_fake_comment
                            ? g_strdup(g_stub_state.sym_lookup_fake_comment)
                            : NULL;
                    p->out_entries[0].source =
                        g_stub_state.sym_lookup_fake_source;
                    p->out_count = 1;
                } else {
                    p->out_count = 0;
                }
            }
            break;

        case DBGAPI_CMD_SYMBOL_LIST:
            /* V1.A.2 - zachyť prefix, vyplň fake záznamy. Format:
             * addr=0x4000+i, name="SYM_<i>", comment="cmt_<i>",
             * source=SYM_SOURCE_LBL (=3). */
            if (data_ptr) {
                st_DBGAPI_SYMBOL_PARAM *p =
                    (st_DBGAPI_SYMBOL_PARAM *)data_ptr;
                if (g_stub_state.sym_last_prefix) {
                    g_free(g_stub_state.sym_last_prefix);
                }
                g_stub_state.sym_last_prefix =
                    p->prefix ? g_strdup(p->prefix) : NULL;
                int n = g_stub_state.sym_list_fake_count;
                if ((size_t)n > p->out_max) n = (int)p->out_max;
                for (int i = 0; i < n && p->out_entries; i++) {
                    p->out_entries[i].addr    = (uint16_t)(0x4000 + i);
                    p->out_entries[i].name    =
                        g_strdup_printf("SYM_%d", i);
                    p->out_entries[i].comment =
                        g_strdup_printf("cmt_%d", i);
                    p->out_entries[i].source  = 3;  /* LBL */
                }
                p->out_count = (size_t)(n > 0 ? n : 0);
            }
            break;

        case DBGAPI_CMD_STEP_OUT:
            /* V1.A.3 - vyplň status + return_addr podle scénáře.
             * Pokud status != 0, vrátíme false (= submit failed); test
             * pak ověří že dispatch handler správně rozliší error path. */
            if (data_ptr) {
                st_DBGAPI_STEP_OUT_PARAM *p =
                    (st_DBGAPI_STEP_OUT_PARAM *)data_ptr;
                p->status      = g_stub_state.step_out_fake_status;
                p->return_addr = g_stub_state.step_out_fake_return_addr;
                if (p->status != 0) {
                    return false;
                }
            }
            break;

        case DBGAPI_CMD_GET_RASTER_POS:
            /* V1.A.3 - polling smyčka run_until_* opakovaně čte raster
             * snapshot. Simulujeme pokrok přes raster_seq_step_lines /
             * raster_seq_step_cycles - každé volání posune scanline a
             * total_cycles. raster_seq_idx slouží jako counter pro
             * test inspekci. */
            if (result_ptr) {
                st_DBGAPI_RASTER_POS *out =
                    (st_DBGAPI_RASTER_POS *)result_ptr;
                int idx = g_stub_state.raster_seq_idx;
                uint32_t total = g_stub_state.raster_initial_total_cycles
                    + (uint32_t)(idx * g_stub_state.raster_seq_step_cycles);
                uint32_t line  = (uint32_t)g_stub_state.raster_initial_scanline
                    + (uint32_t)(idx * g_stub_state.raster_seq_step_lines);
                /* Frame wrap při scanline >= 312 (= PAL frame). */
                uint32_t frame = g_stub_state.raster_initial_frame
                    + (line / 312u);
                line = line % 312u;
                out->frame_number = frame;
                out->scanline     = (uint16_t)line;
                out->column_pixel = g_stub_state.raster_initial_column;
                out->total_cycles = total;
                out->frame_cycles = g_stub_state.raster_initial_frame_cycles;
                g_stub_state.raster_seq_idx++;
            }
            break;

        case DBGAPI_CMD_IO_READ:
            /* V1.A.5 - I/O read: stub vyplní param.value podle scénáře. */
            if (data_ptr) {
                st_DBGAPI_IO_PARAM *p = (st_DBGAPI_IO_PARAM *)data_ptr;
                g_stub_state.io_last_port  = p->port;
                p->value                    = g_stub_state.io_read_fake_value;
                g_stub_state.io_last_value  = p->value;
                g_stub_state.io_read_calls++;
            }
            break;

        case DBGAPI_CMD_IO_WRITE:
            /* V1.A.5 - I/O write: stub zachytí port + value. */
            if (data_ptr) {
                st_DBGAPI_IO_PARAM *p = (st_DBGAPI_IO_PARAM *)data_ptr;
                g_stub_state.io_last_port  = p->port;
                g_stub_state.io_last_value = p->value;
                g_stub_state.io_write_calls++;
            }
            break;

        case DBGAPI_CMD_IRQ_INJECT:
            /* V1.A.5 - IRQ inject: stub zachytí source + vector. */
            if (data_ptr) {
                st_DBGAPI_IRQ_INJECT_PARAM *p =
                    (st_DBGAPI_IRQ_INJECT_PARAM *)data_ptr;
                if (g_stub_state.irq_inject_last_source) {
                    g_free(g_stub_state.irq_inject_last_source);
                }
                g_stub_state.irq_inject_last_source =
                    p->source ? g_strdup(p->source) : NULL;
                g_stub_state.irq_inject_last_vector       = p->vector;
                g_stub_state.irq_inject_last_vector_valid = p->vector_valid;
                g_stub_state.irq_inject_calls++;
            }
            break;

        case DBGAPI_CMD_NMI_INJECT:
            /* V1.A.5 - NMI inject: jen counter (bez parametru). */
            g_stub_state.nmi_inject_calls++;
            break;

        case DBGAPI_CMD_WATCH_ADD:
            /* V1.A.6 - Watch add: stub zachytí parametry + zapíše out_index. */
            if (data_ptr) {
                st_DBGAPI_WATCH_ADD_PARAM *p =
                    (st_DBGAPI_WATCH_ADD_PARAM *)data_ptr;
                g_stub_state.watch_add_last_mode = p->mode;
                g_stub_state.watch_add_last_addr = p->addr;
                g_stub_state.watch_add_last_type = p->type;
                if (g_stub_state.watch_add_last_name) {
                    g_free(g_stub_state.watch_add_last_name);
                }
                g_stub_state.watch_add_last_name =
                    p->name ? g_strdup(p->name) : NULL;
                if (g_stub_state.watch_add_last_expr_text) {
                    g_free(g_stub_state.watch_add_last_expr_text);
                }
                g_stub_state.watch_add_last_expr_text =
                    p->expr_text ? g_strdup(p->expr_text) : NULL;
                p->out_index = g_stub_state.watch_add_fake_index;
            }
            break;

        case DBGAPI_CMD_WATCH_REMOVE:
            /* V1.A.6 - Watch remove: stub zachytí name + index, vyplní
             * out_removed podle scénáře. */
            if (data_ptr) {
                st_DBGAPI_WATCH_REMOVE_PARAM *p =
                    (st_DBGAPI_WATCH_REMOVE_PARAM *)data_ptr;
                if (g_stub_state.watch_remove_last_name) {
                    g_free(g_stub_state.watch_remove_last_name);
                }
                g_stub_state.watch_remove_last_name =
                    p->name ? g_strdup(p->name) : NULL;
                g_stub_state.watch_remove_last_index = p->index;
                if (g_stub_state.watch_remove_found) {
                    p->out_removed = 1;
                    if (p->index < 0) p->index = 0;
                } else {
                    p->out_removed = 0;
                    p->index = -1;
                    return false;
                }
            }
            break;

        case DBGAPI_CMD_WATCH_LIST:
            /* V1.A.6 - Watch list: stub vyplní fake_count entries. */
            if (data_ptr) {
                st_DBGAPI_WATCH_LIST_PARAM *p =
                    (st_DBGAPI_WATCH_LIST_PARAM *)data_ptr;
                int n = g_stub_state.watch_list_fake_count;
                if (n > p->out_max) n = p->out_max;
                for (int i = 0; i < n && p->out_entries; i++) {
                    p->out_entries[i].index = i;
                    p->out_entries[i].name = g_strdup_printf("W_%d", i);
                    p->out_entries[i].mode = DBGAPI_WATCH_MODE_ADDRESS;
                    p->out_entries[i].type = DBGAPI_WATCH_TYPE_U8;
                    p->out_entries[i].addr = (uint16_t)(0x100 + i);
                    p->out_entries[i].expr_text = NULL;
                    p->out_entries[i].value_str =
                        g_strdup_printf("v_%d", i);
                }
                p->out_count = (n > 0) ? n : 0;
            }
            break;

        case DBGAPI_CMD_WATCH_EVAL:
            /* V1.A.6 - Watch eval: stub zachytí kritéria + vyplní
             * out_value_*. Pokud fake_error nastavený, vrátíme false. */
            if (data_ptr) {
                st_DBGAPI_WATCH_EVAL_PARAM *p =
                    (st_DBGAPI_WATCH_EVAL_PARAM *)data_ptr;
                if (g_stub_state.watch_eval_last_name) {
                    g_free(g_stub_state.watch_eval_last_name);
                }
                g_stub_state.watch_eval_last_name =
                    p->name ? g_strdup(p->name) : NULL;
                g_stub_state.watch_eval_last_index = p->index;
                if (g_stub_state.watch_eval_last_expr_text) {
                    g_free(g_stub_state.watch_eval_last_expr_text);
                }
                g_stub_state.watch_eval_last_expr_text =
                    p->expr_text ? g_strdup(p->expr_text) : NULL;
                p->out_value_int = g_stub_state.watch_eval_fake_value_int;
                p->out_value_str =
                    g_strdup(g_stub_state.watch_eval_fake_value_str
                             ? g_stub_state.watch_eval_fake_value_str : "");
                if (g_stub_state.watch_eval_fake_error) {
                    p->out_error =
                        g_strdup(g_stub_state.watch_eval_fake_error);
                    return false;
                }
                p->out_error = NULL;
            }
            break;

        case DBGAPI_CMD_GET_CALLSTACK:
            /* V1.A.6 - Callstack snapshot: stub alokuje fake entries
             * pole a vyplní deterministické záznamy. Pole musí být
             * g_malloc - dispatch handler ho uvolní přes
             * callstack_snapshot_free (= shim alias na g_free v test build). */
            if (data_ptr) {
                st_DBGAPI_CALLSTACK_GET_PARAM *p =
                    (st_DBGAPI_CALLSTACK_GET_PARAM *)data_ptr;
                int n = g_stub_state.callstack_fake_count;
                if (n > 0) {
                    /* Použijeme prosté pole 24 B per entry (= layout
                     * st_CALLSTACK_ENTRY v callstack.h). Pro stub nás
                     * zajímají jen prvních ~16 B (return_addr, call_site,
                     * target, sp_at_entry, cycles_at_entry). Alokujeme
                     * 256 B per entry pro jistotu future ABI změn. */
                    size_t entry_sz = 64;
                    p->entries = g_malloc0((size_t)n * entry_sz);
                    /* Deterministický pattern - test si přepisuje jednotlivá
                     * pole přes cast na vlastní layout pokud chce. */
                    p->count = n;
                } else {
                    p->entries = NULL;
                    p->count = 0;
                }
                p->active = g_stub_state.callstack_fake_active;
                p->current_depth = g_stub_state.callstack_fake_current_depth;
                p->cycles_now = g_stub_state.callstack_fake_cycles_now;
                p->max_depth_reached = 0;
                p->divergence_count = 0;
                p->overflow_count = 0;
            }
            break;

        case DBGAPI_CMD_CDL_START:
            g_stub_state.cdl_start_calls++;
            break;
        case DBGAPI_CMD_CDL_STOP:
            g_stub_state.cdl_stop_calls++;
            break;
        case DBGAPI_CMD_CDL_RESET:
            g_stub_state.cdl_reset_calls++;
            break;
        case DBGAPI_CMD_CDL_EXPORT:
            if (data_ptr) {
                st_DBGAPI_CDL_EXPORT_PARAM *p =
                    (st_DBGAPI_CDL_EXPORT_PARAM *)data_ptr;
                if (g_stub_state.cdl_export_last_path) {
                    g_free(g_stub_state.cdl_export_last_path);
                }
                g_stub_state.cdl_export_last_path =
                    p->meta_path ? g_strdup(p->meta_path) : NULL;
                p->out_result = g_stub_state.cdl_export_fake_result;
                p->out_region_count =
                    g_stub_state.cdl_export_fake_region_count;
                if (p->out_result != 0) {
                    return false;
                }
            }
            break;

        /* 0017 FÁZE 1 - Tracking lifecycle (trace-suite) */
        case DBGAPI_CMD_TRACE_START:
        case DBGAPI_CMD_TRACE_STOP:
        case DBGAPI_CMD_TRACE_RESET:
        case DBGAPI_CMD_TRACE_SAVE:
            if (data_ptr) {
                st_DBGAPI_TRACE_PARAM *p =
                    (st_DBGAPI_TRACE_PARAM *)data_ptr;
                g_stub_state.trace_last_channel = (int)p->channel;
                if (cmd == DBGAPI_CMD_TRACE_START) {
                    g_stub_state.trace_start_calls++;
                } else if (cmd == DBGAPI_CMD_TRACE_STOP) {
                    g_stub_state.trace_stop_calls++;
                } else if (cmd == DBGAPI_CMD_TRACE_RESET) {
                    g_stub_state.trace_reset_calls++;
                } else {
                    g_stub_state.trace_save_calls++;
                    if (g_stub_state.trace_save_last_path) {
                        g_free(g_stub_state.trace_save_last_path);
                    }
                    g_stub_state.trace_save_last_path =
                        p->path ? g_strdup(p->path) : NULL;
                }
                p->out_result = g_stub_state.trace_fake_result;
                if (p->out_result != 0) {
                    return false;
                }
            }
            break;

        case DBGAPI_CMD_PROFILER_SET_ACTIVE:
            /* V1.A.7 - profiler_start / profiler_stop */
            if (data_ptr) {
                st_DBGAPI_PROFILER_SET_ACTIVE_PARAM *p =
                    (st_DBGAPI_PROFILER_SET_ACTIVE_PARAM *)data_ptr;
                g_stub_state.profiler_set_active_calls++;
                g_stub_state.profiler_last_active = p->active;
            }
            break;

        case DBGAPI_CMD_PROFILER_RESET:
            /* V1.A.7 - profiler_reset */
            g_stub_state.profiler_reset_calls++;
            break;

        case DBGAPI_CMD_PROFILER_EXPORT:
            /* V1.A.7 - profiler_export */
            if (data_ptr) {
                st_DBGAPI_PROFILER_EXPORT_PARAM *p =
                    (st_DBGAPI_PROFILER_EXPORT_PARAM *)data_ptr;
                if (g_stub_state.profiler_export_last_path) {
                    g_free(g_stub_state.profiler_export_last_path);
                }
                g_stub_state.profiler_export_last_path =
                    p->filepath ? g_strdup(p->filepath) : NULL;
                g_stub_state.profiler_export_last_format = p->format;
                p->result = g_stub_state.profiler_export_fake_result;
                p->entry_count =
                    g_stub_state.profiler_export_fake_entry_count;
                if (p->result != 0) {
                    return false;
                }
            }
            break;

        case DBGAPI_CMD_GET_PROFILER:
            /* V1.A.7 - profiler_get. Alokuje fake pole entries
             * (callee-allocated, uvolňuje dispatch.c přes g_free).
             * Layout je st_PROF_ENTRY z profiler.h (= identický s
             * st_MCP_PROF_ENTRY_MIRROR v dispatch.c). Aby nebylo
             * potřeba taháat profiler.h do test buildu, alokujeme
             * raw buffer o správné velikosti (24 B per entry =
             * sizeof(st_MCP_PROF_ENTRY_MIRROR) - viz dispatch.c). */
            if (data_ptr) {
                st_DBGAPI_PROFILER_GET_PARAM *p =
                    (st_DBGAPI_PROFILER_GET_PARAM *)data_ptr;
                g_stub_state.profiler_get_calls++;
                int n = g_stub_state.profiler_get_fake_count;
                if (n > 0) {
                    /* Per-entry velikost = 48 B (NE 24 B), viz layout
                     * st_PROF_ENTRY v src/emulator/debugger/profiler.h:
                     *   uint16_t target_addr;     (offset  0)
                     *   uint8_t  kind;            (offset  2)
                     *   uint8_t  _pad;            (offset  3)
                     *   (4 B implicit padding na uint64 alignment)
                     *   uint64_t calls;           (offset  8)
                     *   uint64_t cycles_incl_sum; (offset 16)
                     *   uint64_t cycles_excl_sum; (offset 24)
                     *   uint64_t cycles_incl_min; (offset 32)
                     *   uint64_t cycles_incl_max; (offset 40)
                     *   total 48 B.
                     *
                     * Pozn: V1.A.7 (Kontrolor lesson learned) - původní
                     * hodnota 24u byla podhodnocená a způsobovala read
                     * out-of-bounds v _handle_profiler_get při iteraci
                     * přes void* entries cast na st_MCP_PROF_ENTRY_MIRROR.
                     * Hard assert nad shodou layoutu je v dispatch.c
                     * (= _Static_assert nad st_MCP_PROF_ENTRY_MIRROR). */
                    size_t bytes = (size_t)n * 48u;
                    void *buf = g_malloc0(bytes);
                    p->entries = buf;
                    p->entry_count = n;
                } else {
                    p->entries = NULL;
                    p->entry_count = 0;
                }
                p->active = g_stub_state.profiler_get_fake_active;
                p->total_cycles_64 =
                    g_stub_state.profiler_get_fake_total_cycles_64;
                p->total_calls =
                    g_stub_state.profiler_get_fake_total_calls;
                p->irq_entries =
                    g_stub_state.profiler_get_fake_irq_entries;
                p->unmatched_returns =
                    g_stub_state.profiler_get_fake_unmatched_returns;
                p->max_depth_reached =
                    g_stub_state.profiler_get_fake_max_depth_reached;
                p->overflow_count =
                    g_stub_state.profiler_get_fake_overflow_count;
            }
            break;

        case DBGAPI_CMD_MEM_WRITE_FORCE:
            /* V1.A.5 - raw memory write bez region check. Stub
             * zachytí addr + len + obsah do interního bufferu. */
            if (data_ptr) {
                st_DBGAPI_MEM_PARAM *p = (st_DBGAPI_MEM_PARAM *)data_ptr;
                g_stub_state.mwf_addr = p->addr;
                g_stub_state.mwf_len  = p->len;
                if (p->buf && p->len > 0) {
                    uint16_t copy_len = p->len;
                    if (copy_len > sizeof(g_stub_state.mwf_buf)) {
                        copy_len = sizeof(g_stub_state.mwf_buf);
                    }
                    memcpy(g_stub_state.mwf_buf, p->buf, copy_len);
                    g_stub_state.mwf_len = copy_len;
                }
            }
            break;

        case DBGAPI_CMD_MEDIA_LOAD_MZF:
        case DBGAPI_CMD_MEDIA_LOAD_BINARY:
        case DBGAPI_CMD_MEDIA_INSERT:
        case DBGAPI_CMD_MEDIA_EJECT:
            /* V1.B.1 - Media Tools. Stub zachytí slot, filepath,
             * addr a ro pro inspekci. result/out_size se naplní podle
             * media_fake_*. */
            if (data_ptr) {
                st_DBGAPI_MEDIA_PARAM *p =
                    (st_DBGAPI_MEDIA_PARAM *)data_ptr;
                g_stub_state.media_last_cmd = (int)cmd;
                g_stub_state.media_last_slot = (int)p->slot;
                if (g_stub_state.media_last_filepath) {
                    g_free(g_stub_state.media_last_filepath);
                    g_stub_state.media_last_filepath = NULL;
                }
                if (p->filepath) {
                    g_stub_state.media_last_filepath =
                        g_strdup(p->filepath);
                }
                g_stub_state.media_last_load_addr = p->load_addr;
                g_stub_state.media_last_ro = p->read_only;
                p->out_result = g_stub_state.media_fake_result;
                p->out_size   = g_stub_state.media_fake_out_size;
                if (p->out_result != 0) {
                    return false;
                }
            }
            break;

        case DBGAPI_CMD_MEDIA_STATE:
            /* V1.B.1 - Media state snapshot. Stub vyplní fake_count
             * slot info se synthetickými hodnotami. */
            if (data_ptr) {
                st_DBGAPI_MEDIA_STATE_PARAM *p =
                    (st_DBGAPI_MEDIA_STATE_PARAM *)data_ptr;
                g_stub_state.media_state_calls++;
                int n = g_stub_state.media_state_fake_count;
                if (n < 0) n = 0;
                if (n > 11) n = 11;
                memset(p, 0, sizeof(*p));
                /* Default ordering: CMT, FDC0_FD0..3, FDC1_FD0..3, QD, IDE8. */
                static const en_DBGAPI_MEDIA_SLOT order[11] = {
                    DBGAPI_MEDIA_SLOT_CMT,
                    DBGAPI_MEDIA_SLOT_FDC0_FD0,
                    DBGAPI_MEDIA_SLOT_FDC0_FD1,
                    DBGAPI_MEDIA_SLOT_FDC0_FD2,
                    DBGAPI_MEDIA_SLOT_FDC0_FD3,
                    DBGAPI_MEDIA_SLOT_FDC1_FD0,
                    DBGAPI_MEDIA_SLOT_FDC1_FD1,
                    DBGAPI_MEDIA_SLOT_FDC1_FD2,
                    DBGAPI_MEDIA_SLOT_FDC1_FD3,
                    DBGAPI_MEDIA_SLOT_QD,
                    DBGAPI_MEDIA_SLOT_IDE8,
                };
                for (int i = 0; i < n; i++) {
                    p->slots[i].slot = order[i];
                    p->slots[i].inserted = (i % 2 == 0) ? 1 : 0;
                    p->slots[i].read_only = 0;
                    snprintf(p->slots[i].filepath,
                              sizeof(p->slots[i].filepath),
                              "/fake/media_%d.img", i);
                }
                p->count = n;
            }
            break;

        case DBGAPI_CMD_SETTINGS_GET:
        case DBGAPI_CMD_SETTINGS_SET:
            /* V1.B.2 - settings. Stub zachytí module / element /
             * new_value a vrátí fake_value / fake_type / fake_result.
             * Když fake_result != 0, vrací false (= fail). */
            if (data_ptr) {
                st_DBGAPI_SETTINGS_PARAM *p =
                    (st_DBGAPI_SETTINGS_PARAM *)data_ptr;
                g_stub_state.settings_last_cmd = (int)cmd;
                if (g_stub_state.settings_last_module) {
                    g_free(g_stub_state.settings_last_module);
                    g_stub_state.settings_last_module = NULL;
                }
                if (g_stub_state.settings_last_element) {
                    g_free(g_stub_state.settings_last_element);
                    g_stub_state.settings_last_element = NULL;
                }
                if (g_stub_state.settings_last_new_value) {
                    g_free(g_stub_state.settings_last_new_value);
                    g_stub_state.settings_last_new_value = NULL;
                }
                if (p->module) {
                    g_stub_state.settings_last_module =
                        g_strdup(p->module);
                }
                if (p->element) {
                    g_stub_state.settings_last_element =
                        g_strdup(p->element);
                }
                if (p->new_value) {
                    g_stub_state.settings_last_new_value =
                        g_strdup(p->new_value);
                }
                /* Stub vyplní out_value (= caller g_free).
                 * settings_fake_value zůstává vlastněn stubem; pro
                 * každé volání vytvoříme novou kopii. */
                if (g_stub_state.settings_fake_value) {
                    p->out_value =
                        g_strdup(g_stub_state.settings_fake_value);
                } else {
                    p->out_value = NULL;
                }
                p->out_type   = g_stub_state.settings_fake_type;
                p->out_result = g_stub_state.settings_fake_result;
                if (p->out_result != 0) {
                    return false;
                }
            }
            break;

        case DBGAPI_CMD_PLATFORM_SET:
            /* V1.B.2 - platform. Stub vrátí fake_active_kind +
             * fake_result. Defaultně active=2, result=-10. Test smí
             * přenastavit pro emulaci "match" scénáře (= result=0). */
            if (data_ptr) {
                st_DBGAPI_PLATFORM_PARAM *p =
                    (st_DBGAPI_PLATFORM_PARAM *)data_ptr;
                g_stub_state.platform_last_target_kind = p->target_kind;
                p->out_active_kind =
                    g_stub_state.platform_fake_active_kind;
                p->out_result = g_stub_state.platform_fake_result;
                if (p->out_result != 0) {
                    return false;
                }
            }
            break;

        case DBGAPI_CMD_INPUT_PRESS_KEY:
        case DBGAPI_CMD_INPUT_RELEASE_KEY:
        case DBGAPI_CMD_INPUT_RELEASE_ALL:
        case DBGAPI_CMD_INPUT_JOY_SET:
        case DBGAPI_CMD_INPUT_JOY_CLEAR:
            /* V1.C.1 - HID Tools. Stub jen volá dolní vrstvu (=
             * hid_keymap_press/release/joystick_set funkce, které
             * jsou v test_hid_keymap_stub.c). Real dbgapi.c handler
             * dělá totéž, ale v testu obejdeme dbgapi vrstvu úplně
             * - data_ptr je už st_DBGAPI_HID_KEY/JOY_PARAM a stub jen
             * předá do hid_keymap stubu. */
            if (cmd == DBGAPI_CMD_INPUT_RELEASE_ALL) {
                hid_keymap_release_all();
            } else if (cmd == DBGAPI_CMD_INPUT_PRESS_KEY ||
                       cmd == DBGAPI_CMD_INPUT_RELEASE_KEY) {
                if (!data_ptr) return false;
                st_DBGAPI_HID_KEY_PARAM *p =
                    (st_DBGAPI_HID_KEY_PARAM *)data_ptr;
                if (p->col < 0 || p->col > 9 || p->bit < 0 || p->bit > 7) {
                    return false;
                }
                if (cmd == DBGAPI_CMD_INPUT_PRESS_KEY) {
                    hid_keymap_press(p->col, p->bit, p->needs_shift);
                } else {
                    hid_keymap_release(p->col, p->bit, p->needs_shift);
                }
            } else if (cmd == DBGAPI_CMD_INPUT_JOY_SET) {
                if (!data_ptr) return false;
                st_DBGAPI_HID_JOY_PARAM *p =
                    (st_DBGAPI_HID_JOY_PARAM *)data_ptr;
                if (!hid_keymap_joystick_set(p->port, p->mcp_mask)) {
                    return false;
                }
            } else if (cmd == DBGAPI_CMD_INPUT_JOY_CLEAR) {
                if (!data_ptr) return false;
                st_DBGAPI_HID_JOY_PARAM *p =
                    (st_DBGAPI_HID_JOY_PARAM *)data_ptr;
                if (!hid_keymap_joystick_clear(p->port)) {
                    return false;
                }
            }
            break;

        case DBGAPI_CMD_GET_CPU_IM2_VECTOR:
            /* V1.D.1 - IM2 vector. Stub vyplní deterministickými fake
             * hodnotami: IM=2, I=0x40, vec=0x80, isr_addr=0x4080,
             * isr_target=0x1234. Test si přebere podle scénáře přes
             * g_stub_state pole (= jednoduché defaults, žádný global
             * field zatím není). */
            if (data_ptr) {
                st_DBGAPI_CPU_IM2_VECTOR_PARAM *p =
                    (st_DBGAPI_CPU_IM2_VECTOR_PARAM *)data_ptr;
                p->im         = 2;
                p->i_reg      = 0x40;
                p->last_vec   = 0x80;
                p->available  = 1;
                p->isr_addr   = 0x4080;
                p->isr_target = 0x1234;
            }
            break;

        case DBGAPI_CMD_GET_CPU_INTERRUPT_BUS:
            /* V1.D.1 - IRQ bus. Stub vyplní Z80 core flags a fakuje
             * platform note + per-chip placeholdery. */
            if (data_ptr) {
                st_DBGAPI_CPU_IRQ_BUS_PARAM *p =
                    (st_DBGAPI_CPU_IRQ_BUS_PARAM *)data_ptr;
                memset(p, 0, sizeof(*p));
                p->iff1 = 1;
                p->iff2 = 1;
                p->im   = 1;
                p->halted = 0;
                p->int_line = 0;
                p->nmi_line = 0;
                p->i_reg = 0;
                p->ei_pending = 0;
                snprintf(p->platform_note, sizeof(p->platform_note),
                          "stub platform note");
                p->daisy_chain_available = 0;
                snprintf(p->daisy_chain_reason, sizeof(p->daisy_chain_reason),
                          "stub placeholder");
                p->non_chain_available = 0;
                snprintf(p->non_chain_reason, sizeof(p->non_chain_reason),
                          "stub placeholder");
                p->nmi_sources_available = 0;
                snprintf(p->nmi_sources_reason, sizeof(p->nmi_sources_reason),
                          "stub placeholder");
            }
            break;

        case DBGAPI_CMD_GET_MEMORY_MAP:
            /* V1.D.1 - memory map. Stub vyplní 16 slotů s deterministickým
             * patternem: addr_start = i*0x1000, source=0 (unknown), ro_rw=0. */
            if (data_ptr) {
                st_DBGAPI_MEMORY_MAP_PARAM *p =
                    (st_DBGAPI_MEMORY_MAP_PARAM *)data_ptr;
                memset(p, 0, sizeof(*p));
                snprintf(p->platform, sizeof(p->platform), "stub");
                snprintf(p->mode_note, sizeof(p->mode_note), "stub mode");
                p->slot_count = 16;
                for (int i = 0; i < 16; i++) {
                    p->slots[i].addr_start  = (uint16_t)(i * 0x1000);
                    p->slots[i].addr_end    = (uint16_t)(i * 0x1000 + 0x0FFF);
                    p->slots[i].source      = 0;
                    p->slots[i].ro_rw       = 0;
                    p->slots[i].slot_offset = 0;
                }
            }
            break;

        case DBGAPI_CMD_GET_MEMEXT_INFO:
            /* V1.D.1 - memext info. Stub default: type=none, connected=0. */
            if (data_ptr) {
                st_DBGAPI_MEMEXT_INFO_PARAM *p =
                    (st_DBGAPI_MEMEXT_INFO_PARAM *)data_ptr;
                memset(p, 0, sizeof(*p));
                snprintf(p->type, sizeof(p->type), "none");
                p->connected     = 0;
                p->map_available = 0;
            }
            break;

        case DBGAPI_CMD_PERIPH_ATTACH:
        case DBGAPI_CMD_PERIPH_DETACH:
            /* V1.B.2 - periph attach/detach. Stub zaznamenává cmd /
             * kind / option_value a vrací fake_result +
             * fake_requires_restart. */
            if (data_ptr) {
                st_DBGAPI_PERIPH_PARAM *p =
                    (st_DBGAPI_PERIPH_PARAM *)data_ptr;
                g_stub_state.periph_last_cmd = (int)cmd;
                g_stub_state.periph_last_kind = (int)p->kind;
                if (g_stub_state.periph_last_option_value) {
                    g_free(g_stub_state.periph_last_option_value);
                    g_stub_state.periph_last_option_value = NULL;
                }
                if (p->option_value) {
                    g_stub_state.periph_last_option_value =
                        g_strdup(p->option_value);
                }
                p->out_result = g_stub_state.periph_fake_result;
                p->out_requires_restart =
                    g_stub_state.periph_fake_requires_restart;
                if (p->out_result != 0) {
                    return false;
                }
            }
            break;

        case DBGAPI_CMD_STACK_HISTORY_GET:
            /* V1.D.2.A - bulk snapshot SP ring bufferu. Stub naplní
             * `samples` pole (caller-allocated), `count`, `active`,
             * `slope`. Pokud `active == 0`, count nastavíme na 0
             * (= reálný handler emituje empty array pro disabled state).
             *
             * Pattern samplů: cycles[i] = 1000 + i*10, sp[i] = 0xF000 - i.
             */
            if (data_ptr) {
                st_DBGAPI_STACK_HISTORY_GET_PARAM *p =
                    (st_DBGAPI_STACK_HISTORY_GET_PARAM *)data_ptr;
                g_stub_state.stack_history_calls++;
                p->active = g_stub_state.stack_history_fake_active;
                p->slope  = g_stub_state.stack_history_fake_slope;
                if (p->active && g_stub_state.stack_history_fake_count > 0
                              && p->samples != NULL) {
                    uint32_t n = g_stub_state.stack_history_fake_count;
                    if (n > p->max_count) n = p->max_count;
                    for (uint32_t i = 0; i < n; i++) {
                        p->samples[i].cycles =
                            (uint32_t)(1000u + i * 10u);
                        p->samples[i].sp = (uint16_t)(0xF000u - i);
                    }
                    p->count = n;
                } else {
                    p->count = 0;
                }
            }
            break;

        case DBGAPI_CMD_STACK_REGIONS_LIST:
            /* V1.D.2.A - snapshot definovaných stack regions. Stub
             * naplní pevné pole regions[] o fake_count položkách.
             *
             * Pattern:
             *   regions[i].name      = "stack_<i>"
             *   regions[i].base      = 0xF000 - i*0x100
             *   regions[i].limit     = base - 0x80
             *   regions[i].watermark = limit + 0x10
             *   regions[i].push_count = (i+1) * 100
             *   regions[i].pop_count  = (i+1) * 90
             *   regions[i].current_sp_in_region = (i == 0) ? 1 : 0
             */
            if (data_ptr) {
                st_DBGAPI_STACK_REGIONS_LIST_PARAM *p =
                    (st_DBGAPI_STACK_REGIONS_LIST_PARAM *)data_ptr;
                g_stub_state.stack_regions_calls++;
                int n = g_stub_state.stack_regions_fake_count;
                if (n < 0) n = 0;
                if (n > DBGAPI_STACK_REGIONS_MAX) {
                    n = DBGAPI_STACK_REGIONS_MAX;
                }
                p->count  = n;
                p->sp_now = g_stub_state.stack_regions_fake_sp_now;
                for (int i = 0; i < n; i++) {
                    /* '\0'-terminated name s pevnou délkou v poli. */
                    g_snprintf(p->regions[i].name,
                                sizeof(p->regions[i].name),
                                "stack_%d", i);
                    uint16_t base = (uint16_t)(0xF000u - (uint16_t)i * 0x100u);
                    p->regions[i].base  = base;
                    p->regions[i].limit = (uint16_t)(base - 0x80u);
                    p->regions[i].watermark =
                        (uint16_t)(p->regions[i].limit + 0x10u);
                    p->regions[i].push_count = (uint64_t)(i + 1) * 100ull;
                    p->regions[i].pop_count  = (uint64_t)(i + 1) * 90ull;
                    p->regions[i].current_sp_in_region =
                        (i == 0) ? 1 : 0;
                }
            }
            break;

        case DBGAPI_CMD_STACK_DUMP:
            /* V1.D.2.B - Stack dump pro get_stack Resource. Stub vyplní
             * buf[] deterministickým patternem + sp_now / sp_odd.
             *
             * SP-anchored mode (lines_above > 0): handler vypočte
             * base = sp_now + lines_above*2 a buf je DESC od base. Stub
             * naplní celý buffer (sizeof buf = len) jednoduchým ramp
             * patternem nezávislým na addr - test verifikuje pořadí
             * a sp_now propagation.
             */
            if (data_ptr) {
                st_DBGAPI_STACK_DUMP_PARAM *p =
                    (st_DBGAPI_STACK_DUMP_PARAM *)data_ptr;
                g_stub_state.stack_dump_calls++;
                uint16_t sp = g_stub_state.stack_dump_fake_sp_now;
                if (sp == 0) sp = 0xF000u;
                p->sp_now = sp;
                p->sp_odd = g_stub_state.stack_dump_fake_sp_odd;
                /* V SP-anchored mode handler psal addr = sp + lines_above*2 */
                if (p->lines_above > 0) {
                    p->addr = (uint16_t)(sp + (uint16_t)(p->lines_above * 2));
                }
                uint8_t base = g_stub_state.stack_dump_fake_pattern_base;
                if (base == 0) base = 0x10;
                if (p->buf) {
                    for (uint16_t i = 0; i < p->len; i++) {
                        p->buf[i] = (uint8_t)(base + (uint8_t)i);
                    }
                }
            }
            break;

        case DBGAPI_CMD_BP_VARS_LIST:
            /* V1.D.2.B - Snapshot bp_vars storage. Stub naplní fake_count
             * záznamů podle deterministického patternu. */
            if (data_ptr) {
                st_DBGAPI_BP_VARS_LIST_PARAM *p =
                    (st_DBGAPI_BP_VARS_LIST_PARAM *)data_ptr;
                g_stub_state.bp_vars_calls++;
                int n = g_stub_state.bp_vars_fake_count;
                if (n < 0) n = 0;
                if ((size_t)n > p->capacity) n = (int)p->capacity;
                for (int i = 0; i < n; i++) {
                    st_DBGAPI_BP_VAR_ENTRY *e = &p->entries[i];
                    memset(e, 0, sizeof(*e));
                    g_snprintf(e->name, sizeof(e->name), "VAR_%d", i);
                    e->value = (int32_t)(0x1000 + i);
                    if ((i % 2) == 0) {
                        g_snprintf(e->comment, sizeof(e->comment),
                                    "cmt_%d", i);
                        e->has_comment   = 1;
                        e->persist_value = 1;
                    }
                }
                p->out_count = (n > 0) ? (size_t)n : 0;
                /* truncated: simulace by vyžadovala parametr ze stubu;
                 * zde předpokládáme že fake_count <= capacity. */
                p->truncated = 0;
            }
            break;

        case DBGAPI_CMD_BOOKMARKS_LIST:
            /* V1.D.2.B - Snapshot bookmarks storage. Stub naplní fake_count
             * záznamů podle deterministického patternu. */
            if (data_ptr) {
                st_DBGAPI_BOOKMARKS_LIST_PARAM *p =
                    (st_DBGAPI_BOOKMARKS_LIST_PARAM *)data_ptr;
                g_stub_state.bookmarks_calls++;
                int n = g_stub_state.bookmarks_fake_count;
                if (n < 0) n = 0;
                if ((size_t)n > p->capacity) n = (int)p->capacity;
                for (int i = 0; i < n; i++) {
                    st_DBGAPI_BOOKMARK_ENTRY *e = &p->entries[i];
                    memset(e, 0, sizeof(*e));
                    e->id = (uint32_t)(i + 1);
                    g_snprintf(e->user_input, sizeof(e->user_input),
                                "BM_%d", i);
                    g_snprintf(e->comment, sizeof(e->comment),
                                "bcmt_%d", i);
                    e->has_comment   = 1;
                    e->addr          = (uint16_t)(0x4000 + i * 0x10);
                    e->addr_resolved = 1;
                    e->cmd_origin    = (i == 0)
                        ? DBGAPI_CMD_ORIGIN_USER
                        : DBGAPI_CMD_ORIGIN_MCP;
                }
                p->out_count = (n > 0) ? (size_t)n : 0;
                p->truncated = 0;
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_I8255:
            /* V1.D.3.A - Snapshot Intel 8255 PPI. Stub naplní deterministické
             * fields. Default control_word = 0x90 = Mode Set CW
             * (bit 7=1, mode A=0, PA=input, PC upper=output, mode B=0,
             * PB=output, PC lower=output). */
            if (data_ptr) {
                st_DBGAPI_PERIPH_I8255_PARAM *p =
                    (st_DBGAPI_PERIPH_I8255_PARAM *)data_ptr;
                g_stub_state.periph_i8255_calls++;
                memset(p, 0, sizeof(*p));
                uint8_t cw = g_stub_state.periph_i8255_fake_cw;
                if (cw == 0) cw = 0x90;
                p->port_a              = 0xAB;
                p->port_b              = 0x00;
                p->port_c              = 0x55;
                p->control_word        = cw;
                if (cw & 0x80) {
                    p->cw_decoded   = 1;
                    p->mode_group_a = (uint8_t)((cw >> 5) & 0x03);
                    if (p->mode_group_a > 2) p->mode_group_a = 2;
                    p->mode_group_b = (uint8_t)((cw >> 2) & 0x01);
                    p->pa_dir       = (uint8_t)((cw >> 4) & 0x01);
                    p->pc_upper_dir = (uint8_t)((cw >> 3) & 0x01);
                    p->pb_dir       = (uint8_t)((cw >> 1) & 0x01);
                    p->pc_lower_dir = (uint8_t)(cw & 0x01);
                }
                p->signal_pc00         = 1;
                p->signal_pc01         = 0;
                p->signal_pc02         = 1;
                p->signal_pc03         = 0;
                p->signal_pc04         = 1;
                p->pa_keyboard_column  = 5;
                p->pa_joy1_enabled     = 1;
                p->pa_joy2_enabled     = 0;
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_I8253:
            /* V1.D.3.A - Snapshot 8253 CTC. Stub vyplní per kanál
             * deterministický pattern (counter = 0x1000 + i*0x100,
             * mode = i, BCD = 0). */
            if (data_ptr) {
                st_DBGAPI_PERIPH_I8253_PARAM *p =
                    (st_DBGAPI_PERIPH_I8253_PARAM *)data_ptr;
                g_stub_state.periph_i8253_calls++;
                memset(p, 0, sizeof(*p));
                p->last_cw_byte = 0x36;
                for (int i = 0; i < 3; i++) {
                    st_DBGAPI_PERIPH_I8253_CHANNEL *c = &p->ch[i];
                    c->value        = (uint16_t)(0x1000 + i * 0x100);
                    c->preset_value = (uint16_t)(0x2000 + i * 0x100);
                    c->preset_latch = 0;
                    c->read_latch   = 0;
                    c->out          = (uint8_t)(i & 0x01);
                    c->gate         = 1;
                    c->mode         = (uint8_t)i;
                    c->bcd          = 0;
                    c->rlf          = 3; /* LSB+MSB */
                    c->state        = 8; /* countdown */
                    c->load_done    = 1;
                    c->latch_op     = 0;
                    c->rl_byte      = 0;
                }
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_Z80_PIO:
            /* V1.D.3.A - Snapshot Z80 PIO. Default available=1 (= MZ-800/
             * MZ-1500 build), testy mohou nastavit fake_available=0 pro
             * MZ-700 simulaci. */
            if (data_ptr) {
                st_DBGAPI_PERIPH_Z80_PIO_PARAM *p =
                    (st_DBGAPI_PERIPH_Z80_PIO_PARAM *)data_ptr;
                g_stub_state.periph_z80_pio_calls++;
                memset(p, 0, sizeof(*p));
                p->available = g_stub_state.periph_z80_pio_fake_available;
                if (!p->available) {
                    p->interrupt_port_id = 0xFF;
                    break;
                }
                p->interrupt         = 3; /* PENDING */
                p->interrupt_port_id = 0; /* Port A */
                p->port_a.data_output    = 0x10;
                p->port_a.masked_input   = 0x20;
                p->port_a.io_mask        = 0x0F;
                p->port_a.mode           = 1; /* input */
                p->port_a.int_vec        = 0x40;
                p->port_a.icmask         = 0xFF;
                p->port_a.icena          = 1;
                p->port_a.icfnc          = 0;
                p->port_a.iclvl          = 1;
                p->port_a.port_int       = 1; /* PENDING */
                p->port_a.last_ctrl_byte = 0xCF;
                p->port_b.data_output    = 0x30;
                p->port_b.masked_input   = 0x40;
                p->port_b.io_mask        = 0xF0;
                p->port_b.mode           = 0; /* output */
                p->port_b.int_vec        = 0x80;
                p->port_b.icmask         = 0x00;
                p->port_b.icena          = 0;
                p->port_b.icfnc          = 1;
                p->port_b.iclvl          = 0;
                p->port_b.port_int       = 0; /* NONE */
                p->port_b.last_ctrl_byte = 0x07;
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_SN76489:
            /* V1.D.3.B - Snapshot SN76489 PSG. Default available=1 (=
             * MZ-800 mono), stereo=0 (= 1 PSG). Testy nastaví
             * fake_available=0 (= MZ-700) nebo fake_stereo=1 (= MZ-1500
             * / MZ-800 stereo) explicitně. Per-kanál deterministický
             * pattern: ch0 type=tone attn=0 div=0x100, ch1 type=tone
             * attn=4 div=0x200, ch2 type=tone attn=8 div=0x300, ch3
             * type=noise attn=12 noise_div=2 noise_type=1. PSG1 pattern
             * je inverzní attn (15-i*4) pro snadnou diferenciaci. */
            if (data_ptr) {
                st_DBGAPI_PERIPH_SN76489_PARAM *p =
                    (st_DBGAPI_PERIPH_SN76489_PARAM *)data_ptr;
                g_stub_state.periph_sn76489_calls++;
                memset(p, 0, sizeof(*p));
                p->available = g_stub_state.periph_sn76489_fake_available;
                if (!p->available) {
                    p->psg_count = 0;
                    break;
                }
                p->stereo    = g_stub_state.periph_sn76489_fake_stereo;
                p->psg_count = p->stereo ? 2 : 1;
                p->psg0_latch_cs   = 1;
                p->psg0_latch_attn = 1;
                /* PSG0 kanály - pattern: 3 TONE + 1 NOISE. */
                p->psg0_ch[0].type           = 0; /* TONE */
                p->psg0_ch[0].attenuation    = 0;
                p->psg0_ch[0].tone_divider   = 0x100;
                p->psg0_ch[1].type           = 0;
                p->psg0_ch[1].attenuation    = 4;
                p->psg0_ch[1].tone_divider   = 0x200;
                p->psg0_ch[2].type           = 0;
                p->psg0_ch[2].attenuation    = 8;
                p->psg0_ch[2].tone_divider   = 0x300;
                p->psg0_ch[3].type           = 1; /* NOISE */
                p->psg0_ch[3].attenuation    = 12;
                p->psg0_ch[3].noise_div_type = 2; /* div_64 */
                p->psg0_ch[3].noise_type     = 1; /* white */
                /* PSG1 kanály - jen pokud stereo. Inverzní attn pattern. */
                if (p->psg_count >= 2) {
                    p->psg1_latch_cs   = 2;
                    p->psg1_latch_attn = 0;
                    p->psg1_ch[0].type           = 0;
                    p->psg1_ch[0].attenuation    = 15;
                    p->psg1_ch[0].tone_divider   = 0x180;
                    p->psg1_ch[1].type           = 0;
                    p->psg1_ch[1].attenuation    = 11;
                    p->psg1_ch[1].tone_divider   = 0x280;
                    p->psg1_ch[2].type           = 0;
                    p->psg1_ch[2].attenuation    = 7;
                    p->psg1_ch[2].tone_divider   = 0x380;
                    p->psg1_ch[3].type           = 1;
                    p->psg1_ch[3].attenuation    = 3;
                    p->psg1_ch[3].noise_div_type = 3; /* tone2_controlled */
                    p->psg1_ch[3].noise_type     = 0; /* periodic */
                }
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_AY3_8910:
            /* V1.D.3.B - Placeholder AY-3-8910 (= chip neimplementován).
             * Vždy available=0. */
            if (data_ptr) {
                st_DBGAPI_PERIPH_AY3_8910_PARAM *p =
                    (st_DBGAPI_PERIPH_AY3_8910_PARAM *)data_ptr;
                g_stub_state.periph_ay3_8910_calls++;
                memset(p, 0, sizeof(*p));
                p->available = 0;
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_BEEPER:
            /* V1.D.3.B - Beeper snapshot. Default ctc0_out=1, gate0=1,
             * pc0=1 → level=1 audible. Testy mohou jednotlivé bity
             * vynulovat pro ověření level dopočtu. */
            if (data_ptr) {
                st_DBGAPI_PERIPH_BEEPER_PARAM *p =
                    (st_DBGAPI_PERIPH_BEEPER_PARAM *)data_ptr;
                g_stub_state.periph_beeper_calls++;
                memset(p, 0, sizeof(*p));
                p->available = 1;
                p->ctc0_out  = (uint8_t)(g_stub_state.periph_beeper_fake_ctc0_out & 0x01);
                p->gate0     = (uint8_t)(g_stub_state.periph_beeper_fake_gate0 & 0x01);
                p->pc0       = (uint8_t)(g_stub_state.periph_beeper_fake_pc0 & 0x01);
                p->level     = (uint8_t)(p->ctc0_out & p->gate0 & p->pc0);
                p->source[0] = 'P';
                p->source[1] = 'C';
                p->source[2] = '0';
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_GDG:
            /* V1.D.3.C - GDG snapshot. Default MZ-800 layout (16-color
             * palette, has_border_reg=1, has_pal_group=1, has_cksw=1).
             * Test pro MZ-700/MZ-1500 přepíše fake_platform string +
             * palette_count + flags. Deterministický raster: beam_row=42,
             * total_screens=100, total_ticks=12345, palette entries
             * 0..n-1 = (i * 16) pro klientskou ověřitelnost. */
            if (data_ptr) {
                st_DBGAPI_PERIPH_GDG_PARAM *p =
                    (st_DBGAPI_PERIPH_GDG_PARAM *)data_ptr;
                g_stub_state.periph_gdg_calls++;
                memset(p, 0, sizeof(*p));
                p->available     = 1;
                memcpy(p->platform, g_stub_state.periph_gdg_fake_platform,
                       sizeof(p->platform));
                p->palette_count  = g_stub_state.periph_gdg_fake_palette_count;
                p->has_border_reg = g_stub_state.periph_gdg_fake_has_border_reg;
                p->has_pal_group  = g_stub_state.periph_gdg_fake_has_pal_group;
                p->has_cksw       = g_stub_state.periph_gdg_fake_has_cksw;
                p->regDMD         = g_stub_state.periph_gdg_fake_regDMD;
                p->regBOR         = 0x12;
                p->regPALGRP      = 0x03;
                p->regct53g7      = 0x01;
                p->beam_row       = 42;
                p->total_screens  = 100;
                p->total_ticks    = 12345;
                p->sts_vsync      = 0;
                p->sts_hsync      = 1;
                p->hbln           = 1;
                p->vbln           = 1;
                p->cksw           = 0;
                p->tempo          = 7;
                p->tempo_divider  = 16;
                unsigned pn = p->palette_count;
                if (pn > 16) pn = 16;
                for (unsigned i = 0; i < pn; i++) {
                    p->palette[i] = (uint8_t)(i * 16);
                }
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_WD1793:
            /* V1.D.3.C - WD1793 snapshot. Default available=1, deterministic
             * registry pattern (status=0x80=NOT READY, command=0x00, track
             * 5, sector 1, data 0xAB). Drive 0 fake_drive0_present
             * řiditelný (default 0 = no DSK). Drives 1..3 = empty. */
            if (data_ptr) {
                st_DBGAPI_PERIPH_WD1793_PARAM *p =
                    (st_DBGAPI_PERIPH_WD1793_PARAM *)data_ptr;
                g_stub_state.periph_wd1793_calls++;
                memset(p, 0, sizeof(*p));
                p->available = g_stub_state.periph_wd1793_fake_available;
                if (!p->available) {
                    break;
                }
                p->bus_xlate_invert = 1;
                p->hd_patch         = 0;
                p->reg_status       = g_stub_state.periph_wd1793_fake_status;
                p->reg_command      = 0x00;
                p->reg_track        = 5;
                p->reg_sector       = 1;
                p->reg_data         = 0xAB;
                p->motor            = 0x01;
                p->side             = 0x00;
                p->density          = 0x00;
                p->positioned_track = 5;
                p->positioned_sector = 1;
                p->status_mode      = 0;
                if (g_stub_state.periph_wd1793_fake_drive0_present) {
                    p->drives[0].present        = 1;
                    p->drives[0].readonly       = 0;
                    p->drives[0].storage_mode   = 0; /* CACHED */
                    p->drives[0].geometry_valid = 1;
                    p->drives[0].tracks         = 40;
                    p->drives[0].sides          = 1;
                    p->drives[0].total_data_bytes = 40 * 16 * 256;
                    /* image_basename test pro security: vstup full path,
                     * stub naplní jen basename (= dispatch handler tu
                     * extrakci dělá; stub může vrátit hotový basename). */
                    strcpy(p->drives[0].image_basename, "test.dsk");
                }
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_CMT:
            /* V1.D.3.C - CMT snapshot. Default state=STOP. Test pro PLAY
             * nastaví fake_state=1, pro RECORD fake_state=2. Image basename
             * empty by default. */
            if (data_ptr) {
                st_DBGAPI_PERIPH_CMT_PARAM *p =
                    (st_DBGAPI_PERIPH_CMT_PARAM *)data_ptr;
                g_stub_state.periph_cmt_calls++;
                memset(p, 0, sizeof(*p));
                p->available         = 1;
                p->state             = g_stub_state.periph_cmt_fake_state;
                p->paused            = 0;
                p->filled            = (p->state != 0) ? 1 : 0;
                p->polarity_inverted = 0;
                p->cmtspeed          = 0;
                p->cpu_boost         = 0;
                p->mzfsize_check     = 1;
                p->output            = 0;
                p->playsts           = 0;
                p->start_time        = 0;
                p->paused_time       = 0;
                if (p->filled) {
                    strcpy(p->image_basename, "tape.mzf");
                }
            }
            break;

        case DBGAPI_CMD_GET_PERIPH_QD:
            /* V1.D.3.C - QD snapshot. Default available=1, type=IMAGE,
             * status=QDSTS_NO_DISC, head position 0. Test pro detached
             * nastaví fake_available=0, pro VIRTUAL fake_type=1. */
            if (data_ptr) {
                st_DBGAPI_PERIPH_QD_PARAM *p =
                    (st_DBGAPI_PERIPH_QD_PARAM *)data_ptr;
                g_stub_state.periph_qd_calls++;
                memset(p, 0, sizeof(*p));
                p->available = g_stub_state.periph_qd_fake_available;
                if (!p->available) {
                    break;
                }
                p->type            = g_stub_state.periph_qd_fake_type;
                p->status          = 0; /* QDSTS_NO_DISC */
                p->readonly        = 0;
                p->user_readonly   = 0;
                p->fs_readonly     = 0;
                p->storage_mode    = 0; /* CACHED */
                p->vrtsts          = 0;
                p->image_position  = 0;
                p->virt_files_count = 0;
                p->virt_file_num   = 0;
                p->virt_mzfbody_size = 0;
                p->out_crc16       = 0;
                if (p->type == 0 /* IMAGE */) {
                    strcpy(p->image_basename, "disk.mzq");
                }
            }
            break;

        case DBGAPI_CMD_GET_INPUT_KEYBOARD_STATE:
            /* V1.D.4 - keyboard state snapshot. Default = idle (= all
             * matrix bytes 0xff). Test může nastavit konkrétní pozice
             * přes g_stub_state.kbd_fake_*. */
            if (data_ptr) {
                st_DBGAPI_INPUT_KBD_STATE_PARAM *p =
                    (st_DBGAPI_INPUT_KBD_STATE_PARAM *)data_ptr;
                g_stub_state.input_kbd_state_calls++;
                memset(p, 0, sizeof(*p));
                for (int c = 0; c < 10; c++) {
                    p->real_matrix[c]    = g_stub_state.kbd_real_matrix[c];
                    p->virtual_matrix[c] = g_stub_state.kbd_virtual_matrix[c];
                    p->effective[c]      = (uint8_t)(p->real_matrix[c] &
                                                     p->virtual_matrix[c]);
                }
                /* Decode aktivních pozic - jednoduchá kopie loop logiky. */
                uint32_t pcount = 0;
                uint32_t cap = sizeof(p->pressed_keys) /
                               sizeof(p->pressed_keys[0]);
                for (int c = 0; c < 10; c++) {
                    for (int b = 0; b < 8; b++) {
                        if ((p->effective[c] & (1u << b)) == 0) {
                            pcount++;
                            if (p->pressed_count < cap) {
                                p->pressed_keys[p->pressed_count].col = (uint8_t)c;
                                p->pressed_keys[p->pressed_count].bit = (uint8_t)b;
                                /* Stub neimplementuje reverse_lookup -
                                 * vrátí prázdný string. */
                                p->pressed_keys[p->pressed_count].name[0] = '\0';
                                p->pressed_count++;
                            }
                        }
                    }
                }
                if (pcount > cap) p->pressed_truncated = 1;
            }
            break;

        case DBGAPI_CMD_GET_INPUT_KEYBOARD_MATRIX_INFO:
            /* V1.D.4 - statická matrix info. Stub vrátí fixní platform
             * "mztest" + jeden ukázkový entry (RETURN col 0 bit 0). */
            if (data_ptr) {
                st_DBGAPI_INPUT_KBD_MATRIX_INFO_PARAM *p =
                    (st_DBGAPI_INPUT_KBD_MATRIX_INFO_PARAM *)data_ptr;
                g_stub_state.input_kbd_matrix_info_calls++;
                memset(p, 0, sizeof(*p));
                strcpy(p->platform, "mztest");
                p->key_count = 1;
                strcpy(p->keys[0].name, "RETURN");
                p->keys[0].col = 0;
                p->keys[0].bit = 0;
                p->keys[0].needs_shift = 0;
            }
            break;

        case DBGAPI_CMD_GET_INPUT_JOYSTICK_STATE:
            /* V1.D.4 - joystick state. Default: port0 connected joystick
             * s state_bits=0 (idle), port1 not connected. Test může
             * přepsat g_stub_state.joy_*. */
            if (data_ptr) {
                st_DBGAPI_INPUT_JOY_STATE_PARAM *p =
                    (st_DBGAPI_INPUT_JOY_STATE_PARAM *)data_ptr;
                g_stub_state.input_joy_state_calls++;
                memset(p, 0, sizeof(*p));
                p->port[0].connected    = g_stub_state.joy_port0_connected;
                p->port[0].state_bits   = g_stub_state.joy_port0_state_bits;
                p->port[0].native_state = 0xff;
                strcpy(p->port[0].device_name,
                       g_stub_state.joy_port0_connected ? "joystick" : "none");
                p->port[1].connected    = 0;
                p->port[1].native_state = 0xff;
                strcpy(p->port[1].device_name, "none");
            }
            break;

        case DBGAPI_CMD_GET_FRAME_FRAMEBUFFER_INFO:
            /* V1.D.4 - framebuffer info. Stub vrátí fixní 928x288 INDEX8
             * + dirty flag z g_stub_state.fb_dirty. Paletu vyplní 16x
             * fake hodnotami. */
            if (data_ptr) {
                st_DBGAPI_FRAME_FB_INFO_PARAM *p =
                    (st_DBGAPI_FRAME_FB_INFO_PARAM *)data_ptr;
                g_stub_state.frame_fb_info_calls++;
                memset(p, 0, sizeof(*p));
                p->width             = 928;
                p->height            = 288;
                p->bytes_per_pixel   = 1;
                p->pixel_format      = 0;
                p->has_palette       = 1;
                p->last_screen_id    = g_stub_state.fb_screen_id;
                p->framebuffer_state = g_stub_state.fb_state;
                p->dirty             = (p->framebuffer_state != 0) ? 1 : 0;
                p->palette_size      = 16;
                for (int i = 0; i < 16; i++) {
                    p->palette[i] = (uint32_t)(0x101010u * (uint32_t)i);
                }
            }
            break;

        case DBGAPI_CMD_GET_FRAME_SCREENSHOT_RAW:
            /* V1.D.4 - screenshot raw. Stub vrátí 4x4 RGBA buffer
             * (= jednoduchý smoke test). available=g_stub_state.
             * fb_available. */
            if (data_ptr) {
                st_DBGAPI_FRAME_SCREENSHOT_RAW_PARAM *p =
                    (st_DBGAPI_FRAME_SCREENSHOT_RAW_PARAM *)data_ptr;
                g_stub_state.frame_screenshot_raw_calls++;
                p->pixel_format = 0;
                if (!g_stub_state.fb_available) {
                    p->available = 0;
                    p->width = 0;
                    p->height = 0;
                    p->buffer_size = 0;
                    break;
                }
                p->available        = 1;
                p->width            = 4;
                p->height           = 4;
                p->bytes_per_pixel  = 4;
                p->downscale_factor = 1;
                p->source_screen_id = g_stub_state.fb_screen_id;
                p->buffer_size      = 4 * 4 * 4;
                /* V1.E.6.C - stub default: SDL snapshot path (= GUI
                 * cesta, kompatibilní s pre-V1.E.6.C behavior). Skutečný
                 * fallback handler v dbgapi.c běží jen v plné binárce
                 * (vyžaduje g_iface_video + g_framebuffer), test stub
                 * jen ověřuje že dispatch zpracoval JSON tvar. */
                p->fallback_source  = 0; /* SCREENSHOT_SRC_SDL_SNAPSHOT */
                if (p->buffer && p->buffer_capacity >= p->buffer_size) {
                    for (size_t i = 0; i < p->buffer_size; i++) {
                        p->buffer[i] = (uint8_t)(i & 0xff);
                    }
                } else {
                    p->buffer_size = 0;
                    p->available = 0;
                }
            }
            break;

        case DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG:
            /* V1.D.4 - PNG screenshot. Vždy available=0 (= dep není). */
            if (data_ptr) {
                st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM *p =
                    (st_DBGAPI_FRAME_SCREENSHOT_PNG_PARAM *)data_ptr;
                g_stub_state.frame_screenshot_png_calls++;
                memset(p, 0, sizeof(*p));
                p->available = 0;
                strcpy(p->reason, "PNG encoder dependency not available");
            }
            break;

        case DBGAPI_CMD_GET_VIDEO_TEXT_DUMP:
            /* V1.D.4 - text dump. Stub vrátí 40x25 plné spaces 0x20
             * pokud g_stub_state.text_dump_available=1, jinak
             * available=0. */
            if (data_ptr) {
                st_DBGAPI_VIDEO_TEXT_DUMP_PARAM *p =
                    (st_DBGAPI_VIDEO_TEXT_DUMP_PARAM *)data_ptr;
                g_stub_state.video_text_dump_calls++;
                memset(p, 0, sizeof(*p));
                strcpy(p->platform, "mztest");
                if (!g_stub_state.text_dump_available) {
                    p->available = 0;
                    strcpy(p->reason, "test stub: text dump disabled");
                    break;
                }
                p->available  = 1;
                p->cols       = 40;
                p->rows       = 25;
                p->cell_count = 1000;
                memset(p->chars,      0x20, p->cell_count); /* SPACE */
                memset(p->attributes, 0x71, p->cell_count); /* fake attr */
            }
            break;

        case DBGAPI_CMD_GET_WATCH_SNAPSHOT:
            /* V1.D.2.C - per-watch snapshot mirror lookup. Stub
             * uloží vstupní name (= caller alokoval p->name fixed buf),
             * vyplní výstup z fake fields a inkrementuje counter. */
            if (data_ptr) {
                st_DBGAPI_WATCH_SNAPSHOT_PARAM *p =
                    (st_DBGAPI_WATCH_SNAPSHOT_PARAM *)data_ptr;
                g_stub_state.watch_snapshot_calls++;
                g_free(g_stub_state.watch_snapshot_last_name);
                g_stub_state.watch_snapshot_last_name = g_strdup(p->name);
                /* Vynulujeme všechny OUT fields, name zůstává jako IN. */
                char keep_name[sizeof(p->name)];
                memcpy(keep_name, p->name, sizeof(keep_name));
                memset(p, 0, sizeof(*p));
                memcpy(p->name, keep_name, sizeof(p->name));
                if (g_stub_state.watch_snapshot_fake_found) {
                    p->found           = 1;
                    p->snapshot_active = g_stub_state.watch_snapshot_fake_snapshot_active;
                    p->min_max_valid   = g_stub_state.watch_snapshot_fake_min_max_valid;
                    p->row_id          = g_stub_state.watch_snapshot_fake_row_id;
                    p->type_snap       = g_stub_state.watch_snapshot_fake_type_snap;
                    p->snap_int        = g_stub_state.watch_snapshot_fake_snap_int;
                    p->cur_int         = g_stub_state.watch_snapshot_fake_cur_int;
                    p->delta_int       = g_stub_state.watch_snapshot_fake_delta_int;
                    p->min_int         = g_stub_state.watch_snapshot_fake_min_int;
                    p->max_int         = g_stub_state.watch_snapshot_fake_max_int;
                    p->change_count    = g_stub_state.watch_snapshot_fake_change_count;
                }
            }
            break;

        default:
            /* PAUSE/RUN/RESET - jen success, žádná data */
            break;
    }
    return true;
}


/**
 * @brief Stub pro `dbgapi_ui_submit_cmd_sync` (back-compat wrapper).
 *
 * Dispatch.c sice volá jen _with_origin variantu, ale linker by mohl
 * tento symbol vyžadovat (= když je v jiné překladové jednotce). Pro
 * jistotu definujeme thin shim.
 */
bool dbgapi_ui_submit_cmd_sync(st_DBGAPI_CMDRQ_QUEUE *queue,
                                en_DBGAPI_CMD cmd,
                                void *data_ptr,
                                void *result_ptr,
                                int timeout_ms) {
    return dbgapi_ui_submit_cmd_sync_with_origin(queue, cmd,
                                                  DBGAPI_CMD_ORIGIN_USER,
                                                  data_ptr, result_ptr,
                                                  timeout_ms);
}


/* V1.D.1 - last user action tracker shim pro test build.
 *
 * Reálná implementace v dbgapi.c drží statický buffer s mutex zámkem;
 * v test buildu (= žádné emu vlákno) stačí stub který drží stejný stav
 * v globálních proměnných. Test si může nastavit hodnoty před voláním
 * mcp_dispatch_request a verifikovat že get_state Resource vrátí
 * správný last_user_action JSON.
 */
static bool          s_stub_lua_valid     = false;
static en_DBGAPI_CMD s_stub_lua_cmd       = DBGAPI_CMD_NONE;
static uint64_t      s_stub_lua_timestamp = 0;

bool dbgapi_get_last_user_action ( en_DBGAPI_CMD *out_cmd,
                                    uint64_t *out_timestamp_us )
{
    if ( out_cmd ) *out_cmd = s_stub_lua_cmd;
    if ( out_timestamp_us ) *out_timestamp_us = s_stub_lua_timestamp;
    return s_stub_lua_valid;
}


/**
 * @brief Stub setter - test si nastaví fake last user action.
 *
 * Definováno mimo standardní reset cesty (= test si zavolá explicitně).
 */
void dispatch_stub_set_last_user_action ( bool valid,
                                            en_DBGAPI_CMD cmd,
                                            uint64_t timestamp_us )
{
    s_stub_lua_valid     = valid;
    s_stub_lua_cmd       = cmd;
    s_stub_lua_timestamp = timestamp_us;
}


/* V1.D.1 - dbgapi_cmd_to_str shim pro test build.
 *
 * Reálná implementace v dbgapi.c je velký switch nad enum. V testu nás
 * zajímají jen základní cmd (pause, run, ...) - stačí minimální mapping
 * + fallback "unknown".
 */
const char *dbgapi_cmd_to_str ( en_DBGAPI_CMD cmd )
{
    switch ( cmd )
    {
        case DBGAPI_CMD_NONE:           return "none";
        case DBGAPI_CMD_PAUSE:          return "pause";
        case DBGAPI_CMD_RUN:            return "run";
        case DBGAPI_CMD_RESET:          return "reset";
        case DBGAPI_CMD_STEP_INTO:      return "step_into";
        case DBGAPI_CMD_STEP_OVER:      return "step_over";
        case DBGAPI_CMD_BP_ADD:         return "bp_add";
        case DBGAPI_CMD_BP_REMOVE:      return "bp_remove";
        default:                        return "unknown";
    };
}


/* F015a - bpt_type_to_string / bp_zone_to_string shim pro test build.
 *
 * Reálná implementace je v breakpoints.c (s_bpt_type_names /
 * s_bp_zone_names, index = enum hodnota). Standalone mcp_dispatch test
 * breakpoints.c nelinkuje, proto věrný shim s identickými UPPER_SNAKE
 * řetězci (= jediná pravda pro názvy typu/zóny BP). Fallback pro
 * out-of-range stejný jako reálná funkce.
 */
const char *bpt_type_to_string ( en_BPT_TYPE type )
{
    switch ( type )
    {
        case BPT_TYPE_PC_EXEC:      return "PC_EXEC";
        case BPT_TYPE_MEM_R:        return "MEM_R";
        case BPT_TYPE_MEM_W:        return "MEM_W";
        case BPT_TYPE_IORQ_R:       return "IORQ_R";
        case BPT_TYPE_IORQ_W:       return "IORQ_W";
        case BPT_TYPE_IRQ:          return "IRQ";
        case BPT_TYPE_HW_EVENT:     return "HW_EVENT";
        case BPT_TYPE_SP_THRESHOLD: return "SP_THRESHOLD";
        case BPT_TYPE_GLOBAL:       return "GLOBAL";
        case BPT_TYPE_IRQ_SIG:      return "IRQ_SIG";
        default:                    return "PC_EXEC";
    };
}

const char *bp_zone_to_string ( en_BP_ZONE zone )
{
    switch ( zone )
    {
        case BP_ZONE_CPU_VIEW:   return "CPU_VIEW";
        case BP_ZONE_ROM_LOWER:  return "ROM_LOWER";
        case BP_ZONE_ROM_UPPER:  return "ROM_UPPER";
        case BP_ZONE_RAM:        return "RAM";
        case BP_ZONE_VRAM_FB:    return "VRAM_FB";
        case BP_ZONE_PCG:        return "PCG";
        case BP_ZONE_MMEXT_BANK: return "MMEXT_BANK";
        default:                 return "CPU_VIEW";
    };
}


/* H2 - *_from_string shimy pro test build.
 *
 * Standalone mcp_dispatch test nelinkuje breakpoints.c ani bp_event.c,
 * přesto _bp_fill_param_from_json (dispatch.c) volá kanonické enum
 * konvertory. Shim věrně replikuje UPPER_SNAKE slovník reálných funkcí
 * (breakpoints.c s_bpt_type_names / s_bp_zone_names / s_bp_match_mode_names
 * / s_bp_sp_mode_names / s_bp_port_mode_names + bp_event.c s_trigger_names),
 * včetně legacy aliasů zóny (PEHU_BANK->MMEXT_BANK, VRAM_RF->VRAM_FB).
 * Neznámý řetězec vrací false (= dispatch pak vrátí invalid_params). */

bool bpt_type_from_string ( const char *s, en_BPT_TYPE *out )
{
    if ( !s || !out ) return false;
    if ( strcmp ( s, "PC_EXEC" ) == 0 )      { *out = BPT_TYPE_PC_EXEC;      return true; }
    if ( strcmp ( s, "MEM_R" ) == 0 )        { *out = BPT_TYPE_MEM_R;        return true; }
    if ( strcmp ( s, "MEM_W" ) == 0 )        { *out = BPT_TYPE_MEM_W;        return true; }
    if ( strcmp ( s, "IORQ_R" ) == 0 )       { *out = BPT_TYPE_IORQ_R;       return true; }
    if ( strcmp ( s, "IORQ_W" ) == 0 )       { *out = BPT_TYPE_IORQ_W;       return true; }
    if ( strcmp ( s, "IRQ" ) == 0 )          { *out = BPT_TYPE_IRQ;          return true; }
    if ( strcmp ( s, "HW_EVENT" ) == 0 )     { *out = BPT_TYPE_HW_EVENT;     return true; }
    if ( strcmp ( s, "SP_THRESHOLD" ) == 0 ) { *out = BPT_TYPE_SP_THRESHOLD; return true; }
    if ( strcmp ( s, "GLOBAL" ) == 0 )       { *out = BPT_TYPE_GLOBAL;       return true; }
    if ( strcmp ( s, "IRQ_SIG" ) == 0 )      { *out = BPT_TYPE_IRQ_SIG;      return true; }
    return false;
}

bool bp_zone_from_string ( const char *s, en_BP_ZONE *out )
{
    if ( !s || !out ) return false;
    if ( strcmp ( s, "PEHU_BANK" ) == 0 )  { *out = BP_ZONE_MMEXT_BANK; return true; }
    if ( strcmp ( s, "VRAM_RF" ) == 0 )    { *out = BP_ZONE_VRAM_FB;    return true; }
    if ( strcmp ( s, "CPU_VIEW" ) == 0 )   { *out = BP_ZONE_CPU_VIEW;   return true; }
    if ( strcmp ( s, "ROM_LOWER" ) == 0 )  { *out = BP_ZONE_ROM_LOWER;  return true; }
    if ( strcmp ( s, "ROM_UPPER" ) == 0 )  { *out = BP_ZONE_ROM_UPPER;  return true; }
    if ( strcmp ( s, "RAM" ) == 0 )        { *out = BP_ZONE_RAM;        return true; }
    if ( strcmp ( s, "VRAM_FB" ) == 0 )    { *out = BP_ZONE_VRAM_FB;    return true; }
    if ( strcmp ( s, "PCG" ) == 0 )        { *out = BP_ZONE_PCG;        return true; }
    if ( strcmp ( s, "MMEXT_BANK" ) == 0 ) { *out = BP_ZONE_MMEXT_BANK; return true; }
    return false;
}

bool bp_match_mode_from_string ( const char *s, en_BP_MATCH_MODE *out )
{
    if ( !s || !out ) return false;
    if ( strcmp ( s, "SINGLE" ) == 0 ) { *out = BP_MATCH_SINGLE; return true; }
    if ( strcmp ( s, "RANGE" ) == 0 )  { *out = BP_MATCH_RANGE;  return true; }
    if ( strcmp ( s, "MASK" ) == 0 )   { *out = BP_MATCH_MASK;   return true; }
    return false;
}

bool bp_addr_space_from_string ( const char *s, en_BP_ADDR_SPACE *out )
{
    if ( !s || !out ) return false;
    if ( strcmp ( s, "cpu_view" ) == 0 )    { *out = BP_ADDR_SPACE_CPU_VIEW;    return true; }
    if ( strcmp ( s, "bank_offset" ) == 0 ) { *out = BP_ADDR_SPACE_BANK_OFFSET; return true; }
    return false;
}

bool bp_sp_mode_from_string ( const char *s, en_BP_SP_MODE *out )
{
    if ( !s || !out ) return false;
    if ( strcmp ( s, "SINGLE" ) == 0 ) { *out = BP_SP_SINGLE; return true; }
    if ( strcmp ( s, "WINDOW" ) == 0 ) { *out = BP_SP_WINDOW; return true; }
    return false;
}

bool bp_port_mode_from_string ( const char *s, en_BP_PORT_MODE *out )
{
    if ( !s || !out ) return false;
    if ( strcmp ( s, "8BIT" ) == 0 )  { *out = BP_PORT_8BIT;  return true; }
    if ( strcmp ( s, "16BIT" ) == 0 ) { *out = BP_PORT_16BIT; return true; }
    return false;
}

bool bp_event_trigger_from_string ( const char *s, en_BP_EVENT_TRIGGER *out )
{
    if ( !s || !out ) return false;
    if ( strcmp ( s, "rising" ) == 0 )  { *out = BP_EVT_TRIG_RISING;  return true; }
    if ( strcmp ( s, "falling" ) == 0 ) { *out = BP_EVT_TRIG_FALLING; return true; }
    if ( strcmp ( s, "changed" ) == 0 ) { *out = BP_EVT_TRIG_CHANGED; return true; }
    if ( strcmp ( s, "low" ) == 0 )     { *out = BP_EVT_TRIG_LOW;     return true; }
    if ( strcmp ( s, "high" ) == 0 )    { *out = BP_EVT_TRIG_HIGH;    return true; }
    return false;
}


/* ===========================================================================
 * g_mzhal stub (mzhal krok 8)
 * ===========================================================================
 *
 * dispatch.c cte capability/clock/geometry pole z g_mzhal (produkcne je
 * definuje per-EXE mzhal.c, ktery ve standalone testu neni - potrebuje
 * per-arch hlavicky). Stub poskytuje deterministicke placeholder hodnoty
 * odpovidajici MZ-800/PAL (stejna filozofie jako "test" placeholdery
 * v _dispatch_detect_platform). Vsechny delitele jsou nenulove, aby
 * pripadne cesty s delenim nespadly na SIGFPE.
 */
#include "../../src/emulator/mzarch/mzhal.h"

const st_MZHAL g_mzhal = {
    .arch = 800,
    .tvsys = 50,
    .arch_name = "mz800",
    .tvsys_name = "PAL",
    .full_name = "MZ-800",
    .arch_display_name = "MZ-800",
    .gdgclk_base = 17721600,
    .gdgclk_real_base = 17734475,
    .gdgclk2cpu_divider = 5,
    .gdgclk_ctc0_divider = 16,
    .cpu_hz = 17721600 / 5,
    .ctc0_input_hz = 17721600 / 16,
    .video_border_left_width = 154,
    .video_border_right_width = 134,
    .video_border_top_height = 46,
    .video_border_bottom_height = 42,
    .video_canvas_width = 640,
    .video_canvas_height = 200,
    .video_display_width = 928,
    .video_display_height = 288,
    .video_screen_width = 1136,
    .video_screen_height = 312,
    .video_screen_ticks = 354432,
    .video_screens_per_sec = 50,
    .video_h_sync_ticks = 80,
    .video_h_back_porch_ticks = 106,
    .video_h_front_porch_ticks = 22,
    .psg_divider = 80,
    .psg_count = 2,
    .have_pioz80 = 1,
    .have_fdc = 1,
    .have_ide8 = 1,
    .have_ramdisk = 1,
    .have_qdisk = 1,
    .audio_src_channels = 9,
    .audio_ctc0_gate_pc00 = 1,
    .has_key_tab = 1,
    .has_rear_dip_switch700 = 1,
    .mem_vram_size = 0x4000,
    .mem_exvram_size = 0x4000,
    .mem_pcg_size = 0,
    .iorq_rd_ticks = 12,
    .iorq_wr_ticks = 9,
    .mreq_rd_m1_ticks = 7,
    .mreq_rd_ticks = 9,
    .mreq_wr_ticks = 9,
};
