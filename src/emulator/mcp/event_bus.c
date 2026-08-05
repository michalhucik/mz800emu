/*
 * File:   event_bus.c
 *
 * Implementace EVENT busu pro MCP backend (V1.A.4).
 *
 * Datová struktura:
 *   - g_bus_mutex chrání g_subscribers GHashTable<int conn_id -> Subscriber*>
 *   - Každý Subscriber má svůj sub_mutex chránící topics_set + queue + drop_count
 *   - Každý Subscriber má vlastní GCond pro probuzení poll() při emit
 *
 * Lock ordering: vždy g_bus_mutex před sub_mutex. Funkce které najdou
 * subscriber přes get_subscriber_locked() drží g_bus_mutex po dobu hledání,
 * pak ho uvolní, drží jen sub_mutex.
 *
 * Emit cesta (EMU vlákno):
 *   1) Lock g_bus_mutex, iteruj subscribers, vybere ti co mají topic.
 *   2) Pro každého: lock sub_mutex, push event, broadcast cond, unlock.
 *   3) Unlock g_bus_mutex.
 *
 * Poll cesta (dispatch handler vlákno):
 *   1) Lock g_bus_mutex, najdi subscriber, unlock g_bus_mutex.
 *   2) Lock sub_mutex, pokud queue prázdná a timeout > 0 - g_cond_wait_until.
 *   3) Pop až max_events, sestav JsonArray, unlock.
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MZ800EMU_MCP_TEST_BUILD
#include "../mzarch/mzcommon_config.h"
#endif

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED

#include "event_bus.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>


/* ====================================================================== */
/* Interní typy                                                             */
/* ====================================================================== */

/**
 * @brief Per-connection stav v event busu.
 *
 * Vlastní mutex chrání topics_set + queue + drop_count + waiters_count.
 * Cond signalizuje příchod eventu pro probuzení blokujícího poll.
 *
 * Vlastnictví: alokuje attach(), uvolňuje detach() / shutdown(). Položky
 * v queue jsou heap-alokované st_EVENT_ITEM, vlastníme je.
 */
typedef struct st_EVENT_BUS_SUBSCRIBER {
    int             conn_id;        /**< Identifikátor connection. */
    GHashTable     *topics_set;     /**< Set: gchar* (vlastní) -> GINT_TO_POINTER(1). */
    GQueue         *queue;          /**< FIFO st_EVENT_ITEM* (vlastní). */
    GMutex          sub_mutex;      /**< Per-subscriber lock. */
    GCond           sub_cond;       /**< Signalizace nového eventu (probudí poll). */
    int             dropped_count;  /**< Backpressure counter (drop FIFO oldest). */
    int             waiters_count;  /**< Počet aktuálních poll() bloker. Pro shutdown wake-all. */
} st_EVENT_BUS_SUBSCRIBER;


/**
 * @brief Jeden event v queue.
 *
 * topic je vlastněný (g_strdup ze static literal v emit cestě). data
 * je vlastněný JsonNode (= deep-copy z emit payload).
 */
typedef struct st_EVENT_ITEM {
    gchar      *topic;      /**< Topic string (g_strdup). */
    gint64      ts_us;      /**< Timestamp v mikrosekundách (g_get_monotonic_time). */
    JsonNode   *data_node;  /**< Payload (deep-copy, vlastní). */
} st_EVENT_ITEM;


/* ====================================================================== */
/* Globální stav                                                            */
/* ====================================================================== */

/** @brief Globální zámek nad mapou subscribers. */
static GMutex      g_bus_mutex;

/** @brief Map int conn_id -> st_EVENT_BUS_SUBSCRIBER*. Vlastní hodnoty. */
static GHashTable *g_subscribers = NULL;

/** @brief Flag - bus je inicializován. */
static gboolean    g_bus_initialized = FALSE;

/** @brief Flag - bus se ukončuje (= probudí všechny waitery v poll). */
static gboolean    g_bus_shutting_down = FALSE;


/* ====================================================================== */
/* Interní helpery                                                          */
/* ====================================================================== */

/**
 * @brief Uvolní jeden event item.
 *
 * Zavolá se z g_queue_free_full a explicit cestou v emit dropu.
 */
static void _event_item_free(gpointer p) {
    st_EVENT_ITEM *item = (st_EVENT_ITEM *)p;
    if (!item) return;
    g_free(item->topic);
    if (item->data_node) json_node_free(item->data_node);
    g_free(item);
}


/**
 * @brief Alokuje a inicializuje subscriber strukturu.
 */
static st_EVENT_BUS_SUBSCRIBER *_subscriber_new(int conn_id) {
    st_EVENT_BUS_SUBSCRIBER *sub = g_new0(st_EVENT_BUS_SUBSCRIBER, 1);
    sub->conn_id = conn_id;
    sub->topics_set = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    sub->queue = g_queue_new();
    g_mutex_init(&sub->sub_mutex);
    g_cond_init(&sub->sub_cond);
    sub->dropped_count = 0;
    sub->waiters_count = 0;
    return sub;
}


