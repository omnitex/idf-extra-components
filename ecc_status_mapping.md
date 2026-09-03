# ECC status: GigaDevice vs Micron-shaped buckets

Notes from reviewing `a99c1e19` (`refine_ecc_status_ext`) and how C0h/F0h bits should map into `nand_ecc_status_t`.

## Hardware

Two independent 2-bit fields, **same bit positions**, different registers:

| Field | Register | Bits | Driver masks |
|---|---|---|---|
| **ECCS** | C0h (`REG_STATUS`) | [5:4] | `STAT_ECC1` (`1<<5`), `STAT_ECC0` (`1<<4`) |
| **ECCSE** | F0h (`REG_STATUS_EXT`) | [5:4] | same masks |

`STAT_ECCSE_SHIFT` (4) is the same extract as packing F0h with `STAT_ECC1`/`STAT_ECC0`. It is not a different encoding.

GD5F4GM8 (Table 12-3 style):

| ECCS | Meaning | Use ECCSE? |
|---|---|---|
| `00` | no error | no (F0h may be stale) |
| `01` | 1–7 bits corrected | **yes** |
| `10` | uncorrectable | no |
| `11` | 8 bits corrected | no |

ECCSE, only when ECCS = `01`:

| ECCSE | Bits corrected |
|---|---|
| `00` | 1–4 |
| `01` | 5 |
| `10` | 6 |
| `11` | 7 |

Read F0h only when ECCS is `01`. Extra SPI transaction is otherwise wasted, and ECCSE is don’t-care.

Enabled in tree only for GD5F4GM8 IDs `95h`/`85h`. Other GM parts likely share the table but were not verified.

## Packing bit *masks* (`!!`)

`STAT_ECCn` are **masks**, not bit numbers. These macros do not produce a 0–3 (or 0–7) integer:

```c
#define PACK_2BITS_STATUS(status, bit1, bit0) \
    ((((status) & (bit1)) << 1) | ((status) & (bit0)))
```

Examples for ECCS on C0h:

| ECCS | Current pack | Wanted |
|---|---|---|
| `00` | 0 | 0 |
| `01` | 16 | 1 |
| `10` | 64 | 2 |
| `11` | 80 | 3 |

`refine_ecc_status_ext` compares to `NAND_ECC_BITS_CORRECTED` (1) and `NAND_ECC_MAX_BITS_CORRECTED` (3). With the current pack those compares never match, so F0h is unused and `11b` is never remapped to 7–8. The same class of bug applies to `PACK_3BITS_STATUS` for Micron/FM.

Collapse each mask to `{0,1}` before shifting:

```c
#define PACK_2BITS_STATUS(status, bit1, bit0) \
    ((!!((status) & (bit1)) << 1) | (!!((status) & (bit0))))
```

If bits are always contiguous [5:4], `(reg >> 4) & 0x3` is equivalent. `!!` is the general form when the caller passes arbitrary masks.

Use the same pack on **both** C0h and F0h; do not add a second shift constant.

## Driver enum (Micron-shaped)

`nand_ecc_status_t` is the Micron 3-bit C0h encoding, plus aliases so 2-bit chips can reuse the same constants:

| Enum | Value | Typical Micron meaning | 2-bit alias |
|---|---|---|---|
| `NAND_ECC_OK` | 0 | no error | ECCS `00` |
| `NAND_ECC_1_TO_3_BITS_CORRECTED` / `NAND_ECC_BITS_CORRECTED` | 1 | 1–3 corrected | ECCS `01` (“some”) |
| `NAND_ECC_NOT_CORRECTED` | 2 | uncorrectable | ECCS `10` |
| `NAND_ECC_4_TO_6_BITS_CORRECTED` / `NAND_ECC_MAX_BITS_CORRECTED` | 3 | 4–6 corrected | ECCS `11` (“max”) |
| `NAND_ECC_7_8_BITS_CORRECTED` | 5 | 7–8 corrected | (3-bit only, or GD remap) |
| `NAND_ECC_MAX` | 6 | invalid / init error | — |

This is **not** a linear bit count. Concatenating or ORing ECCS+ECCSE does not land on `{0,1,2,3,5}`.

Refresh (`nand_ecc_exceeds_data_refresh_threshold`, default threshold **4**) only looks at three buckets:

