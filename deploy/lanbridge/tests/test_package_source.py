#!/usr/bin/python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Run with /usr/bin/python3 -m unittest discover -s deploy/lanbridge/tests."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location("package_source", Path(__file__).resolve().parents[1] / "package-source.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PackageSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lanbridge source 中文 ")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.root = self.directory / "源码 仓库"
        self.root.mkdir()
        self.git("init", "-q")
        self.write("tracked.txt", "before\n")
        self.write("deleted.txt", "delete me\n")
        self.write(".gitignore", "ignored.txt\n.cache/\n")
        self.git("add", ".")
        self.git("-c", "user.name=Source Test", "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture")
        self.archive = self.directory / "输出 文件.tar.gz"
        self.manifest = self.directory / "清单 文件.json"

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), *args], check=True, capture_output=True).stdout

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data, encoding="utf-8")
        return path

    def package(self):
        return MODULE.package(self.root, self.archive, self.manifest)

    def test_current_tracked_and_untracked_sources_and_archive_hashes(self):
        self.write("tracked.txt", "current worktree\n")
        (self.root / "deleted.txt").unlink()
        self.write("docs/交接 文档.md", "最新交接\n")
        self.write("ignored.txt", "secret\n")
        self.write("build/generated.txt", "output\n")
        self.write("dist/generated.txt", "output\n")
        self.write(".cache/cache.txt", "cache\n")
        executable = self.write("run me.sh", "#!/bin/sh\nexit 0\n")
        executable.chmod(0o751)
        (self.root / "relative link").symlink_to("docs/交接 文档.md")
        result = self.package()
        records = {item["path"]: item for item in result["files"]}
        self.assertEqual(set(records), {".gitignore", "tracked.txt", "docs/交接 文档.md", "run me.sh", "relative link"})
        self.assertEqual(json.loads(self.manifest.read_text(encoding="utf-8")), result)
        self.assertTrue(result["git"]["dirty"])
        self.assertEqual(result["archive"]["sha256"], hashlib.sha256(self.archive.read_bytes()).hexdigest())
        extracted = self.directory / "extracted"
        with tarfile.open(self.archive) as archive:
            self.assertEqual(len(archive.getmembers()), len(records))
            archive.extractall(extracted)
            for member in archive.getmembers():
                name = member.name.removeprefix("lanbridge/")
                record = records[name]
                self.assertEqual(format(member.mode, "04o"), record["mode"])
                self.assertEqual(member.uid, 0)
                self.assertEqual(member.mtime, 0)
                path = extracted / member.name
                if member.issym():
                    data = os.fsencode(os.readlink(path))
                    self.assertEqual(record["type"], "symlink")
                    self.assertEqual(os.readlink(path), "docs/交接 文档.md")
                else:
                    data = path.read_bytes()
                    self.assertEqual(record["type"], "file")
                    self.assertEqual(path.stat().st_mode & 0o777, int(record["mode"], 8))
                    self.assertEqual(data, (self.root / name).read_bytes())
                self.assertEqual(record["bytes"], len(data))
                self.assertEqual(record["sha256"], hashlib.sha256(data).hexdigest())

    def test_archive_bytes_are_reproducible(self):
        self.package()
        second = self.directory / "another.tar.gz"
        MODULE.package(self.root, second, self.directory / "another.json")
        self.assertEqual(self.archive.read_bytes(), second.read_bytes())

    def test_refuses_existing_archive_or_manifest_without_writing_other_output(self):
        for existing, other in ((self.archive, self.manifest), (self.manifest, self.archive)):
            with self.subTest(existing=existing):
                existing.write_bytes(b"preserve")
                with self.assertRaisesRegex(RuntimeError, "Refusing to overwrite"):
                    self.package()
                self.assertEqual(existing.read_bytes(), b"preserve")
                self.assertFalse(other.exists())
                existing.unlink()

    def test_refuses_symlink_to_outside_root(self):
        outside = self.directory / "private.txt"
        outside.write_text("secret", encoding="utf-8")
        (self.root / "escape").symlink_to("../private.txt")
        with self.assertRaisesRegex(RuntimeError, "escapes source root"):
            self.package()
        self.assertFalse(self.archive.exists())
        self.assertFalse(self.manifest.exists())

    def test_refuses_tracked_parent_replaced_by_symlink(self):
        tracked = self.write("src/file.txt", "source")
        self.git("add", "src/file.txt")
        tracked.unlink()
        tracked.parent.rmdir()
        other = self.root / "local"
        other.mkdir()
        (other / "file.txt").write_text("private", encoding="utf-8")
        tracked.parent.symlink_to("local", target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, "parent is a symlink"):
            self.package()
        self.assertFalse(self.archive.exists())

    def test_outputs_inside_worktree_do_not_archive_themselves(self):
        self.archive = self.root / "delivery" / "source.tar.gz"
        self.manifest = self.root / "delivery" / "source.json"
        result = self.package()
        self.assertEqual({entry["path"] for entry in result["files"]},
                         {".gitignore", "tracked.txt", "deleted.txt"})

    def test_refuses_subdirectory_as_source_and_same_output(self):
        child = self.root / "child"
        child.mkdir()
        with self.assertRaisesRegex(RuntimeError, "worktree root"):
            MODULE.package(child, self.archive, self.manifest)
        with self.assertRaisesRegex(RuntimeError, "different paths"):
            MODULE.package(self.root, self.archive, self.archive)

    def test_cli_succeeds_with_unicode_paths(self):
        result = subprocess.run([
            "/usr/bin/python3", str(MODULE.__file__), "--source", str(self.root),
            "--archive", str(self.archive), "--manifest", str(self.manifest),
        ], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SHA256", result.stdout)


if __name__ == "__main__":
    unittest.main()
