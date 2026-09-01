import json
import tempfile
import unittest
from pathlib import Path

from analyze_component import (
    build_scan_command,
    filter_compile_database,
    parse_scan_findings,
    render_inventory,
    resolve_source_filter,
)


class FilterCompileDatabaseTests(unittest.TestCase):
    def test_keeps_only_translation_units_below_source_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source_dir = root / "component" / "src"
            source_dir.mkdir(parents=True)
            kept = source_dir / "driver.c"
            ignored = root / "other" / "helper.c"
            ignored.parent.mkdir()
            database = [
                {"directory": str(root), "command": "cc driver.c", "file": str(kept)},
                {"directory": str(root), "command": "cc helper.c", "file": str(ignored)},
            ]
            input_path = root / "compile_commands.json"
            output_path = root / "filtered.json"
            input_path.write_text(json.dumps(database))

            count = filter_compile_database(input_path, output_path, source_dir)

            self.assertEqual(count, 1)
            self.assertEqual(json.loads(output_path.read_text()), [database[0]])

    def test_resolves_relative_file_against_entry_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source_dir = root / "component" / "src"
            source_dir.mkdir(parents=True)
            database = [
                {
                    "directory": str(root),
                    "arguments": ["cc", "-c", "component/src/driver.c"],
                    "file": "component/src/driver.c",
                }
            ]
            input_path = root / "compile_commands.json"
            output_path = root / "filtered.json"
            input_path.write_text(json.dumps(database))

            count = filter_compile_database(input_path, output_path, source_dir)

            self.assertEqual(count, 1)

    def test_auto_keeps_nested_vendor_sources_and_drops_test_apps(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            component = root / "dhara"
            vendor = component / "dhara" / "dhara"
            vendor.mkdir(parents=True)
            test_app = component / "test_app" / "main"
            test_app.mkdir(parents=True)
            scratch = component / "TEMP_verification_scratch"
            scratch.mkdir()
            kept = vendor / "map.c"
            dropped_test = test_app / "test.c"
            dropped_scratch = scratch / "probe.c"
            database = [
                {"directory": str(root), "command": "cc map.c", "file": str(kept)},
                {
                    "directory": str(root),
                    "command": "cc test.c",
                    "file": str(dropped_test),
                },
                {
                    "directory": str(root),
                    "command": "cc probe.c",
                    "file": str(dropped_scratch),
                },
            ]
            input_path = root / "compile_commands.json"
            output_path = root / "filtered.json"
            input_path.write_text(json.dumps(database))
            source_filter = resolve_source_filter(component, source_subdirs=[])

            count = filter_compile_database(input_path, output_path, source_filter)

            self.assertEqual(count, 1)
            self.assertEqual(json.loads(output_path.read_text()), [database[0]])

    def test_explicit_source_subdir_still_narrows_to_that_tree(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            component = root / "dhara"
            vendor = component / "dhara" / "dhara"
            vendor.mkdir(parents=True)
            extra = component / "port"
            extra.mkdir()
            kept = vendor / "map.c"
            ignored = extra / "port.c"
            database = [
                {"directory": str(root), "command": "cc map.c", "file": str(kept)},
                {"directory": str(root), "command": "cc port.c", "file": str(ignored)},
            ]
            input_path = root / "compile_commands.json"
            output_path = root / "filtered.json"
            input_path.write_text(json.dumps(database))
            source_filter = resolve_source_filter(
                component, source_subdirs=["dhara/dhara"]
            )

            count = filter_compile_database(input_path, output_path, source_filter)

            self.assertEqual(count, 1)
            self.assertEqual(json.loads(output_path.read_text()), [database[0]])


class InventoryTests(unittest.TestCase):
    def test_renders_agent_friendly_summary(self):
        records = [
            {
                "kind": "method",
                "file": "src/nand.c",
                "line": 12,
                "method": "nand_init",
                "code": "nand_init",
            },
            {
                "kind": "call",
                "file": "src/nand.c",
                "line": 20,
                "method": "nand_init",
                "code": "memcpy(dst, src, len)",
            },
        ]

        markdown = render_inventory(records, 2, "device compile database")

        self.assertIn("# Joern component inventory", markdown)
        self.assertIn("Translation units: 2", markdown)
        self.assertIn("`src/nand.c:12` `nand_init`", markdown)
        self.assertIn("`src/nand.c:20` in `nand_init`: `memcpy(dst, src, len)`", markdown)

    def test_parses_joern_scan_results(self):
        output = (
            "[INFO] scanning\n"
            "Result: 8.0 : Dangerous function gets() used: src/a.c:12:main\n"
        )

        findings = parse_scan_findings(output)

        self.assertEqual(
            findings,
            [
                {
                    "score": "8.0",
                    "title": "Dangerous function gets() used",
                    "location": "src/a.c:12:main",
                }
            ],
        )

    def test_scanner_uses_frontend_argument_delimiter(self):
        command = build_scan_command(
            Path("/joern/bin/joern-scan"),
            Path("/component"),
            Path("/output/compile_commands.component.json"),
        )

        self.assertEqual(
            command[-3:],
            [
                "--frontend-args",
                "--compilation-database",
                "/output/compile_commands.component.json",
            ],
        )


if __name__ == "__main__":
    unittest.main()
