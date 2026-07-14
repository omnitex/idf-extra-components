#!/usr/bin/env python3
"""
Run cppcheck, semgrep, clang-tidy, lizard, and flawfinder against one idf-extra-components
component and emit a single structured JSON report.

This is local-only tooling: it does not touch .github/workflows, does not upload
anything anywhere, and needs no CI-team involvement. Point it at a component
directory, get back one JSON file with every finding from all tools in a
common shape, ready to be read by a human or fed straight to an agent.

Note: lizard and flawfinder are typically available after sourcing the ESP-IDF environment (`. $IDF_PATH/export.sh`).

Usage:
    # Pattern/style scan only, no build needed (cppcheck + semgrep):
    python3 analyze_component.py spi_nand_flash

    # Add clang-tidy with real project context (needs a compile_commands.json
    # from a normal `idf.py build`, e.g. spi_nand_flash/test_app/build):
    python3 analyze_component.py spi_nand_flash \\
        --compile-commands spi_nand_flash/test_app/build/compile_commands.json

    # Narrow tools, exclude vendored code, custom output path:
    python3 analyze_component.py spi_nand_flash \\
        --tools cppcheck,semgrep --exclude dhara -o /tmp/report.json

    # Skip default artifact dirs (build, .git, ...) and only use --exclude:
    python3 analyze_component.py spi_nand_flash --no-default-excludes --exclude vendor

    # Broader (noisier) semgrep coverage:
    python3 analyze_component.py spi_nand_flash --semgrep-config p/security-audit,p/c

    # Use as a local pre-merge gate:
    python3 analyze_component.py spi_nand_flash --fail-on error --quiet

Output:
    JSON report written to --output (default: <component>/component-analysis-report.json)
    {
      "component": str, "target_dir": str, "generated_at": ISO8601 str,
      "excludes": [str], "excluded_rule_ids": [str],
      "tools": {"<tool>": {"available": bool, "version": str|None, "note": str|None}},
      "findings": [{"tool", "rule_id", "severity", "message", "cwe", "file", "line", "column"}, ...],
      "summary": {"<severity>": count, ...}
    }
"""

import argparse
import csv
import io
import json
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx"}

# Directory basenames skipped by default at any depth (ESP-IDF build trees, VCS,
# component manager checkouts, Python caches, Infer output). Override with
# --no-default-excludes; add more via --exclude.
DEFAULT_EXCLUDES = ("build", ".git", "managed_components", "__pycache__", "infer-out")

# Finding rule_ids suppressed by default across all tools: low-signal style
# nits that are noisy in embedded C (e.g. cppcheck's cstyleCast fires on every
# `(uint8_t *)ptr`-style cast, which is idiomatic here). Override with
# --no-default-rule-excludes; add more via --exclude-rule-id.
DEFAULT_EXCLUDED_RULE_IDS = ("cstyleCast",)

# path:line:col: severity: message [rule]  (clang-tidy's "note:" context lines are
# skipped on purpose - they elaborate the preceding warning, not a new finding)
CLANG_TIDY_DIAG_RE = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+):(?P<column>\d+): (?P<severity>error|warning): "
    r"(?P<message>.+?)(?: \[(?P<rule>[\w,.\-]+)\])?$"
)
CWE_RE = re.compile(r"CWE-\d+")


def resolve_target_dir(component: str) -> Path:
    for candidate in (Path(component), REPO_ROOT / component):
        if candidate.is_dir():
            return candidate.resolve()
    print(f"Error: not a directory: {component}", file=sys.stderr)
    sys.exit(1)


def tool_version(binary: str) -> str | None:
    if not shutil.which(binary):
        return None
    result = subprocess.run([binary, "--version"], capture_output=True, text=True)
    return (result.stdout or result.stderr).strip().splitlines()[0]


def lizard_command() -> list[str] | None:
    """Return argv prefix to invoke lizard, or None if unavailable."""
    if shutil.which("lizard"):
        return ["lizard"]
    try:
        import lizard  # noqa: F401
        return [sys.executable, "-m", "lizard"]
    except ImportError:
        return None


