/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <cstdio>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "esp_nand_ubi.h"
#include "nand_ubi_test_helpers.h"

extern "C" {
#include "esp_nand_ubi_media.h"
#include "nand_ubi_eba.h"
}

using namespace nand_ubi_test;

TEST_CASE("esp_nand_ubi public header is usable", "[nand_ubi][skeleton]")
{
    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    REQUIRE(cfg.reserved_pebs == 4);
    REQUIRE(cfg.read_only == false);
}

TEST_CASE("on-flash header structs have the exact UBI layout", "[nand_ubi][media]")
{
    REQUIRE(sizeof(nand_ubi_ec_hdr_t) == 64);
    REQUIRE(sizeof(nand_ubi_vid_hdr_t) == 64);
    REQUIRE(sizeof(nand_ubi_vtbl_record_t) == 172);
    REQUIRE(offsetof(nand_ubi_ec_hdr_t, ec) == 8);
    REQUIRE(offsetof(nand_ubi_ec_hdr_t, image_seq) == 24);
    REQUIRE(offsetof(nand_ubi_ec_hdr_t, hdr_crc) == 60);
    REQUIRE(offsetof(nand_ubi_vid_hdr_t, vol_id) == 8);
    REQUIRE(offsetof(nand_ubi_vid_hdr_t, lnum) == 12);
    REQUIRE(offsetof(nand_ubi_vid_hdr_t, sqnum) == 40);
    REQUIRE(offsetof(nand_ubi_vid_hdr_t, hdr_crc) == 60);
    REQUIRE(offsetof(nand_ubi_vtbl_record_t, vol_type) == 12);
    REQUIRE(offsetof(nand_ubi_vtbl_record_t, name_len) == 14);
    REQUIRE(offsetof(nand_ubi_vtbl_record_t, name) == 16);
    REQUIRE(offsetof(nand_ubi_vtbl_record_t, flags) == 144);
    REQUIRE(offsetof(nand_ubi_vtbl_record_t, crc) == 168);
}

TEST_CASE("nand_ubi_crc32 matches the Linux UBI CRC", "[nand_ubi][crc]")
{
    // Standard CRC-32 of "123456789" is 0xCBF43926; the UBI variant (kernel
    // crc32_le, no trailing inversion) is the bitwise complement: 0x340BC6D9.
    // This proves byte-for-byte compatibility with mtd-utils ubinize.
    const char *check = "123456789";
    REQUIRE(nand_ubi_crc32(check, (uint32_t)strlen(check)) == 0x340BC6D9u);
}

TEST_CASE("EC header CRC matches an independently computed ubinize value", "[nand_ubi][crc]")
{
    nand_ubi_ec_hdr_t h;
    fill_ec_hdr(&h, 0xABCD1234u, 2048, 4096);
    // Reference value computed offline from the same field values.
    REQUIRE(__builtin_bswap32(h.hdr_crc) == 0xCB7E5895u);
}

TEST_CASE("VID header CRC matches an independently computed ubinize value", "[nand_ubi][crc]")
{
    nand_ubi_vid_hdr_t h;
    fill_vid_hdr(&h, 0, 0, 1);
    REQUIRE(__builtin_bswap32(h.hdr_crc) == 0xAE2772D2u);
}

TEST_CASE("valid EC header is accepted", "[nand_ubi][validate]")
{
    nand_ubi_ec_hdr_t h;
    fill_ec_hdr(&h, 0xABCD1234u, 2048, 4096);
    REQUIRE(nand_ubi_ec_hdr_valid(&h));
}

TEST_CASE("EC header with wrong magic is rejected", "[nand_ubi][validate]")
{
    nand_ubi_ec_hdr_t h;
    fill_ec_hdr(&h, 0xABCD1234u, 2048, 4096);
    h.magic = to_be32(0xDEADBEEFu);
    REQUIRE_FALSE(nand_ubi_ec_hdr_valid(&h));
}

TEST_CASE("EC header with corrupted CRC is rejected", "[nand_ubi][validate]")
{
    nand_ubi_ec_hdr_t h;
    fill_ec_hdr(&h, 0xABCD1234u, 2048, 4096);
    h.image_seq = to_be32(0x00000001u); // change a covered field without fixing CRC
    REQUIRE_FALSE(nand_ubi_ec_hdr_valid(&h));
}

TEST_CASE("valid VID header is accepted", "[nand_ubi][validate]")
{
    nand_ubi_vid_hdr_t h;
    fill_vid_hdr(&h, 0, 7, 42);
    REQUIRE(nand_ubi_vid_hdr_valid(&h));
}

