/**
 * @file   mz700_devbus.c
 * @brief  Adaptér devbus pro jádro MZ-700 (kontrakt v mz700_devbus.h).
 *
 * Adaptér předává jádru devbus (st_DEVBUS_ARCH_OPS) statické porty MZ-700,
 * okna MMIO stránky 0E000h, WAIT, čas a invalidaci obrazu. Po každé změně
 * háčků (hooks_changed) přepočítá g_mz700_cpu_ops a nainstaluje callbacky
 * CPU: obálky jen tehdy, když je připojené zařízení potřebuje.
 *
 * @par Licence: GPLv3
 */

#include "mz700_devbus.h"

#include <string.h>

#include "emulator.h"
#include "mzarch/mzarch.h"
#include "mzarch/mz700/mz700_iorq.h"
#include "mzarch/mz700/gdg/mz700_gdg.h"
#include "mzarch/mz700/gdg/mz700_framebuffer.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/rom.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/devbus/devbus.h"
#include "hw-generic/unicard/unicard.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/memory/memext.h"
#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#endif
#if CFG_HWEXT_HAVE_IDE8
#include "hw-generic/ide8/ide8.h"
#endif
#if CFG_HWEXT_HAVE_RAMDISK
#include "hw-generic/ramdisk/ramdisk.h"
#endif
#if CFG_HWEXT_HAVE_QDISK
#include "hw-generic/qdisk/qdisk.h"
#endif
#include "mzarch/mz700/mz700_bus_owners.h"
#include "mzarch/mz700/devices/pcg700/pcg700_devbus.h"
#include "mzarch/mz700/devices/suc80/suc80_devbus.h"
#include "mzarch/mz700/devices/kp80/kp80_devbus.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/bptmap.h"
#include "debugger/debugger.h"
#include "debugger/trace/cputrack.h"
#include "debugger/trace/iorqlog.h"
#include "debugger/trace/intlog.h"
#include "debugger/trace/ramlog_hook.h"
#include "debugger/coverage/coverage_hook.h"
#include "debugger/trace/hwlog.h"
#include "debugger/trace/marklog.h"
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include <json-glib/json-glib.h>
#include "mcp/event_bus.h"
#endif
#endif


st_MZ700_CPU_OPS g_mz700_cpu_ops = {
    memory_write_cb,
    port_read_cb,
    port_write_cb,
    memory_write_byte_raw,
};


/**
 * @brief Bitmapa sledovaných MMIO adres 0E000h-0E0FFh (bit = adresa - 0E000h).
 *
 * 0E000h-0E008h vždy; rozsahy připojených zařízení přidá mz700_devbus_recompute().
 * Čte ji jen ladicí cesta (memory_write_with_logging_cb).
 */
static uint8_t s_mmio_tracked[256 / 8];

/** @brief true během mz700_devbus_exit(): odpojení zařízení už nepřekresluje obraz. */
static bool s_exiting = false;
/** @brief Naposledy nastavené kreslení řádku (NULL = výchozí framebufferu). */
static framebuffer_mz700_row_fn_t s_row_fn_installed = NULL;
/** @brief Zdroj glyfů a render_row při posledním přepočtu (změna = překreslit). */
static devbus_glyph_fn s_glyph_installed = NULL;
/** @copydoc s_glyph_installed */
static devbus_row_fn s_render_installed = NULL;


void mz700_install_cpu_callbacks(void)
{
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /*
     * Pomalou cestu (with_logging_cb) použijeme, pokud ji potřebuje
     * diagnostika debuggeru (TEST_DEBUGGER_NEED_DEBUG_CALLBACKS: CPU
     * Instruction History, Memory Heatmap, iorqlog ...). Logovací varianty
     * samy volají g_mz700_cpu_ops, takže zařízení devbus vidí i ladicí běh.
     */
    if (TEST_DEBUGGER_NEED_DEBUG_CALLBACKS)
    {
        z80_set_mread(g_mzarch_main.cpu, memory_read_with_logging_cb, NULL);
        z80_set_mwrite(g_mzarch_main.cpu, memory_write_with_logging_cb, NULL);
        z80_set_pread(g_mzarch_main.cpu, port_read_with_logging_cb, NULL);
        z80_set_pwrite(g_mzarch_main.cpu, port_write_with_logging_cb, NULL);
        return;
    }
    if (g_ramlog_active)
    {
        /* Záznam zápisů bez jiné diagnostiky (ramlog, háček A): jen write
         * callback se záznamem (volá g_mz700_cpu_ops.mwrite), čtení a porty
         * čisté. */
        z80_set_mread(g_mzarch_main.cpu, memory_read_cb, NULL);
        z80_set_mwrite(g_mzarch_main.cpu, ramlog_hook_cpu_write_cb, NULL);
        z80_set_pread(g_mzarch_main.cpu, g_mz700_cpu_ops.pread, NULL);
        z80_set_pwrite(g_mzarch_main.cpu, g_mz700_cpu_ops.pwrite, NULL);
        return;
    }
#endif
    z80_set_mread(g_mzarch_main.cpu, memory_read_cb, NULL);
    z80_set_mwrite(g_mzarch_main.cpu, g_mz700_cpu_ops.mwrite, NULL);
    z80_set_pread(g_mzarch_main.cpu, g_mz700_cpu_ops.pread, NULL);
    z80_set_pwrite(g_mzarch_main.cpu, g_mz700_cpu_ops.pwrite, NULL);
}


