/**
 * @file cmt_request.c
 * @brief Zásahy GUI do kazety přes emu vlákno (kontrakt v cmt_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS):
 * - otevření: str = cesta, i[0] = 1 spustit přehrávání;
 * - rychlost, hack, Fix FNAME, polarita, režim načítání: i[0] = hodnota;
 * - vlastní pulzy: str = šest double (long high/low/total, short
 *   high/low/total) jako bitové obrazy uint64 v šestnácti šestnáctkových
 *   číslicích oddělených mezerou (přesně a nezávisle na locale);
 * - rychlost bloku: i[0] = blok, i[1] = rychlost, i[2] = 1 blok MZ,
 *   i[3] = 1 pulzy v str;
 * - kontrola pásky (uložení, export, rychlost bloku, Load directly a úpravy
 *   *_on_tape()): i[4] = 1 kontrolovat, i[5] = očekávaný čítač výměn pásky
 *   (cmttmz_get_tape_serial());
 * - smazání bloků: str = seznam rozsahů (cmt_request_block_ranges_encode()),
 *   i[0] = počet bloků (kontrola dekódování);
 * - přesun bloku: i[0] = blok, i[1] = cílová pozice;
 * - import: str = cesta, i[0] = pozice (-1 = na konec);
 * - export celé pásky: str = cesta;
 * - kopie pulzů pro analýzu: i[0] = blok;
 * - uplatnění analýzy: i[0] = 1 Replace / 0 Add, i[1] = číslo zařazení
 *   (ukazatele na data volajícího jsou v registru s_apply_slots, aby je šlo
 *   zrušit), i[5] = čítač výměn pásky kopie pulzů;
 * - zápis informací o pásce: i[0] = číslo zápisu (kopie formuláře
 *   v s_info_slots);
 * - převod profilu: str = ID profilu, i[0] = blok, i[1] = rychlost.
 */

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "cmt.h"
#include "cmthack.h"
#include "cmt_tmz.h"
#include "cmt_request.h"
#include "cmtext.h"
#include "cmtext_container.h"
#include "device_request.h"


/** @brief Počet double v textovém zápisu pulzů. */
#define CMT_REQUEST_PULSES_VALUES 6


/**
 * @brief Čítač operací okna zahozených kvůli změně stavu decku (cmt_request_note_window_drop()).
 *
 * Zvyšuje ho emu vlákno, čte UI vlákno; jen atomické operace GLib.
 */
static gint s_window_drop_serial = 0;


void cmt_request_note_window_drop(void)
{
    g_atomic_int_inc(&s_window_drop_serial);
}


unsigned cmt_request_get_window_drop_serial(void)
{
    return (unsigned)g_atomic_int_get(&s_window_drop_serial);
}


/**
 * @brief Zapíše pulzy do textového argumentu zásahu.
 * @param a Argumenty (str se přepíše).
 * @param p Pulzy.
 */
static void cmt_request_pulses_encode(st_DEVICE_REQ_ARGS *a, const st_MZTAPE_PULSES_LENGTH *p)
{
    const double v[CMT_REQUEST_PULSES_VALUES] = {
        p->long_pulse.high, p->long_pulse.low, p->long_pulse.total,
        p->short_pulse.high, p->short_pulse.low, p->short_pulse.total,
    };
    size_t pos = 0;
    for (int n = 0; n < CMT_REQUEST_PULSES_VALUES; n++)
    {
        uint64_t bits;
        memcpy(&bits, &v[n], sizeof(bits));
        pos += (size_t)snprintf(&a->str[pos], sizeof(a->str) - pos, "%s%016" PRIx64, n ? " " : "", bits);
    };
}


/**
 * @brief Přečte pulzy z textového argumentu zásahu.
 * @param a Argumenty.
 * @param[out] p Pulzy (platné jen při true).
 * @return true = všech šest hodnot přečteno.
 */
static bool cmt_request_pulses_decode(const st_DEVICE_REQ_ARGS *a, st_MZTAPE_PULSES_LENGTH *p)
{
    uint64_t bits[CMT_REQUEST_PULSES_VALUES];
    if (sscanf(a->str, "%" SCNx64 " %" SCNx64 " %" SCNx64 " %" SCNx64 " %" SCNx64 " %" SCNx64,
               &bits[0], &bits[1], &bits[2], &bits[3], &bits[4], &bits[5]) != CMT_REQUEST_PULSES_VALUES)
    {
        return false;
    };
    double v[CMT_REQUEST_PULSES_VALUES];
    memcpy(v, bits, sizeof(v));
    p->long_pulse.high = v[0];
    p->long_pulse.low = v[1];
    p->long_pulse.total = v[2];
    p->short_pulse.high = v[3];
    p->short_pulse.low = v[4];
    p->short_pulse.total = v[5];
    return true;
}


/** @brief Zásah: otevření pásky (str cesta, i[0] spustit přehrávání). */
static int cmt_request_exec_open(const st_DEVICE_REQ_ARGS *a)
{
    if (EXIT_SUCCESS != cmt_open_file_by_extension((char *)a->str)) return -1;
    if (a->i[0]) cmt_play();
    return 0;
}


