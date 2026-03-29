/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A node in the hash table storing one key-value pair.
 *
 * The `data` field is a flexible array member sized at creation time
 * (value_size bytes, set in hash_table_create).
 */
typedef struct hash_node {
    uint32_t        key;
    struct hash_node *next;  /* chaining for collision resolution */
    uint8_t         data[];  /* value_size bytes of user payload */
} hash_node_t;

/**
 * @brief Open-addressed (separate chaining) hash table with power-of-two
 *        capacity and automatic rehashing.
 *
 * Fields are intentionally public so tests can inspect count / capacity.
 */
typedef struct hash_table {
    hash_node_t **buckets;   /* array of bucket head pointers, length=capacity */
    size_t        capacity;  /* current number of buckets (always power of two) */
    size_t        count;     /* number of entries currently stored */
    size_t        value_size;/* size of each node's data payload in bytes */
    float         load_factor; /* rehash threshold (e.g. 0.75) */
} hash_table_t;

/** Callback type for hash_table_iterate.  Return true to continue, false to stop early. */
typedef bool (*hash_table_iter_cb_t)(hash_node_t *node, void *user_data);

/**
 * @brief  Create a new hash table.
 *
 * @param  initial_capacity  Minimum number of buckets (rounded up to power of two).
 * @param  value_size        Size in bytes of the user-payload stored in each node.
 * @param  load_factor       Threshold (0 < lf < 1) at which the table rehashes.
 * @return Pointer to the new hash_table_t, or NULL on OOM.
 */
hash_table_t *hash_table_create(size_t initial_capacity, size_t value_size, float load_factor);

/**
 * @brief  Destroy a hash table and free all memory (nodes + table struct).
 */
void hash_table_destroy(hash_table_t *t);

/**
 * @brief  Look up a key.  Returns the node if found, NULL otherwise.
 */
hash_node_t *hash_table_get(hash_table_t *t, uint32_t key);

/**
 * @brief  Look up a key; insert a zero-initialised node if not present.
 *
 * May trigger a rehash.  Returns the (existing or newly created) node,
 * or NULL on OOM.
 */
hash_node_t *hash_table_get_or_insert(hash_table_t *t, uint32_t key);

/**
 * @brief  Remove the entry with the given key (no-op if not found).
 */
void hash_table_remove(hash_table_t *t, uint32_t key);

/**
 * @brief  Iterate over every entry, calling cb(node, user_data).
 *         Stops early if cb returns false.
 */
void hash_table_iterate(hash_table_t *t, hash_table_iter_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
