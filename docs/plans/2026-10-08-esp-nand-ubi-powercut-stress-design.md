# `esp_nand_ubi` power-cut stress testing: design

## Goal

Show that `esp_nand_ubi`, alone and with LittleFS on top, loses no synced data and
stays mountable when power is lost at any point. Two test drivers share one
implementation of the workloads and checks:

- a host harness on the emulated NAND, which cuts at chosen operations and models
  torn programs and erases
- a board app on real hardware, which resets itself at random times or is unplugged
  by hand

## Terms

Cut
: The end of execution at an arbitrary point. On the host, the fault wrapper stops
  passing operations through and every later call fails until the harness reboots
  the device. On the board, a reset or a power loss.

Reboot
: Attach UBI (and mount LittleFS in `lfs` mode) on the state the cut left behind,
  run the checks, then continue the workload.

Acked
: An operation whose durability call returned success before the cut: a completed
  LEB write sequence recorded in the journal (`ubi` mode), or a successful `fsync()`
  (`lfs` mode). Acked data must survive every cut.

Indeterminate
: An operation that was in progress at the cut. It may be complete, partial or
  absent after reboot. A partial result must be detectable (bad CRC, short length)
  and must not damage acked data.

Campaign
: All boots of one build of the board app. The campaign ID is the first 8 bytes of
  the app ELF SHA-256; flashing a different build starts a new campaign.

## Torn erases and blank PEBs

An interrupted NAND erase can leave a block that reads back as all 0xFF but whose
cells are not fully erased, so bits may flip later (Macronix AN0340). A PEB whose
page 0 is blank therefore has unknown history: it may be factory-fresh or the
result of an interrupted erase.

`esp_nand_ubi` erases a PEB with a blank page 0 before its first use. The erase
happens on allocation, so attach time does not change. A PEB with a valid EC header
and a blank VID header is used without a further erase: the EC header is only ever
written after an erase that completed.

This change is made before the stress testing starts, and the host harness's
torn-erase fault is its regression test.

## Shared workload component

The workloads and checks are a small C component used by both drivers:

```
esp_nand_ubi/test_support/ptest/
    ptest_ubi.c   ptest_ubi_setup(), ptest_ubi_step(), ptest_ubi_verify()
    ptest_lfs.c   ptest_lfs_setup(), ptest_lfs_step(), ptest_lfs_verify()
    ptest.h       result struct: pass/fail per check, counters, recovered watermark
```

A step performs one randomly chosen operation from a seeded PRNG. Verify runs after
every reboot and fills the result struct; it never modifies data except to repair
what the workload itself defines as repairable (a leftover temp file in `lfs` mode).

### `ubi` mode

Runs on one UBI volume of 64 LEBs plus one journal LEB.

Each step picks a LEB, erases it, then writes every page of it. Each page carries
`{lnum, generation, page_index, crc32}` followed by payload derived from
`(lnum, generation, page_index)`. After the last page, the step appends a record
`{lnum, generation, crc32}` to the journal LEB. That journal record is the ack.
When the journal LEB is full, the step erases it and writes a fresh one containing
the current generation of every LEB.

Checks after reboot:

- attach succeeds
- every LEB either reads entirely as 0xFF or contains pages of a single generation;
  pages of the generation being written at the cut may be missing at the end of the
  LEB
- for every LEB, the generation in the journal is either the one on flash or one
  less (the LEB being rewritten at the cut)
- both volume-table copies are valid after attach (`vtbl_copy_good`)

### `lfs` mode

Runs LittleFS on one UBI volume. Each step picks an action with weights 70/20/10:

- append a record `{len, seq, payload, crc32}` (32 to 2000 bytes) to the current log
  segment, then `fsync()`; the `seq` of the last successful `fsync()` is the acked
  watermark
- rewrite blob `k` (50 to 400 KiB, contents derived from `(k, generation)`) by
  writing a temp file, `fsync()`, then `rename()` over the old blob
- create or delete a small file in a subdirectory, to drive metadata compaction

When a log segment passes a size limit, a new segment starts and the oldest one is
deleted, so the volume keeps reclaiming blocks.

Checks after reboot:

- mount succeeds without a reformat
- log records parse in order with no gaps in `seq`; only the last record may be
  torn, and a torn last record counts as indeterminate
- every blob matches its old or new generation completely
- at most one temp file exists, and it is deleted