uint32_t cmt_request_open(const char *path, bool play)
{
    if (!path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0;
    a.i[0] = play ? 1 : 0;
    return device_request_submit("cmt_open", cmt_request_exec_open, &a);
}


/**
 * @brief Je vložená páska ta, kterou zásah čeká (i[4], i[5])?
 * @param a Argumenty. @return true = bez kontroly, nebo čítač výměn pásky souhlasí.
 */
static bool cmt_request_tape_matches(const st_DEVICE_REQ_ARGS *a)
{
    return !a->i[4] || ((unsigned)a->i[5] == cmttmz_get_tape_serial());
}


/** @brief Zásah: uložení pásky (str cesta, i[0] 1 = dosavadní cesta pásky, i[4] / i[5] kontrola pásky). */
static int cmt_request_exec_save(const st_DEVICE_REQ_ARGS *a)
{
    if (!cmt_request_tape_matches(a)) return CMT_REQUEST_RC_TAPE_CHANGED;
    return (EXIT_SUCCESS == cmttmz_save(a->i[0] ? NULL : a->str)) ? 0 : -1;
}


/**
 * @brief Zařadí uložení pásky (společné tělo cmt_request_save() a *_on_tape()).
 * @param check true = kontrolovat pásku. @param tape_serial Čítač výměn pásky.
 * @param path Cíl, NULL nebo prázdný = dosavadní cesta pásky.
 * @return Pořadové číslo zásahu, 0 = nezařazeno (cesta delší než mez fronty).
 */
static uint32_t cmt_request_save_submit(bool check, unsigned tape_serial, const char *path)
{
    st_DEVICE_REQ_ARGS a;
    if (path && path[0])
    {
        if (!device_request_args_set_str(&a, path)) return 0;
        a.i[0] = 0;
    }
    else
    {
        device_request_args_set_str(&a, NULL);
        a.i[0] = 1;
    };
    a.i[4] = check ? 1 : 0;
    a.i[5] = (int)tape_serial;
    return device_request_submit("cmt_save", cmt_request_exec_save, &a);
}


uint32_t cmt_request_save(const char *path)
{
    return cmt_request_save_submit(false, 0, path);
}


uint32_t cmt_request_save_on_tape(unsigned tape_serial, const char *path)
{
    return cmt_request_save_submit(true, tape_serial, path);
}


/** @brief Zásah: export bloku (i[0] blok, str cesta, i[4] / i[5] kontrola pásky). */
static int cmt_request_exec_export_block(const st_DEVICE_REQ_ARGS *a)
{
    if (!cmt_request_tape_matches(a)) return CMT_REQUEST_RC_TAPE_CHANGED;
    return (EXIT_SUCCESS == cmttmz_export_block(a->i[0], a->str)) ? 0 : -1;
}


/**
 * @brief Zařadí export bloku (společné tělo cmt_request_export_block() a *_on_tape()).
 * @param check true = kontrolovat pásku. @param tape_serial Čítač výměn pásky.
 * @param block_id Blok. @param path Cíl. @return Pořadové číslo zásahu, 0 = nezařazeno.
 */
static uint32_t cmt_request_export_submit(bool check, unsigned tape_serial, int block_id, const char *path)
{
    if (block_id < 0 || !path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0;
    a.i[0] = block_id;
    a.i[4] = check ? 1 : 0;
    a.i[5] = (int)tape_serial;
    return device_request_submit("cmt_export_block", cmt_request_exec_export_block, &a);
}


uint32_t cmt_request_export_block(int block_id, const char *path)
{
    return cmt_request_export_submit(false, 0, block_id, path);
}


uint32_t cmt_request_export_block_on_tape(unsigned tape_serial, int block_id, const char *path)
{
    return cmt_request_export_submit(true, tape_serial, block_id, path);
}


/** @brief Zásah: výchozí rychlost (i[0] en_CMTSPEED). */
static int cmt_request_exec_set_speed(const st_DEVICE_REQ_ARGS *a)
{
    return (EXIT_SUCCESS == cmt_change_speed((en_CMTSPEED)a->i[0])) ? 0 : -1;
}


uint32_t cmt_request_set_speed(int cmtspeed)
{
    if (!cmtspeed_is_valid((en_CMTSPEED)cmtspeed)) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = cmtspeed;
    return device_request_submit("cmt_set_speed", cmt_request_exec_set_speed, &a);
}


/** @brief Zásah: výchozí vlastní pulzy (str pulzy). */
static int cmt_request_exec_set_custom_pulses(const st_DEVICE_REQ_ARGS *a)
{
    st_MZTAPE_PULSES_LENGTH p;
    if (!cmt_request_pulses_decode(a, &p)) return -1;
    return (EXIT_SUCCESS == cmt_change_custom_pulses(&p)) ? 0 : -1;
}


uint32_t cmt_request_set_custom_pulses(const st_MZTAPE_PULSES_LENGTH *pulses)
{
    if (!pulses) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    cmt_request_pulses_encode(&a, pulses);
    return device_request_submit("cmt_set_custom_pulses", cmt_request_exec_set_custom_pulses, &a);
}


/** @brief Zásah: rychlost bloku (i[0] blok, i[1] rychlost, i[2] MZ, i[3] pulzy v str, i[4] / i[5] kontrola pásky). */
static int cmt_request_exec_set_block_speed(const st_DEVICE_REQ_ARGS *a)
{
    if (!cmt_request_tape_matches(a)) return CMT_REQUEST_RC_TAPE_CHANGED;
    st_MZTAPE_PULSES_LENGTH p;
    const st_MZTAPE_PULSES_LENGTH *pp = NULL;
    if (a->i[3])
    {
        if (!cmt_request_pulses_decode(a, &p)) return -1;
        pp = &p;
    };
    return (EXIT_SUCCESS == cmt_tape_change_block_speed(a->i[0], (en_CMTSPEED)a->i[1], a->i[2] ? true : false, pp)) ? 0 : -1;
}


/**
 * @brief Zařadí rychlost bloku (společné tělo cmt_request_set_block_speed() a *_on_tape()).
 * @param check true = kontrolovat pásku. @param tape_serial Čítač výměn pásky.
 * @param block_id Blok. @param cmtspeed Rychlost. @param is_mz Blok MZ. @param pulses Pulzy.
 * @return Pořadové číslo zásahu, 0 = nezařazeno.
 */
static uint32_t cmt_request_block_speed_submit(bool check, unsigned tape_serial, int block_id, int cmtspeed, bool is_mz,
                                               const st_MZTAPE_PULSES_LENGTH *pulses)
{
    if (block_id < 0) return 0;
    en_CMTSPEED s = (en_CMTSPEED)cmtspeed;
    if (s != CMTSPEED_NONE && !cmtspeed_is_valid(s) && !cmtspeed_is_custom(s)) return 0;
    if (cmtspeed_is_custom(s) && !pulses) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = block_id;
    a.i[1] = cmtspeed;
    a.i[2] = is_mz ? 1 : 0;
    if (pulses)
    {
        a.i[3] = 1;
        cmt_request_pulses_encode(&a, pulses);
    };
    a.i[4] = check ? 1 : 0;
    a.i[5] = (int)tape_serial;
    return device_request_submit("cmt_set_block_speed", cmt_request_exec_set_block_speed, &a);
}


uint32_t cmt_request_set_block_speed(int block_id, int cmtspeed, bool is_mz,
                                     const st_MZTAPE_PULSES_LENGTH *pulses)
{
    return cmt_request_block_speed_submit(false, 0, block_id, cmtspeed, is_mz, pulses);
}


uint32_t cmt_request_set_block_speed_on_tape(unsigned tape_serial, int block_id, int cmtspeed, bool is_mz,
                                             const st_MZTAPE_PULSES_LENGTH *pulses)
{
    return cmt_request_block_speed_submit(true, tape_serial, block_id, cmtspeed, is_mz, pulses);
}


/** @brief Zásah: CMT hack (i[0] 0/1). */
static int cmt_request_exec_set_hack(const st_DEVICE_REQ_ARGS *a)
{
    cmthack_load_rom_patch(a->i[0] ? 1u : 0u);
    return 0;
}


uint32_t cmt_request_set_hack(bool enabled)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = enabled ? 1 : 0;
    return device_request_submit("cmt_set_hack", cmt_request_exec_set_hack, &a);
}


int cmt_loading_mode_apply(int mode, int hack, int boost)
{
    bool hack_now = CMTHACK_TEST_IS_INSTALLED ? true : false;
    bool boost_now = CMT_TEST_CPU_BOOST_ENABLED ? true : false;
    bool hack_new, boost_new;
    if (mode >= 0)
    {
        hack_new = (mode == CMT_LOADING_MODE_DIRECT);
        boost_new = (mode == CMT_LOADING_MODE_BOOST);
    }
    else
    {
        hack_new = (hack < 0) ? hack_now : (hack != 0);
        boost_new = (boost < 0) ? boost_now : (boost != 0);
    };
    /* přímé načtení by přehrávanou pásku zastavilo a při nahrávání by hack
     * pásku nepoužil (cmthack.c): hack se mění jen ve STOP */
    if ((hack_new != hack_now) && !CMT_TEST_STOP) return -1;
    if (boost_new != boost_now) cmt_cpu_boost_set(boost_new ? CMT_CPU_BOOST_ENABLED : CMT_CPU_BOOST_DISABLED);
    if (hack_new != hack_now)
    {
        cmthack_load_rom_patch(hack_new ? 1u : 0u);
        /* rozpracovanou hlavičku bez těla by jinak příští LOAD po návratu
         * do Direct přeskočil */
        if (!hack_new) cmthack_tape_state_clear();
    };
    if (CMTHACK_TEST_IS_INSTALLED) return CMT_LOADING_MODE_DIRECT;
    return CMT_TEST_CPU_BOOST_ENABLED ? CMT_LOADING_MODE_BOOST : CMT_LOADING_MODE_TAPE;
}


/**
 * @brief Zásah režimu načítání: i[0] režim (en_CMT_LOADING_MODE), nebo -1 a pak i[1] CMT hack, i[2] CPU Boost (0/1, -1 = ponechat).
 *
 * Pravidla (změna hacku jen ve STOP, úklid stavu hacku nad páskou při
 * vypnutí) jsou v cmt_loading_mode_apply(), kterou volá i MCP.
 *
 * @param a Argumenty.
 * @return Výsledný režim (en_CMT_LOADING_MODE: hack = Direct, jinak podle
 *         CPU Boost), -1 = odmítnuto mimo STOP.
 */
static int cmt_request_exec_set_loading_mode(const st_DEVICE_REQ_ARGS *a)
{
    return cmt_loading_mode_apply(a->i[0], a->i[1], a->i[2]);
}


/**
 * @brief Zařadí zásah režimu načítání.
 * @param mode Režim (en_CMT_LOADING_MODE), -1 = podle @p hack a @p boost.
 * @param hack 0/1, -1 = ponechat (jen s @p mode -1). @param boost 0/1, -1 = ponechat (jen s @p mode -1).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 */
static uint32_t cmt_request_loading_submit(int mode, int hack, int boost)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = mode;
    a.i[1] = hack;
    a.i[2] = boost;
    return device_request_submit(CMT_REQUEST_LOADING_MODE_NAME, cmt_request_exec_set_loading_mode, &a);
}