/**
 * @brief Uvolní subscriber strukturu (interní cleanup).
 *
 * Volá se z detach (s držením g_bus_mutex) nebo z shutdown.
 * Předpokládá, že na sub->sub_mutex není nikdo blokovaný - shutdown
 * zajistí wakeup přes broadcast + g_bus_shutting_down flag.
 */
static void _subscriber_free(gpointer p) {
    st_EVENT_BUS_SUBSCRIBER *sub = (st_EVENT_BUS_SUBSCRIBER *)p;
    if (!sub) return;
    /* Queue vyprázdníme přes free_full s naším destructorem. */
    g_queue_free_full(sub->queue, _event_item_free);
    g_hash_table_destroy(sub->topics_set);
    g_cond_clear(&sub->sub_cond);
    g_mutex_clear(&sub->sub_mutex);
    g_free(sub);
}


/**
 * @brief Najde subscriber podle conn_id (caller drží g_bus_mutex).
 *
 * @return pointer nebo NULL.
 */
static st_EVENT_BUS_SUBSCRIBER *_get_subscriber_locked(int conn_id) {
    if (!g_subscribers) return NULL;
    return (st_EVENT_BUS_SUBSCRIBER *)g_hash_table_lookup(
        g_subscribers, GINT_TO_POINTER(conn_id));
}


/* ====================================================================== */
/* Public API - init / shutdown / attach / detach                          */
/* ====================================================================== */

void event_bus_init(void) {
    /* Idempotentní - viz Doxygen. */
    static gboolean once = FALSE;
    if (once) return;
    g_mutex_init(&g_bus_mutex);
    g_subscribers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                           NULL, _subscriber_free);
    g_bus_initialized = TRUE;
    g_bus_shutting_down = FALSE;
    once = TRUE;
}


void event_bus_shutdown(void) {
    if (!g_bus_initialized) return;

    g_mutex_lock(&g_bus_mutex);
    g_bus_shutting_down = TRUE;
    /* Probudíme všechny případné poll() waitery, aby vyskočili z cond_wait. */
    if (g_subscribers) {
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, g_subscribers);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            st_EVENT_BUS_SUBSCRIBER *sub = (st_EVENT_BUS_SUBSCRIBER *)value;
            g_mutex_lock(&sub->sub_mutex);
            g_cond_broadcast(&sub->sub_cond);
            g_mutex_unlock(&sub->sub_mutex);
        }
        g_hash_table_destroy(g_subscribers);
        g_subscribers = NULL;
    }
    g_mutex_unlock(&g_bus_mutex);
    g_mutex_clear(&g_bus_mutex);
    g_bus_initialized = FALSE;
}


void event_bus_attach(int conn_id) {
    if (!g_bus_initialized) return;
    g_mutex_lock(&g_bus_mutex);
    if (g_subscribers && !_get_subscriber_locked(conn_id)) {
        st_EVENT_BUS_SUBSCRIBER *sub = _subscriber_new(conn_id);
        g_hash_table_insert(g_subscribers, GINT_TO_POINTER(conn_id), sub);
    }
    g_mutex_unlock(&g_bus_mutex);
}


void event_bus_detach(int conn_id) {
    if (!g_bus_initialized) return;
    g_mutex_lock(&g_bus_mutex);
    if (g_subscribers) {
        st_EVENT_BUS_SUBSCRIBER *sub = _get_subscriber_locked(conn_id);
        if (sub) {
            /* Probudit waitery, ať vyskočí z poll. */
            g_mutex_lock(&sub->sub_mutex);
            g_cond_broadcast(&sub->sub_cond);
            g_mutex_unlock(&sub->sub_mutex);
            /* g_hash_table_remove zavolá náš _subscriber_free destructor. */
            g_hash_table_remove(g_subscribers, GINT_TO_POINTER(conn_id));
        }
    }
    g_mutex_unlock(&g_bus_mutex);
}


/* ====================================================================== */
/* Public API - subscribe / unsubscribe                                    */
/* ====================================================================== */

bool event_bus_subscribe(int conn_id, const char *const *topics) {
    if (!g_bus_initialized || !topics) return false;
    g_mutex_lock(&g_bus_mutex);
    st_EVENT_BUS_SUBSCRIBER *sub = _get_subscriber_locked(conn_id);
    if (!sub) { g_mutex_unlock(&g_bus_mutex); return false; }
    g_mutex_lock(&sub->sub_mutex);
    g_mutex_unlock(&g_bus_mutex);

    for (int i = 0; topics[i] != NULL; i++) {
        const char *t = topics[i];
        if (!t || !t[0]) continue;
        if (!g_hash_table_contains(sub->topics_set, t)) {
            g_hash_table_insert(sub->topics_set, g_strdup(t),
                                 GINT_TO_POINTER(1));
        }
    }
    g_mutex_unlock(&sub->sub_mutex);
    return true;
}