/**
 * @brief Obsluhuje port statický switch MZ-700 (port_read_cb / port_write_cb)?
 *
 * Jediná pravda je statická tabulka vlastníků (mz700_bus_owners.c), která
 * zrcadlí větve `case` obou funkcí v mz700_iorq.c se stejnými podmínkami
 * překladu. Port statického zařízení, které je odpojené (50h, 78h-7Fh ...),
 * se zařízení devbus nedá.
 *
 * @param p Dolní bajt adresy portu.
 * @return true = port patří statickému switchi.
 */
static bool mz700_devbus_port_is_static(uint8_t p)
{
    return mz700_bus_port_is_static(p);
}


bool mz700_bus_owner_present(unsigned kind)
{
    switch ((en_MZ700_BUS_PRESENT) kind) {
    case MZ700_BUS_PRESENT_ALWAYS:      return true;
    case MZ700_BUS_PRESENT_CMTHACK:     return CMTHACK_TEST_IS_INSTALLED != 0;
    case MZ700_BUS_PRESENT_UNICARD:     return UNICARD_TEST_IS_CONNECTED;
#if CFG_HWEXT_HAVE_RAMDISK
    case MZ700_BUS_PRESENT_PEZIK68:     return g_ramdisk.pezik[RAMDISK_PEZIK_68].connected != 0;
    case MZ700_BUS_PRESENT_RAMDISK_STD: return g_ramdisk.std.connected != 0;
#endif
#if CFG_HWEXT_HAVE_IDE8
    case MZ700_BUS_PRESENT_IDE8:        return IDE8_TEST_CONNECTED;
#endif
#if CFG_HWEXT_HAVE_FDC
    case MZ700_BUS_PRESENT_FDC0:        return g_fdc[FDC0].connected != 0;
    case MZ700_BUS_PRESENT_FDC1:        return g_fdc[FDC1].connected != 0;
#endif
    case MZ700_BUS_PRESENT_MEMEXT:      return MEMEXT_TEST_CONNECTED;
#if CFG_HWEXT_HAVE_QDISK
    case MZ700_BUS_PRESENT_QDISK:       return g_qdisk.connected != 0;
#endif
#if HAVE_PIOZ80
    case MZ700_BUS_PRESENT_PIOZ80:      return true;
#endif
    default:                            return false;
    }
}


/** @brief WAIT pro devbus_cpu_wait() (vzor mzarch_main_insideop(), větev VRAM). */
static void mz700_devbus_cpu_wait(unsigned tstates)
{
    z80_add_wait_states(g_mzarch_main.cpu, (int) tstates);
    /* trace-suite: saturující akumulace WAIT T-stavů pro cputrack. */
    uint64_t sum = (uint64_t) g_mzarch_main.instruction_wait_tstates + tstates;
    g_mzarch_main.instruction_wait_tstates = (sum > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t) sum;
}


/** @brief Čas emulace v taktech GDG pro devbus_time_ticks(). */
static uint64_t mz700_devbus_time_ticks(void)
{
    return gdg_get_total_ticks();
}


/** @brief Poslední bajt na datové sběrnici (model nebuzené sběrnice LATCH). */
static uint8_t mz700_devbus_bus_latch(void)
{
    return g_mzarch_main.regDBUS_latch;
}


