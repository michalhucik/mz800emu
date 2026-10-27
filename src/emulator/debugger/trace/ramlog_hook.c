/**
 * @file   ramlog_hook.c
 * @brief  Napojení záznamu zápisů do paměti na emulátor - obecná část (viz ramlog_hook.h).
 *
 * @par Licence: GPLv3
 */

#include "emulator/mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "ramlog_hook.h"

#include <string.h>

#include "emulator/mzarch/mzarch.h"
#include "emulator/mzarch/mzarch_platform_functions.h"
#include "emulator/debugger/debugger.h"
#include "emulator/debugger/trace/tlog_common.h"
#include "hw-generic/memory/memory.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "emulator/cfgmain.h"
#if MZARCH == 700
#include "emulator/mzarch/mz700/devbus/mz700_devbus.h"
#endif

#include <stdio.h>
#include <glib.h>


/** @brief Zdroj a insn_pc záznamu podle g_ramlog_src a výchozího zdroje volajícího. */
static void rl_source(en_RAMLOG_SRC default_src, uint8_t *src, uint16_t *pc)
{
    if (g_ramlog_src != RAMLOG_SRC_INSN) {
        *src = g_ramlog_src;
        *pc = g_ramlog_src_pc;
    } else if (default_src == RAMLOG_SRC_INSN) {
        *src = RAMLOG_SRC_INSN;
        *pc = g_mzarch_main.instruction_addr;
    } else {
        /* Zápis debuggerem bez nastaveného zdroje: žádná instrukce. */
        *src = (uint8_t) default_src;
        *pc = 0;
    }
}


/** @brief Společná část záznamu (čas, snímek, zdroj, CPU adresa). */
static void rl_base(st_RAMLOG_REC *r, uint16_t addr, en_RAMLOG_SRC default_src)
{
    memset(r, 0, sizeof(*r));
    r->t = tlog_common_get_pxclk_total();
    r->frame = tlog_common_get_screens_total();
    r->cpu_addr = addr;
    r->count = 1;
    rl_source(default_src, &r->src, &r->insn_pc);
    if (r->src == RAMLOG_SRC_INSN && g_mzarch_main.cpu) {
        int ts = g_mzarch_main.cpu->op_tstate;
        r->tin = (uint8_t) ((ts < 0) ? 0 : (ts > 255) ? 255 : ts);
    }
}


void ramlog_hook_write(uint16_t addr, uint8_t value, en_RAMLOG_SRC default_src,
                       ramlog_write_fn fn, void *ctx)
{
    st_RAMLOG_TARGET t[RAMLOG_TARGETS_MAX];
    bool mmio = false;
    unsigned n = ramlog_platform_targets(addr, t, &mmio);
    if (mmio) {
        fn(addr, value, ctx);
        return;
    }
    bool want[RAMLOG_TARGETS_MAX] = { false };
    bool any = false;
    for (unsigned i = 0; i < n; i++) {
        want[i] = ramlog_match(addr, true, (en_RAMLOG_SPACE) t[i].space, t[i].offset);
        any = any || want[i];
    }
    /* Grafická VRAM bez zapsaného plánu: zaznamenat jen podle CPU rozsahu. */
    bool none_written = (n == 0) && ramlog_match(addr, true, RAMLOG_SPACE_COUNT, 0);
    if (!any && !none_written) {
        fn(addr, value, ctx);
        return;
    }
    /* Čas a zdroj před zápisem (zápis do VRAM posune čas o WAIT). */
    st_RAMLOG_REC base;
    rl_base(&base, addr, default_src);
    uint8_t old[RAMLOG_TARGETS_MAX];
    for (unsigned i = 0; i < n; i++) old[i] = t[i].cell ? *t[i].cell : value;

    fn(addr, value, ctx);

    for (unsigned i = 0; i < n; i++) {
        if (!want[i]) continue;
        st_RAMLOG_REC r = base;
        r.space = t[i].space;
        r.offset = t[i].offset;
        if (t[i].has_plane) {
            r.flags |= RAMLOG_FLAG_PLANE;
            r.plane = t[i].plane;
        }
        if (t[i].cell) {
            r.old_value = old[i];
            r.new_value = *t[i].cell;
            if (r.old_value == r.new_value) r.flags |= RAMLOG_FLAG_UNCHANGED;
        } else {
            r.old_value = value;
            r.new_value = value;
            r.flags |= RAMLOG_FLAG_SUPPRESSED | RAMLOG_FLAG_UNCHANGED;
        }
        ramlog_record(&r);
    }
    if (none_written) {
        st_RAMLOG_REC r = base;
        r.space = RAMLOG_SPACE_VRAM;
        r.offset = addr & 0x3FFFu;
        r.old_value = value;
        r.new_value = value;
        r.flags |= RAMLOG_FLAG_SUPPRESSED | RAMLOG_FLAG_UNCHANGED;
        ramlog_record(&r);
    }
}