uint32_t cmt_request_set_loading_mode(int mode)
{
    if ((mode < 0) || (mode >= CMT_LOADING_MODE_COUNT)) return 0;
    return cmt_request_loading_submit(mode, -1, -1);
}


uint32_t cmt_request_set_loading_hack(bool enabled)
{
    return cmt_request_loading_submit(-1, enabled ? 1 : 0, -1);
}


uint32_t cmt_request_set_loading_boost(bool enabled)
{
    return cmt_request_loading_submit(-1, -1, enabled ? 1 : 0);
}


int cmt_load_directly_apply(int block_id, en_CMT_LOAD_DIRECTLY_REFUSAL *why)
{
    en_CMT_LOAD_DIRECTLY_REFUSAL dummy;
    if (!why) why = &dummy;
    *why = CMT_LOAD_DIRECTLY_OK;
    if (!CMT_TEST_FILLED)
    {
        *why = CMT_LOAD_DIRECTLY_NO_TAPE;
        return -1;
    };
    if (!CMT_TEST_STOP)
    {
        *why = CMT_LOAD_DIRECTLY_NOT_STOPPED;
        return -1;
    };
    /* ROM bez podpory hacku: cmthack_load_rom_patch(1) by nic neudělal, blok
     * a CPU Boost by se změnily zbytečně - odmítnout dřív, než se cokoli změní */
    if (!CMTHACK_TEST_IS_INSTALLED && !cmthack_rom_supported())
    {
        *why = CMT_LOAD_DIRECTLY_NO_HACK;
        return -1;
    };
    st_CMTEXT_CONTAINER *c = g_cmt.ext ? cmtext_get_container(g_cmt.ext) : NULL;
    if (!c || (block_id < 0) || (block_id >= cmtext_container_get_count_blocks(c)))
    {
        *why = CMT_LOAD_DIRECTLY_NO_BLOCK;
        return -1;
    };
    /* typ indexu, ne ID bloku: blok 0x46, ze kterého MZF sestavit nejde, má
     * typ TZX (okno ho nenabízí, resource ho nevede jako soubor) */
    if (cmtext_container_get_block_type(c, block_id) != CMTEXT_BLOCK_TYPE_MZF)
    {
        *why = CMT_LOAD_DIRECTLY_NOT_MZF;
        return -1;
    };
    if (EXIT_SUCCESS != cmt_tape_select_block(block_id, 0))
    {
        *why = CMT_LOAD_DIRECTLY_CANNOT_OPEN;
        return -1;
    };
    /* na rozdíl od cmt_loading_mode_apply(CMT_LOADING_MODE_DIRECT, ...)
     * zapnutý hack nemění ani CPU Boost */
    if (CMTHACK_TEST_IS_INSTALLED) return 0;
    if (CMT_TEST_CPU_BOOST_ENABLED) cmt_cpu_boost_set(CMT_CPU_BOOST_DISABLED);
    cmthack_load_rom_patch(1u);
    return 1;
}


/**
 * @brief Zásah Load directly: výběr bloku a režim Direct najednou (i[0] blok, i[4] / i[5] kontrola pásky).
 *
 * Nejdřív ověří pásku (čítač výměn pásky), pak provede
 * cmt_load_directly_apply() (pravidla sdílená s MCP). Výběr i režim jsou ve
 * STOP a pod zámkem stavu zařízení, mezi kontrolou a provedením se deck
 * změnit nemůže.
 *
 * @param a Argumenty.
 * @return 1 = blok vybraný a režim přepnutý do Direct; 0 = blok vybraný,
 *         Direct už byl; -1 = nic se nezměnilo (deck mimo STOP, blok
 *         neexistuje, blok bez MZF nebo nejde otevřít, bez pásky);
 *         CMT_REQUEST_RC_TAPE_CHANGED = jiná páska, nic se nezměnilo.
 */
static int cmt_request_exec_load_directly(const st_DEVICE_REQ_ARGS *a)
{
    if (!cmt_request_tape_matches(a)) return CMT_REQUEST_RC_TAPE_CHANGED;
    return cmt_load_directly_apply(a->i[0], NULL);
}


uint32_t cmt_request_load_directly_on_tape(uint32_t tape_serial, int block_id)
{
    if (block_id < 0) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = block_id;
    a.i[4] = 1;
    a.i[5] = (int)tape_serial;
    return device_request_submit(CMT_REQUEST_LOAD_DIRECTLY_NAME, cmt_request_exec_load_directly, &a);
}


/** @brief Zásah: Fix FNAME Terminator (i[0] 0/1). */
static int cmt_request_exec_set_fix_fname_terminator(const st_DEVICE_REQ_ARGS *a)
{
    CMTHACK_SET_FIX_FNAME_TERMINATOR(a->i[0] ? 1u : 0u);
    return 0;
}


uint32_t cmt_request_set_fix_fname_terminator(bool enabled)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = enabled ? 1 : 0;
    return device_request_submit("cmt_set_fix_fname_terminator", cmt_request_exec_set_fix_fname_terminator, &a);
}


/**
 * @brief Zásah: Direct Save (i[0] 0/1), jen ve STOP.
 * @param a Argumenty.
 * @return 1 = patch zápisu je v ROM, 0 = není, -1 = deck mimo STOP (nic se nezměnilo).
 */
static int cmt_request_exec_set_direct_save(const st_DEVICE_REQ_ARGS *a)
{
    /* jako přepínač CMT hacku: patch rutin kazety se mění jen ve STOP */
    if (!CMT_TEST_STOP) return -1;
    cmthack_direct_save_set(a->i[0] ? 1u : 0u);
    return CMTHACK_TEST_SAVE_INSTALLED ? 1 : 0;
}


uint32_t cmt_request_set_direct_save(bool enabled)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = enabled ? 1 : 0;
    return device_request_submit(CMT_REQUEST_DIRECT_SAVE_NAME, cmt_request_exec_set_direct_save, &a);
}