/**
 * @brief Hlášení kolize čtení (dva budiče devbus na témže bitu).
 *
 * Značka pro iorqlog (port_read_with_logging_cb vybere
 * IORQLOG_EVENT_IORQ_CONFLICT a podtyp CONFLICT v Event Vieweru) a událost
 * `bus_conflict` na sběrnici událostí MCP (jen s odběratelem). Výsledek
 * čtení je model emulátoru (politika AND / FIRST), ne chování HW.
 *
 * @param info Popis kolize z jádra devbus.
 * @pre Emu vlákno, uvnitř portového callbacku (nebo čtení MMIO).
 */
static void mz700_devbus_bus_conflict(const st_DEVBUS_CONFLICT_INFO *info)
{
    (void) info;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if (!info->mmio) TRACELOG_IORQ_MARK_CONFLICT();
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    if (event_bus_has_subscriber("bus_conflict"))
    {
        JsonObject *payload = json_object_new();
        json_object_set_int_member(payload, info->mmio ? "addr" : "port", (gint64) info->addr);
        json_object_set_boolean_member(payload, "mmio", info->mmio ? TRUE : FALSE);
        json_object_set_int_member(payload, "value", (gint64) info->result);
        json_object_set_int_member(payload, "conflict_mask", (gint64) info->conflict);
        json_object_set_int_member(payload, "driven_mask", (gint64) info->driven);
        json_object_set_string_member(payload, "policy", devbus_conflict_name(info->policy));
        json_object_set_int_member(payload, "ticks", (gint64) info->ticks);
        json_object_set_int_member(payload, "cycles", (gint64) g_mzarch_main.cpu->total_cycles);
        JsonArray *parts = json_array_new();
        for (unsigned i = 0; i < info->count; i++)
        {
            JsonObject *p = json_object_new();
            const st_DEVBUS_DEVICE_DESC *d = info->part[i].slot >= 0 ? devbus_desc(info->part[i].slot) : NULL;
            json_object_set_string_member(p, "id", d ? d->id : "board");
            json_object_set_int_member(p, "value", (gint64) info->part[i].value);
            json_object_set_int_member(p, "driven_mask", (gint64) info->part[i].driven);
            json_array_add_object_element(parts, p);
        }
        json_object_set_array_member(payload, "drivers", parts);
        event_bus_emit("bus_conflict", payload);
    }
#endif
#endif
}


/** @brief Překreslení obrazu pro devbus_video_invalidate(). */
static void mz700_devbus_video_invalidate(void)
{
    g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
}


/**
 * @brief Kreslení řádku plátna se zařízením s video háčkem.
 *
 * Generátor znaků = zdroj glyfů zařízení (glyph_source), jinak CG-ROM.
 * Řádek nakreslí render_row zařízení, jinak výchozí kreslení MZ-700
 * s tímto generátorem. Nastaví ho mz700_devbus_recompute() jen tehdy,
 * když je připojený některý video háček; jinak framebuffer kreslí sám.
 *
 * @param beam_row Řádek paprsku v rozsahu plátna.
 * @pre Emu vlákno, uvnitř zpracování události GDG (nebo plné překreslení).
 */
static void mz700_devbus_row(unsigned beam_row)
{
    const uint8_t *cg = g_devbus_hooks.glyph ? g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx) : g_rom.cgrom;
    if (g_devbus_hooks.row)
    {
        uint8_t *dst = g_framebuffer.pixels + (beam_row * VIDEO_DISPLAY_WIDTH + VIDEO_BORDER_LEFT_WIDTH);
        g_devbus_hooks.row(g_devbus_hooks.row_ctx, beam_row - VIDEO_BEAM_CANVAS_FIRST_ROW, cg, dst);
    }
    else
    {
        framebuffer_mz700_row_with_cg(beam_row, cg);
    }
}


/** @brief Nastaví bit sledované MMIO adresy (jen 0E000h-0E0FFh). */
static void mz700_devbus_track(unsigned addr)
{
    if (addr < 0xE000u || addr > 0xE0FFu) return;
    unsigned i = addr - 0xE000u;
    s_mmio_tracked[i >> 3] |= (uint8_t) (1u << (i & 7));
}


/**
 * @brief Přepočítá g_mz700_cpu_ops, bitmapu sledování a nainstaluje callbacky.
 *
 * hooks_changed adaptéru: volá ji jádro devbus po připojení, odpojení
 * a změně MMIO masky, která mění potřebu obálky.
 *
 * @pre Emu vlákno mezi instrukcemi, nebo zastavená emulace.
 */
