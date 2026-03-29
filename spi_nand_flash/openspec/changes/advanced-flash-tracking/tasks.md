# Advanced Flash Tracking - Implementation Tasks

## How to read this file

Every implementation task follows the TDD cycle:

1. **Write the failing test** — add a `TEST_CASE` to the relevant `.cpp` file in `host_test/main/`. The test must reference the API you are about to implement.
2. **Build and verify RED** — the build must fail (undefined symbol or header not found), or the test must fail at runtime with a clear message. If the test passes immediately, the test is wrong — fix it first.
3. **Write minimal code** — implement only enough to make that test pass.
4. **Build and verify GREEN** — run the test binary, confirm the test passes and no other tests regressed.
5. **Commit** — one commit per GREEN cycle; message format `feat(nand-tracking): <short description>`.

### How to build and run

```bash
# From spi_nand_flash/host_test/
idf.py build
./build/nand_flash_host_test.elf                          # run all tests
./build/nand_flash_host_test.elf "[advanced]"             # run only [advanced] tag
./build/nand_flash_host_test.elf "[advanced-init]"        # run a specific tag
```

Tag all new test cases with `[advanced]` plus a feature-specific tag so they can be run in isolation during development.

### Adding a new test file

1. Create `host_test/main/test_<feature>.cpp`.
2. Add the filename to `host_test/main/CMakeLists.txt` in the `SRCS` list.
3. Include `<catch2/catch_test_macros.hpp>` and `"nand_emul_advanced.h"`.

### Checking for memory leaks

```bash
valgrind --leak-check=full --error-exitcode=1 \
    ./build/nand_flash_host_test.elf "[advanced]"
```

Run valgrind after every deinit path test.

---

## Phase 0: Safety Net

**Goal**: Lock in a regression test for the existing API before touching any code. This test must pass at every point during the entire implementation.

### P0.T1: Backward compatibility regression test

**Files:**
- Create: `host_test/main/test_backward_compat.cpp`
- Modify: `host_test/main/CMakeLists.txt` — add `test_backward_compat.cpp` to `SRCS`

**Step 1: Write the failing test**

```cpp
// host_test/main/test_backward_compat.cpp
#include <string.h>
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("existing init/read/write/erase API is unchanged", "[advanced][backward-compat]")
{
    // Use the OLD API only — no advanced headers
    nand_file_mmap_emul_config_t conf = {"", 32 * 1024 * 1024, true};
    spi_nand_flash_config_t cfg = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    spi_nand_flash_device_t *dev;

    REQUIRE(spi_nand_flash_init_device(&cfg, &dev) == ESP_OK);

    uint32_t sector_num, sector_size, block_size;
    REQUIRE(spi_nand_flash_get_capacity(dev, &sector_num) == ESP_OK);
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK);

    REQUIRE(sector_num > 0);
    REQUIRE(sector_size > 0);
    REQUIRE(block_size > 0);

    // Erase, write, read back a page — basic smoke test
    uint32_t test_block = 0;
    uint32_t pages_per_block = block_size / sector_size;
    uint32_t test_page = test_block * pages_per_block;

    REQUIRE(nand_wrap_erase_block(dev, test_block) == ESP_OK);

    uint8_t *write_buf = (uint8_t *)malloc(sector_size);
    uint8_t *read_buf  = (uint8_t *)malloc(sector_size);
    REQUIRE(write_buf != NULL);
    REQUIRE(read_buf  != NULL);

    memset(write_buf, 0xA5, sector_size);
    REQUIRE(nand_wrap_prog(dev, test_page, write_buf) == ESP_OK);
    REQUIRE(nand_wrap_read(dev, test_page, read_buf)  == ESP_OK);
    REQUIRE(memcmp(write_buf, read_buf, sector_size)  == 0);

    free(write_buf);
    free(read_buf);
    REQUIRE(spi_nand_flash_deinit_device(dev) == ESP_OK);
}
```

**Step 2: Build and verify GREEN** (this test uses only existing code, so it must pass immediately)

```bash
idf.py build && ./build/nand_flash_host_test.elf "[backward-compat]"
```

Expected: PASS. If it fails, fix the test — do not proceed with Phase 1 until this is GREEN.

**Step 3: Commit**

```bash
git add host_test/main/test_backward_compat.cpp host_test/main/CMakeLists.txt
git commit -m "test(nand-tracking): add backward compat regression test"
```

---

## Phase 1: Core Infrastructure

**Goal**: Define all headers, extend the internal handle, implement init/deinit, and no-op backends. Every task starts with a failing test.

### P1.T1 + P1.T2 + P1.T3 + P1.T4: Define all public types (headers only)

These four original tasks are pure header definitions with no runtime behavior. Write a compile-only test first.

**Files:**
- Create: `include/nand_emul_advanced.h`
- Create: `host_test/main/test_advanced_types.cpp`
- Modify: `host_test/main/CMakeLists.txt`

**Step 1: Write the failing test** (will not compile until the header exists)

```cpp
// host_test/main/test_advanced_types.cpp
#include "nand_emul_advanced.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("advanced header types compile and have correct sizes", "[advanced][types]")
{
    // Verify struct sizes match the snapshot format spec
    // snapshot_header_t must be exactly 64 bytes
    STATIC_REQUIRE(sizeof(snapshot_header_t) == 64);

    // Verify page_metadata_t has the expected fields (compile-time probe)
    page_metadata_t pm = {};
    (void)pm.page_num;
    (void)pm.program_count;
    (void)pm.program_count_total;
    (void)pm.read_count;
    (void)pm.read_count_total;
    (void)pm.first_program_timestamp;
    (void)pm.last_program_timestamp;

    // Verify block_metadata_t has the expected fields
    block_metadata_t bm = {};
    (void)bm.block_num;
    (void)bm.erase_count;
    (void)bm.first_erase_timestamp;
    (void)bm.last_erase_timestamp;
    (void)bm.total_page_programs;
    (void)bm.total_page_programs_total;
    (void)bm.is_bad_block;

    // Verify nand_wear_stats_t has write-amplification fields
    nand_wear_stats_t ws = {};
    (void)ws.logical_write_bytes_recorded;
    (void)ws.write_amplification;
    (void)ws.wear_leveling_variation;

    // Verify vtable types compile
    nand_metadata_backend_ops_t backend_ops = {};
    nand_failure_model_ops_t    failure_ops  = {};
    (void)backend_ops;
    (void)failure_ops;

    // Verify config struct compiles
    nand_emul_advanced_config_t adv_cfg = {};
    (void)adv_cfg.metadata_backend;
    (void)adv_cfg.failure_model;
    (void)adv_cfg.track_block_level;
    (void)adv_cfg.track_page_level;

    // Verify histogram types
    nand_wear_histogram_t hist = {};
    REQUIRE(hist.n_bins == 0);
    nand_wear_histograms_t hists = {};
    (void)hists.block_erase_count;
    (void)hists.page_lifetime_programs;

    REQUIRE(true); // reached here = compiled correctly
}
```