TEST_CASE("VID header with wrong magic is rejected", "[nand_ubi][validate]")
{
    nand_ubi_vid_hdr_t h;
    fill_vid_hdr(&h, 0, 7, 42);
    h.magic = to_be32(UBI_EC_HDR_MAGIC); // right family, wrong header type
    REQUIRE_FALSE(nand_ubi_vid_hdr_valid(&h));
}

TEST_CASE("VID header with corrupted CRC is rejected", "[nand_ubi][validate]")
{
    nand_ubi_vid_hdr_t h;
    fill_vid_hdr(&h, 0, 7, 42);
    h.lnum = to_be32(8);
    REQUIRE_FALSE(nand_ubi_vid_hdr_valid(&h));
}

TEST_CASE("valid volume-table record is accepted", "[nand_ubi][validate]")
{
    nand_ubi_vtbl_record_t record;
    fill_vtbl_record(&record, "data", UBI_VID_DYNAMIC, 7);
    REQUIRE(nand_ubi_vtbl_record_valid(&record));

    record.reserved_pebs = to_be32(8);
    REQUIRE_FALSE(nand_ubi_vtbl_record_valid(&record));
}

TEST_CASE("CRC-valid empty Linux volume-table record is accepted", "[nand_ubi][validate]")
{
    nand_ubi_vtbl_record_t record {};
    record.crc = to_be32(nand_ubi_crc32(&record, UBI_VTBL_RECORD_SIZE_CRC));
    REQUIRE(nand_ubi_vtbl_record_valid(&record));
}

TEST_CASE("EBA alloc and free", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(16, 16, &eba) == ESP_OK);
    REQUIRE(eba.eba != nullptr);
    REQUIRE(eba.peb_state != nullptr);
    REQUIRE(eba.peb_count == 16);
    REQUIRE(eba.leb_count == 16);
    nand_ubi_eba_free(&eba);
    REQUIRE(eba.eba == nullptr);
    REQUIRE(eba.peb_state == nullptr);
}

TEST_CASE("EBA uses 16-bit entries: highest PEB round-trips, larger chips are refused", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(UBI_EBA_MAX_PEBS + 1, 4, &eba) == ESP_ERR_NOT_SUPPORTED);

    REQUIRE(nand_ubi_eba_alloc(UBI_EBA_MAX_PEBS, 4, &eba) == ESP_OK);
    nand_ubi_eba_set(&eba, 0, (int32_t)UBI_EBA_MAX_PEBS - 1);
    REQUIRE(nand_ubi_eba_get_pnum(&eba, 0) == (int32_t)UBI_EBA_MAX_PEBS - 1);
    nand_ubi_eba_set(&eba, 0, UBI_LEB_UNMAPPED);
    REQUIRE(nand_ubi_eba_get_pnum(&eba, 0) == UBI_LEB_UNMAPPED);
    nand_ubi_eba_free(&eba);
}

TEST_CASE("all PEBs free after alloc", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(8, 8, &eba) == ESP_OK);
    for (uint32_t pnum = 0; pnum < 8; pnum++) {
        REQUIRE(nand_ubi_eba_peb_is_free(&eba, pnum));
    }
    nand_ubi_eba_free(&eba);
}

TEST_CASE("all LEBs unmapped after alloc", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(8, 8, &eba) == ESP_OK);
    for (uint32_t lnum = 0; lnum < 8; lnum++) {
        REQUIRE(nand_ubi_eba_get_pnum(&eba, lnum) == UBI_LEB_UNMAPPED);
    }
    nand_ubi_eba_free(&eba);
}

TEST_CASE("EBA set and get round-trip", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(8, 8, &eba) == ESP_OK);
    nand_ubi_eba_set(&eba, 0, 3);
    nand_ubi_eba_set(&eba, 1, 7);
    REQUIRE(nand_ubi_eba_get_pnum(&eba, 0) == 3);
    REQUIRE(nand_ubi_eba_get_pnum(&eba, 1) == 7);
    REQUIRE(nand_ubi_eba_get_pnum(&eba, 2) == UBI_LEB_UNMAPPED);
    nand_ubi_eba_free(&eba);
}

TEST_CASE("PEB state transitions: free to used to free", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(4, 4, &eba) == ESP_OK);
    REQUIRE(nand_ubi_eba_peb_is_free(&eba, 2));
    nand_ubi_eba_peb_set_used(&eba, 2);
    REQUIRE_FALSE(nand_ubi_eba_peb_is_free(&eba, 2));
    nand_ubi_eba_peb_set_free(&eba, 2);
    REQUIRE(nand_ubi_eba_peb_is_free(&eba, 2));
    nand_ubi_eba_free(&eba);
}

