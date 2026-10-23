idf_component_register(SRCS "ptest_ubi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_blockdev
                       PRIV_REQUIRES esp_rom)