bool event_bus_unsubscribe(int conn_id, const char *const *topics) {
    if (!g_bus_initialized) return false;
    g_mutex_lock(&g_bus_mutex);
    st_EVENT_BUS_SUBSCRIBER *sub = _get_subscriber_locked(conn_id);
    if (!sub) { g_mutex_unlock(&g_bus_mutex); return false; }
    g_mutex_lock(&sub->sub_mutex);
    g_mutex_unlock(&g_bus_mutex);

    if (!topics || topics[0] == NULL) {
        /* Unsubscribe all. */
        g_hash_table_remove_all(sub->topics_set);
    } else {
        for (int i = 0; topics[i] != NULL; i++) {
            if (topics[i] && topics[i][0]) {
                g_hash_table_remove(sub->topics_set, topics[i]);
            }
        }
    }
    g_mutex_unlock(&sub->sub_mutex);
    return true;
}


/* ====================================================================== */
/* Public API - poll                                                        */
/* ====================================================================== */

/**
 * @brief Sestaví JSON event objekt z queue itemu (callee owns).
 *
 * Vstupní item se po zavolání NESMÍ použít - jeho data_node se transferuje
 * do nového JsonObjectu jako member "data", string topic a ts_us se kopírují.
 */
static JsonNode *_item_to_json_node(st_EVENT_ITEM *item) {
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "topic", item->topic ? item->topic : "");
    json_object_set_int_member(obj, "ts_us", item->ts_us);
    /* Transfer data_node ownership do objektu. */
    if (item->data_node) {
        json_object_set_member(obj, "data", item->data_node);
        item->data_node = NULL; /* aby _event_item_free ho znovu nefreel */
    } else {
        json_object_set_null_member(obj, "data");
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    return node;
}


JsonArray *event_bus_poll(int conn_id, int timeout_ms, int max_events) {
    if (!g_bus_initialized) return NULL;
    if (max_events < 1) max_events = 1;
    if (max_events > EVENT_BUS_MAX_QUEUE_SIZE) max_events = EVENT_BUS_MAX_QUEUE_SIZE;
    if (timeout_ms < 0) timeout_ms = 0;
    if (timeout_ms > 60000) timeout_ms = 60000;

    g_mutex_lock(&g_bus_mutex);
    st_EVENT_BUS_SUBSCRIBER *sub = _get_subscriber_locked(conn_id);
    if (!sub) { g_mutex_unlock(&g_bus_mutex); return NULL; }
    g_mutex_lock(&sub->sub_mutex);
    g_mutex_unlock(&g_bus_mutex);

    /* Pokud queue prázdná a timeout > 0, čekáme. */
    if (g_queue_is_empty(sub->queue) && timeout_ms > 0 && !g_bus_shutting_down) {
        gint64 end_time = g_get_monotonic_time() +
                          (gint64)timeout_ms * G_TIME_SPAN_MILLISECOND;
        sub->waiters_count++;
        while (g_queue_is_empty(sub->queue) && !g_bus_shutting_down) {
            if (!g_cond_wait_until(&sub->sub_cond, &sub->sub_mutex, end_time)) {
                /* Timeout vypršel. */
                break;
            }
        }
        sub->waiters_count--;
    }

    JsonArray *arr = json_array_new();
    int taken = 0;
    while (taken < max_events && !g_queue_is_empty(sub->queue)) {
        st_EVENT_ITEM *item = (st_EVENT_ITEM *)g_queue_pop_head(sub->queue);
        if (!item) break;
        JsonNode *n = _item_to_json_node(item);
        json_array_add_element(arr, n);
        _event_item_free(item);
        taken++;
    }
    g_mutex_unlock(&sub->sub_mutex);
    return arr;
}


/* ====================================================================== */
/* Public API - emit                                                        */
/* ====================================================================== */

