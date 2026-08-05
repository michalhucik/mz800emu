/*
 * File:   tcp_server.h
 *
 * Veřejné API TCP listener vrstvy MCP backendu (V0.A.5).
 *
 * TCP server umožňuje připojení MCP klienta k běžící GUI session
 * emulátoru. Listener je vázán na loopback rozhraní (127.0.0.1) a
 * obsluhuje JSONL request/response dialog přes `mcp_dispatch_request`
 * (sdílí 9 handlerů z V0.A.3 s pipe transportem).
 *
 * Scope V0.A.5:
 *  - 1 server instance per proces (typicky `g_mcp_tcp` v UI vrstvě)
 *  - 1 souběžný klient maximum (= V2+ multi-client deferred)
 *  - Loopback only, žádný auth / TLS (= V2+ pro remote)
 *  - Žádný EVENT broadcast / TRAP forwarding (= V0.A.6 / V1.A)
 *
 * Threading model:
 *  - Listener thread: `socket() + bind() + listen() + accept()` smyčka.
 *  - Per-connection thread: drží jedno spojení, čte JSONL řádky,
 *    dispatchuje, posílá odpovědi.
 *  - Hlavní (GUI) thread: ovládá start/stop, čte status (počet klientů,
 *    běží/neběží, port).
 *
 * Soubor je guardován `MZ800EMU_CFG_MCP_TCP_ENABLED` - při buildu s
 * `NO_MCP_TCP=1` (resp. cascade z `NO_MCP=1`) se stane prázdnou
 * translation unit.
 *
 * Reference: `mcp-server/plans/INSTRUKCE-faze-V0.A.5-tcp-listener.md`,
 * `debugger/rozbory/budoucnost/mcp-server/README.md` sekce 6.2.
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

#ifndef MZ800EMU_MCP_TCP_SERVER_H
#define MZ800EMU_MCP_TCP_SERVER_H

#include "../mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


    /**
     * @brief Defaultní TCP port pro MCP listener (loopback only).
     *
     * Hodnota 23800 je zvolena tak, aby kolidovala minimálně s jinými
     * běžnými službami a aby reflektovala doménu (MZ-800). V budoucnu
     * (V2+) bude konfigurovatelná přes INI; v V0.A.5 je hardcoded jako
     * default a CLI flag `--mcp-tcp-port=N` ji může přepsat.
     */
