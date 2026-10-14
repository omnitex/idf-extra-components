/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* Fault-injecting block device wrapper for host-side power-cut tests (host_test and
 * host_ptest). Host only; C++. */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <vector>

#include "esp_blockdev.h"

namespace nand_fault {

/* BDL wrapper that forwards to a real (emulated) NAND but fails selected writes/erases.
 * A write or erase whose start address lies in [fail_lo, fail_hi) fails while the
 * matching counter is non-zero; the first pass_writes matching writes still succeed.
 * A failing write lands on the inner device in full with program_before_fail, or only
 * its first partial_bytes otherwise.
 * Power-cut model: with die_on_fail, the first injected failure kills the device, and
 * with die_after_erase, the first matching erase completes and then kills it. A dead
 * device fails every operation until the test clears `dead` ("reboot").
 * Torn erase model: a PEB in torn_pebs reads back as erased, but every program into it
 * loses one bit until the PEB is erased again. torn_erases makes the next matching erases
 * leave the PEB torn (and kill the device if die_on_fail).
 * Counted cut: with cut_after >= 0, the wrapper lets cut_after matching writes/erases
 * through (all of them, or those cut_match accepts), then cuts power on the next one
 * as cut_type says and goes dead. ops counts every write/erase that reached the chip. */
enum class cut_kind {
    clean,   /* the operation does not happen */
    partial, /* a write programs a prefix of its data (length varies with the op count, so
              * short prefixes that tear small records also occur); an erase does not happen */
    torn,    /* an erase leaves the PEB torn; a write does not happen */
};

struct nand_op {
    bool is_erase;
    uint64_t addr;
    size_t len;
};
struct faulty_nand {
    esp_blockdev_t bdl;
    esp_blockdev_handle_t inner;
    uint64_t fail_lo = 0;
    uint64_t fail_hi = UINT64_MAX;
    int fail_writes = 0;
    int fail_erases = 0;
    bool program_before_fail = false;
    int pass_writes = 0;
    size_t partial_bytes = 0;
    bool die_on_fail = false;
    bool die_after_erase = false;
    bool dead = false;
    int torn_erases = 0;
    std::set<uint32_t> torn_pebs;
    long cut_after = -1;
    cut_kind cut_type = cut_kind::clean;
    std::function<bool(const nand_op &)> cut_match;
    uint64_t ops = 0;
    uint64_t reads = 0; /* reads that reached the chip */

    /* Returns true when this operation is the one to cut. */
    bool take_cut(const nand_op &op)
    {
        if (cut_after < 0 || (cut_match && !cut_match(op))) {
            return false;
        }
        if (cut_after > 0) {
            cut_after--;
            return false;
        }
        cut_after = -1;
        dead = true;
        return true;
    }

    uint32_t peb_of(uint64_t addr) const
    {
        return (uint32_t)(addr / inner->geometry.erase_size);
    }

