set(src "test_app_main.cpp"
        "test_nand_ubi.cpp"
        "test_nand_ubi_attach.cpp"
        "test_nand_ubi_volume.cpp"
        "test_nand_ubi_rw.cpp"
        "test_nand_ubi_leak.cpp")

idf_component_register(SRCS ${src}
                       WHOLE_ARCHIVE
                       )

target_link_libraries(${COMPONENT_LIB} PRIVATE Catch2WithMain)

# White-box tests reach into the component's private headers (device/EBA state),
# which live in its src/ directory. Media layout and CRC helpers are public
# (esp_nand_ubi_media.h) via the component's INCLUDE_DIRS.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src")

# AddressSanitizer, applied to both this test binary and the esp_nand_ubi component
# lib itself (where the nand_ubi_device_t/EBA/page_buf allocations happen) - matches
# the pattern used by ESP-IDF's own tcp_transport and mqtt host_test apps. On Linux,
# LeakSanitizer runs automatically at process exit and backs the "release: all memory
# freed" checks in test_nand_ubi_leak.cpp; on macOS, Apple Clang's ASAN runtime does
# not implement LeakSanitizer (invalid-access checks still apply), so a real leak
# verdict requires running this binary on Linux (matches CI's run-target-linux job).
target_compile_options(${COMPONENT_LIB} PUBLIC -fsanitize=address)
target_link_options(${COMPONENT_LIB} PUBLIC -fsanitize=address)

idf_component_get_property(nand_ubi_lib esp_nand_ubi COMPONENT_LIB)
target_compile_options(${nand_ubi_lib} PUBLIC -fsanitize=address)
target_link_options(${nand_ubi_lib} PUBLIC -fsanitize=address)
