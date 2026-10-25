/**
 * @file   dbgapi_cmd_props.h
 * @brief  Vlastnosti příkazů dbgapi v jedné tabulce: druh (čtení,
 *         konfigurace debuggeru, řízení běhu, změna stavu stroje) a třída
 *         záznamu vstupů (jen build s debuggerem).
 *
 * Jedna tabulka pro dvě otázky, aby se nerozešly:
 *
 *  - **záznam vstupů** (replay_dbg_classify()): zapsat zásah jako událost,
 *    zachytit vstup, zakázat, nebo propustit;
 *  - **revize stavu** (state_rev.h, dbgapi_cmd_mutates()): mohl příkaz
 *    v okamžiku vyřízení změnit stav stroje?
 *
 * Z druhu vychází i dbgapi_cmd_is_read() (poslední akce uživatele), která
 * u příkazů s operací v parametru upřesní druh podle operace.
 *
 * Invariant (hlídá test `replay_dbg_classify`): každý příkaz enumu má druh
 * (ne DBGAPI_CMD_KIND_UNSET) a třída záznamu jiná než ALLOW znamená druh
 * DBGAPI_CMD_KIND_MUTATES. Opačně to neplatí (RESET jde v záznamu vlastním
 * kanálem, ALLOW, ale stav mění).
 *
 * **Nový příkaz dbgapi musí mít řádek v tabulce** (dbgapi_cmd_props.c),
 * jinak test selže; nezařazený příkaz revize bere jako měnící stav
 * (bezpečný směr).
 */

#ifndef DBGAPI_CMD_PROPS_H
#define DBGAPI_CMD_PROPS_H

#include "../mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>
#include <stdint.h>
#include "dbgapi_cmdrq.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief Druh příkazu dbgapi z pohledu stavu stroje. */
    typedef enum en_DBGAPI_CMD_KIND
    {
        DBGAPI_CMD_KIND_UNSET = 0,    /**< Nezařazený příkaz (chyba tabulky); revize ho bere jako MUTATES. */
        DBGAPI_CMD_KIND_READ,         /**< Čte stav (i export, snapshot do souboru, obraz); stav stroje nemění. */
        DBGAPI_CMD_KIND_DEBUG_CONFIG, /**< Mění jen konfiguraci a nástroje debuggeru (BP, watch, trace, profilery, záznamy pro hostitele). */
        DBGAPI_CMD_KIND_RUN_CONTROL,  /**< Řízení běhu (pauza, rozběh, kroky, rychlost hostitele); stav mění až běh, ne vyřízení. */
        DBGAPI_CMD_KIND_MUTATES       /**< Mění stav stroje při vyřízení (zápisy, vstupy, média, nastavení HW, reset). */
    } en_DBGAPI_CMD_KIND;

    /**
     * @brief Řádek tabulky vlastností příkazu.
     *
     * @invariant replay != REPLAY_DBG_ALLOW => kind == DBGAPI_CMD_KIND_MUTATES.
     */
    typedef struct st_DBGAPI_CMD_PROPS
    {
        uint8_t kind;   /**< en_DBGAPI_CMD_KIND. */
        uint8_t replay; /**< en_REPLAY_DBG_CLASS (replay_dbg.h). */
    } st_DBGAPI_CMD_PROPS;

    /**
     * @brief Vlastnosti příkazu.
     *
     * @param cmd Příkaz bez příznaku BLOCKING.
     * @return Řádek tabulky; pro hodnotu mimo enum řádek s druhem UNSET
     *         a třídou ALLOW (nikdy NULL).
     */
    const st_DBGAPI_CMD_PROPS *dbgapi_cmd_props(en_DBGAPI_CMD cmd);

    /**
     * @brief Mění příkaz při vyřízení stav stroje (pro revizi stavu)?
     *
     * @param cmd Příkaz bez příznaku BLOCKING.
     * @return true pro druh MUTATES a pro nezařazený příkaz (fail-safe).
     */
    bool dbgapi_cmd_mutates(en_DBGAPI_CMD cmd);

    /**
     * @brief Jen čte příkaz s daným parametrem stav (pro last_user_action)?
     *
     * Druh v tabulce platí pro celý příkaz. Příkazy s operací v parametru
     * (COVERAGE, PROFILER_PC, RAMLOG, BPLOG, VIDEOREC, SCREENSHOT_SERIES,
     * AUDIO_CAPTURE) mají v tabulce druh DEBUG_CONFIG, ale část operací
     * jen čte (stav, snímek dat, dotazy, export); u nich rozhoduje
     * operace. Okna GUI takové operace volají periodicky.
     *
     * @param cmd      Příkaz bez příznaku BLOCKING.
     * @param data_ptr Parametr příkazu (typ podle cmd), smí být NULL.
     * @return true pro druh READ a pro čtecí operaci příkazu s operací;
     *         false jinak, i pro příkaz s operací bez parametru.
     *
     * @pre data_ptr, pokud není NULL, ukazuje na parametr typu daného cmd
     *      (čte se jen člen op).
     */
    bool dbgapi_cmd_is_read(en_DBGAPI_CMD cmd, const void *data_ptr);

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* DBGAPI_CMD_PROPS_H */