void event_bus_emit(const char *topic, JsonObject *payload) {
    if (!g_bus_initialized || !topic) {
        if (payload) json_object_unref(payload);
        return;
    }

    g_mutex_lock(&g_bus_mutex);
    if (!g_subscribers || g_bus_shutting_down) {
        g_mutex_unlock(&g_bus_mutex);
        if (payload) json_object_unref(payload);
        return;
    }

    /* Zjisti, jestli vůbec někdo na topic subscribuje. Pokud ne, payload
     * se rovnou uvolní bez kopií. Iterace pod g_bus_mutex je krátká. */
    gboolean any_subscriber = FALSE;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, g_subscribers);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        st_EVENT_BUS_SUBSCRIBER *sub = (st_EVENT_BUS_SUBSCRIBER *)value;
        /* Krátký peek pod sub_mutex - jen contains check. */
        g_mutex_lock(&sub->sub_mutex);
        gboolean has = g_hash_table_contains(sub->topics_set, topic);
        g_mutex_unlock(&sub->sub_mutex);
        if (has) { any_subscriber = TRUE; break; }
    }

    if (!any_subscriber) {
        g_mutex_unlock(&g_bus_mutex);
        if (payload) json_object_unref(payload);
        return;
    }

    /* Sestavíme společný JsonNode pro deep-copy do per-sub queues. */
    JsonNode *src_node = NULL;
    if (payload) {
        src_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(src_node, payload);
    }
    gint64 now_us = g_get_monotonic_time();

    /* Re-iterate a push do queue subscribera který má topic. */
    g_hash_table_iter_init(&it, g_subscribers);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        st_EVENT_BUS_SUBSCRIBER *sub = (st_EVENT_BUS_SUBSCRIBER *)value;
        g_mutex_lock(&sub->sub_mutex);
        if (!g_hash_table_contains(sub->topics_set, topic)) {
            g_mutex_unlock(&sub->sub_mutex);
            continue;
        }
        /* Backpressure: pokud queue plná, dropneme oldest. */
        while (g_queue_get_length(sub->queue) >= EVENT_BUS_MAX_QUEUE_SIZE) {
            st_EVENT_ITEM *old = (st_EVENT_ITEM *)g_queue_pop_head(sub->queue);
            if (old) {
                _event_item_free(old);
                if (sub->dropped_count == 0) {
                    /* První drop logujeme jako varování. */
                    fprintf(stderr,
                            "[mcp-event-bus] WARNING: conn_id=%d queue "
                            "overflow (%d), dropping oldest event\n",
                            sub->conn_id, EVENT_BUS_MAX_QUEUE_SIZE);
                }
                sub->dropped_count++;
            } else {
                break;
            }
        }
        st_EVENT_ITEM *item = g_new0(st_EVENT_ITEM, 1);
        item->topic = g_strdup(topic);
        item->ts_us = now_us;
        item->data_node = src_node ? json_node_copy(src_node) : NULL;
        g_queue_push_tail(sub->queue, item);
        g_cond_broadcast(&sub->sub_cond);
        g_mutex_unlock(&sub->sub_mutex);
    }
    g_mutex_unlock(&g_bus_mutex);

    /* Uvolníme zdrojový node - kopie jsou v per-conn queue. */
    if (src_node) json_node_free(src_node);
}


/* ====================================================================== */
/* Public API - hot-path guard                                              */
/* ====================================================================== */

bool event_bus_has_subscriber(const char *topic) {
    if (!g_bus_initialized || !topic) return false;
    g_mutex_lock(&g_bus_mutex);
    if (!g_subscribers || g_bus_shutting_down) {
        g_mutex_unlock(&g_bus_mutex);
        return false;
    }
    gboolean any = FALSE;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, g_subscribers);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        st_EVENT_BUS_SUBSCRIBER *sub = (st_EVENT_BUS_SUBSCRIBER *)value;
        g_mutex_lock(&sub->sub_mutex);
        gboolean has = g_hash_table_contains(sub->topics_set, topic);
        g_mutex_unlock(&sub->sub_mutex);
        if (has) { any = TRUE; break; }
    }
    g_mutex_unlock(&g_bus_mutex);
    return (bool)any;
}


/* ====================================================================== */
/* Public API - diagnostika                                                 */
/* ====================================================================== */

int event_bus_get_pending_count(int conn_id) {
    if (!g_bus_initialized) return -1;
    g_mutex_lock(&g_bus_mutex);
    st_EVENT_BUS_SUBSCRIBER *sub = _get_subscriber_locked(conn_id);
    if (!sub) { g_mutex_unlock(&g_bus_mutex); return -1; }
    g_mutex_lock(&sub->sub_mutex);
    g_mutex_unlock(&g_bus_mutex);
    int n = (int)g_queue_get_length(sub->queue);
    g_mutex_unlock(&sub->sub_mutex);
    return n;
}


int event_bus_get_dropped_count(int conn_id) {
    if (!g_bus_initialized) return -1;
    g_mutex_lock(&g_bus_mutex);
    st_EVENT_BUS_SUBSCRIBER *sub = _get_subscriber_locked(conn_id);
    if (!sub) { g_mutex_unlock(&g_bus_mutex); return -1; }
    g_mutex_lock(&sub->sub_mutex);
    g_mutex_unlock(&g_bus_mutex);
    int n = sub->dropped_count;
    g_mutex_unlock(&sub->sub_mutex);
    return n;
}


#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