**Step 2: Build and verify RED**

```bash
idf.py build 2>&1 | grep "error:"
```

Expected: compile error — `nand_emul_advanced.h` not found.

**Step 3: Create `include/nand_emul_advanced.h`** with all types from `proposal.md` §1 and §2: `page_metadata_t`, `block_metadata_t`, `nand_wear_stats_t`, `nand_operation_context_t`, `nand_metadata_backend_ops_t`, `nand_failure_model_ops_t`, `nand_emul_advanced_config_t`, `nand_wear_histogram_t`, `nand_wear_histograms_t`, `snapshot_header_t`. Add `_Static_assert(sizeof(snapshot_header_t) == 64, ...)`. Add Doxygen on every public field.

**Step 4: Build and verify GREEN**

```bash
idf.py build && ./build/nand_flash_host_test.elf "[types]"
```

Expected: PASS. Also verify `[backward-compat]` still passes.

**Step 5: Commit**

```bash
git add include/nand_emul_advanced.h host_test/main/test_advanced_types.cpp host_test/main/CMakeLists.txt
git commit -m "feat(nand-tracking): add advanced tracking public header types"
```

---

### P1.T5 + P1.T6 + P1.T7: Extend handle, implement advanced init/deinit

**Files:**
- Modify: `src/nand_linux_mmap_emul.c` — extend `nand_mmap_emul_handle_t`, add `nand_emul_advanced_init()`, `nand_emul_advanced_deinit()`
- Create: `host_test/main/test_advanced_init.cpp`
- Modify: `host_test/main/CMakeLists.txt`

**Step 1: Write the failing test**

```cpp
// host_test/main/test_advanced_init.cpp
#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("advanced init returns ESP_OK with minimal config", "[advanced][advanced-init]")
{
    nand_emul_advanced_config_t cfg = {};
    cfg.base_config = {"", 32 * 1024 * 1024, true};
    cfg.metadata_backend = NULL;  // no backend yet
    cfg.failure_model    = NULL;
    cfg.track_block_level = true;
    cfg.track_page_level  = true;

    spi_nand_flash_device_t *dev;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);
    REQUIRE(dev != NULL);

    // Basic operations must still work after advanced init
    uint32_t sector_num;
    REQUIRE(spi_nand_flash_get_capacity(dev, &sector_num) == ESP_OK);
    REQUIRE(sector_num > 0);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("advanced init with NULL config returns error", "[advanced][advanced-init]")
{
    spi_nand_flash_device_t *dev = NULL;
    REQUIRE(nand_emul_advanced_init(&dev, NULL) != ESP_OK);
    REQUIRE(dev == NULL);
}

TEST_CASE("advanced field is NULL after normal nand_emul_init", "[advanced][advanced-init]")
{
    // The existing init path must leave the advanced pointer NULL
    // (backward compat: existing code must not be affected)
    nand_file_mmap_emul_config_t conf = {"", 32 * 1024 * 1024, true};
    spi_nand_flash_config_t nand_cfg = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    spi_nand_flash_device_t *dev;
    REQUIRE(spi_nand_flash_init_device(&nand_cfg, &dev) == ESP_OK);

    // Expose via a getter rather than poking internals — implement this getter
    REQUIRE(nand_emul_has_advanced_tracking(dev) == false);

    REQUIRE(spi_nand_flash_deinit_device(dev) == ESP_OK);
}
```

**Step 2: Build and verify RED**

```bash
idf.py build 2>&1 | grep "error:"
```

Expected: `nand_emul_advanced_init` undefined.

**Step 3: Implement** in `src/nand_linux_mmap_emul.c`:
- Add `nand_advanced_context_t` internal struct (backend handle, failure handle, flags, geometry, timestamp fn, `logical_write_bytes_recorded`) with an `advanced` pointer to it inside `nand_mmap_emul_handle_t`; the pointer is `NULL` after `nand_emul_init()`.
- Implement `nand_emul_advanced_init()`: call existing base init, allocate `advanced` struct, cache device geometry, set timestamp function to default monotonic counter if not provided, call backend `init()` if non-NULL.
- Implement `nand_emul_advanced_deinit()`: call backend `deinit()` and failure model `deinit()` if present, free `advanced` struct, call base deinit.
- Implement `nand_emul_has_advanced_tracking()` — returns `true` if `advanced != NULL`.

**Step 4: Build and verify GREEN**

```bash
idf.py build && ./build/nand_flash_host_test.elf "[advanced-init]"
```

Also run: `./build/nand_flash_host_test.elf "[backward-compat]"` — must still pass.

Run valgrind on `[advanced-init]` to confirm no leaks.

**Step 5: Commit**

```bash
git commit -m "feat(nand-tracking): add nand_emul_advanced_init/deinit"
```

---

### P1.T8 + P1.T9: No-op backend and no-op failure model

**Files:**
- Create: `src/backends/noop_backend.c`
- Create: `src/failure_models/noop_failure_model.c`
- Modify: `CMakeLists.txt` (component sources) — add new `.c` files

**Step 1: Write the failing test** (add to `test_advanced_init.cpp`)

