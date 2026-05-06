set(srcs
    "api_contract_test.c"
    "main.c"
    "config/app_config.c"
    "workload/workload.c"
    "workload/workload_sequential.c"
    "workload/workload_random.c"
    "workload/workload_mixed.c"
    "workload/workload_read_loop.c"
    "workload/workload_zipf.c"
    "metrics/metrics.c"
    "runner/runner.c"
    "reporter/reporter_json.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "config" "workload" "metrics" "runner" "reporter"
    REQUIRES spi_nand_flash
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
