idf_component_register(SRCS "spi_nand_flash_fatfs_throughput_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES spi_nand_flash_fatfs esp_timer
                       )