```cpp
TEST_CASE("advanced init with no-op backend and failure model succeeds", "[advanced][advanced-init]")
{
    nand_emul_advanced_config_t cfg = {};
    cfg.base_config = {"", 32 * 1024 * 1024, true};
    cfg.metadata_backend        = &nand_noop_backend;       // extern symbol
    cfg.metadata_backend_config = NULL;
    cfg.failure_model           = &nand_noop_failure_model; // extern symbol
    cfg.failure_model_config    = NULL;
    cfg.track_block_level = true;
    cfg.track_page_level  = true;

    spi_nand_flash_device_t *dev;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);

    // Operations must work; no-op model never fails anything
    uint32_t block_size, sector_size;
    REQUIRE(spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    uint32_t pages_per_block = block_size / sector_size;
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    uint8_t *buf = (uint8_t *)malloc(sector_size);
    REQUIRE(buf != NULL);
    memset(buf, 0xBB, sector_size);
    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK);

    memset(buf, 0, sector_size);
    REQUIRE(nand_wrap_read(dev, 0, buf) == ESP_OK);
    // No-op failure model must not corrupt the buffer
    REQUIRE(buf[0] == 0xBB);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

**Step 2: Verify RED** — `nand_noop_backend` undefined.

**Step 3: Implement** the no-op backend: all ops return `ESP_OK`; query methods fill output with zeros; iteration never calls callback. Implement the no-op failure model: all `should_fail_*` return `false`; `corrupt_read_data` is a no-op; `is_block_bad` returns `false`. Declare `extern const nand_metadata_backend_ops_t nand_noop_backend` and `extern const nand_failure_model_ops_t nand_noop_failure_model` in `nand_emul_advanced.h`.

**Step 4: Verify GREEN**

```bash
idf.py build && ./build/nand_flash_host_test.elf "[advanced-init]"
```

Valgrind on `[advanced-init]` — no leaks.

**Step 5: Commit**

```bash
git commit -m "feat(nand-tracking): add no-op backend and failure model"
```

---

## Phase 2: Sparse Hash Backend

**Goal**: Implement the production metadata storage backend. One test per sub-component, written before the code.

### P2.T1–T5: Hash table core (create, insert/lookup, rehash, remove, iterate)

**Files:**
- Create: `src/backends/hash_table.h`
- Create: `src/backends/hash_table.c`
- Create: `host_test/main/test_hash_table.cpp`
- Modify: `host_test/main/CMakeLists.txt`

#### P2.T1a: Write failing test for hash_table_create/destroy

```cpp
// host_test/main/test_hash_table.cpp
#include "backends/hash_table.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("hash_table_create returns non-null and hash_table_destroy frees it", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(16, sizeof(uint32_t), 0.75f);
    REQUIRE(t != NULL);
    REQUIRE(t->count == 0);
    REQUIRE(t->capacity >= 16);
    hash_table_destroy(t);
    // valgrind will catch leaks
}
```

Build RED → implement `hash_table_create` / `hash_table_destroy` → build GREEN → valgrind → commit.

#### P2.T2a: Write failing test for insert and lookup

Add to `test_hash_table.cpp`:

```cpp
TEST_CASE("insert 100 entries, all retrievable by key", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(8, sizeof(uint32_t), 0.75f);
    REQUIRE(t != NULL);

    for (uint32_t i = 0; i < 100; i++) {
        hash_node_t *n = hash_table_get_or_insert(t, i);
        REQUIRE(n != NULL);
        *(uint32_t *)n->data = i * 10;
    }

    REQUIRE(t->count == 100);

    for (uint32_t i = 0; i < 100; i++) {
        hash_node_t *n = hash_table_get(t, i);
        REQUIRE(n != NULL);
        REQUIRE(*(uint32_t *)n->data == i * 10);
    }

    hash_table_destroy(t);
}

TEST_CASE("inserting duplicate key returns the same node", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(8, sizeof(uint32_t), 0.75f);
    hash_node_t *a = hash_table_get_or_insert(t, 42);
    hash_node_t *b = hash_table_get_or_insert(t, 42);
    REQUIRE(a == b);
    REQUIRE(t->count == 1);
    hash_table_destroy(t);
}
```

Build RED → implement insert/lookup → GREEN → commit.

#### P2.T3a: Write failing test for automatic rehashing

```cpp
TEST_CASE("insert 1000 entries triggers rehash, all still retrievable", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(4, sizeof(uint32_t), 0.75f);
    size_t initial_cap = t->capacity;

    for (uint32_t i = 0; i < 1000; i++) {
        hash_node_t *n = hash_table_get_or_insert(t, i);
        REQUIRE(n != NULL);
    }

    REQUIRE(t->capacity > initial_cap); // rehash occurred
    REQUIRE(t->count == 1000);

    for (uint32_t i = 0; i < 1000; i++) {
        REQUIRE(hash_table_get(t, i) != NULL);
    }
    hash_table_destroy(t);
}
```

Build RED → implement rehash → GREEN → commit.

#### P2.T4a: Write failing test for removal

```cpp
TEST_CASE("remove an entry, it is no longer found", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(8, sizeof(uint32_t), 0.75f);
    hash_table_get_or_insert(t, 7);
    hash_table_get_or_insert(t, 14); // same bucket if capacity=8, tests chain removal
    hash_table_remove(t, 7);
    REQUIRE(hash_table_get(t, 7)  == NULL);
    REQUIRE(hash_table_get(t, 14) != NULL); // neighbor unaffected
    hash_table_destroy(t);
}
```

Build RED → implement remove → GREEN → commit.

#### P2.T5a: Write failing test for iteration

```cpp
TEST_CASE("iterate visits all 50 inserted entries", "[advanced][hash-table]")
{
    hash_table_t *t = hash_table_create(8, sizeof(uint32_t), 0.75f);
    for (uint32_t i = 0; i < 50; i++) {
        hash_table_get_or_insert(t, i);
    }

    int visited = 0;
    hash_table_iterate(t, [](hash_node_t *node, void *ud) -> bool {
        (*(int *)ud)++;
        return true; // continue
    }, &visited);

    REQUIRE(visited == 50);

    // Early termination: stop after 10
    int early = 0;
    hash_table_iterate(t, [](hash_node_t *node, void *ud) -> bool {
        return ++(*(int *)ud) < 10;
    }, &early);
    REQUIRE(early == 10);

    hash_table_destroy(t);
}
```

Build RED → implement iterate → GREEN → commit.

---

### P2.T6–T15: Sparse hash backend

**Files:**
- Create: `src/backends/sparse_hash_backend.c`
- Create: `host_test/main/test_sparse_hash_backend.cpp`
- Modify: `host_test/main/CMakeLists.txt`

Each sub-task below follows: write test → RED → implement → GREEN → commit.

#### P2.T7a: Test sparse backend init/deinit

```cpp
// host_test/main/test_sparse_hash_backend.cpp
#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include <catch2/catch_test_macros.hpp>

static spi_nand_flash_device_t *make_advanced_dev_with_sparse_backend(void)
{
    sparse_hash_backend_config_t be_cfg = {};
    be_cfg.initial_capacity = 16;
    be_cfg.load_factor = 0.75f;
    be_cfg.enable_histogram_query = false;

    nand_emul_advanced_config_t cfg = {};
    cfg.base_config = {"", 32 * 1024 * 1024, true};
    cfg.metadata_backend        = &nand_sparse_hash_backend;
    cfg.metadata_backend_config = &be_cfg;
    cfg.track_block_level = true;
    cfg.track_page_level  = true;

    spi_nand_flash_device_t *dev;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);
    return dev;
}

TEST_CASE("sparse backend init/deinit, no leaks", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → implement sparse backend init/deinit → GREEN → valgrind → commit.

#### P2.T9a: Test block erase tracking

```cpp
TEST_CASE("block erase increments erase_count in block metadata", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    REQUIRE(nand_wrap_erase_block(dev, 3) == ESP_OK);

    block_metadata_t meta = {};
    REQUIRE(nand_emul_get_block_wear(dev, 3, &meta) == ESP_OK);
    REQUIRE(meta.erase_count == 1);
    REQUIRE(meta.block_num == 3);

    REQUIRE(nand_wrap_erase_block(dev, 3) == ESP_OK);
    REQUIRE(nand_emul_get_block_wear(dev, 3, &meta) == ESP_OK);
    REQUIRE(meta.erase_count == 2);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("never-erased block returns zero metadata", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();
    block_metadata_t meta = {};
    REQUIRE(nand_emul_get_block_wear(dev, 99, &meta) == ESP_OK);
    REQUIRE(meta.erase_count == 0);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → implement `on_block_erase` + `get_block_info` + wire `nand_emul_get_block_wear()` → GREEN → commit.

#### P2.T11a: Test page program tracking

```cpp
TEST_CASE("page program increments program_count", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    uint8_t *buf = (uint8_t *)malloc(sector_size);
    REQUIRE(buf != NULL);
    memset(buf, 0xCC, sector_size);

    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK); // page 0

    page_metadata_t pm = {};
    REQUIRE(nand_emul_get_page_wear(dev, 0, &pm) == ESP_OK);
    REQUIRE(pm.program_count == 1);
    REQUIRE(pm.program_count_total == 0); // no erase yet, nothing folded

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("program_count resets to 0 after block erase, total preserved", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK); // program_count becomes 1
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK); // fold: total += 1, count = 0

    page_metadata_t pm = {};
    REQUIRE(nand_emul_get_page_wear(dev, 0, &pm) == ESP_OK);
    REQUIRE(pm.program_count       == 0);
    REQUIRE(pm.program_count_total == 1);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → implement `on_page_program`, block-erase fold loop, `get_page_info`, wire `nand_emul_get_page_wear()` → GREEN → commit.