TEST_CASE("bad PEB is not free", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(4, 4, &eba) == ESP_OK);
    nand_ubi_eba_peb_set_bad(&eba, 1);
    REQUIRE_FALSE(nand_ubi_eba_peb_is_free(&eba, 1));
    REQUIRE(nand_ubi_eba_peb_is_free(&eba, 0));
    REQUIRE(nand_ubi_eba_peb_is_free(&eba, 2));
    nand_ubi_eba_free(&eba);
}

TEST_CASE("erase-pending PEB is not free", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(4, 4, &eba) == ESP_OK);
    nand_ubi_eba_peb_set_used(&eba, 1);
    nand_ubi_eba_peb_set_erase_pending(&eba, 1);
    REQUIRE_FALSE(nand_ubi_eba_peb_is_free(&eba, 1));
    REQUIRE(nand_ubi_eba_peb_is_free(&eba, 0));
    REQUIRE(nand_ubi_eba_peb_is_free(&eba, 2));
    nand_ubi_eba_free(&eba);
}

TEST_CASE("sample_free returns distinct free PEBs only", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(64, 64, &eba) == ESP_OK);
    for (uint32_t pnum = 0; pnum < 64; pnum++) {
        if (pnum % 3 != 0) {
            pnum % 2 ? nand_ubi_eba_peb_set_used(&eba, pnum) : nand_ubi_eba_peb_set_bad(&eba, pnum);
        }
    }
    REQUIRE(eba.free_count == 22);
    for (int round = 0; round < 100; round++) {
        int32_t cand[4];
        REQUIRE(nand_ubi_eba_sample_free(&eba, 0, cand, 4) == 4);
        for (int i = 0; i < 4; i++) {
            REQUIRE(nand_ubi_eba_peb_is_free(&eba, (uint32_t)cand[i]));
            for (int j = 0; j < i; j++) {
                REQUIRE(cand[i] != cand[j]);
            }
        }
    }
    nand_ubi_eba_free(&eba);
}

TEST_CASE("sample_free returns every free PEB when fewer than k are free", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(8, 8, &eba) == ESP_OK);
    for (uint32_t pnum = 0; pnum < 8; pnum++) {
        if (pnum != 2 && pnum != 6) {
            nand_ubi_eba_peb_set_used(&eba, pnum);
        }
    }
    int32_t cand[4];
    REQUIRE(nand_ubi_eba_sample_free(&eba, 0, cand, 4) == 2);
    REQUIRE(cand[0] == 2);
    REQUIRE(cand[1] == 6);

    nand_ubi_eba_peb_set_used(&eba, 2);
    nand_ubi_eba_peb_set_used(&eba, 6);
    REQUIRE(eba.free_count == 0);
    REQUIRE(nand_ubi_eba_sample_free(&eba, 0, cand, 4) == 0);
    nand_ubi_eba_free(&eba);
}

TEST_CASE("sample_free finds the last free PEBs on a nearly full chip", "[nand_ubi][eba]")
{
    /* 5 free out of 4096: rejection sampling alone would almost always give up. */
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(4096, 16, &eba) == ESP_OK);
    for (uint32_t pnum = 0; pnum < 4096; pnum++) {
        if (pnum % 800 != 7) {
            nand_ubi_eba_peb_set_used(&eba, pnum);
        }
    }
    REQUIRE(eba.free_count == 6);
    int32_t cand[4];
    REQUIRE(nand_ubi_eba_sample_free(&eba, 0, cand, 4) == 4);
    for (int i = 0; i < 4; i++) {
        REQUIRE(cand[i] % 800 == 7);
    }
    nand_ubi_eba_free(&eba);
}

TEST_CASE("sample_free never returns a PEB below the lower bound", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(64, 64, &eba) == ESP_OK);
    for (int round = 0; round < 200; round++) {
        int32_t cand[4];
        REQUIRE(nand_ubi_eba_sample_free(&eba, 16, cand, 4) == 4);
        for (int i = 0; i < 4; i++) {
            REQUIRE(cand[i] >= 16);
        }
    }
    /* Only PEBs 0..15 and 40 free: the bound leaves exactly one. */
    for (uint32_t p = 16; p < 64; p++) {
        if (p != 40) {
            nand_ubi_eba_peb_set_used(&eba, p);
        }
    }
    int32_t cand[4];
    REQUIRE(nand_ubi_eba_sample_free(&eba, 16, cand, 4) == 1);
    REQUIRE(cand[0] == 40);
    nand_ubi_eba_free(&eba);
}

