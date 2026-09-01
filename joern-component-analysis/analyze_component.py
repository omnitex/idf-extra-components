#!/usr/bin/env python3
"""Build a component-scoped Joern CPG and export LLM-friendly artifacts."""

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
SCAN_RESULT = re.compile(r"^Result:\s*([^:]+)\s*:\s*(.*?):\s*(.*)$")
DEFAULT_EXCLUDED_DIR_NAMES = frozenset(
    {
        "test_app",
        "host_test",
        "tests",
        "test",
        "examples",
        "example",
        "managed_components",
        "joern-out",
        "build",
    }
)


@dataclass(frozen=True)
class SourceFilter:
    component: Path
    include_roots: tuple[Path, ...]
    excluded_dir_names: frozenset[str]
    exclude_temp_dirs: bool = False

    def allows(self, file_path: Path) -> bool:
        if not any(_is_relative_to(file_path, root) for root in self.include_roots):
            return False
        try:
            relative = file_path.relative_to(self.component)
        except ValueError:
            relative = file_path
        for part in relative.parts[:-1]:
            if part in self.excluded_dir_names:
                return False
            if part.startswith("build"):
                return False
            if self.exclude_temp_dirs and part.startswith("TEMP_"):
                return False
        return True


def _is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def resolve_source_filter(
    component: Path,
    source_subdirs: list[str],
    extra_excludes: list[str] | None = None,
    include_tests: bool = False,
) -> SourceFilter:
    component = component.resolve()
    if source_subdirs:
        include_roots = tuple((component / sub).resolve() for sub in source_subdirs)
        return SourceFilter(
            component=component,
            include_roots=include_roots,
            excluded_dir_names=frozenset(extra_excludes or ()),
        )
    excluded = set() if include_tests else set(DEFAULT_EXCLUDED_DIR_NAMES)
    if extra_excludes:
        excluded.update(extra_excludes)
    return SourceFilter(
        component=component,
        include_roots=(component,),
        excluded_dir_names=frozenset(excluded),
        exclude_temp_dirs=not include_tests,
    )


def filter_compile_database(
    input_path: Path, output_path: Path, source_filter: SourceFilter | Path
) -> int:
    if isinstance(source_filter, Path):
        source_filter = SourceFilter(
            component=source_filter.resolve(),
            include_roots=(source_filter.resolve(),),
            excluded_dir_names=frozenset(),
        )
    entries = json.loads(input_path.read_text())
    selected: list[dict[str, Any]] = []

    for entry in entries:
        file_path = Path(entry["file"])
        if not file_path.is_absolute():
            file_path = Path(entry["directory"]) / file_path
        file_path = file_path.resolve()
        if file_path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if not source_filter.allows(file_path):
            continue
        selected.append(entry)

    output_path.write_text(json.dumps(selected, indent=2) + "\n")
    return len(selected)


def render_inventory(
    records: list[dict[str, Any]], translation_units: int, database_label: str
) -> str:
    methods = [record for record in records if record["kind"] == "method"]
    calls = [record for record in records if record["kind"] == "call"]
    lines = [
        "# Joern component inventory",
        "",
        f"- Compile configuration: {database_label}",
        f"- Translation units: {translation_units}",
        f"- Internal methods: {len(methods)}",
        f"- Notable calls: {len(calls)}",
        "",
        "## Internal methods",
        "",
    ]
    lines.extend(
        f"- `{record['file']}:{record['line']}` `{record['method']}`"
        for record in methods
    )
    lines.extend(["", "## Notable calls", ""])
    lines.extend(
        f"- `{record['file']}:{record['line']}` in `{record['method']}`: "
        f"`{record['code']}`"
        for record in calls
    )
    lines.extend(
        [
            "",
            "## Agent guidance",
            "",
            "Use this inventory as navigation evidence, not proof of safety. "
            "Inspect referenced source around each call. Joern may miss behavior "
            "hidden behind unresolved macros, callbacks, hardware, or concurrency.",
            "",
        ]
    )
    return "\n".join(lines)


def run_command(command: list[str], log_path: Path) -> str:
    result = subprocess.run(command, text=True, capture_output=True)
    combined = result.stdout + result.stderr
    log_path.write_text(combined)
    if result.returncode != 0:
        raise RuntimeError(
            f"Command failed ({result.returncode}); see {log_path}:\n"
            + " ".join(command)
        )
    return combined