/** @brief Zásah: polarita (i[0] 0/1). */
static int cmt_request_exec_set_polarity(const st_DEVICE_REQ_ARGS *a)
{
    cmt_rear_dip_switch_cmt_inverted_polarity(a->i[0] ? 1u : 0u);
    return 0;
}


uint32_t cmt_request_set_polarity(bool inverted)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = inverted ? 1 : 0;
    return device_request_submit("cmt_set_polarity", cmt_request_exec_set_polarity, &a);
}


/** @brief Zásah: jeden krok výpočtu délek bloků pro okno (bez argumentů, stav stroje nemění). */
static int cmt_request_exec_lengths(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    cmttmz_lengths_update_budget(CMTTMZ_LENGTHS_STEP_US);
    return 0;
}


uint32_t cmt_request_lengths(void)
{
    /* data jen pro UI: není to akce uživatele a nezvedá revizi stavu stroje */
    return device_request_submit_ui_data(CMT_REQUEST_LENGTHS_NAME, cmt_request_exec_lengths, NULL);
}


/* ------------------------------------------------------------------ */
/* Úpravy pásky                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Výsledky zásahů úprav pásky pro okno (cmt_request_get_edit_result()).
 *
 * Kruh podle pořadového čísla zásahu (seq % CMT_REQUEST_EDIT_RESULTS_MAX).
 * Zapisuje emu vlákno ve funkci zásahu (device_request_poll() drží zámek
 * stavu zařízení), čte UI vlákno pod týmž zámkem.
 *
 * @invariant Položka s `seq` != 0 je výsledek zásahu s tímto číslem.
 */
static st_CMT_REQUEST_EDIT_RESULT s_edit_results[CMT_REQUEST_EDIT_RESULTS_MAX];


bool cmt_request_get_edit_result(uint32_t seq, st_CMT_REQUEST_EDIT_RESULT *out)
{
    if (!seq || !out) return false;
    const st_CMT_REQUEST_EDIT_RESULT *r = &s_edit_results[seq % CMT_REQUEST_EDIT_RESULTS_MAX];
    if (r->seq != seq) return false;
    *out = *r;
    return true;
}


/**
 * @brief Počet bloků vložené pásky (0 bez pásky).
 * @return Počet bloků kontejneru.
 * @pre Vlákno emulátoru (nebo pod device_state_lock()).
 */
static uint32_t cmt_request_tape_block_count(void)
{
    if (!CMT_TEST_FILLED || !g_cmt.ext) return 0;
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    if (!c) return 0;
    int n = cmtext_container_get_count_blocks(c);
    return (n > 0) ? (uint32_t)n : 0;
}


/**
 * @brief Připraví výsledek úpravy (zatím chyba bez textu, nic k výběru).
 * @param[out] r Výsledek.
 * @param op Druh úpravy (en_CMT_REQUEST_EDIT_OP).
 */
static void cmt_request_edit_begin(st_CMT_REQUEST_EDIT_RESULT *r, int op)
{
    memset(r, 0, sizeof(*r));
    r->op = op;
    r->rc = -1;
    r->select_block = -1;
    r->removed_block = -1;
}


/**
 * @brief Dokončí výsledek úpravy a uloží ho pod pořadovým číslem právě prováděného zásahu.
 *
 * Doplní čítač výměn pásky a počet bloků po zásahu; při @p rc -1 bez textu
 * vezme text chyby úpravy (cmttmz_get_edit_error()). Při @p rc != 0 vynuluje
 * blok k výběru a počet. Mimo device_request_poll() (pořadové číslo 0) se
 * výsledek neukládá.
 *
 * @param r Výsledek (select_block, count, new_tape a message podle úpravy).
 * @param rc Návrat zásahu.
 * @return @p rc (pro return z funkce zásahu).
 * @pre Emu vlákno ve funkci zásahu (drží zámek stavu zařízení).
 */
static int cmt_request_edit_finish(st_CMT_REQUEST_EDIT_RESULT *r, int rc)
{
    r->seq = device_request_current_seq();
    r->rc = rc;
    r->tape_serial = (uint32_t)cmttmz_get_tape_serial();
    r->block_count = cmt_request_tape_block_count();
    if (rc != 0)
    {
        r->select_block = -1;
        r->removed_block = -1;
        r->count = 0;
        r->new_tape = false;
        if ((rc == -1) && !r->message[0])
            g_strlcpy(r->message, cmttmz_get_edit_error(), sizeof(r->message));
    };
    if (r->seq)
        s_edit_results[r->seq % CMT_REQUEST_EDIT_RESULTS_MAX] = *r;
    return rc;
}


bool cmt_request_block_ranges_encode(const int *block_ids, uint32_t count, char *out, size_t size)
{
    if (!block_ids || !count || !out || !size) return false;
    out[0] = '\0';
    size_t len = 0;
    uint32_t i = 0;
    while (i < count)
    {
        int first = block_ids[i];
        if (first < 0) return false;
        if (i && (first <= block_ids[i - 1])) return false;
        /* souvislá řada first, first + 1, ... */
        uint32_t j = i;
        while ((j + 1 < count) && (block_ids[j] < INT_MAX) && (block_ids[j + 1] == block_ids[j] + 1)) j++;
        int last = block_ids[j];
        int n = (last > first) ? snprintf(&out[len], size - len, "%s%d-%d", i ? "," : "", first, last)
                               : snprintf(&out[len], size - len, "%s%d", i ? "," : "", first);
        if ((n < 0) || ((size_t)n >= size - len)) return false;
        len += (size_t)n;
        i = j + 1;
    };
    return true;
}


/**
 * @brief Přečte nezáporné desítkové číslo ve tvaru kodéru (bez znaménka, bez úvodních nul).
 * @param[in,out] p Ukazatel do textu (posune se za číslo).
 * @param[out] value Číslo.
 * @return true = číslo přečteno a vejde se do int.
 */
static bool cmt_request_ranges_number(const char **p, int *value)
{
    const char *s = *p;
    if ((*s < '0') || (*s > '9')) return false;
    if ((s[0] == '0') && (s[1] >= '0') && (s[1] <= '9')) return false;
    int64_t v = 0;
    while ((*s >= '0') && (*s <= '9'))
    {
        v = v * 10 + (*s - '0');
        if (v > INT_MAX) return false;
        s++;
    };
    *value = (int)v;
    *p = s;
    return true;
}


int cmt_request_block_ranges_decode(const char *text, int *block_ids, uint32_t cap)
{
    if (!text || !block_ids || !cap) return -1;
    const char *p = text;
    uint32_t n = 0;
    int64_t prev = -1;
    for (;;)
    {
        int first, last;
        if (!cmt_request_ranges_number(&p, &first)) return -1;
        last = first;
        if (*p == '-')
        {
            p++;
            if (!cmt_request_ranges_number(&p, &last) || (last <= first)) return -1;
        };
        if ((int64_t)first <= prev) return -1;
        if ((uint64_t)last - (uint64_t)first + 1u > (uint64_t)(cap - n)) return -1;
        for (int64_t v = first; v <= last; v++) block_ids[n++] = (int)v;
        prev = last;
        if (*p == '\0') break;
        if (*p != ',') return -1;
        p++;
    };
    return (int)n;
}


