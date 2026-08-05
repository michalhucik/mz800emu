/*
 * File:   main_pipe.c
 *
 * Vstupní bod pipe transportu MCP backendu. Spouští se buď přes
 * separátní binárku `mz800emu_pipe(.exe)` (= cíl V0.A.4 dle INSTRUKCE),
 * **nebo** přes klasické `mz800emu(.exe) --pipe` (= fallback z V0.A.4
 * kvůli problému s linkováním pipe binárky v MSYS2/UCRT64).
 *
 * Veřejná funkce `mcp_pipe_main(argc, argv)` provede stejnou inicializaci
 * jako `src/main.c::main` ale:
 *  - vždy s `--headless` injectnutým do argv,
 *  - bez ImGui Tools/UI menu Start logiky,
 *  - místo `sdlapp_run()` blokuje main thread v `g_cond_wait` do
 *    shutdown signálu (EOF / shutdown cmd / SIGINT),
 *  - před `g_cond_wait` zaregistruje signal handler (SIGINT / SIGTERM)
 *    a spawnuje stdin reader thread + pošle HELLO message.
 *
 * Architektura (V0.A.4):
 *  - **3 vlákna:**
 *      1. Main thread: provede emu init, pošle HELLO, spawnuje
 *         stdin reader, a poté blokuje v condition variable wait
 *         do okamžiku, kdy je signalizován shutdown (EOF / shutdown
 *         cmd / SIGINT).
 *      2. Stdin reader thread (`_stdin_reader_thread`): cyklicky čte
 *         JSONL řádky ze stdin, parsuje je, dispatchuje přes
 *         `mcp_dispatch_request` a zapisuje odpověď na stdout
 *         (chráněné `g_stdout_mutex`).
 *      3. Emulator thread (`emulator_thread` z `emulator.c`):
 *         standardní emu vlákno z hlavního codebase, řízené přes
 *         `g_sdlapp` (running flag) a `g_dbgapi_cmdrq_queue`
 *         (pause / run / reset / step / bp / memory přicházejí
 *         z dispatch handlerů přes `dbgapi_ui_submit_cmd_sync`).
 *
 *  - **Reuse stávající infrastruktury:** `--headless` mód SDL3
 *    backendu už existuje (V-1.1) - inject "--headless" do argv
 *    před `sdlapp_options_init`, takže `g_iface_video` a
 *    `g_iface_audio` se inicializují v no-op režimu (žádné okno,
 *    žádný audio device).
 *
 *  - **Shutdown sekvence:**
 *      EOF / SIGINT / shutdown cmd
 *        -> `_request_shutdown()` (= mutex + cond signal + g_shutdown
 *           flag + sdlapp_quit())
 *        -> stdin reader vyskočí ze smyčky
 *        -> main thread se probudí z `g_cond_wait`
 *        -> join stdin reader + emu thread + cleanup
 *
 *  - **Scope V0.A.4:**
 *      Žádný TCP listener (= V0.A.5), žádný EVENT broadcast (= V0.A.5+),
 *      žádný TRAP forwarding (= V0.A.5+). Pipe transport je 1:1
 *      request/response model s lokálním synchronním dispatchem.
 *
 * Reference design: `~/projects/ai2-mz800emu/mcp-emu/main_pipe.c`
 * (176 ř.). Tato implementace není byte-for-byte port - používá GLib
 * synchronizační primitiva a stávající mz800new `sdlapp` / `iface` /
 * `dbgapi` infrastrukturu.
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

#include "../mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>   /* dup, dup2, close, fileno - POSIX/MinGW (UCRT64) */
#include <glib.h>

#include "main.h"
#include "i18n.h"
#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "ui-imgui/debugger/dbgapi_dispatcher.h"
#include "iface/iface.h"
#include "emulator/cfgmain.h"
#include "emulator/debugger/dbgapi_emu.h"
#include "emulator/i18n_lang.h"
#include "emulator.h"

#include "jsonl_io.h"
#include "dispatch.h"
#include "cooperation.h"
#include "event_bus.h"
#include "trap_manager.h"

#include <SDL3/SDL.h>


/* ====================================================================== */
/* Globální stav pipe transportu                                           */
/* ====================================================================== */