static void mz700_devbus_recompute(void)
{
    g_mz700_cpu_ops.pread = g_devbus_hooks.ports ? port_read_cb_devbus : port_read_cb;
    g_mz700_cpu_ops.pwrite = g_devbus_hooks.ports ? port_write_cb_devbus : port_write_cb;
    g_mz700_cpu_ops.mwrite = g_devbus_hooks.mmio ? memory_write_cb_devbus : memory_write_cb;
    g_mz700_cpu_ops.raw_write = g_devbus_hooks.mmio ? memory_write_byte_raw_devbus : memory_write_byte_raw;

    /* Video: s háčkem kreslí řádek adaptér, bez něj framebuffer sám (výchozí
     * funkce). Změna připojení zařízení s obrazem = překreslit plátno. */
    framebuffer_mz700_row_fn_t row_fn = (g_devbus_hooks.glyph || g_devbus_hooks.row) ? mz700_devbus_row : NULL;
    if (row_fn != s_row_fn_installed)
    {
        framebuffer_mz700_set_row_fn(row_fn);
        s_row_fn_installed = row_fn;
    }
    if (g_devbus_hooks.glyph != s_glyph_installed || g_devbus_hooks.row != s_render_installed)
    {
        s_glyph_installed = g_devbus_hooks.glyph;
        s_render_installed = g_devbus_hooks.row;
        mz700_devbus_video_invalidate();
        /* V pauze se plátno samo nepřekreslí (připojení z menu nebo MCP při
         * zastavené emulaci): překreslit hned, jako po načtení snapshotu.
         * Za běhu stačí invalidace, další snímek kreslí s novým háčkem. */
        if (EMULATOR_TEST_PAUSED && g_mzarch_main.cpu && !s_exiting) mzarch_forced_full_screen_refresh();
    }

    memset(s_mmio_tracked, 0, sizeof(s_mmio_tracked));
    for (unsigned a = 0xE000u; a <= 0xE008u; a++) mz700_devbus_track(a);
    for (unsigned slot = 0; slot < devbus_count(); slot++) {
        if (!devbus_is_connected((int) slot)) continue;
        const st_DEVBUS_DEVICE_DESC *d = devbus_desc((int) slot);
        for (unsigned m = 0; m < d->mmio_count; m++) {
            for (unsigned a = d->mmio[m].first; a <= d->mmio[m].last && a <= 0xE0FFu; a++) mz700_devbus_track(a);
        }
    }

    if (g_mzarch_main.cpu) mz700_install_cpu_callbacks();
}


/** @brief Adaptér MZ-700 pro jádro devbus. */
static const st_DEVBUS_ARCH_OPS s_mz700_arch_ops = {
    DEVBUS_PLATFORM_MZ700,
    mz700_devbus_port_is_static,
    mz700_devbus_recompute,
    mz700_devbus_cpu_wait,
    mz700_devbus_time_ticks,
    mz700_devbus_video_invalidate,
    mzarch_main_insideop_mreq,
    0xE009, 0xEFFF,  /* MAPPED: nad 8255 / 8253 / GDG (0E000h-0E008h) */
    0xE000, 0xEFFF,  /* DRAM: celá stránka po OUT 0E1h */
    mz700_devbus_bus_latch,
    NULL,            /* mmio_board_driven: čtecí cesta MMIO (doplní se s obálkou čtení) */
    mz700_devbus_bus_conflict,
};


void mz700_devbus_init(void)
{
    devbus_init(&s_mz700_arch_ops);
    memset(s_mmio_tracked, 0, sizeof(s_mmio_tracked));
    for (unsigned a = 0xE000u; a <= 0xE008u; a++) mz700_devbus_track(a);
}


void mz700_devbus_start(void)
{
    /* Registrace zařízení MZ-700 (pořadí = pořadí v menu a get_devices).
     * Selhání registrace vypíše zařízení samo, emulátor běží bez něj. */
    (void) pcg700_devbus_register();
    suc80_devbus_register();
    kp80_devbus_register();
    devbus_start();
}


void mz700_devbus_exit(void)
{
    s_exiting = true;
    devbus_exit();
    framebuffer_mz700_set_row_fn(NULL);
    s_row_fn_installed = NULL;
    s_glyph_installed = NULL;
    s_render_installed = NULL;
    s_exiting = false;
}


bool mz700_devbus_mmio_tracked(uint16_t addr)
{
    if (addr < 0xE000u || addr > 0xE0FFu) return false;
    unsigned i = addr - 0xE000u;
    return (s_mmio_tracked[i >> 3] >> (i & 7)) & 1u;
}
