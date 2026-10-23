idf_component_register(SRCS "nand_ubi_powercut_main.c" "ptest_stats.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_nand_ubi spi_nand_flash ptest esp_timer esp_app_format esp_driver_spi)

# Test app: reads esp_nand_ubi internals (PEB states, volume-table copies) for its
# post-reboot checks.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../src")
