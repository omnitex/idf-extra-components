set(src "test_app_main.cpp")

if(CONFIG_NAND_FLASH_FAULT_SIM)
    list(APPEND src
        "nand_fault_sim.c"
        "dhara_ftl.cpp"
        "test_fault_sim.cpp"
        "test_ftl_robustness.cpp"
    )
    if(NOT CONFIG_NAND_FLASH_ENABLE_BDL)
        list(APPEND src "test_nand_flash.cpp" "test_nand_flash_ftl.cpp")
    else()
        list(APPEND src "test_nand_flash_bdl.cpp")
    endif()
elseif(CONFIG_NAND_FLASH_ENABLE_BDL)
    list(APPEND src "test_nand_flash_bdl.cpp")
else()
    list(APPEND src "test_nand_flash.cpp" "test_nand_flash_ftl.cpp")
endif()

idf_component_register(SRCS ${src}
                       WHOLE_ARCHIVE
                       )

target_link_libraries(${COMPONENT_LIB} PRIVATE Catch2WithMain)

if(CONFIG_NAND_FLASH_FAULT_SIM)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../priv_include
    )
endif()
