# Attach checkpoint ("fastmap") for esp_nand_ubi

## Purpose

`nand_ubi_attach()` reads the EC and VID headers of every PEB. On an ESP32-S3
with a 4 Gbit SPI NAND (4096 PEBs of 128 KiB, 2 KiB pages) this takes 3.5 s on
a formatted chip and 6.4 s on a blank one. A checkpoint is a copy of the
attach result written to flash, so that the next attach reads the checkpoint
and a small number of PEBs instead of all of them.

Targets for 4096 PEBs:

- attach in about 150 ms after a power cut, without a clean detach;
- no additional RAM after attach apart from the pool lists, and no buffer
  larger than one page during attach or while writing a checkpoint;
- a full scan stays correct at all times, including on Linux.

The feature is enabled with `nand_ubi_config_t.checkpoint`, which defaults to
`CONFIG_ESP_NAND_UBI_CHECKPOINT`.

## Terms

- *Checkpoint*: one PEB holding a snapshot of `peb_state`, `eba[]`, the
  pool, and a few device fields.
- *Region*: the first `A` PEBs of the chip. Checkpoints are stored only in the
  region, and the two volume-table copies normally are too, so attach finds
  them by scanning the region. Region PEBs are never allocated for volume data.
- *Pool*: up to `P` free PEBs listed in the checkpoint. Between two
  checkpoints, every PEB allocated for volume data comes from the pool, in the
  order of the list.
- *Previous pool*: the pool of the checkpoint before the newest one.
- *Deferred erase*: a PEB whose LEB is erased in checkpoint mode is set to
  `ERASE_PENDING` and keeps its contents until a newer checkpoint is durable.

## On-flash format

The checkpoint PEB has the usual EC header and a VID header with:

| Field | Value |
|---|---|
| `vol_id` | `UBI_CKPT_VOL_ID` = `0x7FFFF00F` (an internal volume ID, above `UBI_LAYOUT_VOL_ID`) |
| `lnum` | 0 |
| `compat` | `UBI_COMPAT_DELETE` |
| `sqnum` | next global sqnum when written |
| `data_size`, `data_crc` | length and CRC32 of the payload |

The VID header is written before the payload, as for the volume table, so the
payload CRC is computed in a first rendering pass and the payload is written in
a second one, one page at a time.

Payload, in LEB data, all fields big-endian:

| Part | Size |
|---|---|
| Header: magic `"UBCK"`, version, `image_seq`, `peb_count`, `leb_count`, `A`, `P`, pool count, own `pnum`, previous pool count, `mean_ec`, sum and count of the erase counts in all EC headers | 64 B |
| Pool: PEB numbers | pool count × 2 B |
| Previous pool: PEB numbers | previous pool count × 2 B |
| `peb_state`, packed as in RAM | `peb_count` / 4 B |
| `eba[]` | `leb_count` × 2 B |

For 4096 PEBs, `P` = 64 and a volume covering the chip this is about 9.5 KiB,
five pages. If the payload does not fit in one LEB (124 KiB, about 50 000
PEBs), checkpoint mode is turned off.

Erase counts are not stored. They are in the EC headers, and the allocator
reads them there.

### Linux compatibility

Linux attaches a chip with checkpoints by full scan. A VID header with an
unknown internal `vol_id` and `UBI_COMPAT_DELETE` is put on Linux's erase
list (`drivers/mtd/ubi/attach.c`, `scan_peb()`), so the checkpoint PEBs are
erased and every other PEB is attached from its own headers. Linux never reads
the checkpoint format, and Linux's own fastmap, if enabled, is independent of
it. `esp_nand_ubi` with checkpoints off does the same.

## Invariants

The rules below keep the checkpoint and the per-PEB headers consistent across
power cuts. Rules 1 to 7 are each taken from a Linux fastmap bug; the commit in
`drivers/mtd/ubi` that fixed the bug is listed.

1. Between two checkpoints, only PEBs in the pool, PEBs in `ERASE_PENDING`
   state, and region PEBs change state on flash. Attach rechecks the pool and
   the region. (`2e8f08deabbc`)
2. A PEB referenced by the newest durable checkpoint is not erased until a
   newer checkpoint is durable. (`36a87e44f642`)
3. After a new checkpoint is durable, the previous checkpoint PEB is erased
   before any other write. Attach uses the checkpoint with the highest sqnum.
   (`f78e5623f45b`)
