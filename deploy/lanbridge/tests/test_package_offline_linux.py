#!/usr/bin/python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "package-offline-linux.py"
SPEC = importlib.util.spec_from_file_location("package_offline_linux", SCRIPT)
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)


class PackageOfflineLinuxTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lanbridge offline 测试 ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.app = self.root / "application.flatpak"
        self.app.write_bytes(b"unchanged verified application bytes")
        self.runtime = self.root / "platform.flatpak"
        self.runtime.write_bytes(b"verified pinned runtime bytes")
        self.debs = self.root / "host-debs"
        self.debs.mkdir()
        self.deb = self.debs / "flatpak_1.14.6_amd64.deb"
        self.deb.write_bytes(b"authenticated host package bytes")
        self.host_manifest = self.debs / "HOST-DEBS-MANIFEST.json"
        self.host_manifest.write_text(json.dumps({"packages": [{
            "filename": self.deb.name, "sha256": PACKAGER.digest(self.deb), "bytes": self.deb.stat().st_size,
        }]}))
        self.template = self.root / "installer.sh.in"
        self.template.write_text(
            "#!/usr/bin/env bash\n"
            "# SHA=@PAYLOAD_SHA256@ SIZE=@PAYLOAD_BYTES@ LINE=@PAYLOAD_LINE@\n"
            "# APP=@APP_COMMIT@ RUNTIME=@RUNTIME_COMMIT@\n"
            "exit 0\n__LANBRIDGE_PAYLOAD__\n"
        )
        self.args = argparse.Namespace(
            app_bundle=self.app, app_sha256=PACKAGER.digest(self.app), app_commit="a" * 64,
            runtime_bundle=self.runtime, runtime_sha256=PACKAGER.digest(self.runtime), runtime_commit="b" * 64,
            host_debs=self.debs, template=self.template, output=self.root / "installer.run",
            manifest=self.root / "installer.json",
        )

    def test_single_file_contains_exact_inputs_and_all_checksums(self):
        result = PACKAGER.package(self.args)
        header, payload = self.args.output.read_bytes().split(b"__LANBRIDGE_PAYLOAD__\n", 1)
        self.assertEqual(hashlib.sha256(payload).hexdigest(), result["payload"]["sha256"])
        self.assertEqual(len(payload), result["payload"]["bytes"])
        self.assertIn(b"LINE=6", header)
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
            content = {member.name: archive.extractfile(member).read() for member in archive.getmembers()}
        self.assertEqual(content["app/lanbridge.flatpak"], self.app.read_bytes())
        self.assertEqual(content["runtime/platform.flatpak"], self.runtime.read_bytes())
        self.assertEqual(content["host-debs/" + self.deb.name], self.deb.read_bytes())
        for line in content["SHA256SUMS"].decode("ascii").splitlines():
            expected, name = line.split("  ", 1)
            self.assertEqual(expected, hashlib.sha256(content[name]).hexdigest())
        self.assertEqual(self.args.output.stat().st_mode & 0o777, 0o755)
        self.assertEqual(result["host_package_count"], 1)

    def test_corrupt_application_fails_before_publishing(self):
        self.app.write_bytes(b"corrupt application")
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            PACKAGER.package(self.args)
        self.assertFalse(self.args.output.exists())
        self.assertFalse(self.args.manifest.exists())

    def test_host_package_integrity_is_required(self):
        self.deb.write_bytes(b"corrupt host package")
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            PACKAGER.package(self.args)
        self.assertFalse(self.args.output.exists())

    def test_null_host_hash_cannot_disable_authenticated_checksum(self):
        manifest = json.loads(self.host_manifest.read_text())
        manifest["packages"][0]["sha256"] = None
        self.host_manifest.write_text(json.dumps(manifest))
        self.deb.write_bytes(b"different unauthenticated package!!")
        with self.assertRaisesRegex(ValueError, "authenticated SHA256"):
            PACKAGER.package(self.args)
        self.assertFalse(self.args.output.exists())

    def test_boolean_host_package_size_is_rejected(self):
        manifest = json.loads(self.host_manifest.read_text())
        manifest["packages"][0]["bytes"] = True
        self.host_manifest.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "nonnegative integer"):
            PACKAGER.package(self.args)

    def test_symlink_input_is_rejected(self):
        self.args.runtime_bundle = self.root / "runtime-link.flatpak"
        self.args.runtime_bundle.symlink_to(self.runtime)
        with self.assertRaisesRegex(ValueError, "not a symlink"):
            PACKAGER.package(self.args)

    def test_unsafe_or_duplicate_package_name_is_rejected(self):
        manifest = json.loads(self.host_manifest.read_text())
        manifest["packages"][0]["filename"] = "../outside.deb"
        self.host_manifest.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "Unsafe or duplicate"):
            PACKAGER.package(self.args)

    def test_unmanifested_deb_is_rejected(self):
        (self.debs / "unexpected.deb").write_bytes(b"not in authenticated list")
        with self.assertRaisesRegex(ValueError, "same packages"):
            PACKAGER.package(self.args)

    def test_ubuntu_version_tilde_and_encoded_epoch_are_allowed(self):
        manifest = json.loads(self.host_manifest.read_text())
        name = "libexample_1%3a14.2.0-4ubuntu2~24.04.1_amd64.deb"
        self.deb.rename(self.debs / name)
        manifest["packages"][0]["filename"] = name
        self.host_manifest.write_text(json.dumps(manifest))
        result = PACKAGER.package(self.args)
        self.assertIn("host-debs/" + name, result["files"])

    def test_existing_output_is_never_overwritten(self):
        self.args.output.write_bytes(b"existing installer")
        with self.assertRaisesRegex(ValueError, "Refusing to overwrite"):
            PACKAGER.package(self.args)
        self.assertEqual(self.args.output.read_bytes(), b"existing installer")

    def test_same_inputs_produce_same_single_file(self):
        PACKAGER.package(self.args)
        original = self.args.output.read_bytes()
        self.args.output = self.root / "second.run"
        self.args.manifest = self.root / "second.json"
        PACKAGER.package(self.args)
        self.assertEqual(original, self.args.output.read_bytes())

    def test_template_must_have_all_tokens_and_final_marker(self):
        self.template.write_text("#!/bin/bash\nexit 0\n")
        with self.assertRaisesRegex(ValueError, "payload marker"):
            PACKAGER.package(self.args)
        self.assertFalse(self.args.output.exists())


if __name__ == "__main__":
    unittest.main()