#### P2.T11b-a: Test page read tracking and erase fold

```cpp
TEST_CASE("read_count increments once per page per read call", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    REQUIRE(nand_wrap_read(dev, 0, buf) == ESP_OK);
    REQUIRE(nand_wrap_read(dev, 0, buf) == ESP_OK);
    REQUIRE(nand_wrap_read(dev, 0, buf) == ESP_OK);

    page_metadata_t pm = {};
    REQUIRE(nand_emul_get_page_wear(dev, 0, &pm) == ESP_OK);
    REQUIRE(pm.read_count == 3);
    REQUIRE(pm.read_count_total == 0);

    // Erase: fold read_count into read_count_total
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_emul_get_page_wear(dev, 0, &pm) == ESP_OK);
    REQUIRE(pm.read_count       == 0);
    REQUIRE(pm.read_count_total == 3);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → implement `on_page_read` in sparse backend, fold on erase, call `on_page_read` per page in `nand_emul_read()` → GREEN → commit.

#### P2.T15a: Test aggregate statistics

```cpp
TEST_CASE("get_wear_stats returns correct min/max/avg after known operations", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    // Erase block 0 once, block 1 three times
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);

    nand_wear_stats_t stats = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &stats) == ESP_OK);

    REQUIRE(stats.min_block_erases == 1);
    REQUIRE(stats.max_block_erases == 3);
    // avg = (1+3)/2 = 2; variation = (3-1)/2 = 1.0
    REQUIRE(stats.wear_leveling_variation == Catch::Approx(1.0));

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("wear_leveling_variation is 0.0 when no blocks erased", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();
    nand_wear_stats_t stats = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &stats) == ESP_OK);
    REQUIRE(stats.wear_leveling_variation == 0.0);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → implement `get_stats`, wire `nand_emul_get_wear_stats()` → GREEN → commit.

#### P2.T13a + P2.T14a: Test block and page iteration

```cpp
TEST_CASE("iterate_worn_blocks visits all erased blocks", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    for (uint32_t b = 0; b < 5; b++) {
        REQUIRE(nand_wrap_erase_block(dev, b) == ESP_OK);
    }

    int count = 0;
    REQUIRE(nand_emul_iterate_worn_blocks(dev, [](uint32_t, block_metadata_t *, void *ud) -> bool {
        (*(int *)ud)++;
        return true;
    }, &count) == ESP_OK);

    REQUIRE(count == 5);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → implement `iterate_blocks`, wire `nand_emul_iterate_worn_blocks()` → GREEN → commit.

---

### P2.T3a (memory efficiency): Write the spec test up front

This test drives the sparse backend design — it must be written and RED before backend optimisation work.

```cpp
TEST_CASE("sparse metadata uses less than 5% of emulated flash size", "[advanced][sparse-backend][memory]")
{
    // 32 MB flash; write to 100 random pages — metadata overhead must be tiny
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size, block_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK);

    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    srand(42);
    uint32_t total_pages = (32 * 1024 * 1024) / sector_size;
    for (int i = 0; i < 1000; i++) {
        uint32_t page = rand() % total_pages;
        uint32_t block = page / (block_size / sector_size);
        nand_wrap_erase_block(dev, block); // ignore errors (may double-erase)
        nand_wrap_prog(dev, page, buf);
    }

    memory_usage_t mem = {};
    REQUIRE(nand_emul_get_memory_usage(dev, &mem) == ESP_OK);

    size_t flash_size = 32 * 1024 * 1024;
    size_t budget = flash_size / 20; // 5%
    INFO("Metadata bytes used: " << mem.total_bytes << " budget: " << budget);
    REQUIRE(mem.total_bytes < budget);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Wire `nand_emul_get_memory_usage()` to the sparse backend's memory tracking. Build RED → implement → GREEN → commit.

---

## Phase 3: Integration with Core Operations

**Goal**: Hook metadata tracking and failure injection into `nand_emul_erase_block()`, `nand_emul_write()`, `nand_emul_read()`. Tests written before modifying each handler.

### P3.T1: Timestamp helper

**Step 1: Write failing test** (add to `test_advanced_init.cpp`):

```cpp
TEST_CASE("default timestamp counter increments on each advanced operation", "[advanced][timestamp]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);

    block_metadata_t m0 = {}, m1 = {};
    REQUIRE(nand_emul_get_block_wear(dev, 0, &m0) == ESP_OK);
    REQUIRE(nand_emul_get_block_wear(dev, 1, &m1) == ESP_OK);

    // Timestamps must be strictly increasing
    REQUIRE(m1.first_erase_timestamp > m0.first_erase_timestamp);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → implement default monotonic counter in advanced context, pass to `on_block_erase` → GREEN → commit.

---

### P3.T2 + P3.T3: Erase failure check + metadata tracking

Write tests first, then modify `nand_emul_erase_block()`.

**Step 1: Write failing tests** (create `host_test/main/test_integration.cpp`):

```cpp
// host_test/main/test_integration.cpp
#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include <catch2/catch_test_macros.hpp>

// Helper: make device with threshold failure model, max N erases per block
static spi_nand_flash_device_t *make_threshold_dev(uint32_t max_erases)
{
    static threshold_failure_config_t th_cfg;
    th_cfg = {max_erases, 100000, true};

    static sparse_hash_backend_config_t be_cfg;
    be_cfg = {16, 0.75f, false};

    static nand_emul_advanced_config_t cfg;
    cfg = {};
    cfg.base_config = {"", 32 * 1024 * 1024, true};
    cfg.metadata_backend        = &nand_sparse_hash_backend;
    cfg.metadata_backend_config = &be_cfg;
    cfg.failure_model           = &nand_threshold_failure_model;
    cfg.failure_model_config    = &th_cfg;
    cfg.track_block_level = true;
    cfg.track_page_level  = true;

    spi_nand_flash_device_t *dev;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);
    return dev;
}

