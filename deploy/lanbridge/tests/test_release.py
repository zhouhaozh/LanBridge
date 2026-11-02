#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Prepare fixture assets and exercise publishing with a stateful, local gh stub."""

from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "release.py"
SPEC = importlib.util.spec_from_file_location("lanbridge_release", SCRIPT)
RELEASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RELEASE)
COMMIT = "a" * 40
REPO = "example/LanBridge"


class GitHubStub:
    def __init__(self, tag="v1.28.0"):
        self.tag = tag
        self.commit = COMMIT
        self.release = None
        self.calls = []
        self.upload_directory = None
        self.annotated = False
        self.hide_draft = False
        self.lookup_error = None
        self.upload_fails = False
        self.publish_fails = False
        self.move_tag_on_upload = False
        self.remote_assets = []
        self.upload_mutation = None
        self.digest_mode = "present"

    def run(self, command, **kwargs):
        if command[0] != "gh" or kwargs.get("shell"):
            raise AssertionError("Only gh argument arrays may be executed")
        args = command[1:]
        self.calls.append(args)
        code, output, error = 0, "", ""
        if args[0] == "api":
            endpoint = args[1]
            if "/git/ref/tags/" in endpoint:
                value = {"object": {"type": "tag" if self.annotated else "commit",
                                    "sha": "b" * 40 if self.annotated else self.commit}}
            elif "/git/tags/" in endpoint:
                value = {"object": {"type": "commit", "sha": self.commit}}
            elif "/releases/tags/" in endpoint:
                if self.lookup_error:
                    return subprocess.CompletedProcess(command, 1, "", self.lookup_error)
                if self.release is None or (self.hide_draft and self.release["draft"]):
                    return subprocess.CompletedProcess(command, 1, "", "gh: Not Found (HTTP 404)")
                value = self.release
            elif endpoint.endswith("/releases?per_page=100"):
                value = [[self.release] if self.release else []]
            elif endpoint.endswith("/releases/1/assets?per_page=100"):
                if "--paginate" not in args or "--slurp" not in args:
                    raise AssertionError("All release asset pages must be inspected")
                value = [self.remote_assets[:2], self.remote_assets[2:]]
            else:
                raise AssertionError("Unexpected endpoint: " + endpoint)
            output = json.dumps(value)
        elif args[:2] == ["release", "create"]:
            if self.release is not None:
                raise AssertionError("A draft or published release must never be recreated")
            self.release = {"id": 1, "tag_name": self.tag, "draft": True, "target_commitish": COMMIT}
        elif args[:2] == ["release", "upload"]:
            self.upload_directory = kwargs.get("cwd")
            if not self.release or not self.release["draft"]:
                raise AssertionError("Uploads must only target a draft")
            if self.upload_fails:
                code, error = 1, "upload failed"
            else:
                names = args[3:args.index("--repo")]
                self.remote_assets = [asset for asset in self.remote_assets if asset["name"] not in names]
                for name in names:
                    content = (Path(kwargs["cwd"]) / name).read_bytes()
                    asset = {"name": name, "state": "uploaded", "size": len(content)}
                    if self.digest_mode == "present":
                        asset["digest"] = "sha256:" + hashlib.sha256(content).hexdigest()
                    elif self.digest_mode != "missing":
                        asset["digest"] = "" if self.digest_mode == "empty" else None
                    self.remote_assets.append(asset)
                if self.upload_mutation:
                    self.upload_mutation(self.remote_assets)
            if self.move_tag_on_upload:
                self.commit = "f" * 40
        elif args[:2] == ["release", "edit"]:
            if not self.release or not self.release["draft"]:
                raise AssertionError("Only drafts may be published")
            if self.publish_fails:
                code, error = 1, "publish failed"
            else:
                self.release["draft"] = False
        else:
            raise AssertionError("Unexpected command: " + repr(command))
        return subprocess.CompletedProcess(command, code, output, error)

    def writes(self):
        return [call for call in self.calls if call[0] == "release"]


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lanbridge-release-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.assets = self.directory / "release assets"
        self.assets.mkdir()
        self.tag = "v1.28.0"
        self.make_assets("1.28.0")

    def make_assets(self, version):
        self.names = [f"lanbridge-{version}-linux-x86_64.flatpak", f"lanbridge-{version}-macos-arm64.dmg"]
        for index, name in enumerate(self.names):
            (self.assets / name).write_bytes(f"binary fixture {index}\n".encode())

    def prepare(self, **overrides):
        arguments = {"tag": self.tag, "commit": COMMIT, "repo": REPO, "assets": self.assets}
        arguments.update(overrides)
        return RELEASE.prepare(**arguments)

    def test_version_command_emits_github_outputs_without_gh(self):
        for tag, version, prerelease in (("v0.0.0", "0.0.0", False), ("v1.2.3", "1.2.3", False),
                                         ("v1.2.3-rc.1", "1.2.3-rc.1", True),
                                         ("v1.2.3-0", "1.2.3-0", True)):
            with self.subTest(tag=tag), patch.object(RELEASE.subprocess, "run") as run:
                output = io.StringIO()
                with redirect_stdout(output):
                    self.assertEqual(RELEASE.main(["version", tag]), 0)
                self.assertEqual(output.getvalue(), f"version={version}\nprerelease={str(prerelease).lower()}\n")
                run.assert_not_called()

    def test_rejects_noncanonical_versions(self):
        for tag in ("", "1.2.3", "vv1.2.3", "v1.2", "v01.2.3", "v1.02.3", "v1.2.03",
                    "v1.2.3-01", "v1.2.3-rc.01", "v1.2.3-rc..1", "v1.2.3-", "v1.2.3+build.1",
                    "v1.2.3/../../escape", "v1.2.3;false", "v1.2.3\n"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                RELEASE.parse_tag(tag)

    def test_prepare_command_runs_locally_without_publishing(self):
        with patch.object(RELEASE.subprocess, "run") as run, redirect_stdout(io.StringIO()) as output:
            result = RELEASE.main(["prepare", "--tag", self.tag, "--commit", COMMIT,
                                   "--repo", REPO, "--assets", str(self.assets)])
            self.assertEqual(result, 0)
            self.assertIn("未操作 GitHub", output.getvalue())
            self.assertTrue((self.assets / "RELEASE-MANIFEST.json").is_file())
            run.assert_not_called()

    def test_prepare_generates_reproducible_hashes_metadata_and_chinese_notes_without_gh(self):
        with patch.object(RELEASE.subprocess, "run") as run:
            self.prepare()
            first = {path.name: path.read_bytes() for path in self.assets.iterdir()}
            self.prepare()
            self.assertEqual(first, {path.name: path.read_bytes() for path in self.assets.iterdir()})
            run.assert_not_called()
        manifest = json.loads((self.assets / "RELEASE-MANIFEST.json").read_text())
        self.assertEqual((manifest["tag"], manifest["version"], manifest["commit"], manifest["repository"]),
                         (self.tag, "1.28.0", COMMIT, REPO))
        self.assertFalse(manifest["prerelease"])
        self.assertEqual(set(manifest["assets"]), set(self.names))
        for name, metadata in manifest["assets"].items():
            data = (self.assets / name).read_bytes()
            self.assertEqual(metadata, {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
        checksums = (self.assets / "SHA256SUMS").read_text().splitlines()
        self.assertEqual(len(checksums), 3)
        self.assertEqual({line.split("  ", 1)[1] for line in checksums}, set(self.names + ["RELEASE-MANIFEST.json"]))
        for line in checksums:
            digest, name = line.split("  ", 1)
            self.assertEqual(digest, hashlib.sha256((self.assets / name).read_bytes()).hexdigest())
        notes = (self.assets / "NOTES.md").read_text()
        for fragment in ("Apple Silicon", "Developer ID", "公证", "6.11", "运行时", "联网", "sha256sum", "shasum"):
            self.assertIn(fragment, notes)

    def test_invalid_commit_and_repository_do_not_write_outputs(self):
        for values in ({"commit": "a" * 39}, {"commit": "z" * 40}, {"commit": "main"},
                       {"repo": "owner"}, {"repo": "../repo"}, {"repo": "owner/repo/extra"},
                       {"repo": "owner/repo;false"}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                self.prepare(**values)
        self.assertEqual({path.name for path in self.assets.iterdir()}, set(self.names))

    def test_missing_wrong_empty_or_extra_assets_are_rejected(self):
        binary = self.assets / self.names[0]
        binary.unlink()
        with self.assertRaises(ValueError):
            self.prepare()
        binary.write_bytes(b"")
        with self.assertRaises(ValueError):
            self.prepare()
        binary.write_bytes(b"fixture")
        wrong = self.assets / "lanbridge-9.9.9-linux-x86_64.flatpak"
        binary.rename(wrong)
        with self.assertRaises(ValueError):
            self.prepare()
        wrong.rename(binary)
        extra = self.assets / "unintended.log"
        extra.write_text("not a release asset")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_input_and_output_symlinks_never_read_or_overwrite_outside_files(self):
        outside = self.directory / "private.txt"
        outside.write_bytes(b"preserve private data")
        for name in (self.names[0], "RELEASE-MANIFEST.json", "SHA256SUMS", "NOTES.md"):
            with self.subTest(name=name):
                path = self.assets / name
                previous = path.read_bytes() if path.exists() else None
                path.unlink(missing_ok=True)
                path.symlink_to(outside)
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertEqual(outside.read_bytes(), b"preserve private data")
                path.unlink()
                if previous is not None:
                    path.write_bytes(previous)

    def test_publishes_draft_only_after_exactly_four_assets_are_uploaded(self):
        github = GitHubStub()
        with patch.object(RELEASE.subprocess, "run", side_effect=github.run):
            self.prepare(publish=True)
        writes = github.writes()
        self.assertEqual([call[1] for call in writes], ["create", "upload", "edit"])
        create, upload, publish = writes
        self.assertIn("--draft", create)
        self.assertIn("--verify-tag", create)
        self.assertEqual(create[create.index("--target") + 1], COMMIT)
        self.assertIn("--latest=true", publish)
        self.assertIn("--prerelease=false", publish)
        self.assertIn("--draft=false", publish)
        self.assertIn("--clobber", upload)
        uploaded = set(upload[3:upload.index("--repo")])
        self.assertEqual(uploaded, set(self.names + ["SHA256SUMS", "RELEASE-MANIFEST.json"]))
        self.assertNotIn("NOTES.md", uploaded)
        self.assertEqual(github.upload_directory, self.assets.resolve())
        self.assertFalse(github.release["draft"])

    def test_upload_paths_cannot_be_interpreted_as_gh_asset_labels(self):
        directory = self.directory / "release # assets"
        self.assets.rename(directory)
        self.assets = directory
        github = GitHubStub()
        with patch.object(RELEASE.subprocess, "run", side_effect=github.run):
            self.prepare(publish=True)
        upload = next(call for call in github.calls if call[:2] == ["release", "upload"])
        self.assertFalse(any("#" in argument for argument in upload))
        self.assertEqual(github.upload_directory, directory.resolve())

    def test_annotated_tag_is_peeled_and_rechecked_after_upload(self):
        github = GitHubStub()
        github.annotated = True
        with patch.object(RELEASE.subprocess, "run", side_effect=github.run):
            self.prepare(publish=True)
        self.assertGreaterEqual(sum("/git/tags/" in call[1] for call in github.calls if call[0] == "api"), 2)

    def test_existing_matching_draft_is_reused_even_if_tag_lookup_hides_drafts(self):
        github = GitHubStub()
        github.release = {"id": 1, "tag_name": self.tag, "draft": True, "target_commitish": COMMIT}
        github.hide_draft = True
        with patch.object(RELEASE.subprocess, "run", side_effect=github.run):
            self.prepare(publish=True)
        self.assertEqual([call[1] for call in github.writes()], ["upload", "edit"])

    def test_existing_draft_with_unexpected_assets_is_not_uploaded_or_published(self):
        github = GitHubStub()
        github.release = {"id": 1, "tag_name": self.tag, "draft": True, "target_commitish": COMMIT}
        github.remote_assets = [{"name": "old-debug.log", "state": "uploaded", "size": 12}]
        with patch.object(RELEASE.subprocess, "run", side_effect=github.run), self.assertRaises(ValueError):
            self.prepare(publish=True)
        self.assertEqual(github.writes(), [])
        self.assertEqual(github.remote_assets[0]["name"], "old-debug.log")
        self.assertTrue(github.release["draft"])

    def test_incomplete_or_corrupt_uploaded_assets_are_never_published(self):
        mutations = {
            "missing": lambda assets: assets.pop(),
            "extra": lambda assets: assets.append({"name": "unexpected.txt", "state": "uploaded", "size": 1}),
            "size": lambda assets: assets[0].update(size=assets[0]["size"] + 1),
            "state": lambda assets: assets[0].update(state="starter"),
            "digest": lambda assets: assets[0].update(digest="sha256:" + "0" * 64),
            "digest_algorithm": lambda assets: assets[0].update(digest="sha512:" + "0" * 128),
            "duplicate": lambda assets: assets.append(dict(assets[0])),
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name):
                github = GitHubStub()
                github.upload_mutation = mutation
                with patch.object(RELEASE.subprocess, "run", side_effect=github.run), self.assertRaises(ValueError):
                    self.prepare(publish=True)
                self.assertEqual([call[1] for call in github.writes()], ["create", "upload"])
                self.assertTrue(github.release["draft"])

    def test_missing_optional_digest_still_requires_complete_uploaded_assets(self):
        for mode in ("missing", "null", "empty"):
            with self.subTest(mode=mode):
                github = GitHubStub()
                github.digest_mode = mode
                with patch.object(RELEASE.subprocess, "run", side_effect=github.run):
                    self.prepare(publish=True)
                self.assertFalse(github.release["draft"])
                queries = [call for call in github.calls if call[0] == "api" and "/assets?" in call[1]]
                self.assertEqual(len(queries), 2)

    def test_failed_upload_leaves_draft_and_retry_can_complete_it(self):
        github = GitHubStub()
        github.upload_fails = True
        with patch.object(RELEASE.subprocess, "run", side_effect=github.run):
            with self.assertRaises(RuntimeError):
                self.prepare(publish=True)
            self.assertTrue(github.release["draft"])
            self.assertFalse(any(call[:2] == ["release", "edit"] for call in github.calls))
            github.upload_fails = False
            self.prepare(publish=True)
        self.assertEqual([call[1] for call in github.writes()], ["create", "upload", "upload", "edit"])

    def test_publish_failure_preserves_draft(self):
        github = GitHubStub()
        github.publish_fails = True
        with patch.object(RELEASE.subprocess, "run", side_effect=github.run), self.assertRaises(RuntimeError):
            self.prepare(publish=True)
        self.assertTrue(github.release["draft"])

    def test_published_releases_mismatched_drafts_and_malformed_metadata_are_protected(self):
        for metadata in ({"tag_name": self.tag, "draft": False, "target_commitish": COMMIT},
                         {"tag_name": self.tag, "draft": True, "target_commitish": "main"},
                         {"tag_name": self.tag, "draft": True, "target_commitish": "f" * 40},
                         {"tag_name": self.tag, "draft": "true", "target_commitish": COMMIT},
                         {"tag_name": "v9.9.9", "draft": True, "target_commitish": COMMIT}):
            with self.subTest(metadata=metadata):
                github = GitHubStub()
                github.release = metadata
                with patch.object(RELEASE.subprocess, "run", side_effect=github.run), self.assertRaises(ValueError):
                    self.prepare(publish=True)
                self.assertEqual(github.writes(), [])

    def test_only_an_explicit_404_allows_a_missing_release(self):
        for error in ("gh: Bad credentials (HTTP 401)", "gh: rate limit exceeded (HTTP 403)",
                      "network timeout", "release not found"):
            with self.subTest(error=error):
                github = GitHubStub()
                github.lookup_error = error
                with patch.object(RELEASE.subprocess, "run", side_effect=github.run), self.assertRaises(RuntimeError):
                    self.prepare(publish=True)
                self.assertEqual(github.writes(), [])

    def test_moved_tag_is_rejected_before_creation_or_publication(self):
        for move_during_upload in (False, True):
            with self.subTest(move_during_upload=move_during_upload):
                github = GitHubStub()
                if move_during_upload:
                    github.move_tag_on_upload = True
                else:
                    github.commit = "f" * 40
                with patch.object(RELEASE.subprocess, "run", side_effect=github.run), self.assertRaises(ValueError):
                    self.prepare(publish=True)
                self.assertFalse(any(call[:2] == ["release", "edit"] for call in github.calls))
                if not move_during_upload:
                    self.assertEqual(github.writes(), [])

    def test_prerelease_is_explicitly_excluded_from_latest(self):
        for path in self.assets.iterdir():
            path.unlink()
        version = "1.28.0-rc.1"
        self.tag = "v" + version
        self.make_assets(version)
        github = GitHubStub(self.tag)
        with patch.object(RELEASE.subprocess, "run", side_effect=github.run):
            self.prepare(publish=True)
        for call in github.writes():
            if call[1] in {"create", "edit"}:
                self.assertIn("--prerelease=true", call)
                self.assertIn("--latest=false", call)
        self.assertTrue(json.loads((self.assets / "RELEASE-MANIFEST.json").read_text())["prerelease"])


if __name__ == "__main__":
    unittest.main()