| Enum | `min_bits_corrected` | Refresh at 4? |
|---|---|---|
| OK | 0 | no |
| 1–3 / `BITS_CORRECTED` | 1 | no |
| 4–6 / `MAX_BITS_CORRECTED` | 4 | yes |
| 7–8 | 7 | yes |

Uncorrectable is a read failure (`is_ecc_error`), not a refresh class.

### 2-bit chip, no F0h

| ECCS | Packed (after `!!`) | Enum | Refresh? |
|---|---|---|---|
| `00` | 0 | OK | no |
| `01` | 1 | 1–3 (HW may be 1–7 on GD) | **no** |
| `10` | 2 | not corrected | fail |
| `11` | 3 | 4–6 alias (HW “max”, 8 on GD) | yes |

That is why GD5F4GM8 with only C0h almost never rewrites until all 8 ECC bits are in use: `01b` covers 1–7 and sits in the low bucket.

### 2-bit + ECCSE (intended map)

| ECCS | ECCSE | HW bits | Target enum | Value | Refresh at 4? |
|---|---|---|---|---|---|
| `00` | ignore | 0 | `NAND_ECC_OK` | 0 | no |
| `01` | `00` | 1–4 | `NAND_ECC_1_TO_3_BITS_CORRECTED` | 1 | **no** |
| `01` | `01` | 5 | `NAND_ECC_4_TO_6_BITS_CORRECTED` | 3 | yes |
| `01` | `10` | 6 | `NAND_ECC_4_TO_6_BITS_CORRECTED` | 3 | yes |
| `01` | `11` | 7 | `NAND_ECC_7_8_BITS_CORRECTED` | 5 | yes |
| `10` | ignore | fail | `NAND_ECC_NOT_CORRECTED` | 2 | error |
| `11` | ignore | 8 | `NAND_ECC_7_8_BITS_CORRECTED` | 5 | yes |

Bucket mismatch that the current enum cannot fix: ECCSE=`00` is **1–4** bits, constant name is **1–3**. A page with **exactly 4** corrected bits stays below threshold 4. Commit policy: rewrite from **5** bits onward.

Failed F0h read → `NAND_ECC_4_TO_6_BITS_CORRECTED`: force refresh without failing the page. Do not treat unread F0h as ECCSE=`00`.

After both fields are packed 0–3, a compact lookup (ECCS == 1 only):

```c
return NAND_ECC_1_TO_3_BITS_CORRECTED
     + 2 * !!eccse
     + 2 * (eccse == 3);
/* ECCSE 0→1, 1→3, 2→3, 3→5 */
```

ECCS `11` → `NAND_ECC_7_8_BITS_CORRECTED`. ECCS `00`/`10` → packed value as-is.

## Why bitwise merge fails

Let `packed = (ECCS << 2) | ECCSE` (Gemini nibble). Values are `0x00`–`0x0F`, not members of `nand_ecc_status_t`. Example: ECCS=`01`, ECCSE=`10` → `0x06` = `NAND_ECC_MAX` (treated as init error).

OR `ECCS | ECCSE` also fails (5 bits stays in the 1–3 bucket; stale F0h poisons ECCS `00`/`10`).

There is no bitwise merge of two 2-bit fields that produces `{0,1,2,3,5}`. Refine is a **lookup**, gated on ECCS == `01`.

`(ECCS << 2) | ECCSE` is a fine **debug nibble** (log Table 12-3 as `0x04`–`0x07`). It must not be stored in `ecc_corrected_bits_status`.

## Gemini-style 4-bit GD enum

Idea: pack ECCS into bits [3:2] and ECCSE into bits [1:0] with `!!`, then mask don’t-cares:

| ECCS | `packed & 0x0C` | Result |
|---|---|---|
| `00` | `0x00` | `NO_ERROR` (`0x00`), ignore ECCSE |
| `01` | `0x04` | raw nibble `0x04`–`0x07` (1–4, 5, 6, 7) |
| `10` | `0x08` | uncorrectable |
| `11` | `0x0C` | 8 bits corrected |

That is a faithful GD decoder. It is **not** a drop-in for `nand_ecc_status_t` (`0x08 != 2`, so uncorrectable would not trip `is_ecc_error`).

Still need a second map into driver buckets. Exact 5 vs 6 vs 7 is more resolution than refresh uses.