    void fail_peb(uint32_t pnum)
    {
        fail_lo = (uint64_t)pnum * inner->geometry.erase_size;
        fail_hi = fail_lo + inner->geometry.erase_size;
    }
};

inline faulty_nand *faulty_of(esp_blockdev_handle_t h)
{
    return static_cast<faulty_nand *>(h->ctx);
}

inline esp_err_t faulty_read(esp_blockdev_handle_t h, uint8_t *dst, size_t dst_size, uint64_t addr, size_t len)
{
    if (faulty_of(h)->dead) {
        return ESP_FAIL;
    }
    faulty_of(h)->reads++;
    return faulty_of(h)->inner->ops->read(faulty_of(h)->inner, dst, dst_size, addr, len);
}

inline esp_err_t faulty_write(esp_blockdev_handle_t h, const uint8_t *src, uint64_t addr, size_t len)
{
    faulty_nand *f = faulty_of(h);
    if (f->dead) {
        return ESP_FAIL;
    }
    if (f->take_cut({false, addr, len})) {
        if (f->cut_type == cut_kind::partial) {
            /* Every 4th cut keeps fewer than 64 bytes, the rest anything up to len-1. */
            uint64_t mix = (f->ops + 1) * 0x9E3779B97F4A7C15ull;
            size_t prefix = (mix >> 32) % 4 == 0 ? (size_t)(mix % std::min<size_t>(len, 64))
                                                : (size_t)(mix % len);
            std::vector<uint8_t> part(len, 0xFF);
            memcpy(part.data(), src, prefix);
            (void)f->inner->ops->write(f->inner, part.data(), addr, len);
        }
        return ESP_FAIL;
    }
    f->ops++;
    if (f->fail_writes > 0 && addr >= f->fail_lo && addr < f->fail_hi) {
        if (f->pass_writes > 0) {
            f->pass_writes--;
            return f->inner->ops->write(f->inner, src, addr, len);
        }
        f->fail_writes--;
        if (f->program_before_fail) {
            (void)f->inner->ops->write(f->inner, src, addr, len);
        } else if (f->partial_bytes > 0) {
            (void)f->inner->ops->write(f->inner, src, addr, std::min(len, f->partial_bytes));
        }
        f->dead = f->die_on_fail;
        return ESP_FAIL;
    }
    if (f->torn_pebs.count(f->peb_of(addr)) != 0) {
        std::vector<uint8_t> weak(src, src + len);
        auto bit = std::find_if(weak.begin(), weak.end(), [](uint8_t b) {
            return b != 0;
        });
        if (bit != weak.end()) {
            *bit &= (uint8_t)(*bit - 1); /* clear the lowest set bit */
        }
        return f->inner->ops->write(f->inner, weak.data(), addr, len);
    }
    return f->inner->ops->write(f->inner, src, addr, len);
}

inline esp_err_t faulty_erase(esp_blockdev_handle_t h, uint64_t addr, size_t len)
{
    faulty_nand *f = faulty_of(h);
    if (f->dead) {
        return ESP_FAIL;
    }
    if (f->take_cut({true, addr, len})) {
        if (f->cut_type == cut_kind::torn) {
            (void)f->inner->ops->erase(f->inner, addr, len);
            f->torn_pebs.insert(f->peb_of(addr));
        }
        return ESP_FAIL;
    }
    f->ops++;
    if (f->die_after_erase && addr >= f->fail_lo && addr < f->fail_hi) {
        esp_err_t ret = f->inner->ops->erase(f->inner, addr, len);
        f->torn_pebs.erase(f->peb_of(addr));
        f->dead = true;
        return ret;
    }
    if (f->torn_erases > 0 && addr >= f->fail_lo && addr < f->fail_hi) {
        f->torn_erases--;
        esp_err_t ret = f->inner->ops->erase(f->inner, addr, len);
        f->torn_pebs.insert(f->peb_of(addr));
        f->dead = f->die_on_fail;
        return f->dead ? ESP_FAIL : ret;
    }
    if (f->fail_erases > 0 && addr >= f->fail_lo && addr < f->fail_hi) {
        f->fail_erases--;
        f->dead = f->die_on_fail;
        return ESP_FAIL;
    }
    esp_err_t ret = f->inner->ops->erase(f->inner, addr, len);
    if (ret == ESP_OK) {
        f->torn_pebs.erase(f->peb_of(addr));
    }
    return ret;
}

inline esp_err_t faulty_sync(esp_blockdev_handle_t h)
{
    return faulty_of(h)->inner->ops->sync(faulty_of(h)->inner);
}

inline esp_err_t faulty_ioctl(esp_blockdev_handle_t h, const uint8_t cmd, void *args)
{
    if (faulty_of(h)->dead) {
        return ESP_FAIL;
    }
    return faulty_of(h)->inner->ops->ioctl(faulty_of(h)->inner, cmd, args);
}

inline esp_err_t faulty_release(esp_blockdev_handle_t h)
{
    faulty_nand *f = faulty_of(h);
    esp_err_t ret = f->inner->ops->release(f->inner);
    delete f;
    return ret;
}

inline const esp_blockdev_ops_t faulty_ops = {
    faulty_read, faulty_write, faulty_erase, faulty_sync, faulty_ioctl, faulty_release,
};

/* Wraps inner. Releasing the returned handle also releases inner. */
inline faulty_nand *wrap_faulty_nand(esp_blockdev_handle_t inner)
{
    auto *f = new faulty_nand();
    f->inner = inner;
    f->bdl = *inner;
    f->bdl.ctx = f;
    f->bdl.ops = &faulty_ops;
    return f;
}

} // namespace nand_fault