/**
 * @brief Mutex chránící atomicitu zápisu na stdout.
 *
 * Cílem je, aby každý JSONL řádek (= response, event, hello) opouštěl
 * proces jako celý blok bez prokládání s jinými zápisy. Pro V0.A.4
 * existuje jen jeden producer (stdin reader thread + main thread při
 * hello), takže serialization je triviální, ale mutex je nutný pro
 * budoucí EVENT broadcast (= V0.A.5+).
 */
static GMutex g_stdout_mutex;

/**
 * @brief Vyhrazený výstupní stream pro JSONL protokol (duplikát
 *        původního stdout file descriptoru).
 *
 * MCP stdio (pipe) transport používá stdout jako vyhrazený kanál pro
 * newline-delimited JSON-RPC. Emulátor ale na řadě míst píše informační
 * hlášky přes printf (= na stdout) - INFO o konfiguraci (cfgmain.c),
 * klávesové hinty (emulator_print_hint), změny rychlosti apod. Aby se
 * tento plain text nepromíchal s JSONL a nerozbil protokol u klienta,
 * duplikujeme při startu pipe cesty původní stdout do tohoto streamu
 * (viz `_redirect_stdout_to_stderr`) a samotný stdout (fd 1)
 * přesměrujeme na stderr.
 *
 * NULL = redirect zatím neproběhl nebo selhal; v takovém případě
 * `_safe_stdout_write` použije přímo `stdout` (degradovaný fallback).
 *
 * PRAVIDLO PRO VÝVOJÁŘE:
 *  - Pro běžný výstup (INFO, warning, debug) používej `printf` / `g_print`
 *    jako kdekoliv jinde - po `_redirect_stdout_to_stderr` se tyto výpisy
 *    automaticky přesměrují na stderr (redirect je na úrovni fd 1, ne
 *    per-volání), takže JSONL stream nerozbijí.
 *  - Na `g_jsonl_out` NIKDY nepiš ručně - je vyhrazené výhradně pro JSONL
 *    protokol. Protokolové zprávy posílej jedině přes `_safe_stdout_write`.
 *  - Chybové/log hlášky klidně přes `fprintf(stderr, ...)` / `g_printerr`;
 *    jdou na stderr, který je log a není součástí protokolu.
 */
static FILE *g_jsonl_out = NULL;

/**
 * @brief Mutex + condition variable pro signalizaci shutdown.
 *
 * Main thread čeká v `g_cond_wait` na `g_shutdown_cond`. Pomocné
 * funkce (`_request_shutdown`, EOF cesta, SIGINT) drží lock,
 * nastaví `g_shutdown_requested = TRUE`, signalizují cond.
 */
static GMutex     g_shutdown_mutex;
static GCond      g_shutdown_cond;
static gboolean   g_shutdown_requested = FALSE;

/** @brief Handle stdin reader threadu - pro pozdější join. */
static GThread *g_stdin_thread = NULL;


/* ====================================================================== */
/* Helpery                                                                  */
/* ====================================================================== */

/**
 * @brief Idempotentně signalizuje shutdown.
 *
 * Nastaví flag, broadcastne cond (= probudí main thread), a zavolá
 * `sdlapp_quit(g_sdlapp)`, takže emu vlákno se vrátí z hlavní smyčky
 * při příští kontrole `sdlapp_is_running()`. Bezpečné volat opakovaně
 * z libovolného vlákna.
 */
static void _request_shutdown(void)
{
    g_mutex_lock(&g_shutdown_mutex);
    g_shutdown_requested = TRUE;
    g_cond_broadcast(&g_shutdown_cond);
    g_mutex_unlock(&g_shutdown_mutex);

    /* sdlapp_quit je interně thread-safe (NULL-safe). Bez něj by emu
     * vlákno pokračovalo v běhu i po skončení pipe smyčky. */
    sdlapp_quit(g_sdlapp);
}