**Do not** call `parse(c0, 0x00)` as a stand-in for “F0h not read yet” when ECCS is `01`: that always yields 1–4 and **reintroduces** “refresh never fires until 8 bits”. Pass 0 only for ECCS `00`/`10`/`11`, where ECCSE is don’t-care.

`UNKNOWN` is unreachable: two ECCS bits only produce top-nibble `0x00` / `0x04` / `0x08` / `0x0C`.

## Are Micron buckets incompatible with GigaDevice?

**No** for the two control questions the driver actually asks:

1. Uncorrectable? → fail the read.
2. Severity high enough to rewrite? (threshold 4 in *driver buckets*.)

**Yes** if the enum names are treated as datasheet bit counts:

- Exactly **4** bits on GD is ECCSE `00` (1–4) → stored as “1–3” → threshold 4 does **not** fire. Micron would put 4 in “4–6” and would fire. That is the only refresh-policy hole if we keep this type.
- ioctl / logs that print `NAND_ECC_1_TO_3_BITS_CORRECTED` are wrong for a 4-bit GD page, and cannot show 5 vs 6.
- 2-bit GD without F0h already stored 1–**7** as “1–3”. That fiction predates F0h.

Compatible as a **severity ladder** (ok < some < many < max < fail). Not compatible as a **numeric ECC report**.

The 2-bit aliases were already a “paint GD/Winbond-style 2-bit onto Micron codes” hack. F0h is the same hack with a finer table behind `01`.

## When to keep the enum vs rethink storage

**Keep `nand_ecc_status_t`** if the contract stays:

1. uncorrectable → fail
2. refresh if corrected **class** meets threshold 4
3. accept rewrite from **5** bits on GD (document the 4-bit hole)

Then map GD into existing buckets. Treat names as class IDs, not ranges (or rename to `CORR_LOW` / `MID` / `HIGH`). Gemini’s nibble stays a parser detail, not stored state.

**Change storage** if you need:

- threshold 4 meaning 4 on every vendor
- public ECC API / logs that match datasheets
- more vendor tables without more aliases

Then store a decoded interval (or exact count when known), not a vendor encoding:

```c
struct nand_ecc_result {
    bool uncorrectable;
    uint8_t bits_corrected_min; /* 0 if clean */
    uint8_t bits_corrected_max; /* same as min when exact (5, 6, 7, 8) */
};
```

Refresh: `!uncorrectable && bits_corrected_min >= threshold`.

Policy on the GD 1–4 range:

| Rule | GD 1–4 page | Side effect |
|---|---|---|
| `min >= 4` | no rewrite | current map; misses exact-4 |
| `max >= 4` | rewrite | may also rewrite 1–3 pages in that range |
| exact count | N/A | 1–4 is never exact on this part |

That is an ABI change (`ESP_BLOCKDEV_CMD_GET_PAGE_ECC_STATUS` returns `nand_ecc_status_t`). Not required to start reading F0h.

## Practical recommendation

1. Fix `PACK_2BITS_STATUS` / `PACK_3BITS_STATUS` with `!!` (or `>> 4` if bits are committed contiguous). Without that, `refine_ecc_status_ext` does not run.
2. Delete `STAT_ECCSE_SHIFT`; pack F0h with `STAT_ECC1`/`STAT_ECC0`.
3. Keep the ECCS == `01` gate before reading F0h.
4. Keep a small lookup into Micron-shaped buckets; do not synthesize 3-bit Micron codes by stuffing ECCSE into `STAT_ECC2`.
5. Do not rip out `nand_ecc_status_t` for this F0h fix. Optionally fix the **names/docs** so they do not claim bit ranges the hardware does not share.
6. Revisit stored type only if threshold-accurate counts or a datasheet-faithful public ECC API become requirements.

## Verification against current tree (idf-extra-components, `spi_nand_flash/`)

Confirmed by reading the actual driver code (not just the reviewed commit). `a99c1e19` / `refine_ecc_status_ext` / `STAT_ECCSE_SHIFT` are **not present anywhere in this repo** — grepped `src/`, `priv_include/`, `include/` for `ECCSE`, `refine_ecc_status_ext`, `STAT_ECCSE_SHIFT`, register `0xF0`: zero hits. So this is a gap to close, not a regression in already-merged code.

