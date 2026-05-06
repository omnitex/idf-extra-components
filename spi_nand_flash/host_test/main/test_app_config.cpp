/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstring>

extern "C" {
#include "../../ftl_eval/main/config/app_config.h"
}

#include <catch2/catch_test_macros.hpp>

TEST_CASE("app_config: parses preset and custom scenarios", "[app_config]")
{
    static const char *json = R"json(
{
  "name": "gc_factor_sweep",
  "output": "report.json",
  "nand": { "num_blocks": 1024, "pages_per_block": 64, "page_size": 2048 },
  "scenarios": [
    { "name": "fresh", "preset": "FRESH" },
    {
      "name": "custom_wearout",
      "max_erase_cycles": 500,
      "prog_fail_prob": 0.001,
      "erase_fail_prob": 0.0005,
      "crash_seed": 77,
      "ecc_fail_threshold": 10
    }
  ],
  "ftl_configs": [
    { "name": "dhara_gc3", "ftl": "dhara", "gc_factor": 3 }
  ],
  "workload": {
    "type": "sequential",
    "total_writes": 1000000,
    "write_size_bytes": 4096
  }
}
)json";

    sweep_config_t *cfg = app_config_parse(json);
    REQUIRE(cfg != nullptr);

    REQUIRE(std::strcmp(cfg->name, "gc_factor_sweep") == 0);
    REQUIRE(std::strcmp(cfg->output, "report.json") == 0);
    REQUIRE(cfg->nand.num_blocks == 1024);
    REQUIRE(cfg->nand.pages_per_block == 64);
    REQUIRE(cfg->nand.page_size == 2048);

    REQUIRE(cfg->scenario_count == 2);
    REQUIRE(std::strcmp(cfg->scenarios[0].name, "fresh") == 0);
    REQUIRE(std::strcmp(cfg->scenarios[0].preset, "FRESH") == 0);
    REQUIRE(cfg->scenarios[0].max_erase_cycles == 0);

    REQUIRE(std::strcmp(cfg->scenarios[1].name, "custom_wearout") == 0);
    REQUIRE(cfg->scenarios[1].preset == nullptr);
    REQUIRE(cfg->scenarios[1].max_erase_cycles == 500);
    REQUIRE(cfg->scenarios[1].prog_fail_prob > 0.0009);
    REQUIRE(cfg->scenarios[1].prog_fail_prob < 0.0011);
    REQUIRE(cfg->scenarios[1].erase_fail_prob > 0.0004);
    REQUIRE(cfg->scenarios[1].erase_fail_prob < 0.0006);
    REQUIRE(cfg->scenarios[1].crash_seed == 77);
    REQUIRE(cfg->scenarios[1].ecc_fail_threshold == 10);

    REQUIRE(cfg->ftl_config_count == 1);
    REQUIRE(std::strcmp(cfg->ftl_configs[0].name, "dhara_gc3") == 0);
    REQUIRE(std::strcmp(cfg->ftl_configs[0].ftl, "dhara") == 0);
    REQUIRE(cfg->ftl_configs[0].gc_ratio == 3);

    REQUIRE(std::strcmp(cfg->workload.type, "sequential") == 0);
    REQUIRE(cfg->workload.total_writes == 1000000);
    REQUIRE(cfg->workload.write_size_bytes == 4096);

    app_config_free(cfg);
}

TEST_CASE("app_config: rejects missing required sections", "[app_config]")
{
    static const char *json = R"json(
{
  "name": "broken",
  "output": "report.json",
  "nand": { "num_blocks": 1024, "pages_per_block": 64, "page_size": 2048 },
  "scenarios": [],
  "workload": {
    "type": "sequential",
    "total_writes": 1000000,
    "write_size_bytes": 4096
  }
}
)json";

    REQUIRE(app_config_parse(json) == nullptr);
    app_config_free(nullptr);
}