TEST_CASE("erase fails after threshold exceeded", "[advanced][integration]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(2);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK); // 1st
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK); // 2nd
    // 3rd must fail (threshold = 2)
    esp_err_t ret = nand_wrap_erase_block(dev, 0);
    REQUIRE(ret != ESP_OK);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("erase metadata is updated even after close-to-threshold erase", "[advanced][integration]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(10);

    for (int i = 0; i < 5; i++) {
        REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    }

    block_metadata_t meta = {};
    REQUIRE(nand_emul_get_block_wear(dev, 0, &meta) == ESP_OK);
    REQUIRE(meta.erase_count == 5);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

**Step 2: Verify RED** (threshold model not implemented yet).

**Step 3: Implement** threshold failure model (P5.T1–T3 below), then wire failure check + metadata update into `nand_emul_erase_block()`.

**Step 4: Verify GREEN**

```bash
./build/nand_flash_host_test.elf "[integration]" && ./build/nand_flash_host_test.elf "[backward-compat]"
```

**Step 5: Commit**

---

### P3.T4 + P3.T5: Write failure check + page tracking

Add to `test_integration.cpp`:

```cpp
TEST_CASE("write spanning multiple pages increments all overlapped page program_counts", "[advanced][integration]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size, block_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    // Write 3 pages worth of data starting at page 0
    uint8_t *buf = (uint8_t *)calloc(3, sector_size);
    REQUIRE(buf != NULL);
    // nand_wrap_prog writes one page; call it 3 times
    REQUIRE(nand_wrap_prog(dev, 0, buf)               == ESP_OK);
    REQUIRE(nand_wrap_prog(dev, 1, buf + sector_size)  == ESP_OK);
    REQUIRE(nand_wrap_prog(dev, 2, buf + 2*sector_size) == ESP_OK);

    for (uint32_t p = 0; p < 3; p++) {
        page_metadata_t pm = {};
        REQUIRE(nand_emul_get_page_wear(dev, p, &pm) == ESP_OK);
        REQUIRE(pm.program_count == 1);
    }

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → wire page program loop into `nand_emul_write()` → GREEN → commit.

---

### P3.T6 + P3.T6b + P3.T7: Read failure + read metadata + data corruption

Add to `test_integration.cpp`:

```cpp
TEST_CASE("read_count is incremented after a successful read", "[advanced][integration]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    REQUIRE(nand_wrap_read(dev, 0, buf) == ESP_OK);
    REQUIRE(nand_wrap_read(dev, 0, buf) == ESP_OK);

    page_metadata_t pm = {};
    REQUIRE(nand_emul_get_page_wear(dev, 0, &pm) == ESP_OK);
    REQUIRE(pm.read_count == 2);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("should_fail_read prevents buffer modification", "[advanced][integration]")
{
    // Use threshold model configured to always fail reads (max 0 programs → use
    // a custom always-fail model; or use threshold with max_block_erases=0 and
    // a wrapper — for simplicity use the probabilistic model with rated_cycles=1
    // and force check via a known-deterministic seed).
    // Simplest: implement a tiny test-only failure model that always fails reads.
    // Add a `nand_always_fail_read_model` to test helpers.
    spi_nand_flash_device_t *dev; // set up with always-fail-read model
    // ... setup ...

    uint8_t sentinel = 0xDE;
    uint8_t buf[64];
    memset(buf, sentinel, sizeof(buf));

    // Read must fail and buffer must remain unchanged
    esp_err_t ret = nand_wrap_read(dev, 0, buf);
    REQUIRE(ret != ESP_OK);
    REQUIRE(buf[0] == sentinel); // buffer untouched

    // Cleanup
}
```

Build RED → wire `should_fail_read` before memcpy, `on_page_read` after memcpy, `corrupt_read_data` after metadata → GREEN → commit.

---

## Phase 4: Query API & Snapshots

**Goal**: Expose all public query functions and implement binary snapshot save/load. Tests before every function.

### P4.T1 + P4.T2: `nand_emul_get_block_wear` and `nand_emul_get_page_wear`

These are already tested implicitly in Phase 2 and 3. Add edge-case tests:

```cpp
TEST_CASE("get_block_wear returns ESP_ERR_INVALID_STATE without advanced init", "[advanced][query-api]")
{
    nand_file_mmap_emul_config_t conf = {"", 32 * 1024 * 1024, true};
    spi_nand_flash_config_t nand_cfg = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    spi_nand_flash_device_t *dev;
    REQUIRE(spi_nand_flash_init_device(&nand_cfg, &dev) == ESP_OK);

    block_metadata_t meta = {};
    REQUIRE(nand_emul_get_block_wear(dev, 0, &meta) == ESP_ERR_INVALID_STATE);

    REQUIRE(spi_nand_flash_deinit_device(dev) == ESP_OK);
}
```

Build RED → add guard check to `nand_emul_get_block_wear()` → GREEN → commit.

---

### P4.T3b: `nand_emul_record_logical_write` and WAF

```cpp
TEST_CASE("write amplification factor is 0 when no logical bytes recorded", "[advanced][query-api]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();
    nand_wear_stats_t stats = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &stats) == ESP_OK);
    REQUIRE(stats.write_amplification == 0.0);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("WAF = physical_bytes / logical_bytes after recording logical writes", "[advanced][query-api]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    // Write 2 physical pages
    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK);
    REQUIRE(nand_wrap_prog(dev, 1, buf) == ESP_OK);

    // Record only 1 page worth of logical bytes
    REQUIRE(nand_emul_record_logical_write(dev, sector_size) == ESP_OK);

    nand_wear_stats_t stats = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &stats) == ESP_OK);
    // WAF = (2 * sector_size) / sector_size = 2.0
    REQUIRE(stats.write_amplification == Catch::Approx(2.0).epsilon(0.01));

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("record_logical_write returns NOT_SUPPORTED without advanced init", "[advanced][query-api]")
{
    nand_file_mmap_emul_config_t conf = {"", 32 * 1024 * 1024, true};
    spi_nand_flash_config_t nand_cfg = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    spi_nand_flash_device_t *dev;
    REQUIRE(spi_nand_flash_init_device(&nand_cfg, &dev) == ESP_OK);
    REQUIRE(nand_emul_record_logical_write(dev, 1024) == ESP_ERR_NOT_SUPPORTED);
    REQUIRE(spi_nand_flash_deinit_device(dev) == ESP_OK);
}
```

Build RED → implement `nand_emul_record_logical_write()` and WAF merge in `nand_emul_get_wear_stats()` → GREEN → commit.

---

### P4.T3a: Histograms

```cpp
TEST_CASE("get_wear_histograms returns NOT_SUPPORTED when enable_histogram_query=false", "[advanced][query-api]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend(); // enable_histogram_query=false
    nand_wear_histograms_t hists = {};
    hists.block_erase_count.n_bins   = 8;
    hists.block_erase_count.bin_width = 1;
    hists.page_lifetime_programs.n_bins   = 8;
    hists.page_lifetime_programs.bin_width = 1;
    REQUIRE(nand_emul_get_wear_histograms(dev, &hists) == ESP_ERR_NOT_SUPPORTED);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("histogram bins correctly count known erase distribution", "[advanced][query-api]")
{
    sparse_hash_backend_config_t be_cfg = {16, 0.75f, true}; // enable histograms
    // ... init dev with enable_histogram_query=true ...

    // Erase block 0 once, block 1 twice, block 2 three times
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 2) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 2) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 2) == ESP_OK);

    nand_wear_histograms_t hists = {};
    hists.block_erase_count.n_bins   = 8;
    hists.block_erase_count.bin_width = 1;
    hists.page_lifetime_programs.n_bins   = 4;
    hists.page_lifetime_programs.bin_width = 1;

    REQUIRE(nand_emul_get_wear_histograms(dev, &hists) == ESP_OK);
    // bin 0 (0 <= count < 1): unused blocks → 0 in sparse (sparse only has touched blocks)
    // bin 1 (1 <= count < 2): block 0 → count[1] = 1
    // bin 2 (2 <= count < 3): block 1 → count[2] = 1
    // bin 3 (3 <= count < 4): block 2 → count[3] = 1
    REQUIRE(hists.block_erase_count.count[1] == 1);
    REQUIRE(hists.block_erase_count.count[2] == 1);
    REQUIRE(hists.block_erase_count.count[3] == 1);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → implement `get_histograms` sweep in sparse backend + wire `nand_emul_get_wear_histograms()` → GREEN → commit.