### `PACK_2BITS_STATUS` / `PACK_3BITS_STATUS` bug is real, present today

`spi_nand_flash/src/nand_impl.c:415-416`:

```c
#define PACK_2BITS_STATUS(status, bit1, bit0)         ((((status) & (bit1)) << 1) | ((status) & (bit0)))
#define PACK_3BITS_STATUS(status, bit2, bit1, bit0)   ((((status) & (bit2)) << 2) | (((status) & (bit1)) << 1) | ((status) & (bit0)))
```

Called with **masks**, not bit positions, from `spi_nand_oper.h:57-59`:

```c
#define STAT_ECC0  1 << 4   /* 0x10 */
#define STAT_ECC1  1 << 5   /* 0x20 */
#define STAT_ECC2  1 << 6   /* 0x40 */
```

`is_ecc_error()` in `nand_impl.c:418-438` calls `PACK_2BITS_STATUS(status, STAT_ECC1, STAT_ECC0)` for 2-bit chips and `PACK_3BITS_STATUS(status, STAT_ECC2, STAT_ECC1, STAT_ECC0)` for 3-bit chips, then stores the result in `ecc_corrected_bits_status` and compares it to `nand_ecc_status_t` values. With the masks as written, the packed result is `{0, 16, 64, 80}` for 2-bit chips, not `{0, 1, 2, 3}` — never equal to `NAND_ECC_BITS_CORRECTED`(1) or `NAND_ECC_MAX_BITS_CORRECTED`(3). Only the `NAND_ECC_NOT_CORRECTED` check happens to matter for `is_ecc_err` (it's a non-zero/zero style check via `bits_corrected_status == NAND_ECC_NOT_CORRECTED`, value 2 — also broken the same way), so the refresh-threshold logic downstream (`nand.h:74-82`, `nand_ecc_exceeds_data_refresh_threshold`) is comparing against a value that can never legitimately be 1 or 3.

### GigaDevice has no F0h/ECCSE handling at all

`spi_nand_flash/src/devices/nand_gigadevice.c` never touches `dev->chip.ecc_data`. It falls through to the single global default set once in `nand_impl.c:96-97`:

```c
(*handle)->chip.ecc_data.ecc_status_reg_len_in_bits = 2;
(*handle)->chip.ecc_data.ecc_data_refresh_threshold = 4;
```

So GD5F4GM8 (`95h`/`85h`, the part with C0h ECCS + F0h ECCSE) is processed through the exact same 2-bit generic path as a plain single-register 2-bit chip (e.g. Winbond). No code path reads register `0xF0`, no ECCSE gate exists, no GD-specific ECC callback exists.

Practical effect: GD's ECCS `01` (1–7 bits corrected per datasheet) packs to `NAND_ECC_1_TO_3_BITS_CORRECTED`(1), whose `min_bits_corrected` is 1 (`nand.h:75-76`). Refresh threshold is 4. So a GD5F4GM8 page can sit at 5, 6, or 7 corrected bits — well past the intended refresh point — and never trigger `nand_ecc_exceeds_data_refresh_threshold()`, because ECCS stays `01` and ECCSE is never read. Only ECCS rolling over to `11` (8 bits, packed today as garbage due to the mask bug, but structurally the "max" bucket) would look like it should refresh.

### Gap list to close (not yet started)

1. Fix `PACK_2BITS_STATUS` / `PACK_3BITS_STATUS` in `nand_impl.c` (masks → `!!`-collapsed bits or `>>4` extraction).
2. Add a GD-specific status path: read C0h ECCS; if ECCS == `01b`, issue a second status read of register `0xF0` for ECCSE. No such second read exists in `spi_nand_oper.c` today — needs a new op, or reuse of the existing status-read primitive with a different register address.
3. Add the `(ECCS, ECCSE)` → `nand_ecc_status_t` lookup per the table above; wire it in via a chip-level hook (`nand_gigadevice.c` currently has no ECC-related code to hang this off of) rather than hardcoding another branch into the generic `is_ecc_error()` in `nand_impl.c`.
4. Confirm which other GD part numbers (beyond `95h`/`85h`) share the C0h+F0h scheme before enabling the second read tree-wide; the doc notes this was "not verified" for parts outside GD5F4GM8.