def lizard_version() -> str | None:
    cmd = lizard_command()
    if not cmd:
        return None
    result = subprocess.run([*cmd, "--version"], capture_output=True, text=True)
    return (result.stdout or result.stderr).strip().splitlines()[0]


def merge_excludes(user_excludes: list[str], use_defaults: bool,
                    defaults: tuple[str, ...] = DEFAULT_EXCLUDES) -> list[str]:
    """Defaults first, then user overrides; dedupe while preserving order."""
    merged: list[str] = []
    for name in (*(defaults if use_defaults else ()), *user_excludes):
        if name and name not in merged:
            merged.append(name)
    return merged


def is_excluded(path: Path, base: Path, excludes: list[str]) -> bool:
    rel_parts = path.relative_to(base).parts
    return any(exc in rel_parts for exc in excludes)


def find_excluded_dirs(target_dir: Path, excludes: list[str]) -> list[Path]:
    """Return directories under target_dir whose basename is in excludes.

    Prunes descent into excluded dirs so large build trees are not walked.
    """
    if not excludes:
        return []
    exclude_set = set(excludes)
    found: list[Path] = []
    stack = [target_dir]
    while stack:
        current = stack.pop()
        try:
            children = list(current.iterdir())
        except OSError:
            continue
        for child in children:
            if not child.is_dir():
                continue
            if child.name in exclude_set:
                found.append(child)
                continue
            if child.is_symlink():
                continue  # do not follow directory symlinks
            stack.append(child)
    return found


# ── cppcheck ──────────────────────────────────────────────────────────────

def run_cppcheck(target_dir: Path, std: str, enable: str, excludes: list[str],
                  verbose_level: int = 1) -> tuple[list[dict], str | None]:
    if not shutil.which("cppcheck"):
        return [], "cppcheck not found on PATH"

    cmd = ["cppcheck", f"--std={std}", f"--enable={enable}", "--inline-suppr",
           "--xml", "--xml-version=2", "--check-level=exhaustive", "--force"]
    for path in find_excluded_dirs(target_dir, excludes):
        cmd.append(f"-i{path}")
    cmd.append(str(target_dir))
    if verbose_level >= 2:
        print(f"    $ {' '.join(cmd)}", flush=True)

    result = subprocess.run(cmd, capture_output=True, text=True)
    try:
        root = ET.fromstring(result.stderr)
    except ET.ParseError as e:
        return [], f"could not parse cppcheck XML output: {e}"

    findings = []
    for error in root.findall("./errors/error"):
        location = error.find("location")
        findings.append({
            "tool": "cppcheck",
            "rule_id": error.get("id"),
            "severity": error.get("severity"),
            "message": error.get("msg"),
            "cwe": error.get("cwe"),
            "file": location.get("file") if location is not None else None,
            "line": int(location.get("line")) if location is not None and location.get("line") else None,
            "column": int(location.get("column")) if location is not None and location.get("column") else None,
        })
    return findings, None


# ── semgrep ───────────────────────────────────────────────────────────────

def run_semgrep(target_dir: Path, configs: list[str], excludes: list[str],
                 verbose_level: int = 1) -> tuple[list[dict], str | None]:
    if not shutil.which("semgrep"):
        return [], "semgrep not found on PATH"

    cmd = ["semgrep", "scan", "--json", "--metrics=off", "--quiet"]
    for config in configs:
        cmd.append(f"--config={config}")
    for exc in excludes:
        cmd.extend(["--exclude", exc])
    cmd.append(str(target_dir))
    if verbose_level >= 2:
        print(f"    $ {' '.join(cmd)}", flush=True)

    result = subprocess.run(cmd, capture_output=True, text=True)
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as e:
        return [], f"could not parse semgrep JSON output: {e} (stderr: {result.stderr[:300]!r})"

    findings = []
    for res in payload.get("results", []):
        extra = res.get("extra", {})
        cwe = extra.get("metadata", {}).get("cwe")
        findings.append({
            "tool": "semgrep",
            "rule_id": res.get("check_id"),
            "severity": extra.get("severity", "INFO").lower(),
            "message": extra.get("message"),
            "cwe": cwe[0] if isinstance(cwe, list) and cwe else cwe,
            "file": res.get("path"),
            "line": res.get("start", {}).get("line"),
            "column": res.get("start", {}).get("col"),
        })

    note = None
    parse_errors = [e for e in payload.get("errors", []) if "PartialParsing" in str(e.get("type"))]
    if parse_errors:
        bad_files = sorted({e.get("path") for e in parse_errors})
        note = f"{len(bad_files)} file(s) only partially parsed by semgrep's grammar: {', '.join(bad_files)}"
    return findings, note


