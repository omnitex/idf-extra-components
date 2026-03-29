idf_component_register(SRCS "test_nand_flash.cpp" "test_app_main.cpp"
                             "test_backward_compat.cpp"
                             "test_advanced_types.cpp"
                             "test_advanced_init.cpp"
                       WHOLE_ARCHIVE
                       )

target_link_libraries(${COMPONENT_LIB} PRIVATE Catch2WithMain)
