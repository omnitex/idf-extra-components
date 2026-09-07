# Joern component analysis prototype

This prototype creates a component-scoped Joern Code Property Graph (CPG)
using the exact compiler flags from an ESP-IDF build. It then exports a small
method/call inventory suitable for human or LLM-assisted review.

It does not prove that code is safe. Joern has limited knowledge of callbacks,
RTOS scheduling, interrupts, DMA, hardware registers, linker behavior, and
semantics hidden in unresolved macros.

## Pipeline

1. Filter an application's `compile_commands.json` to translation units that
   belong to the component (any layout, not only `src/`).
2. Run `c2cpg`, Joern's C/C++ parser, to create `cpg.bin.zip`.
3. Run a non-interactive Joern script against that CPG.
4. Write `inventory.md` and machine-readable `inventory.jsonl`.
5. Optionally run Joern's default query database and write
   `scanner-findings.json`.

## Requirements

- Python 3.9 or newer.
- Joern distribution installed. Default expected location:
  `~/bin/joern/joern-cli`.
- JDK version required by the installed Joern release (currently JDK 21 in the
  Joern repository README).
- A successful ESP-IDF/CMake configuration that produced
  `compile_commands.json`.
- Approximately 8 GB free RAM for this prototype's JVM limits.

Install Joern:

```bash
curl -L https://github.com/joernio/joern/releases/latest/download/joern-install.sh \
  -o joern-install.sh
chmod +x joern-install.sh
./joern-install.sh --interactive
```

If Joern is elsewhere, pass `--joern-home` or set `JOERN_HOME`.

## Source layouts

The compile database is the source of truth: only files listed there are
parsed. The script then keeps files that live under the component root.

**Default (auto):** keep any matching `.c`/`.cpp` under the component, but drop
common non-product trees:

- `test`, `tests`, `test_app`, `host_test`
- `example`, `examples`
- `managed_components`, `build*`, `joern-out`
- directories starting with `TEMP_`

That covers:

| Layout | Example | Default behavior |
| --- | --- | --- |
| IDF `src/` | `spi_nand_flash/src/*.c` | kept; `test_app/` dropped |
| Nested vendor | `dhara/dhara/dhara/*.c` | kept |
| Flat sources | `component/*.c` | kept |
| `lib/` or `port/` | `component/lib/*.c` | kept |

Override when auto is too wide or too narrow:

```bash
# Only one subtree
./analyze_component.py ../dhara --compile-db ... --source-subdir dhara/dhara

# Several product trees, still excluding tests
./analyze_component.py ../foo --compile-db ... \
  --source-subdir src --source-subdir port

# Include tests and scratch trees
./analyze_component.py ../foo --compile-db ... --include-tests

# Extra directory name to ignore anywhere in the component
./analyze_component.py ../foo --compile-db ... --exclude-dir third_party
```

`--source-subdir` is relative to the component root. Repeat it as needed. It
does not require a folder named `src`.

## SPI NAND example

Device build configuration:

```bash
cd /Users/martinhavlik/esp/forks/idf-extra-components-worktree/joern-component-analysis

./analyze_component.py \
  /Users/martinhavlik/esp/forks/idf-extra-components-worktree/spi_nand_flash \
  --compile-db /Users/martinhavlik/esp/forks/idf-extra-components-worktree/spi_nand_flash/test_app/build/compile_commands.json
```

Add canned querydb checks:

```bash
./analyze_component.py \
  /Users/martinhavlik/esp/forks/idf-extra-components-worktree/spi_nand_flash \
  --compile-db /Users/martinhavlik/esp/forks/idf-extra-components-worktree/spi_nand_flash/test_app/build/compile_commands.json \
  --run-default-scan
```

For the Linux host implementation, use
`spi_nand_flash/host_test/build/compile_commands.json` instead. Do not combine
the device and Linux databases: they describe different CMake configurations.

Dhara (no `src/` directory):

```bash
./analyze_component.py \
  /Users/martinhavlik/esp/forks/idf-extra-components-worktree/dhara \
  --compile-db /path/to/an/app/build/compile_commands.json
```

If that app's compile database also lists Dhara tests you do not want, pass
`--source-subdir dhara/dhara`.

## Output

Default output directory is `<component>/joern-out/`:

- `compile_commands.component.json`: filtered compilation database.
- `cpg.bin.zip`: reusable Joern graph.
- `inventory.md`: concise method and notable-call navigation index.
- `inventory.jsonl`: same inventory in machine-readable form.
- `c2cpg.log`: parser log; inspect this for parse failures.
- `export.log`: inventory export log.
- `joern-scan.log`: optional querydb scanner output.
- `scanner-findings.json`: optional parsed scanner findings.

To explore the generated graph interactively:

```bash
~/bin/joern/joern-cli/joern
joern> loadCpg("/absolute/path/to/spi_nand_flash/joern-out/cpg.bin.zip")
joern> cpg.method.internal.name.l
joern> cpg.call.name("memcpy").code.l
```

## LLM workflow

Give the agent:

1. `joern-out/inventory.md`
2. `joern-out/scanner-findings.json` when generated
3. `joern-out/c2cpg.log` if parsing was incomplete
4. component architecture documentation and referenced source files

Example prompt:

```text
Review this component using joern-out/inventory.md as a navigation index.
Validate every candidate against source code and layered_architecture.md.
Treat scanner-findings.json as leads, not confirmed defects. Check c2cpg.log
for parser failures and state any resulting blind spots.
```

## Tests

The fast tests cover compile-database filtering and report rendering without
requiring Joern:

```bash
python3 -m unittest -v test_analyze_component.py
```

An actual analysis run is the integration test for the installed Joern
version and chosen ESP-IDF build configuration.
