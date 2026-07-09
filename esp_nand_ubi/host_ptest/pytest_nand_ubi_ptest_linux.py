# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
import glob
from pathlib import Path

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.host_test
@pytest.mark.skipif(
    not bool(glob.glob(f'{Path(__file__).parent.absolute()}/build*/')),
    reason="Skip the idf version that not build"
)
@idf_parametrize('target', ['linux'], indirect=['target'])
def test_nand_ubi_ptest_linux(dut: Dut) -> None:
    # Default run without arguments: 1000 random power cuts with seed 1.
    dut.expect(r'PASS seed=\d+ cycles=\d+', timeout=600)
