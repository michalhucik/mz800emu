/*
 * File:   dispatch_runner.c
 *
 * Běhový obal MCP dispatch s omezenou dobou odpovědi (viz
 * dispatch_runner.h).
 *
 * Proč to existuje: synchronní submit do dbgapi nesmí opustit příkaz,
 * který emu vlákno už převzalo - emu během dispatch zapisuje do dat na
 * zásobníku odesílatele. Dříve tím odesílatelem bylo přímo transportní
 * vlákno (mcp-stdin-reader, TCP klient), takže zaseknuté emu vlákno
 * zablokovalo celé MCP bez odpovědi. Teď odesílá pracovní vlákno
 * z GThreadPool; transport čeká jen do ohlášení zaseknutí a pak odpoví
 * chybou "Emulator busy: ... still running". Pracovní vlákno se zásobníkem
 * zůstane zaparkované, dokud emu příkaz nedokončí, a pak výsledek zahodí.
 *
 * Vlastnictví a životnost jobu (st_MCP_RUNNER_JOB):
 *  - vytvoří ho transport, refcount = 2 (transport + pracovní vlákno),
 *  - požadavek (req) vlastní job, uvolní ho pracovní vlákno po dispatch,
 *  - odpověď převezme transport, nebo ji po opuštění uvolní pracovní vlákno,
 *  - job uvolní ten, kdo pustí poslední referenci.
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

#include "../mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED

#include "dispatch_runner.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>


/**
 * @brief Jeden požadavek předaný pracovnímu vláknu.
 *
 * Členy chráněné `mutex`: done, stalled, abandoned, response, result.
 *
 * Invarianty:
 *  - `abandoned` se nastavuje jen transportem a jen když `stalled`
 *    && !`done`; po něm transport na job už nesahá (kromě unref),
 *  - `response` po `done` vlastní transport, je-li !`abandoned`;
 *    jinak ho uvolní pracovní vlákno,
 *  - `refcount` klesá na 0 přesně jednou; tím se job uvolní.
 */
typedef struct st_MCP_RUNNER_JOB {
    GMutex mutex;                   /**< Zámek stavových členů. */
    GCond cond;                     /**< Signál done / stalled pro transport. */
    gint refcount;                  /**< Počet vlastníků (atomicky). */
    st_JSONL_MESSAGE *req;          /**< Požadavek (vlastní job). */
    char *response;                 /**< Odpověď dispatch (malloc). */
    en_MCP_DISPATCH_RESULT result;  /**< Návratový kód dispatch. */
    bool done;                      /**< Dispatch skončil. */
    bool stalled;                   /**< Dispatch ohlásil zaseknutí. */
    bool abandoned;                 /**< Transport už klientovi odpověděl. */
} st_MCP_RUNNER_JOB;


/** @brief Fond pracovních vláken (vzniká líně, žije do konce procesu). */
static GThreadPool *s_pool = NULL;

/** @brief Inicializační guard pro s_pool. */
static gsize s_pool_init = 0;

/** @brief Počet jobů, jejichž pracovní vlákno ještě neskončilo (atomicky). */
static gint s_inflight = 0;


/**
 * @brief Pustí jednu referenci jobu; poslední ho uvolní.
 * @param job Job (nesmí být NULL).
 */
static void _job_unref(st_MCP_RUNNER_JOB *job) {
    if (!g_atomic_int_dec_and_test(&job->refcount)) return;
    g_mutex_clear(&job->mutex);
    g_cond_clear(&job->cond);
    g_free(job);
}


/**
 * @brief Callback hlídání zaseknutí - probudí transport.
 *
 * Volá ho dbgapi_ui_submit_cmd_sync_watched() z pracovního vlákna, bez
 * zámků dbgapi. Neblokuje.
 *
 * @param user_data st_MCP_RUNNER_JOB*
 */
static void _job_on_stall(void *user_data) {
    st_MCP_RUNNER_JOB *job = (st_MCP_RUNNER_JOB *)user_data;
    g_mutex_lock(&job->mutex);
    job->stalled = true;
    g_cond_signal(&job->cond);
    g_mutex_unlock(&job->mutex);
}


/**
 * @brief Callback hlídání zaseknutí - je požadavek už opuštěný?
 * @param user_data st_MCP_RUNNER_JOB*
 * @return true, pokud transport klientovi už odpověděl "busy".
 */
static bool _job_is_abandoned(void *user_data) {
    st_MCP_RUNNER_JOB *job = (st_MCP_RUNNER_JOB *)user_data;
    g_mutex_lock(&job->mutex);
    bool a = job->abandoned;
    g_mutex_unlock(&job->mutex);
    return a;
}


/**
 * @brief Tělo pracovního vlákna: provede dispatch jednoho jobu.
 *
 * Po dokončení předá odpověď transportu, nebo ji (u opuštěného jobu)
 * zahodí. Vždy uvolní požadavek a svou referenci jobu.
 *
 * @param data      st_MCP_RUNNER_JOB*
 * @param user_data nevyužito
 */
