/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Separate-chaining hash table with power-of-two bucket count and
 * automatic rehashing when load factor is exceeded.
 *
 * Key type: uint32_t
 * Value:    flexible user payload (value_size bytes per node)
 */

#include "backends/hash_table.h"  /* resolved via component public include dir */
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static size_t next_pow2(size_t n)
{
    if (n == 0) {
        return 1;
    }
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

static inline size_t bucket_idx(uint32_t key, size_t capacity)
{
    /* Fibonacci hashing — good distribution for sequential keys */
    return (uint32_t)(key * 2654435761u) & (capacity - 1);
}

static hash_node_t *node_alloc(uint32_t key, size_t value_size)
{
    hash_node_t *n = calloc(1, sizeof(hash_node_t) + value_size);
    if (n) {
        n->key  = key;
        n->next = NULL;
    }
    return n;
}

/* Rehash: double capacity and re-insert all existing nodes. */
static bool rehash(hash_table_t *t)
{
    size_t new_cap = t->capacity * 2;
    hash_node_t **new_buckets = calloc(new_cap, sizeof(hash_node_t *));
    if (!new_buckets) {
        return false;
    }

    for (size_t i = 0; i < t->capacity; i++) {
        hash_node_t *node = t->buckets[i];
        while (node) {
            hash_node_t *next = node->next;
            size_t idx = bucket_idx(node->key, new_cap);
            node->next = new_buckets[idx];
            new_buckets[idx] = node;
            node = next;
        }
    }

    free(t->buckets);
    t->buckets  = new_buckets;
    t->capacity = new_cap;
    return true;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

hash_table_t *hash_table_create(size_t initial_capacity, size_t value_size, float load_factor)
{
    hash_table_t *t = calloc(1, sizeof(hash_table_t));
    if (!t) {
        return NULL;
    }

    size_t cap = next_pow2(initial_capacity < 1 ? 1 : initial_capacity);
    t->buckets = calloc(cap, sizeof(hash_node_t *));
    if (!t->buckets) {
        free(t);
        return NULL;
    }

    t->capacity    = cap;
    t->count       = 0;
    t->value_size  = value_size;
    t->load_factor = load_factor;
    return t;
}

void hash_table_destroy(hash_table_t *t)
{
    if (!t) {
        return;
    }
    for (size_t i = 0; i < t->capacity; i++) {
        hash_node_t *node = t->buckets[i];
        while (node) {
            hash_node_t *next = node->next;
            free(node);
            node = next;
        }
    }
    free(t->buckets);
    free(t);
}

hash_node_t *hash_table_get(hash_table_t *t, uint32_t key)
{
    size_t idx = bucket_idx(key, t->capacity);
    hash_node_t *node = t->buckets[idx];
    while (node) {
        if (node->key == key) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

hash_node_t *hash_table_get_or_insert(hash_table_t *t, uint32_t key)
{
    /* Check if already present */
    hash_node_t *existing = hash_table_get(t, key);
    if (existing) {
        return existing;
    }

    /* Rehash if needed before inserting */
    if ((float)(t->count + 1) > t->load_factor * (float)t->capacity) {
        if (!rehash(t)) {
            return NULL; /* OOM */
        }
    }

    hash_node_t *n = node_alloc(key, t->value_size);
    if (!n) {
        return NULL;
    }

    size_t idx = bucket_idx(key, t->capacity);
    n->next = t->buckets[idx];
    t->buckets[idx] = n;
    t->count++;
    return n;
}

void hash_table_remove(hash_table_t *t, uint32_t key)
{
    size_t idx = bucket_idx(key, t->capacity);
    hash_node_t **pp = &t->buckets[idx];
    while (*pp) {
        if ((*pp)->key == key) {
            hash_node_t *to_free = *pp;
            *pp = to_free->next;
            free(to_free);
            t->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

void hash_table_iterate(hash_table_t *t, hash_table_iter_cb_t cb, void *user_data)
{
    for (size_t i = 0; i < t->capacity; i++) {
        hash_node_t *node = t->buckets[i];
        while (node) {
            hash_node_t *next = node->next; /* safe if cb removes the node */
            if (!cb(node, user_data)) {
                return; /* early termination */
            }
            node = next;
        }
    }
}