/**
 * @brief Přesměruje proces stdout na stderr a vyhradí původní stdout
 *        pro JSONL protokol.
 *
 * Hned na začátku pipe cesty (před prvním printf z init kódu)
 * duplikuje původní stdout file descriptor do samostatného FILE*
 * (`g_jsonl_out`), který od té chvíle používá výhradně JSONL zápis,
 * a samotný stdout (fd 1) přesměruje na stderr (fd 2). Tím všechna
 * volání printf/puts/... (i z knihoven, i budoucí) skončí na stderr
 * a protokolový kanál (původní stdout) zůstane vyhrazen jen pro
 * newline-delimited JSON-RPC.
 *
 * @return TRUE při úspěchu (g_jsonl_out != NULL a fd 1 míří na stderr),
 *         FALSE pokud dup/fdopen selhal. Při FALSE zůstane g_jsonl_out
 *         NULL nebo neúplně nastaven a caller spoléhá na fallback
 *         v `_safe_stdout_write`.
 *
 * @pre  Voláno před jakýmkoliv zápisem na stdout v pipe cestě.
 * @post Při úspěchu je fd 1 (stdout) přesměrován na stderr a
 *       `g_jsonl_out` je ne-bufferovaný stream nad původním stdout.
 * @note POSIX/MinGW: dup/dup2/fileno; ověřeno dostupné na UCRT64.
 */
static gboolean _redirect_stdout_to_stderr(void)
{
    fflush(stdout);

    /* Duplikát původního stdout fd = vyhrazený protokolový kanál. */
    int proto_fd = dup(fileno(stdout));
    if (proto_fd < 0) return FALSE;

    g_jsonl_out = fdopen(proto_fd, "w");
    if (!g_jsonl_out)
    {
        close(proto_fd);
        return FALSE;
    }
    setvbuf(g_jsonl_out, NULL, _IONBF, 0);

    /* fd 1 (stdout) -> stderr: od teď printf teče na stderr. Když
     * selže, JSONL přesto funguje (přes g_jsonl_out), jen by se
     * printf dál mísil do stdout - hlásíme degradaci přes FALSE. */
    if (dup2(fileno(stderr), fileno(stdout)) < 0) return FALSE;

    return TRUE;
}


/**
 * @brief Thread-safe zápis JSONL řádku na protokolový kanál.
 *
 * Drží `g_stdout_mutex` po celou dobu volání `jsonl_write_line`
 * (které samo flushuje). Zapisuje na `g_jsonl_out` (vyhrazený duplikát
 * původního stdout); pokud redirect neproběhl, použije přímo `stdout`
 * jako fallback. Caller předává plně sestavený řádek bez zakončovacího
 * "\n" - jsonl_write_line ho doplní sám.
 */
static void _safe_stdout_write(const char *line)
{
    if (!line) return;
    FILE *out = g_jsonl_out ? g_jsonl_out : stdout;
    g_mutex_lock(&g_stdout_mutex);
    jsonl_write_line(out, line);
    g_mutex_unlock(&g_stdout_mutex);
}


/**
 * @brief Callback pro dispatch "shutdown" handler.
 *
 * Registrovaný přes `mcp_dispatch_set_shutdown_callback` před spawnem
 * stdin readeru. Zavolá se ze stdin reader vlákna po sestavení success
 * response, ale ještě před tím, než `_stdin_reader_thread` stihne
 * response zapsat na stdout - takže klient ji ještě dostane.
 */
static void _on_shutdown_command(void)
{
    _request_shutdown();
}


/**
 * @brief POSIX signal handler pro SIGINT (Ctrl+C) a SIGTERM.
 *
 * Pouze signalizuje shutdown. Volání `g_mutex_lock` v signal handleru
 * není dle POSIX strictly safe, ale GLib mutexy se v praxi chovají
 * jako pthread_mutex a Windows CRT signal handlery běží ve vlastním
 * threadu - viz dokumentace GLib.
 */
static void _signal_handler(int signum)
{
    (void)signum;
    _request_shutdown();
}


/* ====================================================================== */
/* Stdin reader vlákno                                                      */
/* ====================================================================== */

