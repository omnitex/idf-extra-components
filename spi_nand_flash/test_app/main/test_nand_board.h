/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

/* NAND chip slots of the board selected in menuconfig (CONFIG_TEST_NAND_BOARD_*). */

#include <stddef.h>
#include <stdint.h>
#include "driver/spi_master.h"

typedef struct {
    const char *name;
    spi_host_device_t host;
    int cs;
    int clock_hz;   /* SPI clock for the driver; lower on long shared traces */
} nand_slot_t;

extern const nand_slot_t nand_board_slots[];
extern const size_t nand_board_slot_count;

/** Data pins of @p host on the selected board. */
spi_bus_config_t nand_board_bus_config(spi_host_device_t host);

/**
 * Set up the bus and device of slot @p slot, with every other chip select of the board held
 * high so only this chip drives MISO. spi_nand_flash_test_teardown() releases both.
 */
void nand_board_select(const nand_slot_t *slot, uint8_t flags, int clock_hz, spi_device_handle_t *spi);

/** Return all chip select pins of the board to their reset state. */
void nand_board_release_cs(void);