4. If the newest checkpoint fails any check, attach does a full scan. Older
   checkpoints are never used. If no checkpoint can be written, the newest one
   is erased and checkpoint mode is off until the next attach; if it cannot be
   erased, the device becomes read-only. (`2d93fb363275`)
5. The checkpoint states its own `pnum`, `image_seq`, `peb_count`,
   `leb_count`, `A` and `P`; attach rejects it if any of them differs from what
   it observed or configured. (`5283ec72b0cc`)
6. Region PEBs are wear-leveled among themselves, and checkpoint frequency is
   chosen so the region wears no faster than the rest of the chip.
   (`90e0be56144b`, `f9c34bb52997`, `76f9476ece44`)
7. Region and pool PEBs are attached with the same per-PEB function as the full
   scan: same header checks, duplicate resolution by sqnum, `copy_flag`
   handling and bad-block check. (`ecbfa8eabae9`, `44305ebde243`)
8. Deferred erases run oldest data first: PEBs written before the previous
   pool, then the previous pool in list order, which is allocation order. A
   power cut part-way through then never leaves an older copy of a LEB without
   the newer one, so a full scan cannot return data older than the last copy.
9. A mapped PEB whose VID header is no longer valid loses duplicate
   resolution, so a PEB erased after the checkpoint was written never shadows a
   pool PEB holding the same LEB.

## Attach

1. For each PEB in the region, and beyond it until both volume-table copies
   are found: check for a bad block and read the VID header. This finds the
   volume-table copies and the checkpoint with the highest sqnum.
2. Load the volume table.
3. If there is no checkpoint, do a full scan, then write a checkpoint.
4. Read the checkpoint payload a page at a time, write the pool lists,
   `peb_state` and `eba[]` straight into the RAM tables, and compute the CRC on
   the way. Check the CRC, the header fields from invariant 5, and that every
   mapped PEB is `USED` and every pool PEB is `FREE` and outside the region. On
   any mismatch, reset the tables and continue as in step 3.
5. Unmap every LEB mapped into the region, mark region PEBs free, and run the
   per-PEB attach on each region PEB except the checkpoint, then on each pool
   PEB.
6. Erase every `ERASE_PENDING` PEB in the order of invariant 8. The checkpoint
   lists the PEBs its writer erased right after writing it, so a PEB that
   already has a valid EC header and a blank VID header is only marked free;
   after a clean detach, attach writes and erases nothing.
7. `global_sqnum` is the highest sqnum seen in steps 1 and 5, which includes
   the checkpoint's own. The erase-count sum and count come from the
   checkpoint; every erase adds to them, and each checkpoint stores them
   including the erases its writer does right after it, so `mean_ec` stays
   current without a full scan.

The volume table is read in full in step 2: two copies of an EC header, a VID
header and the table (11 pages for 128 volume slots and 2 KiB pages), 26 reads.
With that, reads for 4096 PEBs and `A` = `P` = 64 are about 1 per region PEB in
step 1, 5 payload pages, and 2 to 3 per region and pool PEB in step 5: roughly
360 reads, or about 150 ms at the measured 425 µs per read. On the host
emulator with 93 good PEBs and `A` = `P` = 10, attach after a power cut takes
76 reads.

## Allocation

The allocator takes the first free PEB of the pool. The pool is refilled when a
checkpoint is written: the entries that are still free stay, in order, and
each used entry is replaced by drawing `k` free PEBs outside the region at
random, reading their EC headers, and keeping the one with the lowest erase
count. Wear leveling is decided when the pool is filled. The EC header reads
cost the same per allocation as the allocator without checkpoints, but they
happen at checkpoint time, `k` per PEB used since the last one.

The two pool lists are kept in RAM, 2 × `P` × 2 B.

## Erase

`erase()` on a mapped LEB unmaps it and sets the old PEB to `ERASE_PENDING`,
without a flash operation. The PEB is erased right after the next checkpoint
is durable, in the same call.

After a power cut, an LEB that was erased and not written again since the last
checkpoint reads back its most recent contents. This applies to an attach
from the checkpoint, to a full scan and to Linux. A write to the LEB after the
erase uses a pool PEB with a higher sqnum, which every attach prefers. A clean
detach writes a checkpoint, so the erase is kept. LittleFS does not rely on
erased blocks staying erased across a power cut: it erases a block again before
programming it, and of two copies of a metadata block it uses the one with the
higher revision.