/**
 * @brief Hlavní smyčka stdin reader vlákna.
 *
 * Čte JSONL řádek po řádku z `stdin`. Pro každý:
 *   - parsuje přes `jsonl_parse_line`,
 *   - pokud zpráva je REQUEST, dispatchuje přes `mcp_dispatch_request`,
 *   - serializuje odpověď na stdout chráněnou `g_stdout_mutex`,
 *   - free / cleanup.
 *
 * Loop se ukončí na:
 *   - EOF na stdin (klient zavřel pipe) -> `_request_shutdown`,
 *   - explicit `g_shutdown_requested == TRUE` (externí signal nebo
 *     shutdown command).
 *
 * @param data nevyužito
 * @return NULL
 */
static gpointer _stdin_reader_thread(gpointer data)
{
    (void)data;

    while (TRUE)
    {
        /* Rychlá kontrola - pokud někdo jiný (signal, shutdown cmd)
         * vyhodil flag, neblokuj na readu. */
        g_mutex_lock(&g_shutdown_mutex);
        gboolean done = g_shutdown_requested;
        g_mutex_unlock(&g_shutdown_mutex);
        if (done) break;

        char *line = NULL;
        en_JSONL_RESULT rr = jsonl_read_line(stdin, &line);
        if (rr == JSONL_ERR_EOF)
        {
            /* Klient zavřel pipe - graceful exit. */
            _request_shutdown();
            break;
        }
        if (rr != JSONL_OK)
        {
            /* I/O error - log a pokus o pokračování. */
            fprintf(stderr, "[mcp-pipe] stdin read error (rr=%d)\n", rr);
            if (line) g_free(line);
            /* Při hard error stdin readu raději ukončíme. */
            _request_shutdown();
            break;
        }

        /* Prázdné řádky (jen "\n") přeskočíme. */
        if (!line || line[0] == '\0')
        {
            if (line) g_free(line);
            continue;
        }

        st_JSONL_MESSAGE *msg = NULL;
        en_JSONL_RESULT pr = jsonl_parse_line(line, &msg);
        if (pr != JSONL_OK || !msg)
        {
            fprintf(stderr,
                    "[mcp-pipe] JSONL parse error (pr=%d) - line ignored\n",
                    pr);
            if (msg) jsonl_msg_free(msg);
            g_free(line);
            continue;
        }

        char *response = NULL;
        en_MCP_DISPATCH_RESULT dr = mcp_dispatch_request(msg, &response);

        if (response)
        {
            _safe_stdout_write(response);
            free(response);
        }
        else
        {
            /* MCP_DISPATCH_NOT_A_REQUEST / ALLOC_ERROR -> žádná
             * response. NOT_A_REQUEST není wire-level chyba klienta
             * (např. server-side message v opačném směru), takže jen
             * log. */
            if (dr != MCP_DISPATCH_NOT_A_REQUEST)
            {
                fprintf(stderr,
                        "[mcp-pipe] dispatch produced no response (dr=%d)\n",
                        dr);
            }
        }

        jsonl_msg_free(msg);
        g_free(line);
    }

    return NULL;
}


/* ====================================================================== */
/* CLI - whitelist                                                         */
/* ====================================================================== */

/**
 * @brief Whitelist CLI options pro mz800emu_pipe.
 *
 * Záměrně minimální - pipe binárka je spouštěná jako subprocess
 * MCP wrapperem a uživatelská CLI prakticky nemá smysl. Necháváme:
 *  - --help (=  vypsat options a skončit)
 *  - --headless (= rozpoznat jako standardní flag; pipe ho stejně
 *    interně injektuje, ale duplicita nevadí)
 *  - --home-dir / --cfg-dir / --work-dir (= override paths pro
 *    asset directories - klíčové pro CI / batch runtime)
 *  - --config (= explicit cesta k INI)
 *  - --no-save-ini / --no-first-run-windows (= chování v CI)
 *
 * Ostatní vyhradíme pro standardní `mz800emu.exe`.
 */
