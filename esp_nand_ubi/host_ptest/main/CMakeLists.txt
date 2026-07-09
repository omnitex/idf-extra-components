idf_component_register(SRCS "host_ptest_main.cpp"
                       PRIV_REQUIRES ptest
                       WHOLE_ARCHIVE)

# Reads device internals (PEB states, volume-table copies) for its post-reboot checks.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")

# AddressSanitizer on the soak binary and the code under test, as in host_test.
target_compile_options(${COMPONENT_LIB} PUBLIC -fsanitize=address)
target_link_options(${COMPONENT_LIB} PUBLIC -fsanitize=address)
idf_component_get_property(nand_ubi_lib esp_nand_ubi COMPONENT_LIB)
target_compile_options(${nand_ubi_lib} PUBLIC -fsanitize=address)
idf_component_get_property(ptest_lib ptest COMPONENT_LIB)
target_compile_options(${ptest_lib} PUBLIC -fsanitize=address)
