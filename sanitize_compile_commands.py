#!/usr/bin/env python3
"""
Sanitize ESP-IDF compile_commands.json for Infer (Facebook Static Analyzer).

Infer uses its own internal clang to parse C/C++ code. ESP-IDF's compile_commands.json
contains commands for the Xtensa GCC cross-compiler with GCC-specific flags that clang
rejects. This script bridges the gap by:

1. Replacing the cross-compiler with host clang
2. Stripping GCC-only and Xtensa-specific flags
3. Adding embedded C library (newlib/picolibc) headers from the Xtensa toolchain
4. Adding cross-compilation compatibility defines
5. Preventing host macOS system headers from being included

Tested with: ESP-IDF v6.1, xtensa-esp32-elf-gcc (esp-15.2.0), Infer v1.3.0, macOS

Usage:
    # 1. Build your component test app (generates compile_commands.json):
    cd spi_nand_flash/test_app
    idf.py build

    # 2. Sanitize + auto-filter to that component's sources (IDF / other
    #    components / build/ artifacts are dropped by default). This also
    #    runs Infer automatically at the end (pass --no-run-infer to only
    #    print the command instead).
    #    Works for test_app/, host_test/, and examples/<name>/ layouts:
    python3 sanitize_compile_commands.py build/compile_commands.json

    # Just print the Infer command without running it:
    python3 sanitize_compile_commands.py build/compile_commands.json --no-run-infer

    # Override / extend which paths to keep:
    python3 sanitize_compile_commands.py build/compile_commands.json --component spi_nand_flash
    python3 sanitize_compile_commands.py build/compile_commands.json --include-path ../dhara

    # Keep every entry (no component filter):
    python3 sanitize_compile_commands.py build/compile_commands.json --all

    # Point the rewrite + flag-compatibility probe at Infer's bundled clang
    # instead of /usr/bin/clang (recommended: host clang can accept flags
    # Infer's bundled clang still rejects):
    python3 sanitize_compile_commands.py build/compile_commands.json \
        --infer-clang "$(dirname "$(which infer)")/../lib/infer/facebook-clang-plugins/clang/install/bin/clang"

Output:
    compile_commands_sanitized.json in the same directory as the input file.
    infer_report.json in the component root (not the build/ dir — override
    with --report-dir) once Infer runs: a single prettified JSON
    with normalized findings + a severity summary (same shape as
    analyze_component.py's report). infer-out/ is removed after extraction
    unless --keep-infer-out is passed.
"""

import argparse
import hashlib
import json
import shlex
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


# ── Configuration ──────────────────────────────────────────────────────────────

# Policy drops: clang *accepts* these, we drop them anyway for reasons that
# have nothing to do with compatibility (promoting warnings to errors would
# make capture die on warnings; color codes are cosmetic noise in logs).
# Do NOT add compatibility-only flags here — those belong to the probe layer
# below so they get re-evaluated automatically on toolchain/clang upgrades.
POLICY_EXACT = {
    "-Werror",
}

POLICY_PREFIXES = (
    "-Werror=",
    "-fdiagnostics-color=",
)

# Flags that take an argument (strip both the flag and its argument). These
# are always rewritten regardless of clang compatibility: they either encode
# a GCC-specific path (-specs) or something this script controls itself
# (-o, -isysroot, -fmacro-prefix-map).
STRIP_WITH_ARG = {
    "-specs",             # e.g., -specs=/path/to/picolibc.specs
    "-o",                 # output file
    "-fmacro-prefix-map", # clang doesn't support all GCC prefix-map forms
    "-isysroot",          # we control sysroot ourselves
}

# Prefixes of single-token flags that are always rewritten (combined
# `-flag=value` form of something in STRIP_WITH_ARG).
STRIP_PREFIXES = (
    "-fmacro-prefix-map=",
    "-specs=",
)

# Only single-token flags starting with one of these prefixes get probed
# against clang. Everything else (-I, -D, -U, -include, -std=, ...) is
# assumed compatible and passed through untouched.
PROBE_PREFIXES = ("-f", "-m", "-W")