def parse_scan_findings(output: str) -> list[dict[str, str]]:
    findings = []
    for line in output.splitlines():
        match = SCAN_RESULT.match(line)
        if match:
            findings.append(
                {
                    "score": match.group(1).strip(),
                    "title": match.group(2).strip(),
                    "location": match.group(3).strip(),
                }
            )
    return findings


def build_scan_command(
    scanner: Path, component: Path, filtered_db: Path
) -> list[str]:
    return [
        str(scanner),
        "--overwrite",
        "--tags",
        "default",
        str(component),
        "--frontend-args",
        "--compilation-database",
        str(filtered_db),
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze one C/C++ component with Joern."
    )
    parser.add_argument("component", type=Path, help="Component root")
    parser.add_argument(
        "--compile-db", required=True, type=Path, help="compile_commands.json"
    )
    parser.add_argument(
        "--source-subdir",
        action="append",
        default=[],
        help=(
            "Limit translation units to this subdirectory of the component. "
            "May be repeated. Default: any sources under the component root, "
            "excluding test/example/build trees"
        ),
    )
    parser.add_argument(
        "--exclude-dir",
        action="append",
        default=[],
        help="Extra directory name to ignore anywhere under the component",
    )
    parser.add_argument(
        "--include-tests",
        action="store_true",
        help="Keep test_app/host_test/examples and TEMP_* trees in auto mode",
    )
    parser.add_argument(
        "--joern-home",
        type=Path,
        default=Path(
            os.environ.get("JOERN_HOME", Path.home() / "bin/joern/joern-cli")
        ),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Output directory (default: COMPONENT/joern-out)",
    )
    parser.add_argument(
        "--run-default-scan",
        action="store_true",
        help="Also run the querydb default checks (builds another workspace CPG)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    component = args.component.resolve()
    compile_db = args.compile_db.resolve()
    source_filter = resolve_source_filter(
        component,
        source_subdirs=args.source_subdir,
        extra_excludes=args.exclude_dir,
        include_tests=args.include_tests,
    )
    output_dir = (args.output_dir or component / "joern-out").resolve()
    joern_home = args.joern_home.resolve()
    script_dir = Path(__file__).resolve().parent

    required = [
        component,
        compile_db,
        joern_home / "c2cpg.sh",
        joern_home / "joern",
        script_dir / "export_inventory.sc",
        *source_filter.include_roots,
    ]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        print("Missing required paths:\n- " + "\n- ".join(missing), file=sys.stderr)
        return 2

    output_dir.mkdir(parents=True, exist_ok=True)
    filtered_db = output_dir / "compile_commands.component.json"
    translation_units = filter_compile_database(compile_db, filtered_db, source_filter)
    if translation_units == 0:
        print(
            f"No translation units found for {component} in {compile_db}",
            file=sys.stderr,
        )
        return 2

    cpg_path = output_dir / "cpg.bin.zip"
    cpg_path.unlink(missing_ok=True)
    print(f"Building CPG from {translation_units} translation units...")
    run_command(
        [
            str(joern_home / "c2cpg.sh"),
            "-J-Xmx8G",
            str(component),
            "--output",
            str(cpg_path),
            "--compilation-database",
            str(filtered_db),
            "--log-problems",
        ],
        output_dir / "c2cpg.log",
    )

    raw_inventory = output_dir / "inventory.jsonl"
    raw_inventory.unlink(missing_ok=True)
    print("Exporting method and call inventory...")
    run_command(
        [
            str(joern_home / "joern"),
            "-J-Xmx8G",
            "--script",
            str(script_dir / "export_inventory.sc"),
            "--param",
            f"cpgFile={cpg_path}",
            "--param",
            f"outFile={raw_inventory}",
        ],
        output_dir / "export.log",
    )
    records = [
        json.loads(line)
        for line in raw_inventory.read_text().splitlines()
        if line.strip()
    ]
    (output_dir / "inventory.md").write_text(
        render_inventory(records, translation_units, str(compile_db))
    )

    if args.run_default_scan:
        print("Running default querydb checks...")
        scan_output = run_command(
            build_scan_command(
                joern_home / "bin/joern-scan", component, filtered_db
            ),
            output_dir / "joern-scan.log",
        )
        if any(line.startswith("Error:") for line in scan_output.splitlines()):
            raise RuntimeError(
                f"joern-scan reported an argument error; see "
                f"{output_dir / 'joern-scan.log'}"
            )
        (output_dir / "scanner-findings.json").write_text(
            json.dumps(parse_scan_findings(scan_output), indent=2) + "\n"
        )

    print(f"Artifacts written to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
