idf_component_register(SRCS "perf_app_main.c"
                            "perf_benchmarks.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES spi_nand_flash esp_timer
                       )
