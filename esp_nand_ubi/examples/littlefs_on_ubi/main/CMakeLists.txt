idf_component_register(SRCS "littlefs_on_ubi_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_nand_ubi spi_nand_flash joltwallet__littlefs
                       )
