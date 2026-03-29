/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Phase 2, T1a–T5a: Hash table core tests.
 *
 * Tests written first (RED) before any implementation exists.
 * Tags: [advanced][hash-table]
 */

#include "backends/hash_table.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

/* --------------------------------------------------------------------------
 * P2.T1: create / destroy
 * -------------------------------------------------------------------------- */

TEST_CASE("hash_table_create returns non-null and hash_table_destroy frees it",
          "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(16, sizeof(uint32_t), 0.75f);
    REQUIRE(t != NULL);
    REQUIRE(t->count == 0);
    REQUIRE(t->capacity >= 16);
    hash_table_destroy(t);
}

/* --------------------------------------------------------------------------
 * P2.T2: insert and lookup
 * -------------------------------------------------------------------------- */

TEST_CASE("insert 100 entries, all retrievable by key", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(8, sizeof(uint32_t), 0.75f);
    REQUIRE(t != NULL);

    for (uint32_t i = 0; i < 100; i++) {
        hash_node_t *n = hash_table_get_or_insert(t, i);
        REQUIRE(n != NULL);
        *(uint32_t *)n->data = i * 10;
    }

    REQUIRE(t->count == 100);

    for (uint32_t i = 0; i < 100; i++) {
        hash_node_t *n = hash_table_get(t, i);
        REQUIRE(n != NULL);
        REQUIRE(*(uint32_t *)n->data == i * 10);
    }

    hash_table_destroy(t);
}

TEST_CASE("inserting duplicate key returns the same node", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(8, sizeof(uint32_t), 0.75f);
    hash_node_t *a = hash_table_get_or_insert(t, 42);
    hash_node_t *b = hash_table_get_or_insert(t, 42);
    REQUIRE(a == b);
    REQUIRE(t->count == 1);
    hash_table_destroy(t);
}

/* --------------------------------------------------------------------------
 * P2.T3: automatic rehashing
 * -------------------------------------------------------------------------- */

TEST_CASE("insert 1000 entries triggers rehash, all still retrievable",
          "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(4, sizeof(uint32_t), 0.75f);
    size_t initial_cap = t->capacity;

    for (uint32_t i = 0; i < 1000; i++) {
        hash_node_t *n = hash_table_get_or_insert(t, i);
        REQUIRE(n != NULL);
    }

    REQUIRE(t->capacity > initial_cap); // rehash occurred
    REQUIRE(t->count == 1000);

    for (uint32_t i = 0; i < 1000; i++) {
        REQUIRE(hash_table_get(t, i) != NULL);
    }
    hash_table_destroy(t);
}

/* --------------------------------------------------------------------------
 * P2.T4: removal
 * -------------------------------------------------------------------------- */

TEST_CASE("remove an entry, it is no longer found", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(8, sizeof(uint32_t), 0.75f);
    hash_table_get_or_insert(t, 7);
    hash_table_get_or_insert(t, 14); // same bucket if capacity=8, tests chain removal
    hash_table_remove(t, 7);
    REQUIRE(hash_table_get(t, 7)  == NULL);
    REQUIRE(hash_table_get(t, 14) != NULL); // neighbor unaffected
    hash_table_destroy(t);
}

/* --------------------------------------------------------------------------
 * P2.T5: iteration
 * -------------------------------------------------------------------------- */

TEST_CASE("iterate visits all 50 inserted entries", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(8, sizeof(uint32_t), 0.75f);
    for (uint32_t i = 0; i < 50; i++) {
        hash_table_get_or_insert(t, i);
    }

    int visited = 0;
    hash_table_iterate(t, [](hash_node_t *node, void *ud) -> bool {
        (void)node;
        (*(int *)ud)++;
        return true; // continue
    }, &visited);

    REQUIRE(visited == 50);

    // Early termination: stop after 10
    int early = 0;
    hash_table_iterate(t, [](hash_node_t *node, void *ud) -> bool {
        (void)node;
        return ++(*(int *)ud) < 10;
    }, &early);
    REQUIRE(early == 10);

    hash_table_destroy(t);
}