/** @brief Zápis přes čistý callback CPU se synchronizací (MZ-700: g_mz700_cpu_ops.mwrite, jinak memory_write_cb()). */
static void rl_fn_cpu(uint16_t addr, uint8_t value, void *ctx)
{
#if MZARCH == 700
    /* Obálka devbus se zařízením, jinak memory_write_cb. */
    g_mz700_cpu_ops.mwrite((z80_t *) ctx, addr, value, NULL);
#else
    memory_write_cb((z80_t *) ctx, addr, value, NULL);
#endif
}


void ramlog_hook_cpu_write_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data)
{
    (void) user_data;
    ramlog_hook_write(addr, value, RAMLOG_SRC_INSN, rl_fn_cpu, cpu);
}


void ramlog_hook_logging_write(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data)
{
    (void) user_data;
    ramlog_hook_write(addr, value, RAMLOG_SRC_INSN, rl_fn_cpu, cpu);
}


void ramlog_hook_load_block(uint16_t addr, uint16_t size, bool ramonly)
{
    uint8_t src = (g_ramlog_src != RAMLOG_SRC_INSN) ? g_ramlog_src : (uint8_t) RAMLOG_SRC_LOAD;
    uint16_t pc = (g_ramlog_src != RAMLOG_SRC_INSN) ? g_ramlog_src_pc : 0;
    uint32_t a = addr;
    uint32_t end = (uint32_t) addr + size;
    while (a < end) {
        uint32_t page_end = ((a >> 12) + 1u) << 12;
        uint32_t seg_end = (end < page_end) ? end : page_end;
        st_RAMLOG_TARGET t;
        if (ramlog_platform_load_target((uint16_t) a, ramonly, &t)) {
            /* Úsek patří do filtru, když do něj patří kterýkoli bajt. */
            bool hit = false;
            for (uint32_t b = a; b < seg_end && !hit; b++) {
                uint32_t off = t.cell ? t.offset + (b - a) : t.offset;
                hit = ramlog_match((uint16_t) b, true, (en_RAMLOG_SPACE) t.space, off);
            }
            if (hit) {
                st_RAMLOG_REC r;
                memset(&r, 0, sizeof(r));
                r.t = tlog_common_get_pxclk_total();
                r.frame = tlog_common_get_screens_total();
                r.insn_pc = pc;
                r.cpu_addr = (uint16_t) a;
                r.space = t.space;
                r.offset = t.offset;
                r.src = src;
                r.flags = RAMLOG_FLAG_SUMMARY | (t.cell ? 0u : RAMLOG_FLAG_NO_MAP);
                r.count = (uint16_t) (seg_end - a);
                ramlog_record(&r);
            }
        }
        a = seg_end;
    }
}


void ramlog_hook_cell_write(en_RAMLOG_SPACE space, uint32_t offset, uint8_t old_v, uint8_t new_v)
{
    if (!ramlog_match(0, false, space, offset)) return;
    st_RAMLOG_REC r;
    rl_base(&r, 0, RAMLOG_SRC_DEBUGGER);
    r.tin = 0;
    r.space = (uint8_t) space;
    r.offset = offset;
    r.old_value = old_v;
    r.new_value = new_v;
    r.flags = RAMLOG_FLAG_NO_CPU | ((old_v == new_v) ? RAMLOG_FLAG_UNCHANGED : 0u);
    ramlog_record(&r);
}


bool ramlog_hook_start(const st_RAMLOG_CONFIG *cfg)
{
    uint32_t sizes[RAMLOG_SPACE_COUNT];
    ramlog_platform_space_sizes(sizes);
    bool ok = ramlog_start(cfg, sizes);
    /* Callbacky CPU a maska RAM fast-path podle nového stavu. */
    mzarch_platform_fn_debugger_state_changed(TEST_DEBUGGER_ACTIVE);
    return ok;
}


void ramlog_hook_stop(void)
{
    ramlog_stop();
    mzarch_platform_fn_debugger_state_changed(TEST_DEBUGGER_ACTIVE);
}


st_RAMLOG_INI g_ramlog_ini = {
    RAMLOG_CFG_MODE_OFF, RAMLOG_MODE_RING, RAMLOG_DEFAULT_CAPACITY, 0x0000, 0xFFFF
};

/** Záznam spustila konfigurace (ramlog_recompute_active) a ještě ho nezastavila. */
static bool s_cfg_started = false;