# ── clang-tidy ────────────────────────────────────────────────────────────

def discover_source_files(target_dir: Path, excludes: list[str]) -> list[Path]:
    """Collect C/C++ sources under target_dir, pruning excluded directory basenames."""
    exclude_set = set(excludes)
    files: list[Path] = []
    stack = [target_dir]
    while stack:
        current = stack.pop()
        try:
            children = list(current.iterdir())
        except OSError:
            continue
        for child in children:
            if child.is_dir():
                if child.name in exclude_set or child.is_symlink():
                    continue
                stack.append(child)
            elif child.suffix in SOURCE_EXTS and not is_excluded(child, target_dir, excludes):
                files.append(child)
    return sorted(files)


def run_clang_tidy(target_dir: Path, checks: str, compile_commands: Path | None,
                    excludes: list[str], verbose_level: int = 1) -> tuple[list[dict], str | None]:
    if not shutil.which("clang-tidy"):
        return [], "clang-tidy not found on PATH"

    files = discover_source_files(target_dir, excludes)
    if not files:
        return [], "no C/C++ source files found"

    build_dir = None
    note = None
    if compile_commands:
        build_dir = compile_commands if compile_commands.is_dir() else compile_commands.parent
    else:
        note = ("no --compile-commands given; running per-file without project context "
                "(expect spurious 'file not found' errors for project-relative includes)")

    findings = []
    for i, f in enumerate(files, start=1):
        if verbose_level >= 3:
            print(f"    [{i}/{len(files)}] {f.relative_to(target_dir)}", flush=True)
        cmd = ["clang-tidy", f"-checks={checks}"]
        if build_dir:
            cmd.append(f"-p={build_dir}")
        cmd.append(str(f))
        if not build_dir:
            cmd.append("--")  # no compile DB: run with no extra flags rather than clang-tidy's own guesses
        if verbose_level >= 2:
            print(f"    $ {' '.join(cmd)}", flush=True)
        result = subprocess.run(cmd, capture_output=True, text=True)
        for line in result.stdout.splitlines():
            m = CLANG_TIDY_DIAG_RE.match(line)
            if not m:
                continue
            findings.append({
                "tool": "clang-tidy",
                "rule_id": m.group("rule"),
                "severity": m.group("severity"),
                "message": m.group("message"),
                "cwe": None,
                "file": m.group("file"),
                "line": int(m.group("line")),
                "column": int(m.group("column")),
            })
    return findings, note


# ── lizard (complexity) ───────────────────────────────────────────────────

def run_lizard(target_dir: Path, excludes: list[str], ccn_threshold: int,
               length_threshold: int, param_threshold: int,
               verbose_level: int = 1) -> tuple[list[dict], str | None]:
    cmd_prefix = lizard_command()
    if not cmd_prefix:
        return [], "lizard not found on PATH (source ESP-IDF env with get_idf, or pip install lizard)"

    files = discover_source_files(target_dir, excludes)
    if not files:
        return [], "no C/C++ source files found"

    cmd = [*cmd_prefix, "-l", "cpp", "--csv", *[str(f) for f in files]]
    if verbose_level >= 2:
        print(f"    $ {' '.join(cmd)}", flush=True)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode not in (0, 1):
        return [], f"lizard failed (exit {result.returncode}): {result.stderr[:300]!r}"

    findings = []
    reader = csv.reader(io.StringIO(result.stdout))
    for row in reader:
        if len(row) < 11:
            continue
        try:
            nloc = int(row[0])
            ccn = int(row[1])
            token_count = int(row[2])
            param_count = int(row[3])
            length = int(row[4])
            func_name = row[7]
            file_path = row[6]
            start_line = int(row[9])
        except (ValueError, IndexError):
            continue

        metrics = f"NLOC={nloc}, CCN={ccn}, length={length}, params={param_count}, tokens={token_count}"
        violations = []
        if ccn > ccn_threshold:
            violations.append(("lizard/cyclomatic-complexity", ccn_threshold,
                               f"{func_name}(): cyclomatic complexity {ccn} exceeds threshold {ccn_threshold} ({metrics})"))
        if length > length_threshold:
            violations.append(("lizard/function-length", length_threshold,
                               f"{func_name}(): length {length} lines exceeds threshold {length_threshold} ({metrics})"))
        if param_count > param_threshold:
            violations.append(("lizard/parameter-count", param_threshold,
                               f"{func_name}(): {param_count} parameters exceeds threshold {param_threshold} ({metrics})"))

        for rule_id, _threshold, message in violations:
            findings.append({
                "tool": "lizard",
                "rule_id": rule_id,
                "severity": "warning",
                "message": message,
                "cwe": None,
                "file": file_path,
                "line": start_line,
                "column": None,
            })

    return findings, None