static const st_SDLAPP_OPTION_DEF g_pipe_known_options[] = {
    { "--help",                 SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,   NULL, NULL,
      "Print this option list and exit." },
    { "--mcp-pipe",             SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,   NULL, NULL,
      "Switch to MCP pipe transport (already detected in main.c; harmless "
      "to re-validate inside mcp_pipe_main)." },
    { "--headless",             SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,   NULL, NULL,
      "Force headless mode (no SDL window, no audio device). Pipe binary "
      "injects this automatically; explicit flag is also accepted." },
    { "--home-dir",             SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING, NULL, "<dirpath>",
      "Override the home directory (where the binary's assets live)." },
    { "--cfg-dir",              SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING, NULL, "<dirpath>",
      "Override the config directory (.ini files, breakpoints, ramdisks)." },
    { "--work-dir",             SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING, NULL, "<dirpath>",
      "Override the work directory (default open path for file dialogs)." },
    { "--config",               SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING, NULL, "<filepath>",
      "Path to the main config file (mz<arch>emu.ini)." },
    { "--no-save-ini",          SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,   NULL, NULL,
      "Do not write the .ini file at exit." },
    { "--no-first-run-windows", SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,   NULL, NULL,
      "Suppress About + Version Check windows on first run." },
    { NULL, SDLAPP_OPTION_FLAG, SDLAPP_OPTVAL_NONE, NULL, NULL, NULL }
};


/* ====================================================================== */
/* main                                                                     */
/* ====================================================================== */

/**
 * @brief Sestaví augmented argv s injectnutým `--headless`.
 *
 * Pipe binárka je vždy headless. Vrácený argv má alokovaný array
 * pointerů (caller `g_free`-uje samotný array po dokončení init).
 * Stringy uvnitř ukazují buď na původní argv tokeny, nebo na
 * statický literál "--headless" - žádný cleanup uvnitř.
 *
 * @param[in]  in_argc  původní argc
 * @param[in]  in_argv  původní argv
 * @param[out] out_argc nové argc (in_argc + 1 nebo in_argc pokud
 *                      už --headless je přítomný)
 * @return nově alokované pole pointerů (caller `g_free`)
 */
static char **_inject_headless_flag(int in_argc, char **in_argv,
                                    int *out_argc)
{
    /* Nejprve scan, jestli --headless není už v argv. */
    gboolean already = FALSE;
    for (int i = 1; i < in_argc; i++)
    {
        if (in_argv[i] && strcmp(in_argv[i], "--headless") == 0)
        {
            already = TRUE;
            break;
        }
    }

    int n = in_argc + (already ? 0 : 1);
    char **out = g_new0(char *, (gsize)n + 1);
    for (int i = 0; i < in_argc; i++) out[i] = in_argv[i];
    if (!already) out[in_argc] = (char *)"--headless";
    out[n] = NULL;
    *out_argc = n;
    return out;
}


/**
 * @brief Hlavní entry-point pipe transportu.
 *
 * Volá se buď z `main()` v `src/main.c` při detekci `--pipe` flagu
 * (= aktuální cesta pro V0.A.4 jako fallback) nebo přímo z `main()`
 * separátní binárky `mz800emu_pipe.exe` (= cíl plánu, ale linkování
 * separátní binárky se v MSYS2/UCRT64 zatím nepodařilo).
 *
 * Provede stejnou základní inicializaci jako `src/main.c::main` ale:
 *   - vždy s `--headless` injectnutým do argv,
 *   - bez ImGui Tools/UI menu Start logiky (nemá smysl bez GUI),
 *   - místo `sdlapp_run()` blokuje main thread v `g_cond_wait` do
 *     shutdown signálu,
 *   - před `g_cond_wait` zaregistruje signal handler (SIGINT/SIGTERM)
 *     a spawnuje stdin reader vlákno + pošle HELLO message.
 *
 * @param argc  počet argumentů (předaných z `main`)
 * @param argv  pole argumentů (předaných z `main`)
 * @return EXIT_SUCCESS pokud shutdown proběhl normálně,
 *         EXIT_FAILURE při chybě inicializace.
 */