#define MCP_TCP_SERVER_DEFAULT_PORT  23800


    /**
     * @brief Opaque handle TCP server instance.
     *
     * Veškerý interní stav (listen socket, per-conn socket, vlákna,
     * mutexy, port, počítadlo klientů) je zapouzdřen v implementaci.
     * Caller pracuje výhradně přes API funkce.
     */
    typedef struct st_MCP_TCP_SERVER st_MCP_TCP_SERVER;


    /**
     * @brief Návratový kód operace startu TCP serveru.
     *
     * Rozlišujeme port collision (uživatelská chyba - jiný proces drží
     * port nebo druhá instance emu) od ostatních chyb (alokace, threading,
     * WinSock init), aby UI mohlo zobrazit smysluplnou hlášku.
     */
    typedef enum en_MCP_TCP_START_RESULT {
        /** @brief Úspěch - listener thread běží, server akceptuje spojení. */
        MCP_TCP_START_OK = 0,
        /** @brief Port je obsazený jiným procesem (EADDRINUSE / WSAEADDRINUSE). */
        MCP_TCP_START_PORT_IN_USE = -1,
        /** @brief Jiná chyba (WinSock init, socket(), listen(), thread spawn). */
        MCP_TCP_START_ERROR = -2,
        /** @brief Server už běží - opakovaný start bez stop. */
        MCP_TCP_START_ALREADY_RUNNING = -3,
    } en_MCP_TCP_START_RESULT;


    /**
     * @brief Vytvoří handle TCP serveru ve stavu "stopped".
     *
     * Pouze alokuje strukturu a inicializuje synchronizační primitiva.
     * Nezakládá socket, nevolá `WSAStartup`, nespouští thread - to dělá
     * teprve `mcp_tcp_server_start`.
     *
     * @return alokovaný handle nebo NULL při OOM (caller `_destroy`)
     *
     * @post is_running == false, client_count == 0, port == 0
     */
    st_MCP_TCP_SERVER *mcp_tcp_server_create(void);


    /**
     * @brief Spustí TCP listener na zadaném portu (loopback).
     *
     * Postup:
     *  1. Idempotentně inicializuje WinSock (`WSAStartup` jen poprvé).
     *  2. Vytvoří listen socket, nastaví SO_REUSEADDR.
     *  3. `bind(127.0.0.1:port)` + `listen(backlog=1)`.
     *  4. Spawnuje listener vlákno.
     *
     * Pokud bind selže s EADDRINUSE / WSAEADDRINUSE, vrátí
     * `MCP_TCP_START_PORT_IN_USE`. Ostatní chyby vrátí `MCP_TCP_START_ERROR`.
     * Opakované volání bez `_stop` vrátí `MCP_TCP_START_ALREADY_RUNNING`.
     *
     * @param srv  handle z `_create`
     * @param port TCP port (1-65535), typicky `MCP_TCP_SERVER_DEFAULT_PORT`
     * @param bind_addr bind adresa pro listen socket; NULL = "127.0.0.1"
     *                  (default loopback). Hodnota "0.0.0.0" otevírá
     *                  server na všech rozhraních (RISKY bez auth -
     *                  V0.B.4: žádná autentizace, V2+ TLS / token).
     *                  Parser používá inet_pton AF_INET; neznámé /
     *                  neparsovatelné řetězce vrátí MCP_TCP_START_ERROR.
     * @return kód z `en_MCP_TCP_START_RESULT`
     *
     * @pre srv != NULL, 1 <= port <= 65535
     * @post při OK je listener thread běžící a server je ve stavu
     *       "running"; při chybě je stav nezměněn (žádný únik socketů)
     *
     * @note Funkce je synchronní - vrací se až po úspěšném startu vlákna.
     */
    en_MCP_TCP_START_RESULT mcp_tcp_server_start(st_MCP_TCP_SERVER *srv,
                                                 int port,
                                                 const char *bind_addr);


    /**
     * @brief Graceful stop TCP listeneru.
     *
     * Postup:
     *  1. Nastaví stop flag (atomic, drženo pod mutexem).
     *  2. `shutdown(listen_sock, SD_BOTH)` + `closesocket` - unblokne
     *     `accept()` v listener vlákně (vrátí chybu, vlákno vyskočí).
     *  3. Pokud existuje aktivní per-conn vlákno, shutdownuje i jeho
     *     socket, vlákno vyskočí ze čtení.
     *  4. Joinne obě vlákna (deterministický cleanup).
     *
     * Idempotentní - opakované volání na zastavený server je no-op.
     *
     * @param srv handle z `_create`; NULL je tolerován (no-op)
     *
     * @post is_running == false, client_count == 0, listener_thread == NULL,
     *       client_thread == NULL
     *
     * @note Funkce je synchronní (čeká na join) - může trvat několik ms.
     */
    void mcp_tcp_server_stop(st_MCP_TCP_SERVER *srv);


    /**
     * @brief Uvolní handle - implicit `_stop` + destroy mutexů + free.
     *
     * Bezpečné volat na NULL.
     *
     * @param srv handle z `_create` nebo NULL
     */
    void mcp_tcp_server_destroy(st_MCP_TCP_SERVER *srv);


    /**
     * @brief Vrátí, zda listener thread aktuálně běží.
     *
     * @param srv handle z `_create`; NULL vrací false
     * @return true pokud server je v "running" stavu
     *
     * @note Read-only přístup pod mutexem, lze volat z libovolného vlákna
     *       (typicky GUI per-frame nebo title bar updater).
     */
    bool mcp_tcp_server_is_running(const st_MCP_TCP_SERVER *srv);


    /**
     * @brief Vrátí aktuální TCP port, na kterém server poslouchá.
     *
     * Vrací 0 pokud server neběží.
     *
     * @param srv handle z `_create`; NULL vrací 0
     */
    int mcp_tcp_server_get_port(const st_MCP_TCP_SERVER *srv);


    /**
     * @brief Vrátí počet aktuálně připojených klientů (0 nebo 1 v V0.A.5).
     *
     * V V0/V1 je `client_count` ∈ {0, 1}. V2+ bude rozšířeno na N.
     *
     * @param srv handle z `_create`; NULL vrací 0
     */
    int mcp_tcp_server_get_client_count(const st_MCP_TCP_SERVER *srv);


    /* ====================================================================== */
    /* Globální handle - pro UI integraci (Tools menu, title bar)             */
    /* ====================================================================== */

    /**
     * @brief Globální handle TCP serveru pro GUI mod.
     *
     * Inicializuje se lazy v Tools menu při prvním kliknutí na "Start"
     * (nebo eagerly v `main()` při `--mcp-tcp-port=N`). Destroy se v
     * `iface_exit` cestě.
     *
     * NULL znamená "server ještě nikdy nebyl použit". Po prvním `_create`
     * zůstává platný do `_destroy` v shutdown sekvenci. `is_running`
     * stav se mění start/stop voláními.
     */
    extern st_MCP_TCP_SERVER *g_mcp_tcp_server;


    /**
     * @brief Idempotentně uvolní `g_mcp_tcp_server` (= shutdown helper).
     *
     * Volá se z `iface_exit` cesty (přesněji z `imgui_exit` nebo
     * obdobného UI cleanupu). Bezpečné volat opakovaně.
     */
    void mcp_tcp_server_shutdown_global(void);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MZ800EMU_CFG_MCP_TCP_ENABLED */

#endif /* MZ800EMU_MCP_TCP_SERVER_H */
