/**
 * @file snapshot_request.c
 * @brief Fronta požadavků na snapshot z GUI, vyřizuje je emu vlákno v pauze.
 *
 * Kontrakt v snapshot_request.h. Požadavky i výsledky jsou GQueue pod
 * s_lock; samotná operace snapshotu běží mimo zámek (může trvat desítky
 * ms a UI mezitím smí zařazovat další požadavky).
 */

#include "snapshot_request.h"
#include "snapshot_quicksave.h"
#include "mzarch/mzarch_config.h"
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/dbgapi_cmdrq.h"
#endif
#include "replay.h"

#include <glib.h>


/**
 * @brief Čekající požadavek (vlastní ho fronta s_pending, pak poll).
 */
typedef struct st_SNAPSHOT_REQ {
    uint32_t             seq;                                  /**< Pořadové číslo (od 1). */
    en_SNAPSHOT_REQ_KIND kind;                                 /**< Druh požadavku. */
    char                 path[SNAPSHOT_REQUEST_PATH_MAX];      /**< Cesta ("" u Quick). */
    char                 description[SNAPSHOT_REQUEST_DESC_MAX]; /**< Popis (jen SAVE). */
    bool                 has_description;                      /**< false = popis NULL. */
} st_SNAPSHOT_REQ;

/** @brief Zámek front s_pending a s_done a čítače s_seq (statický GMutex nepotřebuje init). */
static GMutex s_lock;
/** @brief Čekající požadavky (st_SNAPSHOT_REQ*), pořadí zařazení. */
static GQueue s_pending = G_QUEUE_INIT;
/** @brief Vyřízené výsledky (st_SNAPSHOT_REQ_RESULT*), pořadí vyřízení. */
static GQueue s_done = G_QUEUE_INIT;
/** @brief Poslední přidělené pořadové číslo (pod s_lock). */
static uint32_t s_seq = 0;
/** @brief Počet čekajících a právě vyřizovaných požadavků (mění se pod
 *         s_lock, čte se atomicky i bez zámku pro levný poll). */
static gint s_pending_count = 0;


uint32_t snapshot_request_submit(en_SNAPSHOT_REQ_KIND kind, const char *path,
                                 const char *description)
{
    bool needs_path = (kind == SNAPSHOT_REQ_SAVE || kind == SNAPSHOT_REQ_LOAD);
    if (needs_path && (!path || !path[0])) return 0;

    st_SNAPSHOT_REQ *r = g_new0(st_SNAPSHOT_REQ, 1);
    r->kind = kind;
    if (needs_path) g_strlcpy(r->path, path, sizeof(r->path));
    if (kind == SNAPSHOT_REQ_SAVE && description) {
        g_strlcpy(r->description, description, sizeof(r->description));
        r->has_description = true;
    }

    g_mutex_lock(&s_lock);
    r->seq = ++s_seq;
    if (r->seq == 0) r->seq = ++s_seq; /* 0 je vyhrazená pro chybu */
    uint32_t seq = r->seq;
    g_queue_push_tail(&s_pending, r);
    g_atomic_int_inc(&s_pending_count);
    g_mutex_unlock(&s_lock);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Fronta je jen pro GUI: načtení snímku je akce uživatele (last_user_action);
     * uložení stav stroje nemění, stejně jako čtení se nezaznamená. */
    if (kind == SNAPSHOT_REQ_LOAD) dbgapi_note_user_action("snapshot_load");
    else if (kind == SNAPSHOT_REQ_QUICKLOAD) dbgapi_note_user_action("snapshot_quickload");
#endif
    return seq;
}


/**
 * @brief Provede jeden požadavek (emu vlákno, mimo zámek).
 * @param r Požadavek.
 * @return Výsledek operace snapshotu.
 */
static en_SNAPSHOT_RESULT snapshot_request_execute(const st_SNAPSHOT_REQ *r)
{
    switch (r->kind) {
        case SNAPSHOT_REQ_SAVE:
            return snapshot_save(r->path, r->has_description ? r->description : NULL);
        case SNAPSHOT_REQ_LOAD:
            if (replay_is_active()) return SNAPSHOT_ERR_REPLAY_ACTIVE;
            return snapshot_load(r->path);
        case SNAPSHOT_REQ_QUICKSAVE:
            return snapshot_quicksave();
        case SNAPSHOT_REQ_QUICKLOAD:
            if (replay_is_active()) return SNAPSHOT_ERR_REPLAY_ACTIVE;
            return snapshot_quickload();
    }
    return SNAPSHOT_ERR_IO;
}


void snapshot_request_poll(void)
{
    if (!g_atomic_int_get(&s_pending_count)) return;

    for (;;) {
        g_mutex_lock(&s_lock);
        st_SNAPSHOT_REQ *r = (st_SNAPSHOT_REQ *)g_queue_pop_head(&s_pending);
        g_mutex_unlock(&s_lock);
        if (!r) break;

        st_SNAPSHOT_REQ_RESULT *res = g_new0(st_SNAPSHOT_REQ_RESULT, 1);
        res->seq = r->seq;
        res->kind = r->kind;
        g_strlcpy(res->path, r->path, sizeof(res->path));
        res->result = snapshot_request_execute(r);
        g_free(r);

        g_mutex_lock(&s_lock);
        g_queue_push_tail(&s_done, res);
        /* Počet klesá až s výsledkem ve frontě: kdo vidí 0 čekajících,
         * najde výsledek. Čítač mění jen kód pod s_lock; po souběžném
         * snapshot_request_clear() už může být 0. */
        if (g_atomic_int_get(&s_pending_count) > 0)
            g_atomic_int_add(&s_pending_count, -1);
        g_mutex_unlock(&s_lock);
    }
}


bool snapshot_request_take_result(st_SNAPSHOT_REQ_RESULT *out)
{
    g_mutex_lock(&s_lock);
    st_SNAPSHOT_REQ_RESULT *res = (st_SNAPSHOT_REQ_RESULT *)g_queue_pop_head(&s_done);
    g_mutex_unlock(&s_lock);
    if (!res) return false;
    *out = *res;
    g_free(res);
    return true;
}


int snapshot_request_pending(void)
{
    return g_atomic_int_get(&s_pending_count);
}


void snapshot_request_clear(void)
{
    g_mutex_lock(&s_lock);
    g_queue_clear_full(&s_pending, g_free);
    g_queue_clear_full(&s_done, g_free);
    g_atomic_int_set(&s_pending_count, 0);
    g_mutex_unlock(&s_lock);
}
