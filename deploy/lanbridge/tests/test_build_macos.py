#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Exercise macOS packaging with isolated command stubs on any host."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[3]
SCRIPT = Path("deploy/lanbridge/build-macos.sh")


class MacOSBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lanbridge-macos-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.root = self.directory / "local source tree"
        self.build = self.directory / "build tree"
        self.output = self.directory / "package output"
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        target = self.root / SCRIPT
        target.parent.mkdir(parents=True)
        shutil.copy2(PROJECT / SCRIPT, target)
        self.log = self.directory / "commands.jsonl"
        self.environment = {
            **os.environ,
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "LANBRIDGE_STUB_LOG": str(self.log),
            "LANBRIDGE_STUB_BREW_PREFIX": str(self.directory / "homebrew"),
            "TMPDIR": str(self.directory),
        }
        for name in ("LANBRIDGE_VERSION", "CMAKE_BUILD_PARALLEL_LEVEL"):
            self.environment.pop(name, None)
        self.make_stub(
            "uname",
            'print(os.environ.get("LANBRIDGE_STUB_OS", "Darwin") if sys.argv[1] == "-s" '
            'else os.environ.get("LANBRIDGE_STUB_ARCH", "arm64"))',
        )
        self.make_stub("sysctl", 'print("8")')
        self.make_stub(
            "brew",
            'print(os.environ["LANBRIDGE_STUB_BREW_PREFIX"] + '
            '("/opt/openssl@3" if len(sys.argv) > 2 else ""))',
        )
        self.make_stub(
            "cmake",
            'phase = {"--build": "BUILD", "--install": "INSTALL"}.get(sys.argv[1], "CONFIGURE")\n'
            'status = int(os.environ.get("LANBRIDGE_STUB_" + phase + "_EXIT", "0"))\n'
            'if phase == "INSTALL" and status == 0:\n'
            '    output = Path(sys.argv[sys.argv.index("--prefix") + 1])\n'
            '    (output / "LanBridge.app").mkdir()\n'
            'raise SystemExit(status)',
        )
        self.make_stub("ctest", 'raise SystemExit(int(os.environ.get("LANBRIDGE_STUB_TEST_EXIT", "0")))')
        self.make_stub("ditto", 'Path(sys.argv[2]).mkdir()')
        self.make_stub(
            "hdiutil",
            'if sys.argv[1] == "create":\n'
            '    Path(sys.argv[-1]).write_bytes(b"stub disk image")',
        )
        # The build script intentionally invokes the verifier with /usr/bin/python3.
        # Replace only the verifier in the temporary source tree, never a system tool.
        self.make_stub(
            "verify-macos-bundle.py",
            'raise SystemExit(int(os.environ.get("LANBRIDGE_STUB_VERIFY_EXIT", "0")))',
            directory=self.root / "deploy/lanbridge",
        )

    def make_stub(self, name, body, directory=None):
        stub = (directory or self.bin) / name
        stub.write_text(
            f"#!{sys.executable}\n"
            "import json, os, sys\nfrom pathlib import Path\n"
            'with open(os.environ["LANBRIDGE_STUB_LOG"], "a") as log:\n'
            '    log.write(json.dumps({"tool": Path(sys.argv[0]).name, "args": sys.argv[1:], '
            '"parallel": os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL")}) + "\\n")\n'
            + body + "\n",
            encoding="utf-8",
        )
        stub.chmod(0o755)

    def run_build(self, output=None, **environment):
        return subprocess.run(
            ["bash", str(self.root / SCRIPT), str(self.build), str(output or self.output)],
            env={**self.environment, **environment},
            capture_output=True,
            text=True,
            check=False,
        )

    def commands(self):
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_default_version_keeps_packaging_and_validation_order(self):
        result = self.run_build()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.commands()
        cmake = [call for call in calls if call["tool"] == "cmake"]
        self.assertIn("-DLANBRIDGE_VERSION=1.27.0", cmake[0]["args"])
        self.assertIn("-DBUILD_OSX_BUNDLE=ON", cmake[0]["args"])
        self.assertIn("-DBUILD_INSTALLER=ON", cmake[0]["args"])
        self.assertEqual(cmake[1]["args"], ["--build", str(self.build), "--parallel", "8"])
        tools = [call["tool"] for call in calls]
        test_index = tools.index("ctest")
        install_index = next(index for index, call in enumerate(calls)
                             if call["tool"] == "cmake" and call["args"][0] == "--install")
        verify_index = tools.index("verify-macos-bundle.py")
        self.assertLess(test_index, install_index)
        self.assertLess(install_index, verify_index)
        self.assertLess(verify_index, tools.index("ditto"))
        self.assertLess(tools.index("ditto"), tools.index("hdiutil"))
        image = self.output / "lanbridge-1.27.0-macos-arm64.dmg"
        self.assertEqual(image.read_bytes(), b"stub disk image")
        self.assertEqual(calls[-1]["args"], ["verify", str(image)])
        self.assertEqual(list(self.directory.glob("lanbridge-dmg.*")), [])

    def test_release_and_prerelease_versions_reach_cmake_and_image_name(self):
        for index, version in enumerate(("1.28.0", "2.0.0-rc.1", "2.0.0-beta.1", "2.0.0-alpha-1.0")):
            with self.subTest(version=version):
                self.log.unlink(missing_ok=True)
                output = self.directory / f"release output {index}"
                result = self.run_build(output, LANBRIDGE_VERSION=version)
                self.assertEqual(result.returncode, 0, result.stderr)
                configure = next(call for call in self.commands() if call["tool"] == "cmake")
                self.assertIn(f"-DLANBRIDGE_VERSION={version}", configure["args"])
                image = output / f"lanbridge-{version}-macos-arm64.dmg"
                self.assertTrue(image.is_file())
                self.assertIn(str(image), result.stdout)

    def test_requested_parallelism_does_not_query_host_cpu_count(self):
        result = self.run_build(CMAKE_BUILD_PARALLEL_LEVEL="2")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.commands()
        self.assertFalse(any(call["tool"] == "sysctl" for call in calls))
        build = next(call for call in calls if call["tool"] == "cmake" and call["args"][0] == "--build")
        self.assertEqual(build["args"], ["--build", str(self.build), "--parallel", "2"])
        self.assertEqual(build["parallel"], "2")

    def test_tests_are_noninteractive_and_missing_tests_are_errors(self):
        result = self.run_build()
        self.assertEqual(result.returncode, 0, result.stderr)
        tests = next(call for call in self.commands() if call["tool"] == "ctest")
        self.assertEqual(tests["args"], ["--test-dir", str(self.build / "src/unittests"),
                                         "-LE", "interactive", "--output-on-failure", "--no-tests=error"])

    def test_configuration_build_and_test_failures_stop_before_installing(self):
        for phase, expected in (("CONFIGURE", 23), ("BUILD", 24), ("TEST", 25)):
            with self.subTest(phase=phase):
                self.log.unlink(missing_ok=True)
                result = self.run_build(**{f"LANBRIDGE_STUB_{phase}_EXIT": str(expected)})
                self.assertEqual(result.returncode, expected, result.stderr)
                calls = self.commands()
                self.assertFalse(any(call["tool"] == "cmake" and call["args"][0] == "--install" for call in calls))
                self.assertFalse(any(call["tool"] in {"verify-macos-bundle.py", "ditto", "hdiutil"} for call in calls))
                self.assertFalse((self.output / "LanBridge.app").exists())
                self.assertEqual(list(self.output.glob("*.dmg")), [])

    def test_failed_bundle_verification_prevents_disk_image_creation(self):
        result = self.run_build(LANBRIDGE_STUB_VERIFY_EXIT="26")
        self.assertEqual(result.returncode, 26, result.stderr)
        self.assertFalse(any(call["tool"] in {"ditto", "hdiutil"} for call in self.commands()))
        self.assertEqual(list(self.output.glob("*.dmg")), [])

    def test_existing_image_and_paired_app_are_preserved_before_building(self):
        for index, version in enumerate(("1.27.0", "2.0.0-rc.1")):
            with self.subTest(version=version):
                self.log.unlink(missing_ok=True)
                output = self.directory / f"existing output {index}"
                app = output / "LanBridge.app"
                app.mkdir(parents=True)
                (app / "preserve.txt").write_text("previous application")
                image = output / f"lanbridge-{version}-macos-arm64.dmg"
                image.write_bytes(b"previous disk image")
                result = self.run_build(output, LANBRIDGE_VERSION=version)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(image.read_bytes(), b"previous disk image")
                self.assertEqual((app / "preserve.txt").read_text(), "previous application")
                self.assertFalse(any(call["tool"] in {"cmake", "ctest", "ditto", "hdiutil"} for call in self.commands()))

    def test_invalid_versions_are_rejected_before_building_or_creating_outputs(self):
        for version in ("", "v1.2.3", "1.2", "01.2.3", "1.02.3", "1.2.03", "1.2.3-01",
                        "1.2.3-rc.01", "1.2.3-rc..1", "1.2.3+build.1", "1.2.3/../../escape",
                        "1.2.3;touch injected", "$(touch injected)", "1.2.3\n"):
            with self.subTest(version=version):
                self.log.unlink(missing_ok=True)
                result = self.run_build(LANBRIDGE_VERSION=version)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(call["tool"] in {"brew", "cmake", "ctest", "ditto", "hdiutil"}
                                     for call in self.commands()))
                self.assertFalse(self.build.exists())
                self.assertFalse(self.output.exists())

    def test_non_apple_silicon_hosts_are_rejected(self):
        for environment in ({"LANBRIDGE_STUB_OS": "Linux"}, {"LANBRIDGE_STUB_ARCH": "x86_64"}):
            with self.subTest(environment=environment):
                self.log.unlink(missing_ok=True)
                result = self.run_build(**environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(call["tool"] != "uname" for call in self.commands()))
                self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
