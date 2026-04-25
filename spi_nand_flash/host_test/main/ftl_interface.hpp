/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <cstddef>

/**
 * Abstract base class for FTL implementations in host tests.
 * Subclass to add a new FTL; the robustness test suite works unchanged.
 */
class FTLInterface {
public:
    virtual ~FTLInterface() = default;

    virtual int mount()   = 0;
    virtual int unmount() = 0;

    virtual int read(uint32_t lba, void *buf, size_t size)        = 0;
    virtual int write(uint32_t lba, const void *buf, size_t size) = 0;
    virtual int sync()  = 0;

    virtual uint32_t num_sectors() const = 0;
    virtual uint32_t sector_size() const = 0;
};