static void _worker(gpointer data, gpointer user_data) {
    (void)user_data;
    st_MCP_RUNNER_JOB *job = (st_MCP_RUNNER_JOB *)data;

    st_MCP_DISPATCH_STALL_WATCH watch = {
        .on_stall = _job_on_stall,
        .is_abandoned = _job_is_abandoned,
        .user_data = job,
    };
    char *response = NULL;
    mcp_dispatch_set_thread_stall_watch(&watch);
    en_MCP_DISPATCH_RESULT r = mcp_dispatch_request(job->req, &response);
    mcp_dispatch_set_thread_stall_watch(NULL);

    const char *cmd = jsonl_msg_get_cmd(job->req);

    g_mutex_lock(&job->mutex);
    bool abandoned = job->abandoned;
    if (!abandoned) {
        job->response = response;
        job->result = r;
    }
    job->done = true;
    g_cond_signal(&job->cond);
    g_mutex_unlock(&job->mutex);

    if (abandoned) {
        fprintf(stderr,
                "[MCP] late completion of abandoned request (cmd=%s), "
                "response discarded\n", cmd ? cmd : "?");
        free(response);
    }

    jsonl_msg_free(job->req);
    job->req = NULL;
    g_atomic_int_add(&s_inflight, -1);
    _job_unref(job);
}


/**
 * @brief Líně vytvoří fond pracovních vláken.
 *
 * Fond je neexkluzivní s neomezeným počtem vláken: zaparkované vlákno
 * zaseknutého požadavku neblokuje další požadavky, nečinná vlákna se
 * znovu použijí.
 *
 * @return fond, nebo NULL při chybě
 */
static GThreadPool *_pool_get(void) {
    if (g_once_init_enter(&s_pool_init)) {
        GError *err = NULL;
        s_pool = g_thread_pool_new(_worker, NULL, -1, FALSE, &err);
        if (!s_pool) {
            fprintf(stderr, "[MCP] cannot create dispatch thread pool: %s\n",
                    err ? err->message : "?");
            g_clear_error(&err);
        }
        g_once_init_leave(&s_pool_init, 1);
    }
    return s_pool;
}


en_MCP_DISPATCH_RESULT mcp_dispatch_runner_request(st_JSONL_MESSAGE *req,
                                                   char **out_response) {
    if (out_response) *out_response = NULL;
    if (!req || !out_response) {
        if (req) jsonl_msg_free(req);
        return MCP_DISPATCH_NOT_A_REQUEST;
    }

    /* req_id pro případnou chybovou odpověď - číst PŘED předáním jobu,
     * pracovní vlákno může req uvolnit dřív, než se k němu vrátíme. */
    int64_t req_id = jsonl_msg_get_req_id(req);

    GThreadPool *pool = _pool_get();
    st_MCP_RUNNER_JOB *job = NULL;
    if (pool) {
        job = g_new0(st_MCP_RUNNER_JOB, 1);
        g_mutex_init(&job->mutex);
        g_cond_init(&job->cond);
        job->refcount = 2;
        job->req = req;
        g_atomic_int_inc(&s_inflight);
        GError *err = NULL;
        if (!g_thread_pool_push(pool, job, &err)) {
            /* Selhalo jen spuštění nového vlákna. Podle GLib (gthreadpool.c,
             * g_thread_pool_push: "An error can only occur when a new thread
             * couldn't be created. In that case @data is simply appended to
             * the queue of work to do.") job ve frontě fondu ZŮSTÁVÁ a
             * provede ho první uvolněné vlákno. Job proto nesmíme uvolnit
             * ani dispatchovat sami (use-after-free / dvojí uvolnění);
             * čekáme na něj stejně jako v běžném případě. */
            fprintf(stderr, "[MCP] dispatch thread start failed (%s), "
                    "request stays queued in the pool\n",
                    err ? err->message : "?");
            g_clear_error(&err);
        }
    }

    if (!job) {
        /* Nouzová cesta (fond se nepodařilo vytvořit): dispatch přímo na
         * volajícím vlákně, bez omezení doby odpovědi. */
        en_MCP_DISPATCH_RESULT r = mcp_dispatch_request(req, out_response);
        jsonl_msg_free(req);
        return r;
    }

    en_MCP_DISPATCH_RESULT r;
    g_mutex_lock(&job->mutex);
    while (!job->done && !job->stalled)
        g_cond_wait(&job->cond, &job->mutex);
    if (job->done) {
        *out_response = job->response;
        job->response = NULL;
        r = job->result;
        g_mutex_unlock(&job->mutex);
    } else {
        /* Zaseknutí: klientovi odpovíme hned, pracovní vlákno doběhne
         * samo a svůj výsledek zahodí. */
        job->abandoned = true;
        g_mutex_unlock(&job->mutex);
        r = mcp_dispatch_build_stalled_response(req_id, out_response);
        fprintf(stderr,
                "[MCP] request %lld: emulator thread did not complete the "
                "command in time, answered busy\n", (long long)req_id);
    }
    _job_unref(job);
    return r;
}


bool mcp_dispatch_runner_wait_idle(int timeout_ms) {
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    while (g_atomic_int_get(&s_inflight) > 0) {
        if (g_get_monotonic_time() >= deadline) return false;
        g_usleep(10 * 1000);
    }
    return true;
}

#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