int mcp_pipe_main(int argc, char *argv[])
{
    /* ---- 1) Inject --headless do argv ---- */
    int aug_argc = 0;
    char **aug_argv = _inject_headless_flag(argc, argv, &aug_argc);

    sdlapp_options_init(aug_argc, aug_argv);

    if (sdlapp_option_present("--help"))
    {
        sdlapp_options_print_help(aug_argv[0], g_pipe_known_options);
        g_free(aug_argv);
        return EXIT_SUCCESS;
    }

    if (!sdlapp_options_validate(g_pipe_known_options))
    {
        g_free(aug_argv);
        return EXIT_FAILURE;
    }

    /* JSONL protokol vyžaduje vyhrazený stdout. Přesměruj veškerý
     * běžný výstup emulátoru (printf z cfgmain_init, emulator_print_hint,
     * hlášky o rychlosti, ...) na stderr a původní stdout si ponech jen
     * pro JSONL. MUSÍ proběhnout PŘED cfgmain_init a dalším init kódem,
     * který píše přes printf, jinak by se plain text promíchal s JSONL. */
    if (!_redirect_stdout_to_stderr())
    {
        fprintf(stderr,
                "[mcp-pipe] warning: stdout redirect failed - emulator log "
                "output may corrupt the JSONL stream\n");
    }

    /* stdout/stderr ne-bufferovaně - každý JSONL řádek (přes g_jsonl_out,
     * to je už _IONBF) i každá log hláška musí být okamžitě viditelná. */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    fprintf(stderr,
            "[mcp-pipe] starting mz800emu_pipe (headless JSONL transport)\n");

    /* ---- 2) Init synchronizačních primitiv ---- */
    g_mutex_init(&g_stdout_mutex);
    g_mutex_init(&g_shutdown_mutex);
    g_cond_init(&g_shutdown_cond);

    /* ---- 3) Standardní emu init (analog main.c) ---- */
    g_sdlapp = sdlapp_new();
    if (!g_sdlapp)
    {
        fprintf(stderr, "[mcp-pipe] sdlapp_new failed\n");
        g_free(aug_argv);
        return EXIT_FAILURE;
    }

    i18n_init();
    cfgmain_init();
    i18n_lang_apply();

    if (!iface_init())
    {
        fprintf(stderr, "[mcp-pipe] iface_init failed\n");
        sdlapp_destroy(g_sdlapp);
        g_free(aug_argv);
        return EXIT_FAILURE;
    }

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* dbgapi musí být před emu_thread spawnem - viz main.c. MCP
     * dispatch handlery se opírají o g_dbgapi_cmdrq_queue. */
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    dbgapi_dispatcher_init();
#endif

    /* ---- 4) Spawnem emulator vlákna ---- */
    GThread *emu_thread = g_thread_new("mz-800-emulator",
                                       emulator_thread, NULL);
    if (!emu_thread)
    {
        fprintf(stderr, "[mcp-pipe] failed to spawn emulator thread\n");
        iface_exit();
        sdlapp_destroy(g_sdlapp);
        g_free(aug_argv);
        return EXIT_FAILURE;
    }

    /* ---- 5) Dispatch init + registrace shutdown callbacku +
     *        signal handlerů ----
     *
     * V1.B.3: mcp_dispatch_init alokuje dynamický supported_commands
     * z g_cmd_map[] (= jeden zdroj pravdy pro hello payload).
     * Transport kind se nastavuje hned po init, aby ho dispatch
     * handlery (např. emu_stop) mohly číst.
     */
    mcp_dispatch_init();
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_PIPE);
    mcp_dispatch_set_shutdown_callback(_on_shutdown_command);

    /* V1.A.1: reset cooperation hintu před prvním requestem - zaručí,
     * že nová pipe session startuje s mode=FREE i kdyby předchozí emu
     * stav (= reload v testech) zanechal slot ve "read_only" módu. */
    cooperation_hint_init();

    /* V1.A.4: EVENT bus + TRAP manager init. Event bus emit hooky v
     * BP enforce + dbgapi CMD_PAUSE potřebují bus inicializovaný před
     * tím, než emu vlákno začne. Init je idempotentní, takže opakované
     * volání (= multiple pipe sessions) je benigní. */
    event_bus_init();
    trap_manager_init();

    signal(SIGINT,  _signal_handler);
    signal(SIGTERM, _signal_handler);

    /* ---- 6) Pošleme HELLO message klientovi ---- */
    char *hello = NULL;
    if (mcp_dispatch_build_hello(&hello) == MCP_DISPATCH_OK && hello)
    {
        _safe_stdout_write(hello);
        free(hello);
    }
    else
    {
        fprintf(stderr, "[mcp-pipe] failed to build hello message\n");
    }

    /* ---- 7) Spawnem stdin reader vlákna ---- */
    g_stdin_thread = g_thread_new("mcp-stdin-reader",
                                  _stdin_reader_thread, NULL);
    if (!g_stdin_thread)
    {
        fprintf(stderr, "[mcp-pipe] failed to spawn stdin reader thread\n");
        _request_shutdown();
    }

    /* ---- 8) Main thread spustí sdlapp_run (no-op event loop) ----
     *
     * Architektonická poznámka: hlavní emulator_thread() z
     * src/emulator/emulator.c má **dva** předpoklady navázané na
     * sdlapp state:
     *  a) `while ((!is_initialized) && (!sdlapp_is_running))` -
     *     synchronizace startupu (čeká dokud sdlapp_run() neflagne
     *     `app->running=TRUE`),
     *  b) hlavní `mzarch_main()` smyčka se ukončí přes sdlapp_quit()
     *     (= request shutdown).
     *
     * V GUI módu je sdlapp_run() volán z main(); v pipe módu ho
     * voláme zde my. V headless módu je smyčka prakticky no-op
     * (žádná okna -> manager->process_render = no-op, žádné SDL
     * eventy v normálním provozu).
     *
     * Jakmile `_request_shutdown()` zavolá `sdlapp_quit()`, smyčka
     * vyskočí, my pokračujeme cleanupem a emu thread se ukončí přes
     * standardní mzarch quit cestu.
     *
     * Stdin reader thread běží paralelně a posílá příkazy přes
     * dispatch -> dbgapi -> emu thread, takže I/O není zablokované.
     */
    sdlapp_run(g_sdlapp);

    /* Pojistka: pokud sdlapp_run vyskočil z jiné příčiny (např. SDL
     * QUIT event), explicitně signalujeme shutdown ostatním vláknům. */
    _request_shutdown();

    fprintf(stderr, "[mcp-pipe] shutdown requested - joining threads\n");

    /* ---- 9) Cleanup ---- */
    /* Stdin reader vlákno se ukončí samo (po EOF / flag check). */
    if (g_stdin_thread)
    {
        g_thread_join(g_stdin_thread);
        g_stdin_thread = NULL;
    }

    /* Odregistruj shutdown callback (= žádný další request nesmí
     * volat _on_shutdown_command po join). */
    mcp_dispatch_set_shutdown_callback(NULL);
    mcp_dispatch_set_transport_kind(MCP_DISPATCH_TRANSPORT_NONE);
    mcp_dispatch_shutdown();

    /* Emu thread - sdlapp_quit byl už zavolán v _request_shutdown,
     * takže emulator_thread by měl skončit při příští kontrole
     * sdlapp_is_running. */
    gpointer emuret = g_thread_join(emu_thread);
    int emuretval = emuret ? *(int *)emuret : EXIT_FAILURE;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    dbgapi_dispatcher_shutdown();
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
#endif

    /* V1.A.4: cleanup event bus + trap manager. Po shutdown už emu
     * vlákno neběží, takže žádný emit z BP hooku nemůže nastat. */
    trap_manager_shutdown();
    event_bus_shutdown();

    /* Threading kontrakt (mzhal krok 12): teardown subsystémů až po
     * joinu emu vlákna a shutdownu MCP/dbgapi - viz emulator.h. */
    if (emuretval == EXIT_SUCCESS)
        emulator_teardown();

    iface_exit();
    sdlapp_destroy(g_sdlapp);
    SDL_Quit();

    g_cond_clear(&g_shutdown_cond);
    g_mutex_clear(&g_shutdown_mutex);
    g_mutex_clear(&g_stdout_mutex);

    /* Zavři vyhrazený JSONL stream (= duplikát původního stdout).
     * Po join vláken už nikdo nezapisuje, takže je to bezpečné. */
    if (g_jsonl_out)
    {
        fclose(g_jsonl_out);
        g_jsonl_out = NULL;
    }

    g_free(aug_argv);

    fprintf(stderr, "[mcp-pipe] exit value: %d\n", emuretval);
    return emuretval == EXIT_FAILURE ? EXIT_FAILURE : EXIT_SUCCESS;
}

#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
