/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ftl_interface.hpp"
#include "nand_fault_sim.h"

extern "C" {
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include "esp_err.h"
}

#include <cassert>

class DharaFTL : public FTLInterface {
public:
    DharaFTL(nand_file_mmap_emul_config_t emul_cfg, uint8_t gc_factor = 0)
        : m_emul_cfg(emul_cfg), m_gc_factor(gc_factor), m_dev(nullptr),
          m_sectors(0), m_sector_size(0)
    {
    }

    ~DharaFTL() override
    {
        if (m_dev) {
            unmount();
        }
    }

    int mount() override
    {
        spi_nand_flash_config_t cfg = {&m_emul_cfg, m_gc_factor,
                                       SPI_NAND_IO_MODE_SIO, 0};
        esp_err_t err = spi_nand_flash_init_device(&cfg, &m_dev);
        if (err != ESP_OK) {
            return err;
        }
        (void)spi_nand_flash_get_capacity(m_dev, &m_sectors);
        (void)spi_nand_flash_get_sector_size(m_dev, &m_sector_size);
        return ESP_OK;
    }

    int unmount() override
    {
        if (!m_dev) {
            return ESP_OK;
        }
        esp_err_t err = spi_nand_flash_deinit_device(m_dev);
        m_dev = nullptr;
        return err;
    }

    int read(uint32_t lba, void *buf, size_t size) override
    {
        assert(m_dev && size == m_sector_size);
        return spi_nand_flash_read_page(m_dev, static_cast<uint8_t *>(buf), lba);
    }

    int write(uint32_t lba, const void *buf, size_t size) override
    {
        assert(m_dev && size == m_sector_size);
        return spi_nand_flash_write_page(m_dev, static_cast<const uint8_t *>(buf), lba);
    }

    int sync() override
    {
        assert(m_dev);
        return spi_nand_flash_sync(m_dev);
    }

    uint32_t num_sectors() const override { return m_sectors; }
    uint32_t sector_size() const override { return m_sector_size; }

private:
    nand_file_mmap_emul_config_t m_emul_cfg;
    uint8_t                      m_gc_factor;
    spi_nand_flash_device_t     *m_dev;
    uint32_t                     m_sectors;
    uint32_t                     m_sector_size;
};
