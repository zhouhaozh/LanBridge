#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Exercise packaging gates with local command stubs; never download or build."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[3]
SCRIPT = Path("deploy/lanbridge/build-flatpak.sh")
MANIFEST = Path("deploy/linux/flatpak/org.lanbridge.LanBridge.yml")


class FlatpakBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lanbridge-flatpak-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.root = self.directory / "local source tree"
        self.output = self.directory / "package output"
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        for relative in (SCRIPT, MANIFEST):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(PROJECT / relative, target)
        self.log = self.directory / "commands.jsonl"
        self.environment = {
            **os.environ,
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "LANBRIDGE_STUB_LOG": str(self.log),
        }
        self.environment.pop("LANBRIDGE_VERSION", None)
        self.manifest = {
            "app-id": "org.lanbridge.LanBridge", "branch": "stable",
            "modules": [
                {"name": "libei", "config-opts": ["-Dtests=disabled"]},
                {
                    "name": "lanbridge",
                    "config-opts": ["-DBUILD_TESTS=ON", "-DSKIP_BUILD_TESTS=ON",
                                    "-DBUILD_X11_SUPPORT=ON", "-DLANBRIDGE_VERSION=old"],
                    "run-tests": True,
                    "test-commands": ["QT_QPA_PLATFORM=minimal ctest --test-dir src/unittests -LE interactive --no-tests=error"],
                    "sources": [{"type": "dir", "path": "../../../", "skip": [".git", "build", "dist"]}],
                },
            ],
        }
        self.environment["LANBRIDGE_STUB_MANIFEST"] = json.dumps(self.manifest)
        self.make_stub("uname", 'print(os.environ.get("LANBRIDGE_STUB_OS", "Linux"))')
        self.make_stub(
            "flatpak-builder",
            'if sys.argv[1] == "--show-manifest":\n'
            '    print(os.environ["LANBRIDGE_STUB_MANIFEST"])\n'
            '    raise SystemExit(int(os.environ.get("LANBRIDGE_STUB_SHOW_EXIT", "0")))\n'
            'raise SystemExit(int(os.environ.get("LANBRIDGE_STUB_BUILD_EXIT", "0")))',
        )
        self.make_stub(
            "flatpak",
            'if sys.argv[1:] == ["--default-arch"]:\n'
            '    print("x86_64")\n'
            'elif sys.argv[1] == "build-bundle":\n'
            '    status = int(os.environ.get("LANBRIDGE_STUB_BUNDLE_EXIT", "0"))\n'
            '    if status == 0:\n'
            '        Path(sys.argv[4]).write_bytes(b"stub flatpak bundle")\n'
            '    raise SystemExit(status)\n'
            'else:\n'
            '    raise SystemExit("unexpected flatpak invocation")',
        )

    def make_stub(self, name, body):
        stub = self.bin / name
        stub.write_text(
            f"#!{sys.executable}\n"
            "import json, os, sys\nfrom pathlib import Path\n"
            'with open(os.environ["LANBRIDGE_STUB_LOG"], "a") as log:\n'
            '    log.write(json.dumps({"tool": Path(sys.argv[0]).name, '
            '"args": sys.argv[1:], "qpa": os.environ.get("QT_QPA_PLATFORM")}) + "\\n")\n'
            + body
            + "\n"
        )
        stub.chmod(0o755)

    def run_build(self, output=None, **environment):
        return subprocess.run(
            ["bash", str(self.root / SCRIPT), str(output or self.output)],
            env={**self.environment, **environment},
            capture_output=True,
            text=True,
            check=False,
        )

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_builds_local_manifest_before_exporting_bundle(self):
        result = self.run_build()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.commands()
        builder = next(call for call in calls if call["tool"] == "flatpak-builder")
        self.assertEqual(
            builder["args"],
            [
                "--user", "--force-clean", "--disable-rofiles-fuse",
                f"--state-dir={self.output}/state", f"--repo={self.output}/repo",
                str(self.output / "build"), str(self.root / MANIFEST),
            ],
        )
        bundle = self.output / "lanbridge-1.27.0-linux-x86_64.flatpak"
        self.assertEqual(
            calls[-1]["args"],
            ["build-bundle", "--runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo",
             str(self.output / "repo"), str(bundle), "org.lanbridge.LanBridge", "stable"],
        )
        self.assertTrue(bundle.is_file())

    def test_failed_build_or_tests_prevent_bundle_export(self):
        result = self.run_build(LANBRIDGE_STUB_BUILD_EXIT="42")
        self.assertEqual(result.returncode, 42)
        self.assertFalse(any(call["args"][0] == "build-bundle" for call in self.commands()))
        self.assertFalse(list(self.output.glob("*.flatpak")))
        self.assertNotIn("已构建", result.stdout)

    def test_release_version_is_embedded_and_bundle_name_matches(self):
        result = self.run_build(LANBRIDGE_VERSION="2.3.4-rc.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.commands()
        shown = next(call for call in calls if call["tool"] == "flatpak-builder"
                     and call["args"][0] == "--show-manifest")
        self.assertEqual(shown["args"], ["--show-manifest", str(self.root / MANIFEST)])
        build = next(call for call in calls if call["tool"] == "flatpak-builder"
                     and call["args"][0] == "--user")
        generated = Path(build["args"][-1])
        self.assertEqual(generated.parent, self.output)
        manifest = json.loads(generated.read_text())
        module = manifest["modules"][-1]
        self.assertEqual(manifest["modules"][0], self.manifest["modules"][0])
        self.assertEqual(module["config-opts"], ["-DBUILD_TESTS=ON", "-DSKIP_BUILD_TESTS=ON",
                                               "-DBUILD_X11_SUPPORT=ON", "-DLANBRIDGE_VERSION=2.3.4-rc.1"])
        self.assertEqual(module["sources"][0], {"type": "dir", "path": str(self.root),
                                                 "skip": [".git", "build", "dist"]})
        self.assertTrue(module["run-tests"])
        self.assertEqual(module["test-commands"], self.manifest["modules"][-1]["test-commands"])
        self.assertTrue((self.output / "lanbridge-2.3.4-rc.1-linux-x86_64.flatpak").is_file())

    def test_release_versions_are_validated_before_building(self):
        for index, version in enumerate(("", "1.2", "v1.2.3", "01.2.3", "1.02.3", "1.2.03", "1.2.3-01",
                                         "1.2.3-rc.01", "1.2.3-rc..1", "1.2.3-", "1.2.3+build", "../1.2.3",
                                         "1.2.3;false", "1.2.3\n2.0.0")):
            with self.subTest(version=version):
                self.log.unlink(missing_ok=True)
                result = self.run_build(self.directory / f"invalid-output-{index}", LANBRIDGE_VERSION=version)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(call["tool"] == "flatpak-builder" for call in self.commands()))

    def test_manifest_failure_prevents_release_bundle_export(self):
        result = self.run_build(LANBRIDGE_VERSION="2.3.4", LANBRIDGE_STUB_SHOW_EXIT="41")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(call["tool"] == "flatpak" and call["args"][0] == "build-bundle"
                             for call in self.commands()))

    def test_release_existing_bundle_is_preserved_before_manifest_generation(self):
        self.output.mkdir()
        bundle = self.output / "lanbridge-2.3.4-linux-x86_64.flatpak"
        bundle.write_bytes(b"previous release")
        result = self.run_build(LANBRIDGE_VERSION="2.3.4")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(bundle.read_bytes(), b"previous release")
        self.assertFalse(any(call["tool"] == "flatpak-builder" for call in self.commands()))

    def test_unexpected_release_manifest_is_rejected(self):
        self.manifest["modules"][-1]["name"] = "other-app"
        result = self.run_build(LANBRIDGE_VERSION="2.3.4", LANBRIDGE_STUB_MANIFEST=json.dumps(self.manifest))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(call["tool"] == "flatpak-builder" and call["args"][0] == "--user"
                             for call in self.commands()))

    def test_failed_bundle_export_does_not_report_success(self):
        result = self.run_build(LANBRIDGE_STUB_BUNDLE_EXIT="43")
        self.assertEqual(result.returncode, 43)
        self.assertFalse(list(self.output.glob("*.flatpak")))
        self.assertNotIn("已构建", result.stdout)

    def test_existing_bundle_is_preserved_and_build_is_not_started(self):
        self.output.mkdir()
        bundle = self.output / "lanbridge-1.27.0-linux-x86_64.flatpak"
        bundle.write_bytes(b"previous verified build")
        result = self.run_build()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(bundle.read_bytes(), b"previous verified build")
        self.assertFalse(any(call["tool"] == "flatpak-builder" for call in self.commands()))

    def test_output_inside_copied_source_is_rejected(self):
        result = self.run_build(self.root / "recursive output")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([call["tool"] for call in self.commands()], ["uname"])

    def test_symlink_to_output_inside_copied_source_is_rejected(self):
        unsafe = self.root / "recursive output"
        unsafe.mkdir()
        alias = self.directory / "output alias"
        alias.symlink_to(unsafe, target_is_directory=True)
        result = self.run_build(alias)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([call["tool"] for call in self.commands()], ["uname"])

    def test_non_linux_host_is_rejected(self):
        result = self.run_build(LANBRIDGE_STUB_OS="Darwin")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([call["tool"] for call in self.commands()], ["uname"])

    def test_manifest_uses_current_source_and_x11_permissions(self):
        manifest = (self.root / MANIFEST).read_text()
        module = manifest.split("  - name: lanbridge\n", 1)[1]
        self.assertIn('      - "-DBUILD_X11_SUPPORT=ON"\n', module)
        self.assertIn("      - type: dir\n        path: ../../../\n", module)
        self.assertEqual(((self.root / MANIFEST).parent / "../../../").resolve(), self.root)
        for excluded in (".git", "build", "dist"):
            self.assertIn(f"          - {excluded}\n", module)
        self.assertIn("  - --socket=fallback-x11\n", manifest)
        self.assertNotIn("--socket=wayland", manifest)

    def test_manifest_test_command_is_headless_and_fails_closed(self):
        module = (self.root / MANIFEST).read_text().split("  - name: lanbridge\n", 1)[1]
        for option in ('      - "-DBUILD_TESTS=ON"\n', '      - "-DSKIP_BUILD_TESTS=ON"\n',
                       "    builddir: true\n", "    run-tests: true\n", '    test-rule: ""\n'):
            self.assertIn(option, module)
        commands = module.split("    test-commands:\n", 1)[1].split("    sources:\n", 1)[0]
        test_commands = [line.removeprefix("      - ") for line in commands.splitlines()]
        self.assertEqual(len(test_commands), 1)
        args = shlex.split(test_commands[0])
        self.assertEqual(args[:2], ["QT_QPA_PLATFORM=minimal", "ctest"])
        self.assertEqual(args[args.index("--test-dir") + 1], "src/unittests")
        self.assertEqual(args[args.index("-LE") + 1], "interactive")
        self.assertIn("--output-on-failure", args)
        self.assertIn("--no-tests=error", args)
        self.make_stub("ctest", 'raise SystemExit(int(os.environ.get("LANBRIDGE_STUB_TEST_EXIT", "0")))')
        for expected in (0, 17):
            result = subprocess.run(
                test_commands[0], shell=True, cwd=self.directory,
                env={**self.environment, "LANBRIDGE_STUB_TEST_EXIT": str(expected)},
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, expected, result.stderr)
            self.assertEqual(self.commands()[-1]["qpa"], "minimal")


if __name__ == "__main__":
    unittest.main()