# Extra clang flags for cross-compilation compatibility (not -D/-U defines)
EXTRA_FLAGS = [
    "-fno-blocks",    # Fix __block keyword conflict (clang Blocks vs newlib __block)
]

# Extra defines for cross-compilation compatibility with clang
EXTRA_DEFINES = [
    "-U__APPLE__",    # Prevent ESP-IDF from including macOS headers (mach-o/getsect.h)
    "-D__XTENSA__",   # Tell ESP-IDF headers we're on Xtensa (not RISC-V)
    "-D__ets__",      # ESP-IDF sometimes checks this
    "-U__linux__",    # Don't pull in Linux-isms
]


# ── Toolchain Discovery ───────────────────────────────────────────────────────

def find_xtensa_toolchain_dir():
    """Find the Xtensa toolchain directory under ~/.espressif/tools/."""
    espressif = Path.home() / ".espressif" / "tools" / "xtensa-esp-elf"
    if not espressif.exists():
        return None

    # Pick the latest version
    versions = sorted(espressif.iterdir(), reverse=True)
    for v in versions:
        toolchain_dir = v / "xtensa-esp-elf"
        if toolchain_dir.exists():
            return toolchain_dir
    return None


def get_newlib_include_dirs(toolchain_dir: Path):
    """Return the newlib/picolibc include directories from the Xtensa toolchain."""
    candidates = [
        toolchain_dir / "picolibc" / "include",
        toolchain_dir / "xtensa-esp-elf" / "include",
        toolchain_dir / "picolibc" / "xtensa-esp-elf" / "sys-include",
        toolchain_dir / "xtensa-esp-elf" / "sys-include",
    ]
    return [d for d in candidates if d.exists()]


def get_clang_resource_dir(clang_bin: str):
    """Get clang's built-in include directory (stddef.h, stdarg.h, etc.)."""
    result = subprocess.run(
        [clang_bin, "-print-resource-dir"],
        capture_output=True, text=True
    )
    if result.returncode == 0:
        resource_dir = Path(result.stdout.strip()) / "include"
        if resource_dir.exists():
            return resource_dir
    return None


# ── Path filtering ────────────────────────────────────────────────────────────

