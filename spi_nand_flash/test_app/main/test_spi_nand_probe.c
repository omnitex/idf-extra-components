/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Board bring-up: read the JEDEC ID behind every chip select of the selected board, without
 * initialising the driver, so chips missing from the driver's database still show up.
 */

#include <stdio.h>
#include "driver/spi_master.h"
#include "unity.h"
#include "test_nand_board.h"
#include "test_spi_nand_common.h"

TEST_CASE("probe JEDEC ID on each chip select", "[spi_nand_flash][nand_probe]")
{
    int answered = 0;
    for (size_t i = 0; i < nand_board_slot_count; i++) {
        const nand_slot_t *slot = &nand_board_slots[i];
        spi_device_handle_t spi;
        nand_board_select(slot, 0, 10 * 1000 * 1000, &spi);

        /* 9Fh, dummy byte, then manufacturer ID and up to two device ID bytes. */
        uint8_t tx[5] = { 0x9F };
        uint8_t rx[5] = { 0 };
        spi_transaction_t t = {
            .length = sizeof(tx) * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        esp_err_t ret = spi_device_transmit(spi, &t);
        spi_nand_flash_test_teardown();

        const bool dead = (rx[2] == 0x00 || rx[2] == 0xFF);
        printf("%-28s SPI%d CS GPIO%-2d -> %s %02X %02X %02X%s\n", slot->name, slot->host + 1, slot->cs,
               esp_err_to_name(ret), rx[2], rx[3], rx[4], dead ? "  (no response)" : "");
        if (ret == ESP_OK && !dead) {
            answered++;
        }
    }
    nand_board_release_cs();
    printf("%d of %d chip selects answered\n", answered, (int)nand_board_slot_count);
    TEST_ASSERT_GREATER_THAN(0, answered);
}
