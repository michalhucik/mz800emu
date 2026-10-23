/**
 * @file   replay_dbg.h
 * @brief  Zásahy debuggeru v záznamu vstupů: třídění příkazů dbgapi,
 *         převod parametrů na JSON a zpětné provedení (jen build s debuggerem).
 *
 * Záznam (replay.c) se ptá na každý příkaz fronty dbgapi, do které skupiny
 * patří (replay_dbg_classify()). Příkazy, které mění stav a dají se
 * zopakovat, se po provedení uloží jako událost `dbg` (jméno + argumenty
 * JSON, replay_dbg_args_to_json()); přehrání je provede stejnou cestou
 * (replay_dbg_execute() -> dbgapi_emu_dispatch()).
 *
 * | Skupina | Příkazy |
 * |---------|---------|
 * | RECORD    | `mem_write`, `mem_write_checked`, `mem_write_force`, `regions_write`, `set_reg`, `set_cpu_flags`, `io_write`, `irq_inject`, `nmi_inject` |
 * | INPUT     | `input_press_key`, `input_release_key`, `input_release_all`, `input_joy_set`, `input_joy_clear`, `hid_script_start`, `hid_script_cancel` |
 * | FORBIDDEN | `snapshot_load_*`, `media_*` (kromě `media_state`), `media_run_mzf`, `settings_set`, `platform_set`, `periph_attach`, `periph_detach`, `set_pioz80_interrupt_vector`, `io_read`, `cmt_*` měnící pásku, `memmap_set`, `freeze_add` |
 * | ALLOW     | ostatní (čtení, řízení běhu, breakpointy, trace, ...) |
 *
 * Funkce volá jen emu vlákno (replay_dbg_execute() mění stav stroje).
 */

#ifndef REPLAY_DBG_H
#define REPLAY_DBG_H

#include "../mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>
#include "dbgapi_cmdrq.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Skupina příkazu dbgapi z pohledu záznamu (viz tabulka v hlavičce).
     */
    typedef enum en_REPLAY_DBG_CLASS
    {
        REPLAY_DBG_ALLOW = 0, /**< Stav stroje nemění, nebo ho mění přes jiný zachycený kanál (reset). */
        REPLAY_DBG_RECORD,    /**< Mění stav, uloží se jako událost `dbg`. */
        REPLAY_DBG_INPUT,     /**< Mění vstup (klávesy, joystick); zachytí ho vzorek vstupů. */
        REPLAY_DBG_FORBIDDEN  /**< Mění stav, který záznam neumí zopakovat. */
    } en_REPLAY_DBG_CLASS;

    /**
     * @brief Skupina příkazu (sloupec tabulky vlastností, dbgapi_cmd_props.h).
     * @param cmd Příkaz bez příznaku BLOCKING.
     * @return Třída záznamu vstupů; nezařazený příkaz ALLOW (test
     *         `replay_dbg_classify` hlídá, že žádný nezařazený není).
     */
    en_REPLAY_DBG_CLASS replay_dbg_classify(en_DBGAPI_CMD cmd);

    /**
     * @brief Jméno příkazu skupiny RECORD pro událost (`mem_write`, ...).
     * @return Statický řetězec, nebo NULL u příkazu mimo skupinu RECORD.
     */
    const char *replay_dbg_cmd_name(en_DBGAPI_CMD cmd);

    /**
     * @brief Argumenty příkazu skupiny RECORD jako JSON objekt.
     *
     * Data paměti jako hex řetězec `data`, region jako `kind` + `sub_id`
     * (ID regionu se mezi procesy může lišit), registry číslem.
     *
     * @param cmd      Příkaz skupiny RECORD.
     * @param data_ptr Parametry příkazu (data_ptr ze st_DBGAPI_CMDRQ).
     * @return Nový řetězec (g_free), nebo NULL (jiný příkaz, NULL data,
     *         region nenalezen).
     */
    char *replay_dbg_args_to_json(en_DBGAPI_CMD cmd, const void *data_ptr);

    /**
     * @brief Provede uloženou událost `dbg` stejnou cestou jako fronta dbgapi.
     *
     * @param cmd_name  Jméno příkazu z události.
     * @param args_json Argumenty z události.
     * @param[out] err  Při chybě nový text anglicky (g_free); smí být NULL.
     * @return true = příkaz proveden úspěšně.
     * @pre Emu vlákno, mezi instrukcemi; volající zajistí, že brána
     *      záznamu příkaz nepočítá jako živý zásah.
     * @post Změnil-li příkaz registry CPU, nahrávaný segment cputrack v3
     *       dostal synchronizační záznam (cputrack_sync_check()).
     */
    bool replay_dbg_execute(const char *cmd_name, const char *args_json, char **err);

    /**
     * @brief Příkaz kazety dbgapi jako operace záznamu (`cmt` událost).
     *
     * @param cmd      Příkaz (`DBGAPI_CMD_CMT_TRANSPORT`, `_TAPE_SEEK`, `_RECORD`).
     * @param data_ptr Jeho parametry (po provedení).
     * @param[out] op   en_REPLAY_CMT_OP.
     * @param[out] arg  Hodnota (pauza, blok).
     * @param[out] path Cesta u nahrávání (ukazatel do parametrů), jinak NULL.
     * @return true = příkaz je operace kazety; false = jiný příkaz.
     */
    bool replay_dbg_cmt_op(en_DBGAPI_CMD cmd, const void *data_ptr, int *op, int32_t *arg, const char **path);

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* REPLAY_DBG_H */
