# SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize
import glob
from pathlib import Path


@pytest.mark.spi_nand_flash
@pytest.mark.skipif(
    not bool(glob.glob(f'{Path(__file__).parent.absolute()}/build*/')),
    reason="Skip the idf version that not build"
)
@idf_parametrize('target', ['esp32'], indirect=['target'])
def test_nand_flash_speedtest(dut: Dut) -> None:
    dut.expect_exact("Cache layers under test")
    dut.expect_exact("small files workload")
    dut.expect(r"sequential write\s+\d+ us")
    dut.expect(r"cold sequential read\s+\d+ us")
    dut.expect(r"warm repeat read\s+\d+ us")
    dut.expect_exact("large files workload")
    dut.expect(r"sequential write\s+\d+ us")
    dut.expect(r"cold sequential read\s+\d+ us")
    dut.expect(r"warm repeat read\s+\d+ us")
    dut.expect_exact("Benchmark done")