---

### P4.T6–T10: Snapshot save/load

#### P4.T6a: Test snapshot header format

```cpp
TEST_CASE("snapshot_header_t is exactly 64 bytes (static)", "[advanced][snapshot]")
{
    STATIC_REQUIRE(sizeof(snapshot_header_t) == 64);
}
```

Already covered by P1.T1 compile test — confirm it passes, no new code needed.

#### P4.T7a + P4.T8a: Test snapshot save/load roundtrip

Create `host_test/main/test_snapshot.cpp`:

```cpp
// host_test/main/test_snapshot.cpp
#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>

TEST_CASE("snapshot save/load roundtrip preserves all metadata", "[advanced][snapshot]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    // Perform known operations: erase blocks 0..2, program pages 0..4, read page 1 twice
    for (uint32_t b = 0; b < 3; b++) {
        REQUIRE(nand_wrap_erase_block(dev, b) == ESP_OK);
    }
    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);
    for (uint32_t p = 0; p < 5; p++) {
        REQUIRE(nand_wrap_prog(dev, p, buf) == ESP_OK);
    }
    REQUIRE(nand_wrap_read(dev, 1, buf) == ESP_OK);
    REQUIRE(nand_wrap_read(dev, 1, buf) == ESP_OK);

    // Capture stats before save
    nand_wear_stats_t before = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &before) == ESP_OK);

    // Save snapshot
    const char *snap_path = "/tmp/test_nand_snapshot.bin";
    REQUIRE(nand_emul_save_snapshot(dev, snap_path) == ESP_OK);

    // Deinit and reinit
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
    dev = make_advanced_dev_with_sparse_backend();

    // Load snapshot
    REQUIRE(nand_emul_load_snapshot(dev, snap_path) == ESP_OK);

    // Verify stats match
    nand_wear_stats_t after = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &after) == ESP_OK);
    REQUIRE(after.max_block_erases == before.max_block_erases);
    REQUIRE(after.min_block_erases == before.min_block_erases);
    REQUIRE(after.total_bytes_written == before.total_bytes_written);

    // Verify per-block and per-page metadata
    block_metadata_t bm = {};
    REQUIRE(nand_emul_get_block_wear(dev, 0, &bm) == ESP_OK);
    REQUIRE(bm.erase_count == 1);

    page_metadata_t pm = {};
    REQUIRE(nand_emul_get_page_wear(dev, 1, &pm) == ESP_OK);
    REQUIRE(pm.program_count == 1);
    REQUIRE(pm.read_count    == 2); // two reads in current erase cycle

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
    remove(snap_path);
}

TEST_CASE("loading snapshot with corrupt header returns error, leaves state unchanged", "[advanced][snapshot]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    const char *snap_path = "/tmp/test_nand_corrupt.bin";
    REQUIRE(nand_emul_save_snapshot(dev, snap_path) == ESP_OK);

    // Corrupt byte 4 in the header (version field → wrong magic region)
    FILE *fp = fopen(snap_path, "r+b");
    REQUIRE(fp != NULL);
    fseek(fp, 4, SEEK_SET);
    uint8_t bad = 0xFF;
    fwrite(&bad, 1, 1, fp);
    fclose(fp);

    // Load on a fresh device — must return error, NOT crash or change state
    spi_nand_flash_device_t *dev2 = make_advanced_dev_with_sparse_backend();
    esp_err_t ret = nand_emul_load_snapshot(dev2, snap_path);
    REQUIRE(ret != ESP_OK); // ESP_ERR_INVALID_CRC or similar

    // State after failed load: zero metadata (empty backend)
    block_metadata_t bm = {};
    REQUIRE(nand_emul_get_block_wear(dev2, 0, &bm) == ESP_OK);
    REQUIRE(bm.erase_count == 0); // unchanged / rolled back

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
    REQUIRE(nand_emul_advanced_deinit(dev2) == ESP_OK);
    remove(snap_path);
}
```

Build RED → implement `sparse_hash_save_snapshot` / `sparse_hash_load_snapshot` + `snapshot_header_t` + CRC32 + version check + wire public API → GREEN → commit.

---

### P4.T11 + P4.T12: JSON export

```cpp
TEST_CASE("export_json creates a valid JSON file", "[advanced][snapshot]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    const char *json_path = "/tmp/test_nand_export.json";
    REQUIRE(nand_emul_export_json(dev, json_path) == ESP_OK);

    // File must exist and be non-empty
    FILE *fp = fopen(json_path, "r");
    REQUIRE(fp != NULL);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);
    REQUIRE(size > 0);

    // Must start with '{' (JSON object)
    fp = fopen(json_path, "r");
    int first = fgetc(fp);
    fclose(fp);
    REQUIRE(first == '{');

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
    remove(json_path);
}
```

Build RED → implement JSON export → GREEN → commit.

---

## Phase 5: Built-in Failure Models

**Goal**: Implement threshold and probabilistic failure models. Tests define the expected mathematical behavior before any code is written.

### P5.T1–T4: Threshold failure model

**Files:**
- Create: `src/failure_models/threshold_failure_model.c`
- Create: `host_test/main/test_threshold_failure_model.cpp`
- Modify: `host_test/main/CMakeLists.txt`

**Step 1: Write failing tests**