## Writing a checkpoint

A checkpoint is written when the pool is empty, at the end of
`nand_ubi_create_volume()`, at detach if anything changed since the last one,
at the end of an attach that did a full scan, and when the application calls
`nand_ubi_checkpoint()`, unless the device is read-only.

When the pool is empty, the checkpoint is written inside the `write()` call
that needs the next PEB, under the device lock. With `k` = 4, `P` = 64 and
425 µs per header read, all 64 entries are refilled and that call takes about
120 ms longer: about 110 ms to
fill the pool and 10 ms to write the checkpoint, plus 3 ms per deferred erase.
An application that cannot accept this delay in a particular write calls
`nand_ubi_checkpoint()` when it is idle, for example after a burst of writes or
before sleep; this refills the pool, so the next writes do not run out of it.

1. Copy the current pool list to the previous pool list and refill the pool,
   as described under Allocation.
2. Pick the region PEB for the new checkpoint: the free region PEB with the
   lowest erase count, read from the EC headers of all free region PEBs.
3. Erase the chosen PEB and stamp its EC header.
4. Render the payload once to compute its CRC, write the VID header, then
   render and write the payload page by page. If step 3 or 4 fails, the PEB is
   retired and step 2 is repeated, up to three times.
5. Erase the previous checkpoint PEB (invariant 3).
6. Run the deferred erases (invariant 8).

A power cut before step 4 completes leaves the previous checkpoint as the
newest valid one. Its pool contains every PEB written since it was made, so
attach is correct. A power cut during step 4 leaves a newest checkpoint with a
bad CRC, and attach does a full scan. A power cut during step 5 leaves an older
checkpoint with a lower sqnum, or a torn region PEB, and both are ignored. A
power cut during step 6 leaves pending PEBs listed in the new checkpoint, and
attach finishes erasing them in the same order.

## Sizing `A` and `P`

Attach reads grow with `A + P`. Each checkpoint erases one of `A` region PEBs
and happens once per `P` allocations, while each allocation erases one of
about `peb_count` data PEBs. The region wears at the same rate as the rest of
the chip when `A · P ≈ peb_count`. Region PEBs hold only checkpoints and
volume-table copies, so the lost capacity is `A − 4` PEBs.

A larger `P` with a smaller `A` loses less capacity, but attach reads more
pool PEBs and the write that refills the pool takes longer. The default is
`A = P = ⌈√peb_count⌉`, which minimises `A + P`. `A` is at least 8; if it would
exceed a quarter of the chip, or `P` exceeds 1024, checkpoint mode stays off.

| PEBs | `A` = `P` | Region capacity lost | Approximate attach reads |
|---|---|---|---|
| 1024 | 32 | 3.5 MiB (2.7 %) | 200 |
| 2048 | 46 | 5.3 MiB (2 %) | 270 |
| 4096 | 64 | 7.5 MiB (1.5 %) | 360 |

For 4096 PEBs, `A` = 16 and `P` = 256 loses 1.5 MiB, needs about 830 reads at
attach, and adds about 450 ms to the write that refills the pool.

`A` and `P` are set by `nand_ubi_config_t.ckpt_region_pebs` and
`ckpt_pool_pebs`, which default to `CONFIG_ESP_NAND_UBI_CKPT_REGION_PEBS` and
`CONFIG_ESP_NAND_UBI_CKPT_POOL_PEBS`; 0 selects the defaults above. Both are
stored in the checkpoint. A checkpoint written with other values is rejected,
attach does a full scan and writes a new one.

A chip that was used without checkpoints may hold data in the region. Those
PEBs are attached by the region rescan, and once their LEBs are rewritten the
PEBs are not used for data again.

## Testing

`host_test/main/test_nand_ubi_checkpoint.cpp` runs the power-cut workload from
`test_support/ptest` with checkpoints on. It cuts power at every write and
erase over a workload that writes many checkpoints, with clean, partial and
torn cuts. After every cut the volume is verified twice: attached from the
checkpoint, and attached read-only by a full scan without checkpoint support,
as Linux would attach it. Other cases cover the number of reads at attach, a
corrupt checkpoint, a detach after erasing a LEB, and a chip attached by an
implementation without checkpoint support.