### Losing acked data

The acked watermark is lost with the cut, so a single boot can only check for gaps
and corruption. Two mechanisms catch acked data that disappears:

- each boot stores the recovered watermark in its stats record (below); a value
  lower than the previous boot's is a failure, because a completed boot leaves all
  its data synced
- the board app prints `ACK seq=N` every few acks; when the serial log survives the
  cut, a host script checks that the next boot recovers at least `N`

## Host harness

### Fault model

The host test helper `faulty_nand` wraps the emulated NAND and supports:

- cut after operation N: the device fails every later call until reboot
- partial program: the page holds its first K bytes and the rest stays 0xFF
- torn erase: the erase reports success and the block reads back as 0xFF, but the
  wrapper flips bits in the next page programmed into that block

### Cut strategies

| Strategy | Description | Run in |
|---|---|---|
| `log` | cut at operation 1, 2, 4, 8, ... of a fixed-seed workload | `host_test`, CI |
| targeted | cut inside named steps: mid EC header, mid VID header, between erase and EC header, between the two volume-table copies, during LittleFS metadata compaction | `host_test`, CI |
| random soak | random operation count and fault type per cycle, thousands of cycles | `host_ptest`, manual |

The wrapper identifies the step in progress from the address: page 0 of a PEB is
the EC header, page 1 the VID header, later pages data; the volume-table PEBs are
listed in `dev->vtbl_pnum`.

CI cases use a small emulated chip (64 to 128 PEBs). The random soak is a separate
host binary, `host_ptest`, that takes the seed, the cycle count and the mode on its
command line.

### Replay

Every failure prints `seed`, `cut_op` and the fault type. Passing the same three
values to `host_ptest` reproduces the failure.

### LittleFS on the host

The host harness builds LittleFS from the upstream `lfs.c` with an `lfs_config`
whose callbacks call the UBI volume BDL. `esp_littlefs` and the VFS layer are not
used on the host.

## Board app

`esp_nand_ubi/examples/nand_ubi_powercut` runs on the same hardware as the other
examples. The mode (`ubi` or `lfs`) and the reset method (`auto` or `manual`) are
Kconfig options.

### Volumes

On the first boot of a campaign the app formats the chip's UBI area and creates:

| Volume | Size | Contents |
|---|---|---|
| `ptest_stats` | 4 LEBs | stats records |
| `ptest_data` | rest of the chip | the workload |

### Boot sequence

1. Attach UBI; in `lfs` mode, mount LittleFS.
2. Run the mode's verify.
3. Write the stats record for this boot.
4. Print the stats summary.
5. In `auto` mode, arm the reset timer.
6. Run workload steps until reset or power loss.

### Stats records

Each boot writes one record into its own NAND page of `ptest_stats`, so no page is
programmed twice. The four LEBs form a ring of about 248 records; when it is full,
the oldest LEB is erased and reused.

A record contains:

- `campaign_id`, `boot_no`, mode, `esp_reset_reason()`, the auto-reset delay of the
  previous boot
- check results: attach, mount, reformat, volume-table repair
- data results: recovered watermark, lost acks, corrupt records, indeterminate
  records
- UBI state: free, bad and erase-pending PEB counts at attach, minimum, mean and
  maximum erase count, attach time
- campaign totals: boots, failures, lost acks, corrupt records, highest
  erase-pending count
- `crc32`

The newest valid record describes the whole campaign through its totals. A record
cut during its write fails its CRC, is skipped, and is counted as a torn stats
record; this is expected occasionally.

At boot the app prints the newest record and a table of the last ten records.

### Reset methods

`auto`
: After the stats record is written, a one-shot `esp_timer` fires after a random
  delay between 50 ms and a few seconds and calls `esp_restart()`. The timer task
  runs at a higher priority than the workload, so the reset can land between any
  two operations, including inside a multi-step UBI sequence. The NAND stays
  powered, so a program or erase already started on the chip completes.

`manual`
: The workload runs until the USB cable is pulled. This is the only method that can
  interrupt a program or erase on the chip itself. Cycles are few, and the stats
  volume keeps the results.

## Later

- A volume-table mode that creates and removes volumes, once volume removal exists.
- A hardware watchdog reset with a random timeout, more abrupt than `esp_restart()`.
- A controllable power switch, for timed cuts of the NAND supply on real hardware.