```cpp
// host_test/main/test_threshold_failure_model.cpp
#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include <catch2/catch_test_macros.hpp>

static spi_nand_flash_device_t *make_threshold_dev_local(uint32_t max_erases, uint32_t max_programs)
{
    static threshold_failure_config_t th_cfg;
    th_cfg = {max_erases, max_programs, true};

    static sparse_hash_backend_config_t be_cfg;
    be_cfg = {16, 0.75f, false};

    static nand_emul_advanced_config_t cfg;
    cfg = {};
    cfg.base_config = {"", 32 * 1024 * 1024, true};
    cfg.metadata_backend        = &nand_sparse_hash_backend;
    cfg.metadata_backend_config = &be_cfg;
    cfg.failure_model           = &nand_threshold_failure_model;
    cfg.failure_model_config    = &th_cfg;
    cfg.track_block_level = true;
    cfg.track_page_level  = true;

    spi_nand_flash_device_t *dev;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);
    return dev;
}

TEST_CASE("erase succeeds for max_erases and fails on max+1", "[advanced][threshold]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev_local(10, 100000);

    for (int i = 0; i < 10; i++) {
        REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    }
    REQUIRE(nand_wrap_erase_block(dev, 0) != ESP_OK);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("other blocks are unaffected when one block exceeds threshold", "[advanced][threshold]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev_local(1, 100000);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK); // block 0: at limit
    REQUIRE(nand_wrap_erase_block(dev, 0) != ESP_OK); // block 0: over limit
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK); // block 1: unaffected

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("block is marked bad after erase threshold exceeded", "[advanced][threshold]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev_local(2, 100000);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 0) != ESP_OK); // triggers bad block mark

    block_metadata_t meta = {};
    REQUIRE(nand_emul_get_block_wear(dev, 0, &meta) == ESP_OK);
    REQUIRE(meta.is_bad_block == true);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

**Step 2: Verify RED** — `nand_threshold_failure_model` undefined.

**Step 3: Implement** threshold model. Wire `is_block_bad()` → call backend `set_bad_block()` when model marks block bad.

**Step 4: Verify GREEN**

```bash
./build/nand_flash_host_test.elf "[threshold]" && ./build/nand_flash_host_test.elf "[backward-compat]"
```

**Step 5: Commit**

---

### P5.T5–T10: Probabilistic failure model

**Files:**
- Create: `src/failure_models/probabilistic_failure_model.c`
- Create: `host_test/main/test_probabilistic_failure_model.cpp`
- Modify: `host_test/main/CMakeLists.txt`

**Step 1: Write the Weibull formula test first** — this is a pure math function, the cleanest TDD case:

```cpp
// host_test/main/test_probabilistic_failure_model.cpp
#include "nand_emul_advanced.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>

// Test the Weibull formula independently before wiring it into the model
// Declare the internal function visible for testing via a test-only header or
// by compiling the .c file with UNIT_TEST defined to expose a wrapper.
// Recommended: add a thin public test API nand_emul_weibull_failure_probability().
TEST_CASE("Weibull CDF: P(0) = 0.0", "[advanced][probabilistic]")
{
    REQUIRE(nand_emul_weibull_probability(0, 100000, 2.0) == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("Weibull CDF: P(rated_cycles) ≈ 0.632 (1 - 1/e)", "[advanced][probabilistic]")
{
    double p = nand_emul_weibull_probability(100000, 100000, 2.0);
    REQUIRE(p == Catch::Approx(1.0 - std::exp(-1.0)).epsilon(0.001));
}

TEST_CASE("Weibull CDF: P → 1.0 as cycles → infinity", "[advanced][probabilistic]")
{
    double p = nand_emul_weibull_probability(10000000, 100000, 2.0);
    REQUIRE(p > 0.999);
}

TEST_CASE("Weibull CDF: P is monotonically increasing", "[advanced][probabilistic]")
{
    double prev = 0.0;
    for (uint32_t n = 0; n <= 200000; n += 10000) {
        double p = nand_emul_weibull_probability(n, 100000, 2.0);
        REQUIRE(p >= prev);
        prev = p;
    }
}
```

Build RED → add `nand_emul_weibull_probability()` declaration to header, implement in `probabilistic_failure_model.c` → GREEN → commit.

```cpp
TEST_CASE("probabilistic model: fixed seed produces reproducible failures", "[advanced][probabilistic]")
{
    // Two devices with identical seeds must fail at the same erase count
    auto run_until_fail = [](uint32_t seed) -> int {
        probabilistic_failure_config_t pf_cfg = {};
        pf_cfg.rated_cycles   = 100;
        pf_cfg.wear_out_shape = 2.0;
        pf_cfg.base_bit_error_rate = 1e-3; // high BER to trigger quickly
        pf_cfg.random_seed    = seed;

        sparse_hash_backend_config_t be_cfg = {16, 0.75f, false};
        nand_emul_advanced_config_t cfg = {};
        cfg.base_config = {"", 4 * 1024 * 1024, true};
        cfg.metadata_backend        = &nand_sparse_hash_backend;
        cfg.metadata_backend_config = &be_cfg;
        cfg.failure_model           = &nand_probabilistic_failure_model;
        cfg.failure_model_config    = &pf_cfg;
        cfg.track_block_level = true;
        cfg.track_page_level  = false;

        spi_nand_flash_device_t *dev;
        REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);

        int attempts = 0;
        for (int i = 0; i < 10000; i++) {
            esp_err_t ret = nand_wrap_erase_block(dev, 0);
            attempts++;
            if (ret != ESP_OK) break;
        }

        nand_emul_advanced_deinit(dev);
        return attempts;
    };

    int run1 = run_until_fail(42);
    int run2 = run_until_fail(42);
    REQUIRE(run1 == run2); // deterministic
    REQUIRE(run1 > 0);
}

TEST_CASE("bit flip injection: BER increases with erase wear", "[advanced][probabilistic]")
{
    // Fresh block vs heavily worn block: worn block should have more bit flips
    auto count_flips = [](uint32_t pre_erases) -> int {
        probabilistic_failure_config_t pf_cfg = {};
        pf_cfg.rated_cycles       = 500;
        pf_cfg.wear_out_shape     = 2.0;
        pf_cfg.base_bit_error_rate = 1e-2;
        pf_cfg.random_seed        = 99;

        sparse_hash_backend_config_t be_cfg = {16, 0.75f, false};
        nand_emul_advanced_config_t cfg = {};
        cfg.base_config = {"", 4 * 1024 * 1024, true};
        cfg.metadata_backend        = &nand_sparse_hash_backend;
        cfg.metadata_backend_config = &be_cfg;
        cfg.failure_model           = &nand_probabilistic_failure_model;
        cfg.failure_model_config    = &pf_cfg;
        cfg.track_block_level = true;
        cfg.track_page_level  = true;

        spi_nand_flash_device_t *dev;
        REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);

        uint32_t sector_size;
        REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
        uint8_t *buf = (uint8_t *)calloc(1, sector_size);

        for (uint32_t i = 0; i < pre_erases; i++) {
            nand_wrap_erase_block(dev, 0);
        }
        nand_wrap_erase_block(dev, 0);
        nand_wrap_prog(dev, 0, buf);

        // Count bit flips after 10 reads
        int total_flips = 0;
        uint8_t *read_buf = (uint8_t *)calloc(1, sector_size);
        for (int r = 0; r < 10; r++) {
            memset(read_buf, 0, sector_size);
            nand_wrap_read(dev, 0, read_buf);
            for (size_t i = 0; i < sector_size; i++) {
                total_flips += __builtin_popcount(read_buf[i] ^ buf[i]);
            }
        }

        free(buf);
        free(read_buf);
        nand_emul_advanced_deinit(dev);
        return total_flips;
    };

    int fresh_flips = count_flips(0);
    int worn_flips  = count_flips(400); // 80% of rated_cycles = high failure probability
    INFO("Fresh flips: " << fresh_flips << " worn flips: " << worn_flips);
    REQUIRE(worn_flips > fresh_flips);
}
```

Build RED → implement probabilistic failure model → GREEN → commit.

---

## Phase 6: Wear Lifetime Simulation Example

**Goal**: Example program demonstrating 10K cycle simulation with periodic snapshots.

Unlike previous phases, Phase 6 produces example programs not tested via Catch2. Write a smoke test first that validates the simulation completes and produces output files, then build the full example.

### P6 Smoke test: simulation completes in reasonable time

Add to `host_test/main/test_integration.cpp`:

```cpp
TEST_CASE("100-cycle wear simulation with snapshots completes without error", "[advanced][simulation]")
{
    sparse_hash_backend_config_t be_cfg = {16, 0.75f, false};
    probabilistic_failure_config_t pf_cfg = {};
    pf_cfg.rated_cycles       = 10000;
    pf_cfg.wear_out_shape     = 2.0;
    pf_cfg.base_bit_error_rate = 1e-8;
    pf_cfg.random_seed        = 12345;

    nand_emul_advanced_config_t cfg = {};
    cfg.base_config = {"", 4 * 1024 * 1024, true}; // 4MB for fast test
    cfg.metadata_backend        = &nand_sparse_hash_backend;
    cfg.metadata_backend_config = &be_cfg;
    cfg.failure_model           = &nand_probabilistic_failure_model;
    cfg.failure_model_config    = &pf_cfg;
    cfg.track_block_level = true;
    cfg.track_page_level  = true;

    spi_nand_flash_device_t *dev;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);

    uint32_t sector_size, block_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK);
    uint32_t total_blocks = (4 * 1024 * 1024) / block_size;

    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    srand(99);
    for (int cycle = 0; cycle < 100; cycle++) {
        for (int op = 0; op < 10; op++) {
            uint32_t b = rand() % total_blocks;
            nand_wrap_erase_block(dev, b); // errors OK (simulated failures)
            nand_wrap_prog(dev, b * (block_size / sector_size), buf);
        }

        if (cycle % 10 == 0) {
            char path[64];
            snprintf(path, sizeof(path), "/tmp/sim_%03d.bin", cycle);
            REQUIRE(nand_emul_save_snapshot(dev, path) == ESP_OK);
            remove(path); // clean up
        }
    }

    nand_wear_stats_t stats = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &stats) == ESP_OK);
    REQUIRE(stats.max_block_erases > 0);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Build RED → this will only be GREEN once Phases 2–5 are complete. Use it as the integration smoke test for the full stack. Once GREEN, build the full example program per original P6 tasks (workload generator, snapshot manager, Python analysis script).