/**
 * @brief Ověří tvar seznamu rozsahů jako cmt_request_block_ranges_decode() a spočítá čísla bez ukládání.
 *
 * Mazání s počtem větším než páska tak nemusí alokovat
 * podle nedůvěryhodného počtu, a přesto ohlásí chyby ve stejném pořadí
 * jako dřív.
 *
 * @param text Text seznamu (nesmí být NULL).
 * @param limit Mez čísel bloků (počet bloků pásky).
 * @param[out] total Počet čísel v seznamu.
 * @param[out] first_over Nejmenší číslo >= @p limit (-1 = žádné).
 * @return true = text je platný seznam; false = neplatný (výstupy nemají význam).
 */
static bool cmt_request_block_ranges_scan(const char *text, uint32_t limit, uint64_t *total, int *first_over)
{
    const char *p = text;
    int64_t prev = -1;
    *total = 0;
    *first_over = -1;
    for (;;)
    {
        int first, last;
        if (!cmt_request_ranges_number(&p, &first)) return false;
        last = first;
        if (*p == '-')
        {
            p++;
            if (!cmt_request_ranges_number(&p, &last) || (last <= first)) return false;
        };
        if ((int64_t)first <= prev) return false;
        *total += (uint64_t)last - (uint64_t)first + 1u;
        if ((*first_over < 0) && ((uint64_t)last >= (uint64_t)limit))
            *first_over = ((uint64_t)first >= (uint64_t)limit) ? first : (int)limit;
        prev = last;
        if (*p == '\0') break;
        if (*p != ',') return false;
        p++;
    };
    return true;
}


/** @brief Porovnání int pro qsort() (vzestupně). */
static int cmt_request_int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}


/**
 * @brief Zásah: smazání bloků (str rozsahy, i[0] počet bloků, i[4] / i[5] kontrola pásky).
 *
 * Seznam z textu se ověří proti počtu z i[0] (jinak chyba "Invalid block
 * list", nic se nesmaže) a smaže cmt_tape_remove_blocks(). Blok k výběru
 * po úpravě je ten na místě prvního smazaného, bez něj poslední blok;
 * removed_block = první smazaný blok (číslo před smazáním).
 *
 * Pořadí chyb je stejné, ať je počet větší než páska, nebo ne:
 * neplatný seznam, pak stav decku (cmt_tape_edit_verify(): bez
 * pásky, nahrávání, mimo STOP, záznam vstupů), pak "Block N does not
 * exist". Alokuje se jen pro seznam, který se do pásky vejde.
 *
 * @param a Argumenty.
 * @return 0 = smazáno, -1 = chyba, CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @pre Emu vlákno ve funkci zásahu (drží zámek stavu zařízení).
 */
static int cmt_request_exec_remove_blocks(const st_DEVICE_REQ_ARGS *a)
{
    st_CMT_REQUEST_EDIT_RESULT r;
    cmt_request_edit_begin(&r, CMT_REQUEST_EDIT_REMOVE);
    if (!cmt_request_tape_matches(a)) return cmt_request_edit_finish(&r, CMT_REQUEST_RC_TAPE_CHANGED);
    const uint32_t tape_n = cmt_request_tape_block_count();
    uint64_t total = 0;
    int over = -1;
    if ((a->i[0] <= 0) || !cmt_request_block_ranges_scan(a->str, tape_n, &total, &over) || (total != (uint64_t)a->i[0]))
    {
        g_strlcpy(r.message, "Invalid block list", sizeof(r.message));
        return cmt_request_edit_finish(&r, -1);
    };
    if ((uint32_t)a->i[0] > tape_n)
    {
        /* víc bloků, než má páska: některý neexistuje; stav decku se hlásí dřív (jako dřív) */
        if (!cmt_tape_edit_verify()) return cmt_request_edit_finish(&r, -1);
        snprintf(r.message, sizeof(r.message), "Block %d does not exist", over);
        return cmt_request_edit_finish(&r, -1);
    };
    int *ids = g_new(int, a->i[0]);
    int n = cmt_request_block_ranges_decode(a->str, ids, (uint32_t)a->i[0]);
    if (n != a->i[0])
    {
        g_free(ids);
        g_strlcpy(r.message, "Invalid block list", sizeof(r.message));
        return cmt_request_edit_finish(&r, -1);
    };
    if (EXIT_SUCCESS != cmt_tape_remove_blocks(ids, (uint32_t)n))
    {
        g_free(ids);
        return cmt_request_edit_finish(&r, -1);
    };
    uint32_t left = cmt_request_tape_block_count();
    r.count = (uint32_t)n;
    r.removed_block = ids[0];
    /* změna účinného taktu bloků 0x46 (blok 0x43 smazaný) */
    g_strlcpy(r.message, cmttmz_get_edit_warning(), sizeof(r.message));
    if (left)
        r.select_block = ((uint32_t)ids[0] < left) ? ids[0] : (int)(left - 1);
    g_free(ids);
    return cmt_request_edit_finish(&r, 0);
}


uint32_t cmt_request_remove_blocks_on_tape(uint32_t tape_serial, const int *block_ids, uint32_t count)
{
    if (!block_ids || !count || (count > (uint32_t)INT_MAX)) return 0;
    int *ids = g_new(int, count);
    memcpy(ids, block_ids, sizeof(int) * count);
    qsort(ids, count, sizeof(int), cmt_request_int_cmp);
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    /* kodér odmítne záporná a opakovaná čísla i seznam, který se nevejde */
    bool ok = cmt_request_block_ranges_encode(ids, count, a.str, sizeof(a.str));
    g_free(ids);
    if (!ok) return 0;
    a.i[0] = (int)count;
    a.i[4] = 1;
    a.i[5] = (int)tape_serial;
    return device_request_submit(CMT_REQUEST_REMOVE_BLOCKS_NAME, cmt_request_exec_remove_blocks, &a);
}


/**
 * @brief Zásah: přesun bloku (i[0] blok, i[1] cílová pozice, i[4] / i[5] kontrola pásky).
 * @param a Argumenty.
 * @return 0 = přesunuto, -1 = chyba, CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 */
static int cmt_request_exec_move_block(const st_DEVICE_REQ_ARGS *a)
{
    st_CMT_REQUEST_EDIT_RESULT r;
    cmt_request_edit_begin(&r, CMT_REQUEST_EDIT_MOVE);
    if (!cmt_request_tape_matches(a)) return cmt_request_edit_finish(&r, CMT_REQUEST_RC_TAPE_CHANGED);
    if (EXIT_SUCCESS != cmt_tape_move_block(a->i[0], a->i[1])) return cmt_request_edit_finish(&r, -1);
    r.count = 1;
    r.select_block = a->i[1];
    /* změna účinného taktu bloků 0x46 (přesun přes blok 0x43) */
    g_strlcpy(r.message, cmttmz_get_edit_warning(), sizeof(r.message));
    return cmt_request_edit_finish(&r, 0);
}


uint32_t cmt_request_move_block_on_tape(uint32_t tape_serial, int block_id, int to)
{
    if ((block_id < 0) || (to < 0)) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = block_id;
    a.i[1] = to;
    a.i[4] = 1;
    a.i[5] = (int)tape_serial;
    return device_request_submit(CMT_REQUEST_MOVE_BLOCK_NAME, cmt_request_exec_move_block, &a);
}


/**
 * @brief Zásah: import souboru (str cesta, i[0] pozice, záporná = na konec, i[4] / i[5] kontrola pásky).
 *
 * Kontrola pásky jako u ostatních zásahů *_on_tape(); navíc čítač 0
 * a deck bez pásky = import do nové pásky. Pozice se dopočítá v době
 * provedení (na konec = počet bloků); bez pásky jen záporná (blok 0),
 * nezáporná = chyba "no tape in the deck".
 *
 * @param a Argumenty.
 * @return 0 = importováno, -1 = chyba, CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 */
