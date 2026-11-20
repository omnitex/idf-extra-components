/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "driver/gpio.h"
#include "sdkconfig.h"
#include "soc/spi_pins.h"
#include "test_nand_board.h"
#include "test_spi_nand_common.h"

#if CONFIG_TEST_NAND_BOARD_MULTI_ESP32
#define MHZ  (1000 * 1000)

/* U1-U7 footprints share the HSPI (SPI2) bus; U8 is alone on VSPI (SPI3, the test app's HOST_ID).
 * Names follow the chip ID each chip select returns. CS3 (GPIO17) and CS6 (GPIO25) are
 * unpopulated; CS7 is an SD NAND the driver does not support. The shared HSPI bus runs at
 * 10 MHz: driver init failed to detect its chips at 40 MHz while the 10 MHz probe read them. */
const nand_slot_t nand_board_slots[] = {
    { "CS1 Micron MT29F2G01ABAGD",  SPI2_HOST, 15, 10 * MHZ },
    { "CS2 Zetta ZD35Q1GC",         SPI2_HOST, 16, 10 * MHZ },
    { "CS3 (unpopulated)",          SPI2_HOST, 17, 10 * MHZ },
    { "CS4 Winbond W25N01GV",       SPI2_HOST, 27, 10 * MHZ },
    { "CS5 Winbond W25N02KV",       SPI2_HOST,  0, 10 * MHZ },
    { "CS6 (unpopulated)",          SPI2_HOST, 25, 10 * MHZ },
    { "CS7 MK MKDV32GCL (SD NAND)", SPI2_HOST, 26, 10 * MHZ },
    { "CS_Q GigaDevice GD5F2GQ5UE", SPI3_HOST,  5, 40 * MHZ },
};
#else
const nand_slot_t nand_board_slots[] = {
    { "default", HOST_ID, PIN_CS, 40 * 1000 * 1000 },
};
#endif

const size_t nand_board_slot_count = sizeof(nand_board_slots) / sizeof(nand_board_slots[0]);

spi_bus_config_t nand_board_bus_config(spi_host_device_t host)
{
#if CONFIG_TEST_NAND_BOARD_MULTI_ESP32
    if (host == SPI2_HOST) {
        return (spi_bus_config_t) {
            .mosi_io_num = SPI2_IOMUX_PIN_NUM_MOSI,
            .miso_io_num = SPI2_IOMUX_PIN_NUM_MISO,
            .sclk_io_num = SPI2_IOMUX_PIN_NUM_CLK,
            .quadwp_io_num = SPI2_IOMUX_PIN_NUM_WP,
            .quadhd_io_num = SPI2_IOMUX_PIN_NUM_HD,
            .max_transfer_sz = 64,
        };
    }
#endif
    (void)host;
    return (spi_bus_config_t) {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadwp_io_num = PIN_WP,
        .quadhd_io_num = PIN_HD,
        .max_transfer_sz = 64,
    };
}

void nand_board_select(const nand_slot_t *slot, uint8_t flags, int clock_hz, spi_device_handle_t *spi)
{
    for (size_t i = 0; i < nand_board_slot_count; i++) {
        gpio_set_direction(nand_board_slots[i].cs, GPIO_MODE_OUTPUT);
        gpio_set_level(nand_board_slots[i].cs, 1);
    }
    spi_bus_config_t bus_cfg = nand_board_bus_config(slot->host);
    spi_nand_test_setup_bus(slot->host, &bus_cfg);
    spi_nand_test_add_device(slot->cs, flags, clock_hz, spi);
}

void nand_board_release_cs(void)
{
    for (size_t i = 0; i < nand_board_slot_count; i++) {
        gpio_reset_pin(nand_board_slots[i].cs);
    }
}
