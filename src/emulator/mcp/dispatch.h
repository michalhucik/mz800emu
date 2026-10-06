/*
 * File:   dispatch.h
 *
 * Veřejné API dispatch vrstvy MCP backendu (V0.A.3 - core).
 *
 * Dispatch vrstva spojuje JSONL parser (`jsonl_io.h`) s existujícím
 * `dbgapi` v jádře emulátoru. Pro každý JSONL REQUEST najde podle pole
 * `cmd` příslušný handler, zavolá ho a sestaví JSONL RESPONSE.
 *
 * V této fázi je implementováno 11 jádrových příkazů + následné fáze
 * (V0.B.6, V0.B.7, V1.A.1, V1.A.2, V1.A.3, V1.A.4 = celkem 36 příkazů):
 *  - lokální (nevolají dbgapi):  ping, shutdown
 *  - dbgapi proxy:               get_state, pause, run, reset,
 *                                get_registers, mem_read, mem_write,
 *                                bp_add, bp_list
 *  - V1.A.4 (lokální):           event_subscribe, event_unsubscribe,
 *                                event_poll, trap_respond
 *
 * Pozn.: `mem_write` (V0.B.3) je sensitive (destruktivní) operace -
 * volá `DBGAPI_CMD_MEM_WRITE_CHECKED`, který selže při pokusu zapsat
 * do ROM/CG-ROM/prohibited regionu (= žádný bajt se nezapíše a klient
 * dostane error response).
 *
 * Co tato vrstva NEdělá (= scope V0.A.4+):
 *  - žádný transport (pipe / TCP) - viz V0.A.4 main_pipe, V0.A.5 tcp
 *  - žádný emu_thread state machine
 *  - žádná korelace request/response (caller udržuje req_id)
 *  - žádný TRAP forward / EVENT broadcast (= V0.A.4+)
 *
 * Thread-safety:
 *  - Dispatcher sám je re-entrantní (žádný globální stav).
 *  - Handlery volají `dbgapi_ui_submit_cmd_sync_with_origin` s
 *    `origin = DBGAPI_CMD_ORIGIN_MCP`, což je samo o sobě thread-safe
 *    (CMDRQ fronta má vlastní mutex).
 *
 * Reference: `mcp-server/plans/INSTRUKCE-faze-V0.A.3-dispatch-cmdmap.md`,
 * `debugger/rozbory/budoucnost/mcp-server/README.md` sekce 4b.7.
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

#ifndef MZ800EMU_MCP_DISPATCH_H
#define MZ800EMU_MCP_DISPATCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "jsonl_io.h"


    /**
     * @brief Návratový kód dispatch operace.
     *
     * Doplňuje obsah `out_response` - dispatcher se snaží i při chybě
     * vrátit validní JSONL RESPONSE řádek (= klient dostane structured
     * error), takže non-OK kód je hlavně informace pro caller logiku
     * (např. logging, metrics).
     */
    typedef enum en_MCP_DISPATCH_RESULT {
        /** @brief Úspěch - `*out_response` obsahuje RESPONSE řádek. */
        MCP_DISPATCH_OK = 0,
        /** @brief Neznámý cmd. `*out_response` obsahuje error response. */
        MCP_DISPATCH_UNKNOWN_CMD,
        /** @brief Neplatné parametry. `*out_response` obsahuje error response. */
        MCP_DISPATCH_INVALID_PARAMS,
        /** @brief dbgapi vrátil chybu (timeout, queue full, atd.). */
        MCP_DISPATCH_EMU_ERROR,
        /** @brief Selhání alokace paměti - `*out_response` může být NULL. */
        MCP_DISPATCH_ALLOC_ERROR,
        /** @brief Vstupní zpráva není REQUEST. `*out_response` je NULL. */
        MCP_DISPATCH_NOT_A_REQUEST,
    } en_MCP_DISPATCH_RESULT;


    /**
     * @brief Hlavní dispatch funkce - vezme parsed REQUEST a sestaví
     *        RESPONSE.
     *
     * Postup:
     *  1. Ověří, že `req` má typ `JSONL_MSG_REQUEST`.
     *  2. Najde v `cmd_map[]` záznam podle `cmd` stringu.
     *  3. Pokud cmd neexistuje, sestaví error RESPONSE
     *     `"Unknown command"` a vrátí `MCP_DISPATCH_UNKNOWN_CMD`.
     *  4. Pokud existuje, zavolá příslušný handler. Handler je
     *     odpovědný za sestavení odpovědi (success i error case).
     *
     * Wire protocol error texty jsou anglicky (per CLAUDE.md i18n -
     * jádro emulátoru nepoužívá `_()`/`_L()` a klient může být ne-CZ).
     *
     * @param[in]  req           parsed REQUEST (`jsonl_msg_get_type ==
     *                           JSONL_MSG_REQUEST`)
     * @param[out] out_response  výstupní JSONL RESPONSE řádek (caller
     *                           uvolní `free()`); při `*_NOT_A_REQUEST`
     *                           nastaveno na NULL
     * @return kód výsledku z `en_MCP_DISPATCH_RESULT`
     *
     * @pre  req != NULL, out_response != NULL
     * @post při OK / UNKNOWN_CMD / INVALID_PARAMS / EMU_ERROR je
     *       `*out_response` validní JSONL řádek; při ALLOC_ERROR /
     *       NOT_A_REQUEST je `*out_response` NULL
     */
    en_MCP_DISPATCH_RESULT mcp_dispatch_request(const st_JSONL_MESSAGE *req,
                                                char **out_response);


    /**
     * @brief Sestaví JSONL HELLO řádek se seznamem podporovaných příkazů.
     *
     * Server typicky pošle HELLO hned po navázání spojení. Verze
     * protokolu se předává staticky (V0.A.3 = `"1.0"`). Seznam
     * příkazů se generuje dynamicky z interního `cmd_map[]`.
     *
     * Capability strings v hello payload jsou anglicky (wire-level
     * texty, viz CLAUDE.md i18n pravidlo).
     *
     * @param[out] out_hello  výstupní JSONL HELLO řádek (caller
     *                        uvolní `free()`); při chybě NULL
     * @return `MCP_DISPATCH_OK` nebo `MCP_DISPATCH_ALLOC_ERROR`
     *
     * @pre  out_hello != NULL
     */
    en_MCP_DISPATCH_RESULT mcp_dispatch_build_hello(char **out_hello);


    /**
     * @brief Vrátí NULL-terminated read-only pole jmen podporovaných
     *        příkazů (snapshot z interního `cmd_map[]`).
     *
     * Pole je dynamicky alokované ve `mcp_dispatch_init()` z `cmd_map[]`
     * (= jeden zdroj pravdy, žádný hardcoded mirror). Caller nesmí
     * pointer free-ovat ani modifikovat obsah. Užitečné pro hello
     * payload builder, dokumentaci, testy.
     *
     * @return pointer na NULL-terminated pole; při volání před
     *         `mcp_dispatch_init` vrací prázdné pole `{NULL}`
     */
    const char *const *mcp_dispatch_get_supported_commands(void);


    /**
     * @brief Identifikace transportu pro dispatch handlery, které mají
     *        odlišné chování podle wire-level kanálu.
     *
     * V1.B.3 hot-swap workflow (`emu_stop`) se aplikuje pouze v pipe
     * módu - TCP varianta by zabila živou GUI session a Python wrapper
     * stejně nemá handle na child proces (= TCP attach k existující
     * binárce). Handler `emu_stop` na TCP transportu vrátí error.
     */
    typedef enum en_MCP_DISPATCH_TRANSPORT {
        /** @brief Transport ještě nenastaven (= unit testy bez main). */
        MCP_DISPATCH_TRANSPORT_NONE = 0,
        /** @brief stdin/stdout pipe - subprocess spawn klientem. */
        MCP_DISPATCH_TRANSPORT_PIPE,
        /** @brief TCP socket - attach k existující GUI / headless instanci. */
        MCP_DISPATCH_TRANSPORT_TCP,
    } en_MCP_DISPATCH_TRANSPORT;


    /**
     * @brief Inicializace dispatch vrstvy - alokace dynamického
     *        `supported_commands` pole z `cmd_map[]`.
     *
     * Volá se z `main_pipe_main()` resp. `mcp_tcp_server_start()` před
     * tím, než se začnou zpracovávat requesty. Idempotentní - opakovaná
     * volání nic neudělají (pole už alokované). Páruje s
     * `mcp_dispatch_shutdown()`.
     *
     * @par Side effects:
     *   Alokuje paměť pro pole jmen (count + 1 stringů, každý g_strdup).
     */
    void mcp_dispatch_init(void);


    /**
     * @brief Uvolní zdroje alokované v `mcp_dispatch_init()`.
     *
     * Idempotentní (lze volat i bez init). Po volání vrací
     * `mcp_dispatch_get_supported_commands()` prázdný seznam.
     */
    void mcp_dispatch_shutdown(void);


    /**
     * @brief Nastaví transport kind viditelný pro dispatch handlery.
     *
     * Single-threaded setup - volá main_pipe.c nebo tcp_server.c
     * těsně po `mcp_dispatch_init`. Default po init je
     * `MCP_DISPATCH_TRANSPORT_NONE`.
     *
     * @param[in] kind  identifikace transportu (NONE / PIPE / TCP)
     */
    void mcp_dispatch_set_transport_kind(en_MCP_DISPATCH_TRANSPORT kind);


    /**
     * @brief Vrátí aktuálně registrovaný transport kind.
     *
     * Bezpečné volat i bez set (vrátí NONE). Využívají dispatch
     * handlery citlivé na transport (= V1.B.3 `emu_stop`).
     */
    en_MCP_DISPATCH_TRANSPORT mcp_dispatch_get_transport_kind(void);


    /**
     * @brief Typ callbacku pro signál o `shutdown` příkazu.
     *
     * Volá se v rámci `_handle_shutdown` po sestavení success RESPONSE,
     * tj. ještě **před** tím, než transport vrstva (= `main_pipe.c`)
     * stihne odpověď odeslat. Implementace callbacku má jen nastavit
     * "shutdown requested" flag a probudit hlavní vlákno - nesmí
     * blokovat ani provádět cleanup.
     */
    typedef void (*mcp_dispatch_shutdown_cb_fn)(void);


    /**
     * @brief Zaregistruje callback, který bude zavolán při zpracování
     *        příkazu `shutdown`.
     *
     * Drobné rozšíření V0.A.3 dispatch tabulky pro potřeby V0.A.4
     * pipe transportu (= `main_pipe.c` musí poznat, že klient chce
     * graceful exit). Callback se předá NULL pro odregistraci.
     *
     * Předpokládá single-threaded volání (typicky setup z `main_pipe.c`
     * před spawnem stdin reader threadu).
     *
     * @param[in] cb  callback funkce nebo NULL
     */
    void mcp_dispatch_set_shutdown_callback(mcp_dispatch_shutdown_cb_fn cb);


    /**
     * @brief Hlídání zaseknutého příkazu pro vlákno, které dispatchuje.
     *
     * Registruje ho běhový obal (`dispatch_runner.c`) pro vlákno, na
     * kterém běží `mcp_dispatch_request()`. Každý submit do dbgapi z
     * tohoto vlákna pak:
     *  - nejdřív zjistí `is_abandoned` - pokud je požadavek už opuštěný
     *    (klient dostal odpověď "busy"), další krok se neodešle a vrátí
     *    se jako neprovedený (žádné opožděné vedlejší efekty),
     *  - jinak volá `dbgapi_ui_submit_cmd_sync_watched()` a při zaseknutí
     *    rozpracovaného příkazu (viz mcp_dispatch_stall_limit_ms() v
     *    dispatch.c) zavolá `on_stall`.
     *
     * Členy:
     *  - on_stall:     hlášení zaseknutí; volá se z dispatchujícího
     *                  vlákna bez zámků dbgapi, nesmí blokovat,
     *  - is_abandoned: true = klientovi už bylo odpovězeno, výsledek
     *                  se zahodí,
     *  - user_data:    kontext obou callbacků (vlastní registrující).
     *
     * Invariant: struktura i user_data musí žít, dokud je registrace
     * aktivní (do `mcp_dispatch_set_thread_stall_watch(NULL)`).
     */
    typedef struct st_MCP_DISPATCH_STALL_WATCH {
        void (*on_stall)(void *user_data);
        bool (*is_abandoned)(void *user_data);
        void *user_data;
    } st_MCP_DISPATCH_STALL_WATCH;


    /**
     * @brief Zaregistruje (nebo s NULL zruší) hlídání zaseknutí pro
     *        aktuální vlákno.
     *
     * Registrace je thread-local, jiná vlákna (např. přímé volání
     * `mcp_dispatch_request()` v unit testech) zůstávají beze změny:
     * bez registrace se submit chová jako dřív (rozpracovaný příkaz
     * se čeká bez limitu).
     *
     * @param[in] watch  hlídání nebo NULL; ukazatel se jen uloží
     */
    void mcp_dispatch_set_thread_stall_watch(const st_MCP_DISPATCH_STALL_WATCH *watch);


    /**
     * @brief Dodatečný limit pro dokončení převzatého příkazu (tabulka).
     *
     * Vrací, jak dlouho po vypršení limitu fronty (10 s) se ještě čeká na
     * dokončení příkazu, který emu vlákno převzalo, než klient dostane
     * "Emulator busy: command still running". Běžné příkazy 10 s,
     * příkazy se souborovým I/O nebo velkými daty (trace_save, snapshot
     * save/load, profiler/CDL export, videorec, media load/insert/eject,
     * CMT open/record, PNG screenshot) 600 s. Limit fronty se nemění.
     *
     * @param[in] cmd  en_DBGAPI_CMD (případný BLOCKING flag se ignoruje)
     * @return limit v ms (> 0)
     */
    int mcp_dispatch_stall_limit_ms(int cmd);


    /**
     * @brief Sestaví chybovou odpověď "příkaz se v emu vlákně zasekl".
     *
     * Text začíná stabilním prefixem `Emulator busy:` (kontrakt pro
     * klienty, docs/agent/error_handling.md) a říká, že výsledek není
     * znám (příkaz může doběhnout později), takže opakování NENÍ bez
     * kontroly stavu bezpečné.
     *
     * @param[in]  req_id        ID požadavku
     * @param[out] out_response  JSONL řádek (caller `free()`), při chybě NULL
     * @return `MCP_DISPATCH_EMU_ERROR` nebo `MCP_DISPATCH_ALLOC_ERROR`
     */
    en_MCP_DISPATCH_RESULT mcp_dispatch_build_stalled_response(int64_t req_id,
                                                               char **out_response);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MZ800EMU_MCP_DISPATCH_H */