TEST_CASE("free_count follows every state change", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(8, 8, &eba) == ESP_OK);
    REQUIRE(eba.free_count == 8);
    nand_ubi_eba_peb_set_used(&eba, 0);
    nand_ubi_eba_peb_set_used(&eba, 0);
    nand_ubi_eba_peb_set_bad(&eba, 1);
    nand_ubi_eba_peb_set_erase_pending(&eba, 2);
    REQUIRE(eba.free_count == 5);
    nand_ubi_eba_peb_set_erase_pending(&eba, 0);
    nand_ubi_eba_peb_set_free(&eba, 2);
    nand_ubi_eba_peb_set_free(&eba, 2);
    REQUIRE(eba.free_count == 6);
    nand_ubi_eba_free(&eba);
}

TEST_CASE("peb_state 2-bit packing does not bleed between neighbours", "[nand_ubi][eba]")
{
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(16, 16, &eba) == ESP_OK);
    for (uint32_t pnum = 0; pnum < 16; pnum += 2) {
        nand_ubi_eba_peb_set_bad(&eba, pnum);
    }
    for (uint32_t pnum = 0; pnum < 16; pnum++) {
        if (pnum % 2 == 0) {
            REQUIRE_FALSE(nand_ubi_eba_peb_is_free(&eba, pnum));
        } else {
            REQUIRE(nand_ubi_eba_peb_is_free(&eba, pnum));
        }
    }
    nand_ubi_eba_free(&eba);
}

/* Churn on a chip where most PEBs hold data: each step erases a random mapped PEB and
 * allocates the least-erased of k sampled free PEBs (k >= free PEBs: the exact minimum).
 * Returns max - min erase count. */
static uint32_t churn_wear_gap(uint32_t k, uint32_t seed)
{
    constexpr uint32_t kPebs = 512, kMapped = 448, kSteps = kPebs * 300;
    nand_ubi_eba_t eba;
    REQUIRE(nand_ubi_eba_alloc(kPebs, kPebs, &eba) == ESP_OK);
    nand_ubi_eba_seed(&eba, seed);
    std::vector<uint32_t> ec(kPebs, 0);
    std::vector<uint32_t> mapped;
    for (uint32_t p = 0; p < kMapped; p++) {
        nand_ubi_eba_peb_set_used(&eba, p);
        mapped.push_back(p);
    }
    uint32_t rng = seed * 2654435761u + 1u;
    for (uint32_t step = 0; step < kSteps; step++) {
        rng = rng * 1664525u + 1013904223u;
        uint32_t slot = (rng >> 8) % kMapped;
        ec[mapped[slot]]++;
        nand_ubi_eba_peb_set_free(&eba, mapped[slot]);

        int32_t cand[kPebs - kMapped];
        uint32_t n = nand_ubi_eba_sample_free(&eba, 0, cand, k);
        REQUIRE(n == k);
        int32_t best = cand[0];
        for (uint32_t i = 1; i < n; i++) {
            if (ec[cand[i]] < ec[best]) {
                best = cand[i];
            }
        }
        nand_ubi_eba_peb_set_used(&eba, (uint32_t)best);
        mapped[slot] = (uint32_t)best;
    }
    nand_ubi_eba_free(&eba);
    auto [lo, hi] = std::minmax_element(ec.begin(), ec.end());
    return *hi - *lo;
}

TEST_CASE("wear: lowest-of-k sampling keeps the erase-count spread small", "[nand_ubi][eba][ec]")
{
    uint32_t gap1 = churn_wear_gap(1, 1);
    uint32_t gap2 = churn_wear_gap(2, 1);
    uint32_t gap4 = churn_wear_gap(4, 1);
    uint32_t gap8 = churn_wear_gap(8, 1);
    uint32_t gapx = churn_wear_gap(64, 1);
    printf("wear gap over 512 PEBs, 300 erases each: k=1 %u, k=2 %u, k=4 %u, k=8 %u, exact %u\n",
           (unsigned)gap1, (unsigned)gap2, (unsigned)gap4, (unsigned)gap8, (unsigned)gapx);
    /* Even the exact minimum leaves a gap here: a PEB that stays mapped for a long, random
     * time cannot be picked meanwhile. Seeds 1..3 gave k=1 89..100, k=2 38..51, k=4 26..37,
     * exact 17..22. */
    REQUIRE(gap2 * 3 < gap1 * 2);
    REQUIRE(gap4 * 2 < gap1);
    REQUIRE(gap4 <= gapx * 2);
}
