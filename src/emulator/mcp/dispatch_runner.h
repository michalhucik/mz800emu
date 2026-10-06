/*
 * File:   dispatch_runner.h
 *
 * Běhový obal MCP dispatch s omezenou dobou odpovědi.
 *
 * Transport (pipe, TCP) volá místo `mcp_dispatch_request()` funkci
 * `mcp_dispatch_runner_request()`. Ta spustí dispatch na pracovním
 * vlákně (GThreadPool) a čeká na výsledek. Když se příkaz v emu vlákně
 * zasekne (vyzvednutý příkaz se nedokončí v limitu, viz
 * mcp_dispatch_stall_limit_ms() v dispatch.c), transport dostane okamžitě
 * chybovou odpověď "Emulator busy: ... still running" a může obsluhovat
 * další požadavky. Pracovní vlákno dál čeká na dokončení příkazu, protože
 * emu vlákno pracuje s daty na jeho zásobníku; jeho pozdní výsledek se
 * zahodí.
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
 * ---------------------------------------------------------------------------
 */

#ifndef MZ800EMU_MCP_DISPATCH_RUNNER_H
#define MZ800EMU_MCP_DISPATCH_RUNNER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "dispatch.h"
#include "jsonl_io.h"


    /**
     * @brief Zpracuje jeden požadavek s omezenou dobou odpovědi.
     *
     * Spustí `mcp_dispatch_request()` na pracovním vlákně a čeká, dokud
     * neskončí, nebo dokud dispatch neohlásí zaseknutí rozpracovaného
     * příkazu v emu vlákně. Ve druhém případě vrátí chybovou odpověď
     * z `mcp_dispatch_build_stalled_response()` a požadavek označí jako
     * opuštěný: pracovní vlákno doběhne samo, jeho odpověď se zahodí
     * a další dbgapi kroky téhož požadavku se už neodešlou.
     *
     * Dlouhé čekání handleru mimo dbgapi frontu (např. `run` s čekáním
     * na snímky, `wait_*`) se nepřerušuje - limit hlídá jen příkaz, který
     * převzalo emu vlákno.
     *
     * @param[in]  req           parsed požadavek; funkce PŘEBÍRÁ vlastnictví
     *                           (uvolní ho přes `jsonl_msg_free`, případně
     *                           až pracovní vlákno po opuštění)
     * @param[out] out_response  JSONL odpověď (caller `free()`), nebo NULL
     *                           stejně jako u `mcp_dispatch_request()`
     * @return kód jako `mcp_dispatch_request()`; při zaseknutí
     *         `MCP_DISPATCH_EMU_ERROR`
     *
     * @pre req != NULL, out_response != NULL; volat z transportního vlákna,
     *      ne z emu vlákna.
     * @post `req` už caller nesmí použít.
     *
     * Opuštěný handler po dokončení zaseknutého kroku sice už neodešle
     * další dbgapi příkazy, ale vedlejší efekty mimo dbgapi (např. čekání
     * a zápis souborů v MCP vrstvě, události event_bus) provést může.
     *
     * Thread-safe. Když nejde vytvořit fond vláken, dispatch se provede
     * přímo na volajícím vlákně (bez omezení, jako dřív). Když selže jen
     * spuštění nového vlákna, požadavek zůstává ve frontě fondu (chování
     * g_thread_pool_push) a čeká na volné vlákno.
     */
    en_MCP_DISPATCH_RESULT mcp_dispatch_runner_request(st_JSONL_MESSAGE *req,
                                                       char **out_response);


    /**
     * @brief Počká, až doběhnou opuštěné požadavky (nejvýš @p timeout_ms).
     *
     * Volá se při ukončení po skončení emu vlákna a před `dbgapi_destroy()`:
     * opuštěné pracovní vlákno mohlo ještě dokončovat handler a nesmí
     * sahat na zrušené zámky dbgapi.
     *
     * @param[in] timeout_ms  maximální doba čekání
     * @return true = žádný požadavek už neběží, false = limit vypršel
     */
    bool mcp_dispatch_runner_wait_idle(int timeout_ms);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MZ800EMU_MCP_DISPATCH_RUNNER_H */