def is_under(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def looks_like_component(path: Path) -> bool:
    """Heuristic: ESP-IDF component roots usually have CMakeLists + yml/src/include."""
    if not path.is_dir() or not (path / "CMakeLists.txt").exists():
        return False
    return any((path / marker).exists() for marker in ("idf_component.yml", "src", "include"))


def detect_component_root(input_path: Path) -> Path | None:
    """Infer component root from a compile_commands.json under a component app.

    Supported layouts:
      .../<component>/test_app/build/compile_commands.json
      .../<component>/host_test/build/compile_commands.json
      .../<component>/examples/<example>/build/compile_commands.json
      .../examples/<category>/<example>/build/compile_commands.json
        (standalone esp-idf example tree, e.g.
        esp-idf/examples/storage/littlefs/build/ — no umbrella component,
        the example directory itself is the root)
    Fallback: walk up looking for an idf_component.yml.
    """
    build_dir = input_path.resolve().parent
    if build_dir.name != "build":
        return None
    app_dir = build_dir.parent

    # <component>/(test_app|host_test)/build
    if app_dir.name in ("test_app", "host_test"):
        candidate = app_dir.parent
        return candidate if candidate.is_dir() else None

    # <component>/examples/<example_name>/build
    if app_dir.parent.name == "examples":
        candidate = app_dir.parent.parent
        return candidate if candidate.is_dir() else None

    # examples/<category>/<example_name>/build (standalone esp-idf example,
    # e.g. esp-idf/examples/storage/littlefs/build/): no umbrella component
    # directory to climb to, so the example dir itself is the root.
    if app_dir.parent.parent.name == "examples":
        return app_dir if app_dir.is_dir() else None

    # Fallback: nearest ancestor that looks like a component
    for parent in [app_dir, *app_dir.parents]:
        if looks_like_component(parent):
            return parent
    return None


def find_named_directory(name: str, start: Path) -> Path | None:
    """Resolve a component name to a directory, walking up from start/cwd if needed."""
    direct = Path(name).expanduser()
    if direct.is_dir():
        return direct.resolve()
    cwd_candidate = Path.cwd() / name
    if cwd_candidate.is_dir():
        return cwd_candidate.resolve()

    # Walk up from the compile DB and from cwd: match ancestor name or a child named `name`
    for origin in (start.resolve(), Path.cwd().resolve()):
        for ancestor in [origin, *origin.parents]:
            if ancestor.name == name and ancestor.is_dir():
                return ancestor
            child = ancestor / name
            if child.is_dir():
                return child.resolve()
    return None


def resolve_include_paths(args, input_path: Path) -> list[Path] | None:
    """Return roots to keep, or None when --all (no filtering).

    Default: auto-detect component from input path.
    --component: use those roots instead of auto-detect.
    --include-path: always added on top of whatever roots were chosen.
    """
    if args.all:
        return None

    roots: list[Path] = []

    if args.component:
        for name in args.component:
            match = find_named_directory(name, input_path.parent)
            if match is None:
                print(f"Error: --component not found as a directory: {name}", file=sys.stderr)
                sys.exit(1)
            roots.append(match)
    else:
        detected = detect_component_root(input_path)
        if detected is None and not args.include_path:
            print("Error: could not auto-detect component root from input path "
                  f"({input_path}). Expected one of:\n"
                  "  .../<component>/(test_app|host_test)/build/compile_commands.json\n"
                  "  .../<component>/examples/<example>/build/compile_commands.json\n"
                  "  .../examples/<category>/<example>/build/compile_commands.json (standalone esp-idf example)\n"
                  "Pass --component / --include-path, or --all.",
                  file=sys.stderr)
            sys.exit(1)
        if detected is not None:
            roots.append(detected)

    for p in args.include_path:
        path = Path(p).expanduser().resolve()
        if not path.exists():
            print(f"Error: --include-path not found: {path}", file=sys.stderr)
            sys.exit(1)
        roots.append(path)

    # Deduplicate while preserving order
    unique: list[Path] = []
    for r in roots:
        if r not in unique:
            unique.append(r)
    return unique


def keep_entry(entry: dict, include_roots: list[Path] | None) -> bool:
    """True if this compile DB entry should be kept after filtering."""
    if include_roots is None:
        return True
    file_path = Path(entry["file"]).resolve()
    # Drop CMake/build generated sources even when they live under the component tree
    if "build" in file_path.parts:
        return False
    return any(is_under(file_path, root) for root in include_roots)


# ── Flag compatibility probing ─────────────────────────────────────────────────
#
# Rather than hand-maintaining a denylist of GCC flags that we *believe* clang
# rejects (which silently rots every time IDF bumps its toolchain and starts
# passing new -f/-m flags), probe the actual clang binary once per unique
# flag and cache the verdict. Same approach as the Linux kbuild
# check_clang_compatibility() patch and clangd's ArgStripper: ask the tool,
# don't guess.

FLAG_CACHE_DIR = Path.home() / ".cache" / "sanitize_compile_commands"


def get_clang_version(clang_bin: str) -> str:
    """Return `clang_bin --version` output, used to key the on-disk flag cache."""
    try:
        result = subprocess.run(
            [clang_bin, "--version"], capture_output=True, text=True, timeout=10
        )
        return result.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def _flag_cache_path(clang_version: str) -> Path:
    digest = hashlib.sha256(clang_version.encode()).hexdigest()[:16]
    return FLAG_CACHE_DIR / f"flags_{digest}.json"


def load_flag_cache(clang_version: str) -> dict:
    """Load the persisted flag → ok/fatal verdicts for this clang version."""
    path = _flag_cache_path(clang_version)
    if path.exists():
        try:
            return json.loads(path.read_text())
        except (json.JSONDecodeError, OSError):
            return {}
    return {}


def save_flag_cache(clang_version: str, cache: dict) -> None:
    """Persist the flag cache so repeat runs against the same clang skip probing."""
    try:
        FLAG_CACHE_DIR.mkdir(parents=True, exist_ok=True)
        _flag_cache_path(clang_version).write_text(json.dumps(cache, indent=2, sort_keys=True))
    except OSError:
        pass  # Cache is a pure optimization; failure to persist it is not fatal.


def probe_flag(flag: str, clang_bin: str, cache: dict) -> bool:
    """Return True if `clang_bin` accepts `flag` on a trivial translation unit."""
    if flag in cache:
        return cache[flag]
    try:
        result = subprocess.run(
            [clang_bin, "-fsyntax-only", "-x", "c", "-",
             "-Werror=unknown-warning-option", flag],
            input="int f;\n",
            capture_output=True, text=True, timeout=10,
        )
        ok = result.returncode == 0
    except (OSError, subprocess.SubprocessError):
        ok = False
    cache[flag] = ok
    return ok


# ── Command Sanitization ──────────────────────────────────────────────────────

def inline_response_files(tokens, build_dir: Path):
    """Expand @response_file references into their contents."""
    expanded = []
    for t in tokens:
        if t.startswith("@") and len(t) > 1:
            resp = Path(t[1:])
            if not resp.is_absolute():
                resp = build_dir / resp
            if resp.exists():
                expanded.extend(shlex.split(resp.read_text()))
            # Silently skip non-existent response files
        else:
            expanded.append(t)
    return expanded


def sanitize_command(cmd_str: str, src_file: str, directory: str,
                    newlib_includes: list[Path], clang_resource: Path | None,
                    clang_bin: str, flag_cache: dict, audit: dict | None = None):
    """Sanitize a single compile command for clang/Infer compatibility.

    Returns the new command as an argv list (see the "arguments" form of
    the JSON Compilation Database spec), not a shell-escaped string.

    If `audit` is given (a dict with "policy_drop", "must_drop",
    "probe_kept" sets), record which flags fell into which bucket so
    --audit can report OVER/UNDER-strip risk without re-running probes.
    """
    parts = shlex.split(cmd_str)

    # Inline @response_file references
    tokens = inline_response_files(parts[1:], Path(directory))

    # Filter tokens
    kept = []
    i = 0
    while i < len(tokens):
        t = tokens[i]

        # Policy drop: clang accepts these, we drop them anyway (see comment
        # on POLICY_EXACT / POLICY_PREFIXES above).
        if t in POLICY_EXACT or any(t.startswith(p) for p in POLICY_PREFIXES):
            if audit is not None:
                audit["policy_drop"].add(t)
            i += 1
            continue

        # Rewrite-layer prefix match strip (combined `-flag=value` form of
        # something in STRIP_WITH_ARG)
        if any(t.startswith(p) for p in STRIP_PREFIXES):
            i += 1
            continue

        # Flag + argument strip
        stripped = False
        for flag in STRIP_WITH_ARG:
            if t == flag:
                i += 2  # skip flag and its argument
                stripped = True
                break
            elif t.startswith(flag + "="):
                i += 1  # combined form
                stripped = True
                break
        if stripped:
            continue

        # Skip compilation artifacts
        if t == "-c" or t.startswith("CMakeFiles") or t.endswith(".obj"):
            i += 1
            continue

        # Skip source file reference (we add it ourselves)
        if t == src_file or t == Path(src_file).name:
            i += 1
            continue

        # Probe layer: single-token -f/-m/-W flags get checked against the
        # actual target clang. Drop only if clang itself rejects them.
        if t.startswith(PROBE_PREFIXES):
            if not probe_flag(t, clang_bin, flag_cache):
                if audit is not None:
                    audit["must_drop"].add(t)
                i += 1
                continue
            elif audit is not None:
                audit["probe_kept"].add(t)

        kept.append(t)
        i += 1

    # Add newlib/picolibc includes (lower priority than IDF headers)
    for inc_dir in newlib_includes:
        kept.append(f"-I{inc_dir}")

    # Add clang builtins (stddef.h, stdarg.h, __stddef_*.h, etc.)
    if clang_resource:
        kept.append(f"-I{clang_resource}")

    # Prevent host system headers from being included
    kept.append("-nostdinc")

    # Build the new command as an argv list. Clang's JSON Compilation
    # Database spec recommends "arguments" over "command" specifically to
    # avoid a shell-escaping round-trip: we already manipulate tokens
    # directly, so emitting argv avoids a shlex.quote()/shlex.split() pair
    # that both need to agree on every edge case (spaces, quotes, etc. in
    # paths).
    return (
        [clang_bin, "-c", str(Path(src_file).absolute())]
        + kept
        + EXTRA_FLAGS
        + EXTRA_DEFINES
    )


# ── Infer invocation ────────────────────────────────────────────────────────────

def prettify_infer_report(infer_out_dir: Path, sanitized_db: Path, report_out: Path) -> dict | None:
    """Read infer-out/report.json and write a single normalized, prettified JSON.

    Same shape as analyze_component.py's report ("tool", "findings",
    "summary") so both can be read/diffed/fed to tooling the same way.
    Returns the report dict, or None if infer-out/report.json is missing
    (e.g. capture failed before analysis ran).
    """
    raw_path = infer_out_dir / "report.json"
    if not raw_path.exists():
        return None

    raw = json.loads(raw_path.read_text())
    findings = []
    for e in raw:
        findings.append({
            "tool": "infer",
            "rule_id": e.get("bug_type"),
            "severity": (e.get("severity") or "unknown").lower(),
            "message": e.get("qualifier"),
            "cwe": None,
            "file": e.get("file"),
            "line": e.get("line"),
            "column": e.get("column"),
            "procedure": e.get("procedure"),
            "category": e.get("category"),
            "bug_trace": e.get("bug_trace", []),
        })

    summary: dict[str, int] = {}
    for f in findings:
        summary[f["severity"]] = summary.get(f["severity"], 0) + 1

    report = {
        "tool": "infer",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "compilation_database": str(sanitized_db),
        "findings": findings,
        "summary": summary,
    }
    report_out.write_text(json.dumps(report, indent=2))
    return report


def run_infer(output_path: Path, run: bool, infer_extra_args: list[str],
              keep_infer_out: bool, report_dir: Path) -> None:
    """Print the Infer command for the sanitized database, and optionally run it.

    `run` controls whether Infer is actually invoked (--run-infer / default
    True) or just printed (--no-run-infer). Skips with a warning if the
    `infer` binary is not on PATH, regardless of `run`.

    On a successful run, infer-out/report.json is normalized + prettified
    into <report_dir>/infer_report.json (the component root, not the build
    dir — a build/ directory is typically build-artifact / gitignored and
    gets wiped by `idf.py fullclean`), and infer-out/ is removed unless
    `keep_infer_out` is set — infer-out contains a sqlite capture DB and
    several other artifacts nobody reads after the fact; the one file
    worth keeping is the report.
    """
    infer_bin = shutil.which("infer")
    cmd = ["infer", "run", "--keep-going",
           "--compilation-database", str(output_path), *infer_extra_args]

    print("\n── Infer ──")
    print("Run Infer on the sanitized database:")
    print("  " + " ".join(shlex.quote(c) for c in cmd))

    if not run:
        return

    if not infer_bin:
        print("Warning: 'infer' not found on PATH, skipping auto-run. "
              "Install Infer (https://fbinfer.com) or pass --no-run-infer "
              "to silence this.", file=sys.stderr)
        return

    print("\nRunning Infer (pass --no-run-infer to only print this command)...")
    subprocess.run([infer_bin, *cmd[1:]])

    infer_out_dir = Path.cwd() / "infer-out"
    report_out = report_dir / "infer_report.json"
    report = prettify_infer_report(infer_out_dir, output_path, report_out)

    if report is None:
        print(f"Warning: {infer_out_dir / 'report.json'} not found, "
              "skipping report normalization + cleanup.", file=sys.stderr)
        return

    print(f"\nWrote {report_out} ({len(report['findings'])} findings, "
          f"{report['summary']})")

    if keep_infer_out:
        print(f"Kept {infer_out_dir} (pass without --keep-infer-out to clean it up)")
    else:
        shutil.rmtree(infer_out_dir, ignore_errors=True)
        print(f"Removed {infer_out_dir}")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Sanitize ESP-IDF compile_commands.json for Infer/clang compatibility"
    )
    parser.add_argument(
        "input",
        help="Path to ESP-IDF's compile_commands.json (usually in build/ directory)"
    )
    parser.add_argument(
        "-o", "--output",
        help="Output path (default: <input_dir>/compile_commands_sanitized.json)"
    )
    parser.add_argument(
        "--toolchain-dir",
        help="Path to xtensa-esp-elf toolchain dir (auto-detected if omitted)"
    )
    parser.add_argument(
        "--infer-clang",
        help="Path to the clang binary that will actually parse the sanitized "
             "database (e.g. Infer's bundled "
             ".../facebook-clang-plugins/clang/install/bin/clang). Used both "
             "as the rewritten compiler and as the probe target for -f/-m/-W "
             "flag compatibility. Defaults to /usr/bin/clang, which can "
             "accept flags Infer's bundled clang still rejects."
    )
    parser.add_argument(
        "--component",
        action="append", default=[],
        help="Component directory name/path to keep (repeatable). "
             "If omitted, auto-detected from .../<component>/(test_app|host_test)/build/"
    )
    parser.add_argument(
        "--include-path",
        action="append", default=[],
        help="Extra directory whose sources to keep (repeatable), e.g. a local dependency"
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Keep every compile DB entry (no component filter)"
    )
    parser.add_argument(
        "--test",
        action="store_true",
        help="Verify: try to compile every kept file with clang and report OK/FAIL"
    )
    parser.add_argument(
        "--audit",
        action="store_true",
        help="Print flags that were policy-dropped (clang accepts them, we "
             "drop anyway), must-drop (clang rejects them), and probe-kept "
             "(clang accepts, kept as-is) instead of a hardcoded denylist "
             "diff. Use this to check for over-strip / under-strip."
    )
    parser.add_argument(
        "--run-infer",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Run 'infer run --keep-going --compilation-database <output>' "
             "automatically after sanitizing (default: on). Pass "
             "--no-run-infer to only print the command instead of running "
             "it. Skipped with a warning if the 'infer' binary is not on "
             "PATH."
    )
    parser.add_argument(
        "--infer-arg",
        action="append", default=[],
        help="Extra argument to pass through to 'infer run' (repeatable), "
             "e.g. --infer-arg=--clang-block-listed-flags-with-arg "
             "--infer-arg=-ivfsstatcache"
    )
    parser.add_argument(
        "--keep-infer-out",
        action="store_true",
        help="Keep the infer-out/ directory after extracting infer_report.json "
             "(default: removed, since capture.db/results.db/logs/ etc. are "
             "not useful once the report is extracted)"
    )
    parser.add_argument(
        "--report-dir",
        help="Directory to write infer_report.json into (default: the "
             "detected/--component component root, not the build/ dir — "
             "override if auto-detection picks the wrong root, e.g. under --all)"
    )
    args = parser.parse_args()

    input_path = Path(args.input)
    if not input_path.exists():
        print(f"Error: {input_path} not found", file=sys.stderr)
        sys.exit(1)

    output_path = Path(args.output) if args.output else input_path.parent / "compile_commands_sanitized.json"
    include_roots = resolve_include_paths(args, input_path)

    if args.report_dir:
        report_dir = Path(args.report_dir).expanduser().resolve()
    elif include_roots:
        report_dir = include_roots[0]  # primary component root, not the build/ dir
    else:
        report_dir = output_path.parent  # --all with no detected component root


    # Discover toolchain
    if args.toolchain_dir:
        toolchain_dir = Path(args.toolchain_dir)
    else:
        toolchain_dir = find_xtensa_toolchain_dir()

    if not toolchain_dir:
        print("Error: Could not find Xtensa toolchain. Install ESP-IDF toolchain or pass --toolchain-dir",
              file=sys.stderr)
        sys.exit(1)

    newlib_includes = get_newlib_include_dirs(toolchain_dir)
    if not newlib_includes:
        print(f"Error: No newlib/picolibc includes found in {toolchain_dir}", file=sys.stderr)
        sys.exit(1)

    clang_bin = args.infer_clang if args.infer_clang else "/usr/bin/clang"
    clang_version = get_clang_version(clang_bin)

    clang_resource = get_clang_resource_dir(clang_bin)
    if not clang_resource:
        print("Warning: Could not find clang resource dir (stddef.h etc.)", file=sys.stderr)

    print(f"Toolchain:      {toolchain_dir}")
    print(f"Newlib includes: {len(newlib_includes)} dirs")
    print(f"Clang resource:  {clang_resource}")
    print(f"Clang binary:    {clang_bin}")
    if include_roots is None:
        print("Filter:          --all (keeping every entry)")
    else:
        print("Filter:          " + ", ".join(str(r) for r in include_roots))
    print()

    # Read, filter, sanitize
    data = json.loads(input_path.read_text())
    kept_raw = [e for e in data if keep_entry(e, include_roots)]
    dropped = len(data) - len(kept_raw)
    if include_roots is not None and not kept_raw:
        print(f"Error: filter matched 0 of {len(data)} entries. Check --component / --include-path.",
              file=sys.stderr)
        sys.exit(1)

    flag_cache: dict = load_flag_cache(clang_version)
    audit = {"policy_drop": set(), "must_drop": set(), "probe_kept": set()} if args.audit else None

    new_entries = []
    for entry in kept_raw:
        cmd = entry.get("command", "")
        if not cmd:
            new_entries.append(entry)
            continue

        new_args = sanitize_command(
            cmd, entry["file"], entry.get("directory", ""),
            newlib_includes, clang_resource, clang_bin, flag_cache, audit
        )
        new_entries.append({
            "directory": str(Path(entry["directory"]).absolute()),
            "arguments": new_args,
            "file": str(Path(entry["file"]).absolute()),
        })

    save_flag_cache(clang_version, flag_cache)

    output_path.write_text(json.dumps(new_entries, indent=2))
    print(f"Kept {len(new_entries)} / {len(data)} entries"
          + (f" ({dropped} filtered out)" if dropped else ""))
    print(f"Wrote {output_path}")

    if audit is not None:
        print("\n── Audit ──")
        print(f"POLICY-DROP (clang accepts, dropped by policy): {len(audit['policy_drop'])}")
        for flag in sorted(audit["policy_drop"]):
            print(f"  {flag}")
        print(f"MUST-DROP (clang rejects): {len(audit['must_drop'])}")
        for flag in sorted(audit["must_drop"]):
            print(f"  {flag}")
        print(f"PROBE-KEPT (clang accepts, kept as-is): {len(audit['probe_kept'])}")
        for flag in sorted(audit["probe_kept"]):
            print(f"  {flag}")

    # Optional: quick verification test
    if args.test:
        print(f"\n── Verification Test ({len(new_entries)} files) ──")
        ok_count = 0
        fail_count = 0
        for e in new_entries:
            if "arguments" not in e:
                print(f"  {Path(e['file']).name}: SKIP (no arguments list)")
                continue
            short = Path(e["file"]).name
            result = subprocess.run(
                e["arguments"],
                capture_output=True, text=True, cwd=e["directory"]
            )
            if result.returncode == 0:
                ok_count += 1
                continue
            fail_count += 1
            print(f"  {short}: FAIL({result.returncode})")
            errs = [l for l in result.stderr.split('\n')
                    if 'error' in l.lower() or 'fatal' in l.lower()][:3]
            for l in errs:
                print(f"    {l.strip()[:150]}")
        print(f"  {ok_count} OK, {fail_count} FAIL")

    run_infer(output_path, args.run_infer, args.infer_arg, args.keep_infer_out, report_dir)


if __name__ == "__main__":
    main()