# ── flawfinder (lexical security) ─────────────────────────────────────────

def run_flawfinder(target_dir: Path, excludes: list[str], minlevel: int,
                    verbose_level: int = 1) -> tuple[list[dict], str | None]:
    if not shutil.which("flawfinder"):
        return [], "flawfinder not found on PATH (source ESP-IDF env with get_idf, or pip install flawfinder)"

    files = discover_source_files(target_dir, excludes)
    if not files:
        return [], "no C/C++ source files found"

    cmd = ["flawfinder", "--sarif", "--quiet", "--omittime", f"--minlevel={minlevel}", *[str(f) for f in files]]
    if verbose_level >= 2:
        print(f"    $ {' '.join(cmd)}", flush=True)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode not in (0, 1, 2):
        return [], f"flawfinder failed (exit {result.returncode}): {result.stderr[:300]!r}"

    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as e:
        return [], f"could not parse flawfinder SARIF output: {e} (stderr: {result.stderr[:300]!r})"

    findings = []
    for run in payload.get("runs", []):
        for res in run.get("results", []):
            message = res.get("message", {}).get("text", "")
            cwe_match = CWE_RE.search(message)
            locations = res.get("locations", [])
            if not locations:
                continue
            region = locations[0].get("physicalLocation", {}).get("region", {})
            artifact = locations[0].get("physicalLocation", {}).get("artifactLocation", {})
            findings.append({
                "tool": "flawfinder",
                "rule_id": res.get("ruleId"),
                "severity": res.get("level", "unknown"),
                "message": message,
                "cwe": cwe_match.group() if cwe_match else None,
                "file": artifact.get("uri"),
                "line": region.get("startLine"),
                "column": region.get("startColumn"),
            })

    return findings, None


# ── Report assembly ────────────────────────────────────────────────────────

def summarize(findings: list[dict]) -> dict:
    summary: dict[str, int] = {}
    for f in findings:
        sev = (f.get("severity") or "unknown").lower()
        summary[sev] = summary.get(sev, 0) + 1
    return summary


def should_fail(findings: list[dict], fail_on: str) -> bool:
    tiers = {"error": {"error"}, "warning": {"error", "warning"}}
    wanted = tiers.get(fail_on)  # None for "any"
    return any(wanted is None or (f.get("severity") or "").lower() in wanted for f in findings)


def filter_no_cwe(findings: list[dict], include_no_cwe: bool) -> list[dict]:
    """By default, drop findings without a CWE: they're style/portability nits
    (e.g. cppcheck constVariablePointer, clang-tidy bugprone-* without a CWE
    mapping) rather than a classified weakness. --include-no-cwe opts back in.
    Lizard complexity findings are always kept (metrics, not CWE-classified)."""
    if include_no_cwe:
        return findings
    return [f for f in findings if f.get("cwe") or f.get("tool") == "lizard"]


def filter_excluded_rules(findings: list[dict], excluded_rule_ids: list[str]) -> list[dict]:
    """Drop findings whose rule_id is in excluded_rule_ids (see
    DEFAULT_EXCLUDED_RULE_IDS / --exclude-rule-id / --no-default-rule-excludes)."""
    if not excluded_rule_ids:
        return findings
    excluded = set(excluded_rule_ids)
    return [f for f in findings if f.get("rule_id") not in excluded]