static int cmt_request_exec_import(const st_DEVICE_REQ_ARGS *a)
{
    st_CMT_REQUEST_EDIT_RESULT r;
    cmt_request_edit_begin(&r, CMT_REQUEST_EDIT_IMPORT);
    const bool filled = CMT_TEST_FILLED ? true : false;
    if (!cmt_request_tape_matches(a) && !((a->i[5] == 0) && !filled))
        return cmt_request_edit_finish(&r, CMT_REQUEST_RC_TAPE_CHANGED);
    int at = a->i[0];
    /* pozice bez pásky nemá smysl: chyba místo tichého ignorování */
    if (!filled && (at >= 0))
    {
        g_strlcpy(r.message, "no tape in the deck", sizeof(r.message));
        return cmt_request_edit_finish(&r, -1);
    };
    if (!filled)
        at = 0;
    else if (at < 0)
        at = (int)cmt_request_tape_block_count();
    uint32_t imported = 0;
    if (EXIT_SUCCESS != cmt_tape_import_file(a->str, at, &imported)) return cmt_request_edit_finish(&r, -1);
    r.count = imported;
    r.select_block = imported ? at : -1;
    r.new_tape = !filled;
    g_strlcpy(r.message, cmttmz_get_import_warning(), sizeof(r.message));
    return cmt_request_edit_finish(&r, 0);
}


