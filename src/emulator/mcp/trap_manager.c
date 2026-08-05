/*
 * File:   trap_manager.c
 *
 * Implementace TRAP manageru pro MCP backend (V1.A.4).
 *
 * Datová struktura:
 *   - g_trap_mutex chrání g_next_trap_id + g_pending_traps.
 *   - g_pending_traps je GHashTable s klíčem int64 trap_id (boxed jako
 *     pointer přes wrapped GINT64_TO_POINTER nelze - místo toho používáme
 *     heap-alokovaný int64* klíč a vlastní hash/equal funkce).
 *
 * V1.A.4 minimum: bez TTL, bez per-conn ownership (= jakýkoliv klient
 * smí consume jakýkoliv trap_id - pipe je single connection, není
 * potřeba kontrolovat). V1.A.5+ může přidat conn_id pole.
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

#include "trap_manager.h"

#include <glib.h>
#include <string.h>


/* ====================================================================== */
/* Globální stav                                                            */
/* ====================================================================== */

static GMutex      g_trap_mutex;
static int64_t     g_next_trap_id = 1;        /**< 0 je rezervováno. */
static GHashTable *g_pending_traps = NULL;    /**< int64* -> GINT_TO_POINTER(1). */
static gboolean    g_trap_initialized = FALSE;


/* ====================================================================== */
/* Interní helpery                                                          */
/* ====================================================================== */

/**
 * @brief Hash funkce pro int64* klíče.
 */
static guint _int64_ptr_hash(gconstpointer v) {
    int64_t k = *(const int64_t *)v;
    return (guint)(k ^ (k >> 32));
}


/**
 * @brief Equal funkce pro int64* klíče.
 */
static gboolean _int64_ptr_equal(gconstpointer a, gconstpointer b) {
    return *(const int64_t *)a == *(const int64_t *)b;
}


/* ====================================================================== */
/* Public API                                                               */
/* ====================================================================== */

void trap_manager_init(void) {
    static gboolean once = FALSE;
    if (once) return;
    g_mutex_init(&g_trap_mutex);
    g_pending_traps = g_hash_table_new_full(
        _int64_ptr_hash, _int64_ptr_equal,
        g_free,    /* key destructor (heap int64) */
        NULL);     /* value je GINT_TO_POINTER konstantní */
    g_next_trap_id = 1;
    g_trap_initialized = TRUE;
    once = TRUE;
}


void trap_manager_shutdown(void) {
    if (!g_trap_initialized) return;
    g_mutex_lock(&g_trap_mutex);
    if (g_pending_traps) {
        g_hash_table_destroy(g_pending_traps);
        g_pending_traps = NULL;
    }
    g_mutex_unlock(&g_trap_mutex);
    g_mutex_clear(&g_trap_mutex);
    g_trap_initialized = FALSE;
}


int64_t trap_manager_register(void) {
    if (!g_trap_initialized) return 0;
    g_mutex_lock(&g_trap_mutex);
    int64_t id = g_next_trap_id++;
    if (g_next_trap_id <= 0) g_next_trap_id = 1; /* wrap ochrana, prakticky nemožné */
    int64_t *key = g_new0(int64_t, 1);
    *key = id;
    g_hash_table_insert(g_pending_traps, key, GINT_TO_POINTER(1));
    g_mutex_unlock(&g_trap_mutex);
    return id;
}


en_TRAP_ACTION trap_manager_parse_action(const char *str) {
    if (!str) return TRAP_ACTION_INVALID;
    if (strcmp(str, "continue")  == 0) return TRAP_ACTION_CONTINUE;
    if (strcmp(str, "step_into") == 0) return TRAP_ACTION_STEP_INTO;
    if (strcmp(str, "step_over") == 0) return TRAP_ACTION_STEP_OVER;
    if (strcmp(str, "abort")     == 0) return TRAP_ACTION_ABORT;
    return TRAP_ACTION_INVALID;
}


bool trap_manager_consume(int64_t trap_id) {
    if (!g_trap_initialized) return false;
    if (trap_id <= 0) return false;
    g_mutex_lock(&g_trap_mutex);
    int64_t key = trap_id;
    gboolean found = g_hash_table_remove(g_pending_traps, &key);
    g_mutex_unlock(&g_trap_mutex);
    return found ? true : false;
}


int trap_manager_pending_count(void) {
    if (!g_trap_initialized) return 0;
    g_mutex_lock(&g_trap_mutex);
    int n = g_pending_traps ? (int)g_hash_table_size(g_pending_traps) : 0;
    g_mutex_unlock(&g_trap_mutex);
    return n;
}


#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
