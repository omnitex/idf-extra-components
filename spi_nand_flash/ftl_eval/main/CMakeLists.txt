set(srcs
    "api_contract_test.c"
    "main.c"
    "config/app_config.c"
    "ftl/ftl.c"
    "ftl/dhara_ftl.c"
    "workload/workload.c"
    "workload/workload_sequential.c"
    "workload/workload_random.c"
    "workload/workload_mixed.c"
    "metrics/metrics.c"
    "runner/runner.c"
    "reporter/reporter_json.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "config" "ftl" "workload" "metrics" "runner" "reporter"
    PRIV_INCLUDE_DIRS "../../priv_include"
    REQUIRES spi_nand_flash
)