uint32_t cmt_request_import_on_tape(uint32_t tape_serial, const char *path, int at)
{
    if (!path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0;
    a.i[0] = (at < 0) ? -1 : at;
    a.i[4] = 1;
    a.i[5] = (int)tape_serial;
    return device_request_submit(CMT_REQUEST_IMPORT_NAME, cmt_request_exec_import, &a);
}


/**
 * @brief Zásah: export celé pásky do WAV (str cesta).
 *
 * Volá cmttmz_export_tape_wav(); zpráva a varování jsou
 * v cmttmz_get_export_message() / cmttmz_get_export_warning(), zprávu
 * uložení export nemění.
 *
 * Zásah vždy nese kontrolu pásky (i[4] / i[5], cmt_request_export_tape_on_tape()):
 * při jiném čítači výměn pásky se nic nezapíše.
 *
 * @param a Argumenty (str = cíl s příponou .wav, i[4] / i[5] kontrola pásky).
 * @return 0 = exportováno, -1 = chyba, CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @pre Emu vlákno ve funkci zásahu (drží zámek stavu zařízení).
 */
static int cmt_request_exec_export_tape(const st_DEVICE_REQ_ARGS *a)
{
    if (!cmt_request_tape_matches(a)) return CMT_REQUEST_RC_TAPE_CHANGED;
    return (EXIT_SUCCESS == cmttmz_export_tape_wav(a->str)) ? 0 : -1;
}


uint32_t cmt_request_export_tape_on_tape(uint32_t tape_serial, const char *path)
{
    if (!path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0;
    a.i[4] = 1;
    a.i[5] = (int)tape_serial;
    return device_request_submit(CMT_REQUEST_EXPORT_TAPE_NAME, cmt_request_exec_export_tape, &a);
}


/* ------------------------------------------------------------------ */
/* Nástroje nad páskou: analýza, informace o pásce, převod             */
/* ------------------------------------------------------------------ */

/**
 * @brief Zámek nevyzvednuté kopie pulzů (s_pulses, s_pulses_seq).
 *
 * Zapisuje emu vlákno ve funkci zásahu, vyzvedává libovolné vlákno
 * (cmt_request_take_pulses()) bez device_state_lock().
 */
static GMutex s_pulses_lock;

/** @brief Nevyzvednutá kopie pulzů posledního úspěšného zásahu kopie pulzů (vlastní ji modul), NULL = žádná. */
static st_CMTTMZ_PULSES *s_pulses = NULL;

/** @brief Pořadové číslo zásahu, který s_pulses připravil (0 = žádný). */
static uint32_t s_pulses_seq = 0;


/**
 * @brief Zásah: kopie pulzů surového bloku (i[0] blok, i[4] / i[5] kontrola pásky).
 *
 * Nevyzvednutou kopii předchozího zásahu téhož druhu uvolní vždy (i když
 * tento skončí chybou). Úspěšnou kopii uloží pod pořadovým číslem zásahu.
 *
 * @param a Argumenty.
 * @return 0 = kopie připravená, -1 = chyba (text cmttmz_get_edit_error()),
 *         CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @pre Emu vlákno ve funkci zásahu (drží zámek stavu zařízení).
 */
static int cmt_request_exec_analysis_pulses(const st_DEVICE_REQ_ARGS *a)
{
    st_CMT_REQUEST_EDIT_RESULT r;
    cmt_request_edit_begin(&r, CMT_REQUEST_EDIT_ANALYSIS_PULSES);
    g_mutex_lock(&s_pulses_lock);
    st_CMTTMZ_PULSES *old = s_pulses;
    s_pulses = NULL;
    s_pulses_seq = 0;
    g_mutex_unlock(&s_pulses_lock);
    cmttmz_pulses_free(old);

    if (!cmt_request_tape_matches(a)) return cmt_request_edit_finish(&r, CMT_REQUEST_RC_TAPE_CHANGED);
    st_CMTTMZ_PULSES *p = cmttmz_analyze_copy_pulses(a->i[0]);
    if (!p) return cmt_request_edit_finish(&r, -1);
    r.select_block = a->i[0];
    r.count = p->count;
    g_mutex_lock(&s_pulses_lock);
    s_pulses = p;
    s_pulses_seq = device_request_current_seq();
    g_mutex_unlock(&s_pulses_lock);
    return cmt_request_edit_finish(&r, 0);
}


uint32_t cmt_request_analysis_pulses(uint32_t tape_serial, int block_id)
{
    if (block_id < 0) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = block_id;
    a.i[4] = 1;
    a.i[5] = (int)tape_serial;
    /* kopie pulzů stav stroje nemění: nezvedá revizi stavu */
    return device_request_submit_ui_data(CMT_REQUEST_ANALYSIS_PULSES_NAME, cmt_request_exec_analysis_pulses, &a);
}


st_CMTTMZ_PULSES* cmt_request_take_pulses(uint32_t seq)
{
    if (!seq) return NULL;
    st_CMTTMZ_PULSES *p = NULL;
    g_mutex_lock(&s_pulses_lock);
    if (s_pulses && (s_pulses_seq == seq))
    {
        p = s_pulses;
        s_pulses = NULL;
        s_pulses_seq = 0;
    };
    g_mutex_unlock(&s_pulses_lock);
    return p;
}


/** @brief Počet míst registru čekajících zásahů uplatnění analýzy (s_apply_slots). */
#define CMT_REQUEST_APPLY_SLOTS CMT_REQUEST_EDIT_RESULTS_MAX

/**
 * @brief Jeden čekající zásah uplatnění analýzy: ukazatele na data volajícího.
 *
 * Zásah ve frontě nese jen číslo zařazení (token); ukazatele jsou tady, aby
 * je cmt_request_analysis_cancel() mohl zneplatnit dřív, než je emu vlákno
 * přečte.
 *
 * @invariant token == 0 <=> volné místo.
 * @invariant executing => funkce zásahu ukazatele převzala a právě je čte;
 *            místo uvolní ona (zrušit ho už nejde).
 */
typedef struct st_CMT_REQUEST_APPLY_SLOT {
    uint32_t token;                 /**< číslo zařazení (od 1), 0 = volné místo */
    uint32_t seq;                   /**< pořadové číslo zásahu (doplní se po zařazení; 0 = ještě nedoplněno) */
    const st_CMTTMZ_PULSES *p;      /**< kopie pulzů volajícího */
    const st_CMTTMZ_ANALYSIS *a;    /**< analýza volajícího */
    const uint8_t *use;             /**< výběr řádků volajícího */
    bool executing;                 /**< funkce zásahu data právě čte */
} st_CMT_REQUEST_APPLY_SLOT;

/**
 * @brief Registr čekajících zásahů uplatnění (kruh podle token % CMT_REQUEST_APPLY_SLOTS).
 *
 * Zařazení místo obsadí (případný starší nezačatý zásah na tomtéž místě tím
 * vyprší: jeho funkce data nenajde a skončí chybou, na data volajícího
 * nesáhne). Funkce zásahu místo převezme a po uplatnění uvolní; zrušení
 * (cmt_request_analysis_cancel()) uvolní místo, které ještě nepřevzala.
 */
static st_CMT_REQUEST_APPLY_SLOT s_apply_slots[CMT_REQUEST_APPLY_SLOTS];

/** @brief Číslo posledního zařazení uplatnění (0 = žádné). */
static uint32_t s_apply_token = 0;

/** @brief Zámek s_apply_slots a s_apply_token (zařazuje a ruší libovolné vlákno, převzetí emu vlákno). */
static GMutex s_apply_lock;


/**
 * @brief Zásah: uplatnění analýzy (i[0] replace, i[1] číslo zařazení, i[4] / i[5] kontrola pásky).
 *
 * Ukazatele převezme z registru (s_apply_slots) a po dobu uplatnění ho
 * označí executing; zrušený nebo vypršelý zásah data nenajde a skončí
 * chybou "The analysis request was cancelled" bez sáhnutí na data
 * volajícího. Po návratu na data už nesáhne.
 *
 * @param a Argumenty.
 * @return 0 = uplatněno, -1 = chyba (i zrušeno), CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @pre Emu vlákno ve funkci zásahu (drží zámek stavu zařízení).
 */
static int cmt_request_exec_analysis_apply(const st_DEVICE_REQ_ARGS *a)
{
    st_CMT_REQUEST_EDIT_RESULT r;
    cmt_request_edit_begin(&r, CMT_REQUEST_EDIT_ANALYSIS_APPLY);
    const uint32_t token = (uint32_t)a->i[1];
    const st_CMTTMZ_PULSES *p = NULL;
    const st_CMTTMZ_ANALYSIS *an = NULL;
    const uint8_t *use = NULL;
    g_mutex_lock(&s_apply_lock);
    st_CMT_REQUEST_APPLY_SLOT *slot = &s_apply_slots[token % CMT_REQUEST_APPLY_SLOTS];
    const bool found = token && (slot->token == token);
    if (found)
    {
        p = slot->p;
        an = slot->a;
        use = slot->use;
        slot->executing = true;
    };
    g_mutex_unlock(&s_apply_lock);
    if (!found)
    {
        g_strlcpy(r.message, "The analysis request was cancelled", sizeof(r.message));
        return cmt_request_edit_finish(&r, -1);
    };

    int rc;
    if (!cmt_request_tape_matches(a))
    {
        rc = CMT_REQUEST_RC_TAPE_CHANGED;
    }
    else
    {
        uint32_t added = 0;
        rc = (EXIT_SUCCESS == cmt_tape_analysis_apply(p, an, use, a->i[0], &added)) ? 0 : -1;
        if (rc == 0)
        {
            r.count = added;
            if (added) r.select_block = cmttmz_analysis_first_file_block(an, use, a->i[0]);
            /* takt nových bloků souborů (blok 0x43 s jiným taktem před nimi) */
            g_strlcpy(r.message, cmttmz_get_edit_warning(), sizeof(r.message));
        };
    };
    /* data volajícího už nečteme: místo uvolnit (jen naše, mezitím ho mohl obsadit novější) */
    g_mutex_lock(&s_apply_lock);
    if (slot->token == token) memset(slot, 0, sizeof(*slot));
    g_mutex_unlock(&s_apply_lock);
    return cmt_request_edit_finish(&r, rc);
}


uint32_t cmt_request_analysis_apply(const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYSIS *a, const uint8_t *use,
                                    int replace)
{
    if (!p || !a || ((replace != 0) && (replace != 1))) return 0;
    g_mutex_lock(&s_apply_lock);
    /* číslo zařazení jde do int argumentu zásahu; 0 se nepoužívá */
    s_apply_token = ((s_apply_token == 0) || (s_apply_token >= (uint32_t)INT_MAX)) ? 1 : s_apply_token + 1;
    const uint32_t token = s_apply_token;
    st_CMT_REQUEST_APPLY_SLOT *slot = &s_apply_slots[token % CMT_REQUEST_APPLY_SLOTS];
    /* starší nezačatý zásah na tomto místě vyprší (jeho funkce data nenajde);
     * převzatý (executing) si ukazatele už zkopíroval a místo podle tokenu nesmaže */
    slot->token = token;
    slot->seq = 0;
    slot->p = p;
    slot->a = a;
    slot->use = use;
    slot->executing = false;
    g_mutex_unlock(&s_apply_lock);

    st_DEVICE_REQ_ARGS args;
    device_request_args_set_str(&args, NULL);
    args.i[0] = replace;
    args.i[1] = (int)token;
    args.i[4] = 1;
    args.i[5] = (int)p->tape_serial;
    uint32_t seq = device_request_submit(CMT_REQUEST_ANALYSIS_APPLY_NAME, cmt_request_exec_analysis_apply, &args);

    g_mutex_lock(&s_apply_lock);
    if (slot->token == token)
    {
        /* zásah ještě neproběhl (nebo právě běží): číslo pro zrušení; nezařazený zásah místo uvolní */
        if (seq) slot->seq = seq;
        else memset(slot, 0, sizeof(*slot));
    };
    g_mutex_unlock(&s_apply_lock);
    return seq;
}


bool cmt_request_analysis_cancel(uint32_t seq)
{
    if (!seq) return false;
    bool cancelled = false;
    g_mutex_lock(&s_apply_lock);
    for (unsigned i = 0; i < CMT_REQUEST_APPLY_SLOTS; i++)
    {
        st_CMT_REQUEST_APPLY_SLOT *slot = &s_apply_slots[i];
        if (!slot->token || (slot->seq != seq)) continue;
        if (!slot->executing)
        {
            memset(slot, 0, sizeof(*slot));
            cancelled = true;
        };
        break;
    };
    g_mutex_unlock(&s_apply_lock);
    return cancelled;
}



/**
 * @brief Jedna čekající kopie formuláře informací o pásce (s_info_slots).
 *
 * @invariant info != NULL => kopie patří zápisu s číslem token.
 */
typedef struct st_CMT_REQUEST_INFO_SLOT {
    uint32_t token;             /**< číslo zápisu (od 1) */
    st_CMTTMZ_TAPE_INFO *info;  /**< kopie formuláře (g_malloc, vlastní modul), NULL = volné místo */
} st_CMT_REQUEST_INFO_SLOT;

/**
 * @brief Čekající kopie formulářů informací o pásce (cmt_request_tape_info_set()).
 *
 * Kruh podle čísla zápisu (token % CMT_REQUEST_TAPE_INFO_SLOTS). Zařazení
 * kopii uloží (a uvolní kopii, která na místě zůstala), funkce zásahu ji
 * podle čísla vyjme a uvolní. Kopii zásahu, který se neprovedl (replay,
 * device_request_clear()), uvolní až zápis o CMT_REQUEST_TAPE_INFO_SLOTS dál.
 */
static st_CMT_REQUEST_INFO_SLOT s_info_slots[CMT_REQUEST_TAPE_INFO_SLOTS];

/** @brief Číslo posledního zápisu informací o pásce (0 = žádný). */
static uint32_t s_info_token = 0;

/** @brief Zámek s_info_slots a s_info_token (zařazuje libovolné vlákno, vyjímá emu vlákno). */
static GMutex s_info_lock;


/**
 * @brief Zásah: zápis informací o pásce (i[0] číslo zápisu, i[4] / i[5] kontrola pásky).
 *
 * Kopii formuláře vyjme z kruhu (a uvolní) vždy, i při chybě. Varování
 * jádra jde do message jen při úspěchu.
 *
 * @param a Argumenty.
 * @return 0 = zapsáno nebo beze změny, -1 = chyba (i vypršelá kopie),
 *         CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @pre Emu vlákno ve funkci zásahu (drží zámek stavu zařízení).
 */
static int cmt_request_exec_tape_info_set(const st_DEVICE_REQ_ARGS *a)
{
    st_CMT_REQUEST_EDIT_RESULT r;
    cmt_request_edit_begin(&r, CMT_REQUEST_EDIT_TAPE_INFO);
    const uint32_t token = (uint32_t)a->i[0];
    st_CMTTMZ_TAPE_INFO *info = NULL;
    g_mutex_lock(&s_info_lock);
    st_CMT_REQUEST_INFO_SLOT *slot = &s_info_slots[token % CMT_REQUEST_TAPE_INFO_SLOTS];
    if (slot->info && (slot->token == token))
    {
        info = slot->info;
        slot->info = NULL;
    };
    g_mutex_unlock(&s_info_lock);
    if (!info)
    {
        g_strlcpy(r.message, "The tape information request expired", sizeof(r.message));
        return cmt_request_edit_finish(&r, -1);
    };
    if (!cmt_request_tape_matches(a))
    {
        g_free(info);
        return cmt_request_edit_finish(&r, CMT_REQUEST_RC_TAPE_CHANGED);
    };
    int changed = 0;
    int rc = cmt_tape_info_set(info, &changed);
    g_free(info);
    if (rc != EXIT_SUCCESS) return cmt_request_edit_finish(&r, -1);
    r.count = changed ? 1 : 0;
    g_strlcpy(r.message, cmttmz_get_edit_warning(), sizeof(r.message));
    return cmt_request_edit_finish(&r, 0);
}


uint32_t cmt_request_tape_info_set(uint32_t tape_serial, const st_CMTTMZ_TAPE_INFO *in)
{
    if (!in) return 0;
    st_CMTTMZ_TAPE_INFO *copy = g_try_new(st_CMTTMZ_TAPE_INFO, 1);
    if (!copy) return 0;
    *copy = *in;
    copy->tape_serial = tape_serial;
    g_mutex_lock(&s_info_lock);
    /* číslo zápisu jde do int argumentu zásahu; 0 se nepoužívá */
    s_info_token = ((s_info_token == 0) || (s_info_token >= (uint32_t)INT_MAX)) ? 1 : s_info_token + 1;
    const uint32_t token = s_info_token;
    st_CMT_REQUEST_INFO_SLOT *slot = &s_info_slots[token % CMT_REQUEST_TAPE_INFO_SLOTS];
    st_CMTTMZ_TAPE_INFO *drop = slot->info;
    slot->info = copy;
    slot->token = token;
    g_mutex_unlock(&s_info_lock);
    g_free(drop);

    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = (int)token;
    a.i[4] = 1;
    a.i[5] = (int)tape_serial;
    return device_request_submit(CMT_REQUEST_TAPE_INFO_SET_NAME, cmt_request_exec_tape_info_set, &a);
}


/**
 * @brief Zásah: převod bloku na profil (i[0] blok, i[1] rychlost, str ID profilu, i[4] / i[5] kontrola pásky).
 *
 * "Beze změny" (count 0) se pozná podle čítače výměn pásky: skutečný převod
 * ho zvedne (cmttmz_replace_block()), převod beze změny se pásky nedotkne.
 *
 * @param a Argumenty.
 * @return 0 = převedeno nebo beze změny, -1 = chyba, CMT_REQUEST_RC_TAPE_CHANGED = jiná páska.
 * @pre Emu vlákno ve funkci zásahu (drží zámek stavu zařízení).
 */
static int cmt_request_exec_convert_block(const st_DEVICE_REQ_ARGS *a)
{
    st_CMT_REQUEST_EDIT_RESULT r;
    cmt_request_edit_begin(&r, CMT_REQUEST_EDIT_CONVERT);
    if (!cmt_request_tape_matches(a)) return cmt_request_edit_finish(&r, CMT_REQUEST_RC_TAPE_CHANGED);
    const unsigned before = cmttmz_get_tape_serial();
    if (EXIT_SUCCESS != cmt_tape_convert_block(a->i[0], a->str, (en_CMTSPEED)a->i[1]))
        return cmt_request_edit_finish(&r, -1);
    r.count = (cmttmz_get_tape_serial() != before) ? 1 : 0;
    r.select_block = a->i[0];
    g_strlcpy(r.message, cmttmz_get_edit_warning(), sizeof(r.message));
    return cmt_request_edit_finish(&r, 0);
}


uint32_t cmt_request_convert_block_on_tape(uint32_t tape_serial, int block_id, const char *profile_id,
                                           en_CMTSPEED speed)
{
    if ((block_id < 0) || !profile_id || !profile_id[0]) return 0;
    if (((int)speed < (int)CMTSPEED_NONE) || ((int)speed > (int)CMTSPEED_CUSTOM)) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, profile_id)) return 0;
    a.i[0] = block_id;
    a.i[1] = (int)speed;
    a.i[4] = 1;
    a.i[5] = (int)tape_serial;
    return device_request_submit(CMT_REQUEST_CONVERT_BLOCK_NAME, cmt_request_exec_convert_block, &a);
}


void cmt_request_exit(void)
{
    g_mutex_lock(&s_pulses_lock);
    st_CMTTMZ_PULSES *p = s_pulses;
    s_pulses = NULL;
    s_pulses_seq = 0;
    g_mutex_unlock(&s_pulses_lock);
    cmttmz_pulses_free(p);

    g_mutex_lock(&s_info_lock);
    for (unsigned i = 0; i < CMT_REQUEST_TAPE_INFO_SLOTS; i++)
    {
        g_free(s_info_slots[i].info);
        s_info_slots[i].info = NULL;
        s_info_slots[i].token = 0;
    };
    g_mutex_unlock(&s_info_lock);

    /* registr uplatnění data nevlastní; zásahy, které by ještě běžely, je nenajdou */
    g_mutex_lock(&s_apply_lock);
    memset(s_apply_slots, 0, sizeof(s_apply_slots));
    g_mutex_unlock(&s_apply_lock);
}
