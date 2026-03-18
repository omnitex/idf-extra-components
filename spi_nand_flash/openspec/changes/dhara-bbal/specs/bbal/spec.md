## ADDED Requirements

### Requirement: BBAL context initialization
The system SHALL provide `dhara_bbal_init(bbal, phys_nand)` that scans all physical blocks via `dhara_nand_is_bad()`, builds a heap-allocated `logical_to_phys[]` remapping table containing only good physical block indices in ascending order, sets `logical_nand.num_blocks` to `total_blocks - num_bad`, and returns 0 on success or -1 with `errno = ENOMEM` if allocation fails.

#### Scenario: Init with no bad blocks
- **WHEN** `dhara_bbal_init` is called on a device with zero bad blocks
- **THEN** `logical_nand.num_blocks` equals `phys_nand->num_blocks`, `num_bad` is 0, and `logical_to_phys[i] == i` for all i

#### Scenario: Init with bad blocks present
- **WHEN** `dhara_bbal_init` is called on a device with N bad blocks at arbitrary positions
- **THEN** `logical_nand.num_blocks` equals `phys_nand->num_blocks - N`, `num_bad` equals N, and `logical_to_phys[]` contains only the good physical block indices in ascending order

#### Scenario: Init fails on allocation failure
- **WHEN** `dhara_bbal_init` is called and `malloc` returns NULL
- **THEN** the function returns -1 and `errno` is set to `ENOMEM`

### Requirement: BBAL context deinitialization
The system SHALL provide `dhara_bbal_deinit(bbal)` that frees the heap-allocated `logical_to_phys[]` table and sets the pointer to NULL.

#### Scenario: Deinitialization releases memory
- **WHEN** `dhara_bbal_deinit` is called after a successful `dhara_bbal_init`
- **THEN** the `logical_to_phys` pointer is freed and set to NULL, with no memory leak

### Requirement: logical_nand is first field
The `dhara_bbal_t` struct SHALL have `logical_nand` as its first field so that any `dhara_nand_t *` passed by Dhara can be cast directly to `dhara_bbal_t *` without pointer arithmetic.

#### Scenario: Cast from dhara_nand_t pointer
- **WHEN** Dhara passes `&bbal.logical_nand` to a `dhara_nand_*` callback
- **THEN** casting that pointer to `dhara_bbal_t *` yields the correct `bbal` instance

### Requirement: Remapping table size
The `logical_to_phys[]` allocation SHALL be resized with `realloc` after the scan so the final allocation holds exactly `num_logical` entries.

#### Scenario: Allocation is exactly sized
- **WHEN** `dhara_bbal_init` completes successfully
- **THEN** the table allocation is `num_logical * sizeof(dhara_block_t)` bytes

### Requirement: All 7 dhara_nand_* callbacks implemented
`dhara_bbal.c` SHALL implement all 7 `dhara_nand_*` callbacks: `is_bad`, `mark_bad`, `erase`, `prog`, `is_free`, `read`, and `copy`. Each SHALL translate the logical block or page address to physical using `logical_to_phys[]` and delegate to the underlying physical HAL.

#### Scenario: Page address translation
- **WHEN** a callback receives logical page P for a device with `log2_ppb = L`
- **THEN** the physical page is `(logical_to_phys[P >> L] << L) | (P & ((1 << L) - 1))`

#### Scenario: Block address translation
- **WHEN** `dhara_nand_erase` is called with logical block B
- **THEN** the physical block `logical_to_phys[B]` is erased on the underlying HAL

#### Scenario: is_bad always returns false for in-table blocks
- **WHEN** Dhara calls `dhara_nand_is_bad` on a logical block B
- **THEN** the call is forwarded to the physical HAL using the physical block from `logical_to_phys[B]`

### Requirement: Logical nand geometry unchanged except num_blocks
The `logical_nand` descriptor SHALL copy `log2_page_size` and `log2_ppb` verbatim from `phys_nand`, overriding only `num_blocks`.

#### Scenario: Page and block geometry preserved
- **WHEN** `dhara_bbal_init` is called with any valid `phys_nand`
- **THEN** `bbal.logical_nand.log2_page_size == phys_nand->log2_page_size` and `bbal.logical_nand.log2_ppb == phys_nand->log2_ppb`