---

## Phase 7: Coverage and Documentation

**Goal**: Ensure >80% code coverage; document all public APIs.

### P7.T1: Wear leveling validation test

This test belongs here because it requires the full stack (Phase 1–5 complete). It was originally scheduled last, but should be written at the start of Phase 7 as a RED test:

```cpp
TEST_CASE("write 10000 random ops, wear leveling variation < 1.0", "[advanced][wear-leveling]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size, block_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK);
    uint32_t total_blocks = (32 * 1024 * 1024) / block_size;

    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    srand(7777);
    for (int i = 0; i < 10000; i++) {
        uint32_t b = rand() % total_blocks;
        nand_wrap_erase_block(dev, b);
        nand_wrap_prog(dev, b * (block_size / sector_size), buf);
    }

    nand_wear_stats_t stats = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &stats) == ESP_OK);
    // Random writes across all blocks should produce relatively even wear
    INFO("variation: " << stats.wear_leveling_variation);
    REQUIRE(stats.wear_leveling_variation < 1.0);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

### P7.T7: Measure code coverage

```bash
# Add to CMakeLists.txt in host_test/main/:
# target_compile_options(${COMPONENT_LIB} PRIVATE --coverage)
# target_link_libraries(${COMPONENT_LIB} PRIVATE --coverage)

idf.py build
./build/nand_flash_host_test.elf "[advanced]"
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_report/
# Open coverage_report/index.html
# Target: >80% line coverage for files under src/backends/ and src/failure_models/
```

### P7.T8: API documentation

Add Doxygen comments to all public symbols in `include/nand_emul_advanced.h`. Verify:

```bash
doxygen -g Doxyfile  # generate default config
# Set INPUT = include/nand_emul_advanced.h
# Set WARNINGS = YES, WARN_IF_UNDOCUMENTED = YES
doxygen Doxyfile
# Target: 0 warnings
```

### P7.T12: Final regression run

```bash
./build/nand_flash_host_test.elf           # all tests
valgrind --leak-check=full --error-exitcode=1 \
    ./build/nand_flash_host_test.elf "[advanced]"
# Expected: all pass, 0 leaks
```

---

## Summary

| Phase | Key deliverable | Test file(s) |
|-------|----------------|-------------|
| P0 | Regression safety net | `test_backward_compat.cpp` |
| P1 | Headers + init/deinit + no-op impls | `test_advanced_types.cpp`, `test_advanced_init.cpp` |
| P2 | Hash table + sparse backend | `test_hash_table.cpp`, `test_sparse_hash_backend.cpp` |
| P3 | Integrated operation handlers | `test_integration.cpp` |
| P4 | Full query API + snapshots + JSON | `test_snapshot.cpp`, additions to `test_sparse_hash_backend.cpp` |
| P5 | Threshold + probabilistic failure models | `test_threshold_failure_model.cpp`, `test_probabilistic_failure_model.cpp` |
| P6 | Simulation example (smoke tested) | `test_integration.cpp` (smoke) |
| P7 | Coverage, docs, final regression | All files |

**Critical path**: P0.T1 → P1 types → P1 init → P2 hash table → P2 sparse backend → P3 erase/write/read → P4 snapshots → P5 threshold → P5 probabilistic → P6 smoke → P7 regression

**TDD iron rule**: Every implementation commit must be preceded by a commit that adds the failing test. If a test passes without any implementation, the test is wrong — fix it first.
