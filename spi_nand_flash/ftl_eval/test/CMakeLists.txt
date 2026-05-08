set(srcs "test_main.cpp")

# Fault-sim tests (always included — fault sim is always on in ftl_eval)
list(APPEND srcs
    "dhara_ftl.cpp"
    "test_fault_sim.cpp"
    "test_ftl_robustness.cpp"
)

# Page relief test requires BDL
if(CONFIG_NAND_FLASH_ENABLE_BDL)
    list(APPEND srcs "test_page_relief.cpp")
endif()

# app_config parser tests
list(APPEND srcs "test_app_config.cpp")

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "."
    WHOLE_ARCHIVE
)

target_link_libraries(${COMPONENT_LIB} PRIVATE Catch2WithMain)

# Tests need priv_include for nand_impl.h
target_include_directories(${COMPONENT_LIB} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../priv_include
)
