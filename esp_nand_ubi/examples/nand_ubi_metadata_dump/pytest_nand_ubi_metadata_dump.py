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
def test_nand_ubi_metadata_dump(dut) -> None:
    dut.expect(r"Scanning \d+ PEBs for UBI EC/VID metadata \(read-only\)")
    dut.expect_exact("=== UBI metadata dump summary ===")
    dut.expect(r"counts: EMPTY=\d+ FREE=\d+ MAPPED=\d+ CORRUPT=\d+ BAD=\d+ IO_ERR=\d+")
    dut.expect_exact("NAND UBI metadata dump finished successfully")
    dut.expect_exact("Returned from app_main")
