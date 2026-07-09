# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
import pytest
from pytest_embedded_idf.utils import idf_parametrize
import glob
from pathlib import Path


@pytest.mark.spi_nand_flash
@pytest.mark.skipif(
    not bool(glob.glob(f'{Path(__file__).parent.absolute()}/build*/')),
    reason="Skip the idf version that not build"
)
@idf_parametrize('target', ['esp32'], indirect=['target'])
def test_nand_ubi_example(dut) -> None:
    dut.expect_exact("Attaching NAND UBI volume 0 (raw BDL, not Dhara WL)")
    dut.expect_exact("UBI volume ready")
    for lnum in range(4):
        dut.expect(rf"LEB {lnum}: write/read-back verified OK")
    dut.expect_exact("LEB 0 read after erase correctly returned ESP_ERR_NOT_FOUND (unmapped)")
    dut.expect_exact("NAND UBI example finished successfully")
    dut.expect_exact("Returned from app_main")
