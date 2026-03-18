## ADDED Requirements

### Requirement: Migrate all mapped sectors
The system SHALL provide `dhara_migrate(src_map, dst_map, page_buf, progress_cb, user_data, err)` that iterates every sector `0 .. capacity_src - 1`, reads each mapped sector from `src_map` and writes it to `dst_map`, trims unmapped sectors on `dst_map`, and calls `dhara_map_sync` on `dst_map` upon completion.

#### Scenario: All mapped sectors transferred
- **WHEN** `dhara_migrate` is called with a fully populated `src_map` and a blank `dst_map`
- **THEN** every sector that was mapped on src is readable on dst with identical content, and `dhara_migrate` returns 0

#### Scenario: Unmapped sectors trimmed on destination
- **WHEN** `src_map` has sectors that are not mapped
- **THEN** those sectors are trimmed on `dst_map` via `dhara_map_trim`

#### Scenario: Destination synced on success
- **WHEN** `dhara_migrate` completes without error
- **THEN** `dhara_map_sync` is called on `dst_map` and its return value propagates as the return value of `dhara_migrate`

### Requirement: Reject migration when destination has less capacity
The system SHALL return -1 and set `*err = DHARA_E_MAP_FULL` when `dhara_map_capacity(dst_map) < dhara_map_capacity(src_map)`.

#### Scenario: Insufficient destination capacity
- **WHEN** `dhara_migrate` is called with `dst_map` capacity less than `src_map` capacity
- **THEN** the function returns -1 and `*err` is set to `DHARA_E_MAP_FULL`, with no writes performed

### Requirement: Progress callback invocation
When a non-NULL `progress_cb` is provided, the system SHALL invoke it after each sector is processed with `current = sectors_processed` and `total = capacity_src`.

#### Scenario: Progress reported per sector
- **WHEN** `dhara_migrate` is called with a non-NULL `progress_cb`
- **THEN** the callback is invoked exactly `capacity_src` times, with `current` ranging from 1 to `capacity_src` in order

#### Scenario: NULL progress callback is safe
- **WHEN** `dhara_migrate` is called with `progress_cb = NULL`
- **THEN** the function completes successfully without dereferencing the NULL pointer

### Requirement: Error propagation
The system SHALL stop migration and return -1 on the first read or write error, storing the error code in `*err`.

#### Scenario: Read error aborts migration
- **WHEN** `dhara_map_read` returns -1 for any sector
- **THEN** `dhara_migrate` returns -1 with `*err` set to the error from the failing read, and no further sectors are processed

#### Scenario: Write error aborts migration
- **WHEN** `dhara_map_write` returns -1 for any sector
- **THEN** `dhara_migrate` returns -1 with `*err` set to the error from the failing write
