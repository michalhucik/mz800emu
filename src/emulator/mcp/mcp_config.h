/*
 * File:   mcp_config.h
 *
 * Konfigurace MCP backendu - INI persistence + globální stav (V0.B.4).
 *
 * Modul drží runtime konfiguraci MCP TCP serveru (port, bind adresa,
 * security profile, auto-start flag). Konfigurace se serializuje do
 * sekce [MCP] hlavního INI souboru přes cfgmain modulární mechanismus
 * (= shodný pattern jako snapshot_config_init / i18n_lang_config_init).
 *
 * Scope V0.B.4:
 *  - 4 INI klíče: tcp_port, bind_address, profile, auto_start_tcp
 *  - Security profile se pouze ukládá; plné enforcement (file whitelist,
 *    capability gating, tool restrictions) přijde ve fázi V1.A.
 *  - Bind adresa je dvouhodnotová enum (127.0.0.1 / 0.0.0.0); volné
 *    zadávání IPv4 je out of scope V0.B.4.
 *
 * Pri buildu s NO_MCP_TCP=1 (kaskáda NO_MCP=1) se modul stane
 * prázdnou translation unit a g_mcp_config je nedostupná.
 *
 * Reference: mcp-server/plans/INSTRUKCE-faze-V0.B.4-ini-config.md,
 *            debugger/rozbory/budoucnost/mcp-server/README.md sekce 7.3.
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

#ifndef MZ800EMU_MCP_CONFIG_H
#define MZ800EMU_MCP_CONFIG_H

#include "../mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


    /**
     * @brief Bezpečnostní profil MCP serveru.
     *
     * V0.B.4 ukládá jen vybranou hodnotu; reálné vynucení (whitelist,
     * capability gating, restrikce nástrojů, audit log) přijde v V1.A.
     * GUI dialog na to upozorňuje warning textem.
     */
    typedef enum en_MCP_PROFILE {
        MCP_PROFILE_WILD = 0,       /**< Žádné restrikce, dev výchozí. */
        MCP_PROFILE_CONFINED,       /**< Workspace dir write-only (V1.A). */
        MCP_PROFILE_SANDBOXED,      /**< Whitelist + auth (V1.A). */
        MCP_PROFILE_OBSERVER,       /**< Read-only Resources, žádné Tools (V1.A). */
        MCP_PROFILE__COUNT          /**< Sentinel pro počet hodnot. */
    } en_MCP_PROFILE;


    /**
     * @brief Úroveň logování Python MCP wrapperu (`mcp_server.py`).
     *
     * Wrapper čte tuto hodnotu při startu z `[MCP]` sekce INI.
     * `OFF` znamená že wrapper nainstaluje NullHandler a nepíše
     * žádný log soubor (= default; před commit-em 37e1b34 byl
     * hardcoded INFO).
     *
     * Hodnoty se mapují na Python `logging` konstanty:
     *  - OFF      -> NullHandler, žádný log
     *  - ERROR    -> logging.ERROR
     *  - WARNING  -> logging.WARNING
     *  - INFO     -> logging.INFO
     *  - DEBUG    -> logging.DEBUG
     */
    typedef enum en_MCP_WRAPPER_LOG_LEVEL {
        MCP_WRAPPER_LOG_LEVEL_OFF = 0,    /**< Log vypnutý (default). */
        MCP_WRAPPER_LOG_LEVEL_ERROR,      /**< Jen ERROR. */
        MCP_WRAPPER_LOG_LEVEL_WARNING,    /**< WARNING + ERROR. */
        MCP_WRAPPER_LOG_LEVEL_INFO,       /**< INFO + WARNING + ERROR. */
        MCP_WRAPPER_LOG_LEVEL_DEBUG,      /**< DEBUG + výše. */
        MCP_WRAPPER_LOG_LEVEL__COUNT      /**< Sentinel. */
    } en_MCP_WRAPPER_LOG_LEVEL;


    /**
     * @brief Druh rotace Python wrapper logu.
     *
     * Mapuje se na Python `logging.handlers`:
     *  - NONE  -> FileHandler (append-only single file)
     *  - SIZE  -> RotatingFileHandler (maxBytes)
     *  - TIME  -> TimedRotatingFileHandler (when)
     */
    typedef enum en_MCP_WRAPPER_LOG_ROTATE_KIND {
        MCP_WRAPPER_LOG_ROTATE_NONE = 0,  /**< Žádná rotace (default). */
        MCP_WRAPPER_LOG_ROTATE_SIZE,      /**< Rotace po dosažení velikosti. */
        MCP_WRAPPER_LOG_ROTATE_TIME,      /**< Časová rotace. */
        MCP_WRAPPER_LOG_ROTATE_KIND__COUNT /**< Sentinel. */
    } en_MCP_WRAPPER_LOG_ROTATE_KIND;


    /**
     * @brief Volba bind adresy TCP listeneru.
     *
     * V V0.B.4 podporujeme dvě hodnoty: loopback (default, bezpečné)
     * a wildcard (= remote access, riskantní bez auth). Volné zadávání
     * IPv4 / IPv6 / hostname je out of scope V0.B.4.
     */
    typedef enum en_MCP_BIND_ADDR {
        MCP_BIND_LOCALHOST = 0,     /**< 127.0.0.1 (default). */
        MCP_BIND_ALL_INTERFACES,    /**< 0.0.0.0 (remote, žádný auth - RISKY). */
        MCP_BIND__COUNT             /**< Sentinel. */
    } en_MCP_BIND_ADDR;


    /**
     * @brief Runtime konfigurace MCP serveru.
     *
     * Hodnoty se načítají z INI při startu (cfgmain_init) a ukládají
     * při shutdown (cfgmain_exit). GUI dialog (Tools -> MCP TCP Server
     * -> Settings...) edituje tuto strukturu a volá cfgroot_save pro
     * okamžitý persist.
     *
     * Invariant: tcp_port v rozsahu 1024-65535. bind_addr i profile
     * patří do svých enum rozsahů (validuje cfgmain propagate
     * callback). auto_start_tcp je čistě bool.
     */
    typedef struct st_MCP_CONFIG {
        int               tcp_port;        /**< Port (1024-65535), default 23800. */
        en_MCP_BIND_ADDR  bind_addr;       /**< Bind adresa, default LOCALHOST. */
        en_MCP_PROFILE    profile;         /**< Security profile, default WILD. */
        bool              auto_start_tcp;  /**< Auto-start při startu emu, default false. */

        /**
         * @brief V1.D.2.C - cesta k disk persist Activity log souboru.
         *
         * Pokud non-NULL a non-empty, `mcp_activity_window_log_action`
         * appenduje ISO 8601 formatted entry za každý MCP/USER příkaz.
         * Default NULL (= disk persist vypnutý; jen RAM ring zůstává).
         *
         * Ownership: heap-alokovaný string (`g_strdup`), uvolňuje se
         * v `mcp_config_init` při re-load. Caller čte přes
         * `mcp_config_get_action_log_path()`.
         */
        char             *action_log_path;

        /**
         * @brief Úroveň logování Python wrapperu (default OFF).
         *
         * Hodnotu interpretuje výhradně `mcp-server/mcp_server.py`
         * při svém startu (čte INI). C MCP backend ji jen perzistuje;
         * vlastní log Pythonu C kód neřídí. GUI Settings dialog
         * edituje tuto hodnotu.
         */
        en_MCP_WRAPPER_LOG_LEVEL wrapper_log_level;

        /**
         * @brief Cesta k log souboru Python wrapperu.
         *
         * Empty / NULL => Python wrapper použije default
         * `<dir mcp_server.py>/mcp_server.log` (pokud
         * `wrapper_log_level != OFF`).
         * Doporučená absolutní cesta (cwd Pythonu může záviset
         * na spawn způsobu Claude Code).
         *
         * Ownership: heap-alokovaný string (g_strdup), uvolňuje se
         * při re-propagate.
         */
        char             *wrapper_log_path;

        /**
         * @brief Druh rotace log souboru (default NONE).
         */
        en_MCP_WRAPPER_LOG_ROTATE_KIND wrapper_log_rotate_kind;

        /**
         * @brief Velikost v MB před rotací (default 10).
         *
         * Použito pouze pokud `wrapper_log_rotate_kind == SIZE`.
         * Rozsah 1..10240 MB.
         */
        int               wrapper_log_rotate_size_mb;

        /**
         * @brief Token `when` pro TimedRotatingFileHandler (default "midnight").
         *
         * Použito pouze pokud `wrapper_log_rotate_kind == TIME`.
         * Akceptované hodnoty per Python dokumentace:
         * S, M, H, D, midnight, W0..W6. Wrapper toleruje neznámé
         * hodnoty pádem na FileHandler bez rotace.
         *
         * Ownership: heap-alokovaný string (g_strdup).
         */
        char             *wrapper_log_rotate_when;

        /**
         * @brief Počet historických rotovaných souborů (default 5).
         *
         * Mapuje se na `backupCount` u Rotating/TimedRotating handlerů.
         * Rozsah 0..1000.
         */
        int               wrapper_log_rotate_keep;
    } st_MCP_CONFIG;


    /**
     * @brief Globální runtime konfigurace MCP.
     *
     * Inicializuje se v mcp_config_init() na default hodnoty; INI
     * propagate je přepíše. CLI flag overrides (--mcp-tcp-port,
     * --mcp-bind, --mcp-profile) se aplikují v main.c po cfgmain_init.
     */
    extern st_MCP_CONFIG g_mcp_config;


    /**
     * @brief Registruje MCP konfigurační modul do cfgmain.
     *
     * Volat z cfgmain_init() po cfgroot_new. Vytvoří sekci [MCP]
     * s propagate/save callbacky pro každý ze 4 INI klíčů. Při
     * absentní sekci [MCP] v INI zůstanou default hodnoty.
     *
     * Pre: g_cfgmain != NULL (cfgroot_new už proběhl).
     */
    void mcp_config_init(void);


    /**
     * @brief Vrátí textovou reprezentaci bind adresy pro inet_pton.
     *
     * @param ba Hodnota en_MCP_BIND_ADDR.
     * @return "127.0.0.1" nebo "0.0.0.0"; nikdy NULL.
     */
    const char *mcp_config_bind_addr_str(en_MCP_BIND_ADDR ba);


    /**
     * @brief Vrátí textovou reprezentaci profile pro INI / log.
     *
     * @param p Hodnota en_MCP_PROFILE.
     * @return "wild" / "confined" / "sandboxed" / "observer"; nikdy NULL.
     */
    const char *mcp_config_profile_str(en_MCP_PROFILE p);


    /**
     * @brief Parsuje profile string z CLI / INI.
     *
     * @param s Vstupní řetězec (case-sensitive: "wild", "confined",
     *          "sandboxed", "observer"); NULL nebo neznámá hodnota = WILD.
     * @return Odpovídající en_MCP_PROFILE (default WILD při neznámém).
     */
    en_MCP_PROFILE mcp_config_profile_from_str(const char *s);


    /**
     * @brief Parsuje bind adresu z CLI string.
     *
     * @param s "127.0.0.1" nebo "localhost" -> LOCALHOST; "0.0.0.0"
     *          nebo "*" -> ALL_INTERFACES; NULL / neznámé -> LOCALHOST.
     * @return Odpovídající en_MCP_BIND_ADDR.
     */
    en_MCP_BIND_ADDR mcp_config_bind_addr_from_str(const char *s);


    /**
     * @brief V1.D.2.C - Vrátí cestu k Activity log souboru.
     *
     * @return Heap pointer (NULL = persist vypnutý). Caller nesmí
     *         modifikovat ani g_free; lifetime do dalšího cfg propagate.
     */
    const char *mcp_config_get_action_log_path(void);


    /**
     * @brief Vrátí textovou reprezentaci wrapper log levelu (pro INI / log).
     *
     * @param l Hodnota en_MCP_WRAPPER_LOG_LEVEL.
     * @return Lower-case identifier: "off" / "error" / "warning" /
     *         "info" / "debug"; nikdy NULL.
     */
    const char *mcp_config_wrapper_log_level_str(en_MCP_WRAPPER_LOG_LEVEL l);


    /**
     * @brief Vrátí textovou reprezentaci wrapper rotate kind.
     *
     * @param k Hodnota en_MCP_WRAPPER_LOG_ROTATE_KIND.
     * @return "none" / "size" / "time"; nikdy NULL.
     */
    const char *mcp_config_wrapper_log_rotate_kind_str(en_MCP_WRAPPER_LOG_ROTATE_KIND k);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MZ800EMU_CFG_MCP_TCP_ENABLED */

#endif /* MZ800EMU_MCP_CONFIG_H */