def main():
    parser = argparse.ArgumentParser(
        description="Run static analysis and complexity checks against one component; emit one JSON report.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("component", help="Component name (relative to repo root) or any directory path")
    parser.add_argument("--tools", default="cppcheck,semgrep,clang-tidy,lizard,flawfinder",
                         help="Comma-separated subset of: cppcheck,semgrep,clang-tidy,lizard,flawfinder (default: all)")
    parser.add_argument("--compile-commands",
                         help="Path to compile_commands.json (or its directory) for clang-tidy project context")
    parser.add_argument("--exclude", action="append", default=[],
                         help="Directory basename to exclude at any depth "
                              "(e.g. vendored code); repeatable; merged with defaults")
    parser.add_argument("--no-default-excludes", action="store_true",
                         help="Do not apply DEFAULT_EXCLUDES "
                              f"({', '.join(DEFAULT_EXCLUDES)}); only --exclude applies")
    parser.add_argument("--exclude-rule-id", action="append", default=[],
                         help="Finding rule_id to suppress (e.g. cstyleCast); "
                              "repeatable; merged with defaults")
    parser.add_argument("--no-default-rule-excludes", action="store_true",
                         help="Do not apply DEFAULT_EXCLUDED_RULE_IDS "
                              f"({', '.join(DEFAULT_EXCLUDED_RULE_IDS)}); only --exclude-rule-id applies")
    parser.add_argument("--std", default="c11", help="C standard for cppcheck (default: c11)")
    parser.add_argument("--cppcheck-enable", default="warning,style,performance,portability")
    parser.add_argument("--semgrep-config", default="p/security-audit",
                         help="Comma-separated semgrep registry config(s) (default: p/security-audit)")
    parser.add_argument("--clang-tidy-checks",
                         default="bugprone-*,cert-*,clang-analyzer-*,performance-*,portability-*")
    parser.add_argument("--lizard-ccn", type=int, default=15,
                         help="Cyclomatic complexity threshold for lizard (default: 15)")
    parser.add_argument("--lizard-length", type=int, default=100,
                         help="Function length (lines) threshold for lizard (default: 100)")
    parser.add_argument("--lizard-parameters", type=int, default=6,
                         help="Parameter count threshold for lizard (default: 6)")
    parser.add_argument("--flawfinder-minlevel", type=int, default=1, choices=range(0, 6),
                         metavar="0-5",
                         help="Minimum flawfinder risk level to report (0=none, 5=max; default: 1)")
    parser.add_argument("-o", "--output", help="Output JSON report path "
                         "(default: <component>/component-analysis-report.json)")
    parser.add_argument("--quiet", action="store_true", help="Only write the JSON report, no stdout summary")
    parser.add_argument("-v", "--verbose", action="count", default=1,
                         help="Increase stdout verbosity; repeatable. "
                              "0 (with --quiet): report only. "
                              "1 (default): tool start/finish + summary. "
                              "2 (-v): + subprocess argv + per-rule drop breakdown. "
                              "3 (-vv): + clang-tidy per-file progress + failure output tails")
    parser.add_argument("--fail-on", choices=["error", "warning", "any"],
                         help="Exit non-zero if any finding at/above this severity exists")
    parser.add_argument("--include-no-cwe", action="store_true",
                         help="Keep findings with no CWE mapping (default: dropped as low-signal style/nit noise)")
    args = parser.parse_args()

    target_dir = resolve_target_dir(args.component)
    component_name = target_dir.name
    requested_tools = {t.strip() for t in args.tools.split(",") if t.strip()}
    excludes = merge_excludes(args.exclude, use_defaults=not args.no_default_excludes)
    excluded_rule_ids = merge_excludes(args.exclude_rule_id, use_defaults=not args.no_default_rule_excludes,
                                        defaults=DEFAULT_EXCLUDED_RULE_IDS)

    verbose_level = 0 if args.quiet else args.verbose

    compile_commands = None
    if args.compile_commands:
        compile_commands = Path(args.compile_commands).resolve()
        if not compile_commands.exists():
            print(f"Error: --compile-commands path not found: {compile_commands}", file=sys.stderr)
            sys.exit(1)

    verbose = verbose_level >= 1
    runners = {
        "cppcheck": lambda: run_cppcheck(target_dir, args.std, args.cppcheck_enable, excludes,
                                          verbose_level=verbose_level),
        "semgrep": lambda: run_semgrep(target_dir, [c.strip() for c in args.semgrep_config.split(",")], excludes,
                                        verbose_level=verbose_level),
        "clang-tidy": lambda: run_clang_tidy(target_dir, args.clang_tidy_checks, compile_commands,
                                              excludes, verbose_level=verbose_level),
        "lizard": lambda: run_lizard(target_dir, excludes, args.lizard_ccn,
                                      args.lizard_length, args.lizard_parameters,
                                      verbose_level=verbose_level),
        "flawfinder": lambda: run_flawfinder(target_dir, excludes, args.flawfinder_minlevel,
                                              verbose_level=verbose_level),
    }

    tool_order = ("cppcheck", "semgrep", "clang-tidy", "lizard", "flawfinder")

    if verbose:
        print(f"Analyzing '{component_name}' ({target_dir})", flush=True)
        if excludes:
            print(f"Excluding: {', '.join(excludes)}", flush=True)
        if excluded_rule_ids:
            print(f"Suppressing rule_ids: {', '.join(excluded_rule_ids)}", flush=True)

    tools_report = {}
    all_findings = []
    for name in tool_order:
        if name not in requested_tools:
            continue
        available = lizard_command() if name == "lizard" else shutil.which(name)
        if not available:
            if verbose:
                print(f"==> {name}: not found on PATH, skipping", flush=True)
            note = f"{name} not found on PATH"
            if name == "lizard":
                note += " (source ESP-IDF env with get_idf, or pip install lizard)"
            elif name == "flawfinder":
                note += " (source ESP-IDF env with get_idf, or pip install flawfinder)"
            tools_report[name] = {"available": False, "version": None, "note": note}
            continue
        if verbose:
            print(f"==> Running {name}...", flush=True)
        findings, note = runners[name]()
        version = lizard_version() if name == "lizard" else tool_version(name)
        tools_report[name] = {
            "available": True,
            "version": version,
            "note": note,
        }
        all_findings.extend(findings)
        if verbose:
            note_suffix = f" - {note}" if note else ""
            print(f"    {name}: {len(findings)} finding(s){note_suffix}", flush=True)

    kept_findings = filter_excluded_rules(all_findings, excluded_rule_ids)
    dropped_by_rule = len(all_findings) - len(kept_findings)
    all_findings = kept_findings

    kept_findings = filter_no_cwe(all_findings, args.include_no_cwe)
    dropped_no_cwe = len(all_findings) - len(kept_findings)
    all_findings = kept_findings

    report = {
        "component": component_name,
        "target_dir": str(target_dir),
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "excludes": excludes,
        "excluded_rule_ids": excluded_rule_ids,
        "tools": tools_report,
        "findings": all_findings,
        "summary": summarize(all_findings),
    }

    output_path = Path(args.output) if args.output else target_dir / "component-analysis-report.json"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(report, indent=2))

    if verbose:
        print("==> Done.", flush=True)
        dropped_notes = []
        if dropped_by_rule:
            suffix = f" ({', '.join(excluded_rule_ids)})" if verbose_level >= 2 else ""
            dropped_notes.append(f"{dropped_by_rule} suppressed by rule_id{suffix}; use --no-default-rule-excludes to keep")
        if dropped_no_cwe:
            dropped_notes.append(f"{dropped_no_cwe} without a CWE dropped; use --include-no-cwe to keep")
        dropped_note = f" ({'; '.join(dropped_notes)})" if dropped_notes else ""
        print(f"Findings:   {len(all_findings)} {report['summary']}{dropped_note}")
        print(f"Report:     {output_path}")

    if args.fail_on and should_fail(all_findings, args.fail_on):
        sys.exit(1)


if __name__ == "__main__":
    main()
