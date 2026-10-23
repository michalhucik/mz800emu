/**
 * @file cmt_request.c
 * @brief Zásahy GUI do kazety přes emu vlákno (kontrakt v cmt_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS):
 * - otevření: str = cesta, i[0] = 1 spustit přehrávání;
 * - rychlost, hack, Fix FNAME, polarita: i[0] = hodnota;
 * - vlastní pulzy: str = šest double (long high/low/total, short
 *   high/low/total) jako bitové obrazy uint64 v šestnácti šestnáctkových
 *   číslicích oddělených mezerou (přesně a nezávisle na locale);
 * - rychlost bloku: i[0] = blok, i[1] = rychlost, i[2] = 1 blok MZ,
 *   i[3] = 1 pulzy v str.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cmt.h"
#include "cmthack.h"
#include "cmt_request.h"
#include "device_request.h"


/** @brief Počet double v textovém zápisu pulzů. */
#define CMT_REQUEST_PULSES_VALUES 6


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


/** @brief Zásah: rychlost bloku (i[0] blok, i[1] rychlost, i[2] MZ, i[3] pulzy v str). */
static int cmt_request_exec_set_block_speed(const st_DEVICE_REQ_ARGS *a)
{
    st_MZTAPE_PULSES_LENGTH p;
    const st_MZTAPE_PULSES_LENGTH *pp = NULL;
    if (a->i[3])
    {
        if (!cmt_request_pulses_decode(a, &p)) return -1;
        pp = &p;
    };
    return (EXIT_SUCCESS == cmt_tape_change_block_speed(a->i[0], (en_CMTSPEED)a->i[1], a->i[2] ? true : false, pp)) ? 0 : -1;
}


uint32_t cmt_request_set_block_speed(int block_id, int cmtspeed, bool is_mz,
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
    return device_request_submit("cmt_set_block_speed", cmt_request_exec_set_block_speed, &a);
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
