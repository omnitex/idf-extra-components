| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |

# NAND UBI Power-Cut Test

Test app that interrupts `esp_nand_ubi` at random points and checks, on every boot,
that no committed data was lost and that UBI's own state is consistent. Design:
[`docs/plans/2026-10-08-esp-nand-ubi-powercut-stress-design.md`](../../../docs/plans/2026-10-08-esp-nand-ubi-powercut-stress-design.md).

> **The app erases the whole NAND chip** at the start of each campaign. Use a chip
> that holds nothing you need.

## What one boot does

1. Attaches UBI on the raw NAND.
2. Checks UBI's state: both volume-table copies valid, no PEB left erase-pending,
   and every used PEB is a volume-table copy or mapped to one LEB.
3. Verifies the `ptest_data` volume against its journal (the `ptest` workload from
   `esp_nand_ubi/test_support/ptest`): every committed LEB rewrite is intact, and
   only the rewrite interrupted by the last reset may be incomplete.
4. Appends a record with the results to the `ptest_stats` volume and prints the
   last ten records and the campaign totals.
5. Runs the workload (LEB rewrites) until it is interrupted.

If any check fails, the app prints `HALT` and stops, so the failure stays on screen
and in the stats.

## Campaigns

A campaign is all boots of one build. The campaign ID is the start of the app's
ELF SHA-256. The first boot of a new build erases the chip and creates the two
volumes; later boots of the same build continue the campaign. To start over with
the same configuration, rebuild after any change (for example a different reset
delay), or erase the chip.

## Interrupting the workload

`idf.py menuconfig` -> "NAND UBI power-cut test":

- Auto (default): the app calls `esp_restart()` after a random delay between
  `PTEST_RESET_MIN_MS` and `PTEST_RESET_MAX_MS`. Runs unattended. The NAND stays
  powered, so a program or erase already started on the chip completes; the reset
  can still land between any two flash operations of a UBI update.
- Manual: the workload runs until you remove power, for example by unplugging the
  USB cable. This is the only mode that can interrupt a program or erase inside the
  chip. Reconnect and open the monitor again; the next boot reports the result.

`PTEST_DATA_LEBS` and `PTEST_PAGES_PER_STEP` set the workload size. Every boot
reads all pages of the data LEBs, so larger values make boots slower.

## Hardware and pins

Same wiring as the other `esp_nand_ubi` examples: SPI3 IOMUX pins on ESP32, SPI2
IOMUX pins on other targets (on ESP32-S3: CS 10, MOSI 11, CLK 12, MISO 13).

## Running

```bash
idf.py set-target esp32s3
idf.py -p PORT flash monitor
```

## Reading the output

```
 boot reset     next_ms attach_ms verify_ms match lost corr inflt jtorn  free bad mean_ec
  140 SW           1204      3481      1690    32    0    0     1     0  3935   1       3
  141 SW            377      3479      1702    31    0    0     1     1  3934   1       3
I (5236) ptest: boot 141: attach 3479 ms, verify 1702 ms, OK
I (5236) ptest: campaign totals over 141 boots: 0 failures, 0 lost, 0 corrupt, 97 in-flight rewrites
```

| Column | Meaning |
|---|---|
| `reset` | reset reason of that boot (`SW` for auto resets, `POWERON` after unplugging) |
| `next_ms` | auto-reset delay chosen for that boot |
| `match` | data LEBs exactly as the journal says |
| `lost`, `corr` | committed data missing or damaged; must stay 0 |
| `inflt` | 1 if the previous interruption hit a LEB rewrite (expected, not a failure) |
| `jtorn` | journal records torn by an interruption (expected) |
| `free`, `bad` | free and bad PEBs after attach; `free` should not drift down over a campaign |
| `mean_ec` | mean erase count |