void ramlog_cfg_init(void)
{
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "RAMLOG");
    CFGELM *elm;

    elm = cfgmodule_register_new_element(cmod, "mode", CFGENTYPE_KEYWORD, RAMLOG_CFG_MODE_OFF,
                                         RAMLOG_CFG_MODE_OFF,         "OFF",
                                         RAMLOG_CFG_MODE_WITH_WINDOW, "WITH_WINDOW",
                                         RAMLOG_CFG_MODE_ALWAYS,      "ALWAYS",
                                         -1);
    cfgelement_set_handlers(elm, (void *) &g_ramlog_ini.mode, (void *) &g_ramlog_ini.mode);

    elm = cfgmodule_register_new_element(cmod, "ring", CFGENTYPE_KEYWORD, RAMLOG_MODE_RING,
                                         RAMLOG_MODE_RING, "RING",
                                         RAMLOG_MODE_FILL, "FILL",
                                         -1);
    cfgelement_set_handlers(elm, (void *) &g_ramlog_ini.ring_mode, (void *) &g_ramlog_ini.ring_mode);

    elm = cfgmodule_register_new_element(cmod, "capacity", CFGENTYPE_UNSIGNED, RAMLOG_DEFAULT_CAPACITY,
                                         RAMLOG_MIN_CAPACITY, RAMLOG_MAX_CAPACITY);
    cfgelement_set_handlers(elm, (void *) &g_ramlog_ini.capacity, (void *) &g_ramlog_ini.capacity);

    elm = cfgmodule_register_new_element(cmod, "range_lo", CFGENTYPE_UNSIGNED, 0x0000, 0, 0xFFFF);
    cfgelement_set_handlers(elm, (void *) &g_ramlog_ini.range_lo, (void *) &g_ramlog_ini.range_lo);

    elm = cfgmodule_register_new_element(cmod, "range_hi", CFGENTYPE_UNSIGNED, 0xFFFF, 0, 0xFFFF);
    cfgelement_set_handlers(elm, (void *) &g_ramlog_ini.range_hi, (void *) &g_ramlog_ini.range_hi);

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}


void ramlog_apply_cli_options(void)
{
    const char *v;

    v = sdlapp_option_value("--ramlog-mode");
    if (v) {
        if (g_ascii_strcasecmp(v, "off") == 0) g_ramlog_ini.mode = RAMLOG_CFG_MODE_OFF;
        else if (g_ascii_strcasecmp(v, "window") == 0) g_ramlog_ini.mode = RAMLOG_CFG_MODE_WITH_WINDOW;
        else if (g_ascii_strcasecmp(v, "always") == 0) g_ramlog_ini.mode = RAMLOG_CFG_MODE_ALWAYS;
        else fprintf(stderr, "Invalid --ramlog-mode value: %s (expected off|window|always)\n", v);
    }
    v = sdlapp_option_value("--ramlog-ring");
    if (v) {
        if (g_ascii_strcasecmp(v, "ring") == 0) g_ramlog_ini.ring_mode = RAMLOG_MODE_RING;
        else if (g_ascii_strcasecmp(v, "fill") == 0) g_ramlog_ini.ring_mode = RAMLOG_MODE_FILL;
        else fprintf(stderr, "Invalid --ramlog-ring value: %s (expected ring|fill)\n", v);
    }
    v = sdlapp_option_value("--ramlog-capacity");
    if (v) {
        guint64 c = g_ascii_strtoull(v, NULL, 0);
        if (c < RAMLOG_MIN_CAPACITY) c = RAMLOG_MIN_CAPACITY;
        if (c > RAMLOG_MAX_CAPACITY) c = RAMLOG_MAX_CAPACITY;
        g_ramlog_ini.capacity = (unsigned) c;
    }
    /* Adresy dec i hex (0x), oříznuté na 16 bitů jako --cputrack-pc-lo. */
    v = sdlapp_option_value("--ramlog-lo");
    if (v) g_ramlog_ini.range_lo = (unsigned) (g_ascii_strtoull(v, NULL, 0) & 0xFFFF);
    v = sdlapp_option_value("--ramlog-hi");
    if (v) g_ramlog_ini.range_hi = (unsigned) (g_ascii_strtoull(v, NULL, 0) & 0xFFFF);
    if (g_ramlog_ini.range_lo > g_ramlog_ini.range_hi) {
        unsigned t = g_ramlog_ini.range_lo;
        g_ramlog_ini.range_lo = g_ramlog_ini.range_hi;
        g_ramlog_ini.range_hi = t;
    }
}


void ramlog_recompute_active(bool debugger_active)
{
    bool want = (g_ramlog_ini.mode == RAMLOG_CFG_MODE_ALWAYS)
        || (g_ramlog_ini.mode == RAMLOG_CFG_MODE_WITH_WINDOW && debugger_active);
    st_RAMLOG_STATUS st;
    ramlog_get_status(&st);
    /* Záznam zastavený přes MCP / DSL / GUI konfigurace znovu nespustí
     * (want && s_cfg_started && !active = nic). */
    if (want && !s_cfg_started && !st.active) {
        st_RAMLOG_CONFIG cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.mode = (en_RAMLOG_MODE) g_ramlog_ini.ring_mode;
        cfg.capacity = g_ramlog_ini.capacity;
        if (g_ramlog_ini.range_lo != 0x0000 || g_ramlog_ini.range_hi != 0xFFFF) {
            cfg.n_ranges = 1;
            cfg.ranges[0].phys = false;
            cfg.ranges[0].from = g_ramlog_ini.range_lo;
            cfg.ranges[0].to = g_ramlog_ini.range_hi;
        }
        uint32_t sizes[RAMLOG_SPACE_COUNT];
        ramlog_platform_space_sizes(sizes);
        if (!ramlog_start(&cfg, sizes)) {
            fprintf(stderr, "[RAMLOG] start from configuration failed (out of memory)\n");
            return;
        }
        s_cfg_started = true;
    } else if (!want && s_cfg_started) {
        s_cfg_started = false;
        if (st.active) ramlog_stop();
    }
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
