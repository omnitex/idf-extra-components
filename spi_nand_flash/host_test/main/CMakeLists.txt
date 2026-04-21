set(src "test_app_main.cpp")

if(CONFIG_NAND_FLASH_ENABLE_BDL)
    list(APPEND src "test_nand_flash_bdl.cpp")
else()
    list(APPEND src "test_nand_flash.cpp")
endif()

list(APPEND src "test_ecc_relief.cpp")

idf_component_register(SRCS ${src}
                       WHOLE_ARCHIVE
                       )

target_link_libraries(${COMPONENT_LIB} PRIVATE Catch2WithMain)
