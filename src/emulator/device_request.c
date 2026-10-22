/**
 * @file device_request.c
 * @brief Fronta zásahů GUI do zařízení, provádí je emu vlákno.
 *
 * Kontrakt v device_request.h. Fronta zásahů i výsledků je GQueue pod
 * s_lock; samotné zásahy běží mimo s_lock, ale pod zámkem stavu zařízení
 * s_state_lock (UI mezitím smí zařazovat další zásahy).
 */

#include "device_request.h"
#include "replay.h"

#include <glib.h>
#include <string.h>


/**
 * @brief Čekající zásah (vlastní ho fronta s_pending, pak poll).
 */
typedef struct st_DEVICE_REQ {
    uint32_t            seq;   /**< Pořadové číslo (od 1). */
    const char         *name;  /**< Název zásahu (statický řetězec). */
    device_request_fn_t fn;    /**< Funkce zásahu. */
    st_DEVICE_REQ_ARGS  args;  /**< Kopie argumentů. */
} st_DEVICE_REQ;

/** @brief Zámek front s_pending, s_done a čítače s_seq (statický GMutex nepotřebuje init). */
static GMutex s_lock;
/** @brief Zámek stavu zařízení: emu vlákno při zásahu, UI při kreslení zařízení. */
static GMutex s_state_lock;
/** @brief Čekající zásahy (st_DEVICE_REQ*), pořadí zařazení. */
static GQueue s_pending = G_QUEUE_INIT;
/** @brief Vyřízené výsledky (st_DEVICE_REQ_RESULT*), nejvýš DEVICE_REQUEST_RESULTS_MAX. */
static GQueue s_done = G_QUEUE_INIT;
/** @brief Poslední přidělené pořadové číslo (pod s_lock). */
static uint32_t s_seq = 0;
/** @brief Počet čekajících a právě prováděných zásahů (mění se pod s_lock,
 *         čte se atomicky i bez zámku pro levný poll). */
static gint s_pending_count = 0;


bool device_request_args_set_str(st_DEVICE_REQ_ARGS *args, const char *str)
{
    memset(args, 0, sizeof(*args));
    if (!str) return true;
    return g_strlcpy(args->str, str, sizeof(args->str)) < sizeof(args->str);
}


uint32_t device_request_submit(const char *name, device_request_fn_t fn,
                               const st_DEVICE_REQ_ARGS *args)
{
    if (!fn) return 0;

    st_DEVICE_REQ *r = g_new0(st_DEVICE_REQ, 1);
    r->name = name ? name : "";
    r->fn = fn;
    if (args) {
        r->args = *args;
        r->args.str[sizeof(r->args.str) - 1] = '\0';
    }

    g_mutex_lock(&s_lock);
    r->seq = ++s_seq;
    if (r->seq == 0) r->seq = ++s_seq; /* 0 je vyhrazená pro chybu */
    uint32_t seq = r->seq;
    g_queue_push_tail(&s_pending, r);
    g_atomic_int_inc(&s_pending_count);
    g_mutex_unlock(&s_lock);
    return seq;
}


void device_request_poll(void)
{
    if (!g_atomic_int_get(&s_pending_count)) return;

    /* UI právě kreslí zařízení: zkusit při dalším volání (emu vlákno na UI
     * nečeká). */
    if (!g_mutex_trylock(&s_state_lock)) return;

    for (;;) {
        g_mutex_lock(&s_lock);
        st_DEVICE_REQ *r = (st_DEVICE_REQ *)g_queue_pop_head(&s_pending);
        g_mutex_unlock(&s_lock);
        if (!r) break;

        st_DEVICE_REQ_RESULT *res = g_new0(st_DEVICE_REQ_RESULT, 1);
        res->seq = r->seq;
        res->name = r->name;
        if (replay_is_active()) {
            res->status = DEVICE_REQ_REJECTED_REPLAY;
        } else {
            res->status = DEVICE_REQ_DONE;
            res->rc = r->fn(&r->args);
        }
        g_free(r);

        g_mutex_lock(&s_lock);
        g_queue_push_tail(&s_done, res);
        while (g_queue_get_length(&s_done) > DEVICE_REQUEST_RESULTS_MAX)
            g_free(g_queue_pop_head(&s_done));
        /* Počet klesá až s výsledkem ve frontě: kdo vidí 0 čekajících,
         * najde výsledek (pokud ho mezitím nevytlačily novější). Po
         * souběžném device_request_clear() už může být 0. */
        if (g_atomic_int_get(&s_pending_count) > 0)
            g_atomic_int_add(&s_pending_count, -1);
        g_mutex_unlock(&s_lock);
    }

    g_mutex_unlock(&s_state_lock);
}


bool device_request_take_result(st_DEVICE_REQ_RESULT *out)
{
    g_mutex_lock(&s_lock);
    st_DEVICE_REQ_RESULT *res = (st_DEVICE_REQ_RESULT *)g_queue_pop_head(&s_done);
    g_mutex_unlock(&s_lock);
    if (!res) return false;
    *out = *res;
    g_free(res);
    return true;
}


int device_request_pending(void)
{
    return g_atomic_int_get(&s_pending_count);
}


void device_request_clear(void)
{
    g_mutex_lock(&s_lock);
    g_queue_clear_full(&s_pending, g_free);
    g_queue_clear_full(&s_done, g_free);
    g_atomic_int_set(&s_pending_count, 0);
    g_mutex_unlock(&s_lock);
}


void device_state_lock(void)
{
    g_mutex_lock(&s_state_lock);
}


void device_state_unlock(void)
{
    g_mutex_unlock(&s_state_lock);
}
