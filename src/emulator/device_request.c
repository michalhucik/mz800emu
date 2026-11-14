/**
 * @file device_request.c
 * @brief Fronta zásahů GUI do zařízení, provádí je emu vlákno.
 *
 * Kontrakt v device_request.h. Fronta zásahů i výsledků je GQueue pod
 * s_lock; samotné zásahy běží mimo s_lock, ale pod zámkem stavu zařízení
 * s_state_lock (UI mezitím smí zařazovat další zásahy).
 */

#include "device_request.h"
#include "state_rev.h"
#include "mzarch/mzarch_config.h"
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/dbgapi_cmdrq.h"
#endif
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
    bool                ui_data; /**< true = zásah mění jen data pro UI (device_request_submit_ui_data()): bez revize stavu. */
} st_DEVICE_REQ;

/** @brief Zámek front s_pending, s_done a čítače s_seq (statický GMutex nepotřebuje init). */
static GMutex s_lock;
/** @brief Zámek stavu zařízení: emu vlákno při zásahu, UI při kreslení zařízení. */
static GMutex s_state_lock;
/**
 * @brief Vlákno, které drží s_state_lock (NULL = nikdo).
 *
 * Zapisuje ho jen držitel zámku hned po zamčení a před odemčením. Jiné
 * vlákno může číst neaktuální hodnotu, ale nikdy svoje vlastní vlákno,
 * pokud zámek nedrží: device_state_lock_held() je tak spolehlivé pro
 * otázku "drží zámek volající vlákno".
 */
static gpointer s_state_lock_owner = NULL;
/** @brief Čekající zásahy (st_DEVICE_REQ*), pořadí zařazení. */
static GQueue s_pending = G_QUEUE_INIT;
/** @brief Vyřízené výsledky (st_DEVICE_REQ_RESULT*), nejvýš DEVICE_REQUEST_RESULTS_MAX. */
static GQueue s_done = G_QUEUE_INIT;
/** @brief Poslední přidělené pořadové číslo (pod s_lock). */
static uint32_t s_seq = 0;
/** @brief Počet čekajících a právě prováděných zásahů (mění se pod s_lock,
 *         čte se atomicky i bez zámku pro levný poll). */
static gint s_pending_count = 0;
/**
 * @brief Pořadové číslo zásahu, jehož funkce právě běží (0 = žádná).
 *
 * Zapisuje ho jen emu vlákno v device_request_poll() pod s_state_lock
 * kolem volání funkce zásahu; device_request_current_seq() ho vrací jen
 * vláknu, které zámek drží (tj. kódu volanému z funkce zásahu).
 */
static uint32_t s_current_seq = 0;


bool device_request_args_set_str(st_DEVICE_REQ_ARGS *args, const char *str)
{
    memset(args, 0, sizeof(*args));
    if (!str) return true;
    return g_strlcpy(args->str, str, sizeof(args->str)) < sizeof(args->str);
}


/**
 * @brief Společné zařazení zásahu (device_request_submit(), device_request_submit_ui_data()).
 * @param name Název zásahu (statický). @param fn Funkce (nesmí být NULL, ověřuje volající).
 * @param args Argumenty (smí být NULL). @param ui_data true = zásah jen pro data UI.
 * @return Pořadové číslo zásahu (od 1).
 */
static uint32_t device_request_enqueue(const char *name, device_request_fn_t fn,
                                       const st_DEVICE_REQ_ARGS *args, bool ui_data)
{
    st_DEVICE_REQ *r = g_new0(st_DEVICE_REQ, 1);
    const char *kind = name ? name : "";
    r->name = kind;
    r->fn = fn;
    r->ui_data = ui_data;
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
    /* r už smí uvolnit emu vlákno (poll): dál jen lokální kopie */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Zásahy zařazuje jen GUI (menu a okna zařízení): akce uživatele pro
     * last_user_action, jméno = název zásahu. Zásah jen pro data UI akcí
     * uživatele není. */
    if (!ui_data) dbgapi_note_user_action(kind);
#else
    (void)ui_data;
#endif
    return seq;
}


uint32_t device_request_submit(const char *name, device_request_fn_t fn,
                               const st_DEVICE_REQ_ARGS *args)
{
    if (!fn) return 0;
    return device_request_enqueue(name, fn, args, false);
}


uint32_t device_request_submit_ui_data(const char *name, device_request_fn_t fn,
                                       const st_DEVICE_REQ_ARGS *args)
{
    if (!fn) return 0;
    return device_request_enqueue(name, fn, args, true);
}


void device_request_poll(void)
{
    if (!g_atomic_int_get(&s_pending_count)) return;

    /* UI právě kreslí zařízení: zkusit při dalším volání (emu vlákno na UI
     * nečeká). */
    if (!g_mutex_trylock(&s_state_lock)) return;
    g_atomic_pointer_set(&s_state_lock_owner, g_thread_self());

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
            s_current_seq = r->seq;
            res->rc = r->fn(&r->args);
            s_current_seq = 0;
            /* Revize stavu (state_rev.h, Z6): provedený zásah zařízení;
             * zásah jen pro data UI stav stroje nemění. */
            if (!r->ui_data) STATE_REV_BUMP(STATE_REV_SRC_DEVICE);
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

    g_atomic_pointer_set(&s_state_lock_owner, NULL);
    g_mutex_unlock(&s_state_lock);
}


uint32_t device_request_current_seq(void)
{
    /* jiné vlákno než držitel zámku (poll) by četlo hodnotu bez synchronizace */
    if (!device_state_lock_held()) return 0;
    return s_current_seq;
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


#ifdef MZTEST_HEADLESS
bool device_request_test_take_pending(const char **name, st_DEVICE_REQ_ARGS *args)
{
    g_mutex_lock(&s_lock);
    st_DEVICE_REQ *r = (st_DEVICE_REQ *)g_queue_pop_head(&s_pending);
    if (r) g_atomic_int_add(&s_pending_count, -1);
    g_mutex_unlock(&s_lock);
    if (!r) return false;
    if (name) *name = r->name;
    if (args) *args = r->args;
    g_free(r);
    return true;
}
#endif


void device_state_lock(void)
{
    g_mutex_lock(&s_state_lock);
    g_atomic_pointer_set(&s_state_lock_owner, g_thread_self());
}


void device_state_unlock(void)
{
    g_atomic_pointer_set(&s_state_lock_owner, NULL);
    g_mutex_unlock(&s_state_lock);
}


bool device_state_lock_held(void)
{
    return g_atomic_pointer_get(&s_state_lock_owner) == (gpointer)g_thread_self();
}
