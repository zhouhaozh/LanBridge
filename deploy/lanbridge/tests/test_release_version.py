#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Validate release versions through the same CMake parser used by the project."""

from pathlib import Path
import datetime
import os
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


PROJECT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(shutil.which("cmake"), "CMake is required for release version validation")
class ReleaseVersionTests(unittest.TestCase):
    def parse(self, version):
        with tempfile.TemporaryDirectory(prefix="lanbridge-version-") as temporary:
            script = Path(temporary) / "check.cmake"
            script.write_text(
                f'include("{PROJECT / "cmake/LanBridgeVersion.cmake"}")\n'
                'lanbridge_parse_version("${TEST_VERSION}" major minor patch)\n'
                'file(WRITE "${CMAKE_CURRENT_LIST_DIR}/parsed.txt" "${major}.${minor}.${patch}.0")\n'
            )
            result = subprocess.run(["cmake", "-DTEST_VERSION=" + version, "-P", str(script)],
                                    capture_output=True, text=True, check=False)
            parsed = Path(temporary) / "parsed.txt"
            return result, parsed.read_text() if parsed.exists() else None

    def test_accepts_release_and_prerelease_versions(self):
        for version in ("0.0.0", "2.30.4", "2.3.4-rc.1", "1.0.0-alpha-beta.2a", "1.2.3-0", "1.2.3-x.01a"):
            with self.subTest(version=version):
                result, parsed = self.parse(version)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(parsed, version.split("-", 1)[0] + ".0")

    def test_rejects_noncanonical_or_unsafe_versions(self):
        for version in ("", "1.2", "v1.2.3", "01.2.3", "1.02.3", "1.2.03", "1.2.3-01",
                        "1.2.3-rc.01", "1.2.3-rc..1", "1.2.3-", "1.2.3+build", "../1.2.3",
                        "1.2.3;false", "1.2.3\n2.0.0"):
            with self.subTest(version=version):
                result, parsed = self.parse(version)
                self.assertNotEqual(result.returncode, 0)
                self.assertIsNone(parsed)

    def test_release_appstream_uses_new_version_utc_date_and_generated_install_file(self):
        with tempfile.TemporaryDirectory(prefix="lanbridge-metainfo-") as temporary:
            directory = Path(temporary)
            source = directory / "source"
            source.mkdir()
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'project(release_metadata LANGUAGES NONE)\n'
                'set(CMAKE_INSTALL_DATADIR share)\n'
                'set(CMAKE_PROJECT_REV_FQDN org.lanbridge.LanBridge)\n'
                'set(LANBRIDGE_VERSION "2.3.4-rc.1")\n'
                f'include("{PROJECT / "deploy/linux/deploy.cmake"}")\n'
            )
            epoch = datetime.datetime(2026, 11, 1, 23, 30, tzinfo=datetime.timezone.utc).timestamp()
            result = subprocess.run(
                ["cmake", "-S", str(source), "-B", str(directory / "build")],
                env={**os.environ, "TZ": "Asia/Shanghai", "SOURCE_DATE_EPOCH": str(int(epoch))},
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            generated = directory / "build/org.lanbridge.LanBridge.metainfo.xml"
            releases = ET.fromstring(generated.read_text()).findall("releases/release")
            self.assertEqual(releases[0].attrib, {"version": "2.3.4-rc.1", "date": "2026-11-01"})
            self.assertTrue(any(release.attrib["version"] == "1.27.0" for release in releases[1:]))
            self.assertIn(str(generated), (directory / "build/cmake_install.cmake").read_text())


if __name__ == "__main__":
    unittest.main()
