idf_component_register(SRCS "nand_ubi_example_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_nand_ubi spi_nand_flash
                       )
