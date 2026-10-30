#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Run on Linux with Python 3; all package tools are isolated stateful stubs.

Example: python3 deploy/lanbridge/tests/test_install_offline_linux.py
No actual flatpak, apt, sudo, dpkg or user configuration is accessed.
"""

import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


TEMPLATE = Path(__file__).resolve().parents[1] / "install-offline-linux.sh.in"
APP_COMMIT = "a" * 64
RUNTIME_COMMIT = "b" * 64
APP_REF = "app/org.lanbridge.LanBridge/x86_64/stable"
RUNTIME_REF = "runtime/org.kde.Platform/x86_64/6.11"
INSTALL_OPTIONS = [
    "--user", "--bundle", "--or-update", "--no-pull", "--no-deps",
    "--no-related", "--noninteractive",
]

STUB = r'''
import hashlib, json, os, subprocess, sys
from pathlib import Path

tool = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["LANBRIDGE_TEST_LOG"], "a") as log:
    log.write(json.dumps({"tool": tool, "args": args, "locale": os.environ.get("LC_ALL")}) + "\n")
state_path = Path(os.environ["LANBRIDGE_TEST_STATE"])
state = json.loads(state_path.read_text())

if tool == "id":
    assert args == ["-u"]
    print(os.environ.get("LANBRIDGE_TEST_UID", "1000"))
elif tool == "uname":
    print(os.environ.get("LANBRIDGE_TEST_OS", "Linux") if args == ["-s"]
          else os.environ.get("LANBRIDGE_TEST_ARCH", "x86_64"))
elif tool == "cat":
    if args == ["/etc/os-release"]:
        print('ID=' + os.environ.get("LANBRIDGE_TEST_DISTRO", "ubuntu"))
        print('VERSION_ID="' + os.environ.get("LANBRIDGE_TEST_VERSION", "24.04") + '"')
    else:
        for filename in args:
            if filename != "--":
                sys.stdout.buffer.write(Path(filename).read_bytes())
elif tool == "dpkg-deb":
    assert args[0] == "-f" and args[-1] == "Package"
    print(Path(args[1]).read_text().strip())
elif tool == "dpkg-query":
    assert args[:2] == ["-W", "-f=${db:Status-Status}"]
    if args[2] not in state["installed_packages"]:
        raise SystemExit(1)
    print("installed", end="")
elif tool == "sudo":
    if os.environ.get("LANBRIDGE_TEST_SUDO_REFUSE"):
        raise SystemExit(1)
    assert args[:2] == ["--", "apt-get"]
    raise SystemExit(subprocess.run([str(Path(sys.argv[0]).parent / "apt-get"), *args[2:]],
                                   env=os.environ, check=False).returncode)
elif tool == "apt-get":
    assert os.environ["LC_ALL"] == "C", "APT plan output must use a predictable locale"
    assert "--no-download" in args and "--no-remove" in args
    assert "--no-install-recommends" in args and "--no-upgrade" in args and "install" in args
    debs = [Path(arg) for arg in args if arg.endswith(".deb")]
    packages = [deb.read_text().strip() for deb in debs]
    assert "libc6" not in packages, "existing system package must never be passed to apt"
    archive_option = args[args.index("-o") + 1]
    assert archive_option.startswith("Dir::Cache::archives=")
    archives = Path(archive_option.split("=", 1)[1])
    assert archives.is_absolute() and (archives / "partial").is_dir()
    assert archives.parent == debs[0].parents[2], "APT cache must stay inside the private extraction directory"
    assert {entry.name for entry in archives.iterdir()} == {"partial", *(deb.name for deb in debs)}
    for deb in debs:
        assert (archives / deb.name).read_bytes() == deb.read_bytes(), "APT must receive identical verified local DEBs"
    state.setdefault("apt_cache_verifications", []).append({
        "simulation": "--simulate" in args,
        "archives": str(archives),
        "sha256": {deb.name: hashlib.sha256((archives / deb.name).read_bytes()).hexdigest() for deb in debs},
    })
    state_path.write_text(json.dumps(state))
    if "--simulate" in args:
        if os.environ.get("LANBRIDGE_TEST_SIMULATION_FAIL"):
            print("E: offline dependency closure incomplete")
            raise SystemExit(12)
        print(os.environ.get("LANBRIDGE_TEST_APT_PLAN", "\n".join(
            "Inst " + package + " (1.0 local [amd64])" for package in packages)))
    else:
        assert "-y" in args
        if os.environ.get("LANBRIDGE_TEST_APT_FAIL"):
            raise SystemExit(13)
        state["installed_packages"].extend(packages)
        state_path.write_text(json.dumps(state))
        (Path(sys.argv[0]).parent / "flatpak").symlink_to("mock-tool")
elif tool == "flatpak":
    if args[0] == "info":
        assert args[2] == "--show-commit"
        assert args[1] in ("--user", "--system")
        if args[3] == os.environ["LANBRIDGE_TEST_APP_REF"]:
            assert args[1] == "--user"
            value = state["app"]
        else:
            assert args[3] == os.environ["LANBRIDGE_TEST_RUNTIME_REF"]
            value = state["runtime_system" if args[1] == "--system" else "runtime_user"]
        if not value:
            raise SystemExit(1)
        print(value)
    elif args[0] == "ps":
        assert args == ["ps", "--columns=application"]
        if os.environ.get("LANBRIDGE_TEST_PS_FAIL"):
            raise SystemExit(14)
        if state["active"]:
            print("org.lanbridge.LanBridge")
    elif args[0] == "install":
        assert args[1:-1] == json.loads(os.environ["LANBRIDGE_TEST_INSTALL_OPTIONS"])
        if os.environ.get("LANBRIDGE_TEST_INSTALL_FAIL"):
            raise SystemExit(15)
        bundle = Path(args[-1])
        assert bundle.is_file()
        if bundle.name == "platform.flatpak":
            state["runtime_user"] = ("d" * 64 if os.environ.get("LANBRIDGE_TEST_WRONG_RUNTIME")
                                     else os.environ["LANBRIDGE_TEST_RUNTIME_COMMIT"])
        else:
            assert bundle.name == "lanbridge.flatpak"
            state["app"] = ("e" * 64 if os.environ.get("LANBRIDGE_TEST_WRONG_APP")
                            else os.environ["LANBRIDGE_TEST_APP_COMMIT"])
        state_path.write_text(json.dumps(state))
    else:
        raise SystemExit("forbidden flatpak command: " + repr(args))
else:
    raise SystemExit("unexpected mock command: " + tool)
'''


@unittest.skipUnless(sys.platform.startswith("linux"), "run these isolated tests on Linux (flock/GNU tools)")
class OfflineInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lanbridge offline tests ")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.bin = self.directory / "isolated tools"
        self.bin.mkdir()
        # PATH contains no system directories: a missing stub cannot reach a real package tool.
        for name in ("tail", "wc", "sha256sum", "tar", "gzip", "mktemp", "rm", "mkdir", "flock", "cp"):
            source = shutil.which(name)
            self.assertIsNotNone(source, f"missing test prerequisite: {name}")
            (self.bin / name).symlink_to(source)
        self.bash = shutil.which("bash")
        self.assertIsNotNone(self.bash)
        mock = self.bin / "mock-tool"
        mock.write_text(f"#!{sys.executable}\n" + STUB)
        mock.chmod(0o755)
        for name in ("id", "uname", "cat", "dpkg-deb", "dpkg-query", "apt-get", "sudo", "flatpak"):
            (self.bin / name).symlink_to(mock.name)
        self.log = self.directory / "commands.jsonl"
        self.state = self.directory / "state.json"
        self.state.write_text(json.dumps({
            "app": "", "runtime_user": "", "runtime_system": "", "active": False,
            "installed_packages": ["libc6"],
        }))
        self.home = self.directory / "private home"
        self.home.mkdir()
        self.temp = self.directory / "extraction area"
        self.temp.mkdir()
        # Do not inherit LD_PRELOAD, shell hooks, sudo configuration, or user's Flatpak variables.
        self.environment = {
            "PATH": str(self.bin), "HOME": str(self.home), "TMPDIR": str(self.temp),
            "XDG_CACHE_HOME": str(self.home / "cache"), "LC_ALL": "C.UTF-8",
            "LANBRIDGE_TEST_LOG": str(self.log), "LANBRIDGE_TEST_STATE": str(self.state),
            "LANBRIDGE_TEST_APP_COMMIT": APP_COMMIT,
            "LANBRIDGE_TEST_RUNTIME_COMMIT": RUNTIME_COMMIT,
            "LANBRIDGE_TEST_APP_REF": APP_REF, "LANBRIDGE_TEST_RUNTIME_REF": RUNTIME_REF,
            "LANBRIDGE_TEST_INSTALL_OPTIONS": json.dumps(INSTALL_OPTIONS),
        }
        self.installer = self.directory / "LanBridge offline installer.run"
        self.make_installer()

    def make_installer(self, bad_inner_checksum=False):
        files = {
            "app/lanbridge.flatpak": b"verified app bundle",
            "runtime/platform.flatpak": b"verified runtime bundle",
            "host-debs/base existing.deb": b"libc6",
            "host-debs/flatpak offline.deb": b"flatpak",
            "host-debs/portal offline.deb": b"xdg-desktop-portal",
            "MANIFEST.json": b'{"format": 1}\n',
        }
        files["SHA256SUMS"] = "".join(
            hashlib.sha256(content).hexdigest() + "  " + name + "\n"
            for name, content in sorted(files.items())
        ).encode()
        if bad_inner_checksum:
            files["app/lanbridge.flatpak"] = b"changed app bundle"
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w:gz") as tar:
            for name in ("app", "runtime", "host-debs"):
                member = tarfile.TarInfo(name)
                member.type = tarfile.DIRTYPE
                member.mode = 0o755
                tar.addfile(member)
            for name, content in files.items():
                member = tarfile.TarInfo(name)
                member.size = len(content)
                member.mode = 0o644
                tar.addfile(member, io.BytesIO(content))
        payload = archive.getvalue()
        template = TEMPLATE.read_text()
        values = {
            "@APP_COMMIT@": APP_COMMIT, "@RUNTIME_COMMIT@": RUNTIME_COMMIT,
            "@PAYLOAD_SHA256@": hashlib.sha256(payload).hexdigest(),
            "@PAYLOAD_BYTES@": str(len(payload)),
            "@PAYLOAD_LINE@": str(template.count("\n") + 1),
        }
        for token, value in values.items():
            template = template.replace(token, value)
        self.installer.write_bytes(template.encode() + payload)

    def set_state(self, **changes):
        state = self.read_state()
        state.update(changes)
        self.state.write_text(json.dumps(state))

    def read_state(self):
        return json.loads(self.state.read_text())

    def run_installer(self, *args, **environment):
        result = subprocess.run(
            [self.bash, str(self.installer), *args],
            env={**self.environment, **environment},
            capture_output=True, text=True, check=False, timeout=20,
        )
        self.assertEqual(list(self.temp.iterdir()), [], "installer must clean only its own extraction directory")
        return result

    def commands(self, tool=None):
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return [call for call in calls if tool is None or call["tool"] == tool]

    def installs(self):
        return [call["args"] for call in self.commands("flatpak") if call["args"][0] == "install"]

    def assert_no_package_operations(self):
        self.assertFalse([call for call in self.commands()
                          if call["tool"] in ("flatpak", "apt-get", "sudo", "dpkg-deb", "dpkg-query")])

    def test_help_is_available_without_host_or_package_checks(self):
        result = self.run_installer("--help", LANBRIDGE_TEST_UID="0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("GNOME X11", result.stdout)
        self.assertEqual(self.commands(), [])

    def test_verify_only_does_not_install_or_touch_user_cache(self):
        (self.bin / "flatpak").unlink()
        result = self.run_installer("--verify-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_no_package_operations()
        self.assertEqual(list(self.home.iterdir()), [])

    def test_corrupt_and_truncated_payload_fail_before_any_package_operation(self):
        original = self.installer.read_bytes()
        for damaged in (original[:-5], original[:-1] + bytes([original[-1] ^ 1])):
            with self.subTest(length=len(damaged)):
                self.installer.write_bytes(damaged)
                result = self.run_installer()
                self.assertNotEqual(result.returncode, 0)
                self.assert_no_package_operations()
                self.assertEqual(list(self.home.iterdir()), [])

    def test_file_checksums_are_verified_after_archive_hash(self):
        self.make_installer(bad_inner_checksum=True)
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("内含文件校验失败", result.stderr)
        self.assert_no_package_operations()

    def test_root_and_unsupported_hosts_are_rejected(self):
        for environment in ({"LANBRIDGE_TEST_UID": "0"}, {"LANBRIDGE_TEST_DISTRO": "debian"},
                            {"LANBRIDGE_TEST_VERSION": "22.04"}, {"LANBRIDGE_TEST_ARCH": "aarch64"},
                            {"LANBRIDGE_TEST_OS": "Darwin"}):
            with self.subTest(environment=environment):
                result = self.run_installer(**environment)
                self.assertNotEqual(result.returncode, 0)
                self.assert_no_package_operations()

    def test_same_app_and_runtime_are_idempotent_even_while_running(self):
        self.set_state(app=APP_COMMIT, runtime_user=RUNTIME_COMMIT, active=True)
        before = self.read_state()
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.read_state(), before)
        self.assertEqual(self.installs(), [])
        self.assertFalse(any(call["args"][0] == "ps" for call in self.commands("flatpak")))
        self.assertEqual(self.commands("apt-get"), [])

    def test_existing_different_user_runtime_is_never_overwritten(self):
        self.set_state(runtime_user="c" * 64)
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("拒绝覆盖", result.stderr)
        self.assertEqual(self.installs(), [])

    def test_running_app_blocks_any_required_change(self):
        for current_app in ("c" * 64, APP_COMMIT):
            with self.subTest(app=current_app):
                self.set_state(app=current_app, runtime_user="", active=True)
                result = self.run_installer()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("正在运行", result.stderr)
                self.assertEqual(self.installs(), [])

    def test_unavailable_running_state_blocks_changes(self):
        result = self.run_installer(LANBRIDGE_TEST_PS_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.installs(), [])

    def test_exact_system_runtime_is_reused_without_modifying_it(self):
        self.set_state(runtime_system=RUNTIME_COMMIT)
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.installs()), 1)
        self.assertTrue(self.installs()[0][-1].endswith("/app/lanbridge.flatpak"))
        self.assertEqual(self.read_state()["runtime_user"], "")
        self.assertEqual(self.read_state()["runtime_system"], RUNTIME_COMMIT)

    def test_different_system_runtime_is_preserved_and_user_runtime_installed(self):
        self.set_state(runtime_system="c" * 64)
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.read_state()["runtime_system"], "c" * 64)
        self.assertEqual(self.read_state()["runtime_user"], RUNTIME_COMMIT)
        self.assertEqual(len(self.installs()), 2)

    def test_success_uses_only_offline_user_installs_and_never_launches_or_uninstalls(self):
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        installs = self.installs()
        self.assertEqual(len(installs), 2)
        self.assertTrue(installs[0][-1].endswith("/runtime/platform.flatpak"))
        self.assertTrue(installs[1][-1].endswith("/app/lanbridge.flatpak"))
        for args in installs:
            self.assertEqual(args[1:-1], INSTALL_OPTIONS)
        self.assertEqual(self.read_state()["app"], APP_COMMIT)
        self.assertEqual(self.read_state()["runtime_user"], RUNTIME_COMMIT)
        self.assertEqual(self.commands("apt-get"), [])
        self.assertEqual(self.commands("sudo"), [])
        self.assertTrue(all(call["args"][0] in ("info", "ps", "install")
                            for call in self.commands("flatpak")))
        self.assertTrue(all(call["args"][0] == "info" for call in self.commands("flatpak")
                            if "--system" in call["args"]))

    def test_missing_flatpak_installs_only_missing_local_debs_after_simulation(self):
        (self.bin / "flatpak").unlink()
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        apt_calls = self.commands("apt-get")
        self.assertEqual(len(apt_calls), 2)
        self.assertIn("--simulate", apt_calls[0]["args"])
        self.assertNotIn("--simulate", apt_calls[1]["args"])
        self.assertIn("-y", apt_calls[1]["args"])
        for call in apt_calls:
            debs = [arg for arg in call["args"] if arg.endswith(".deb")]
            self.assertEqual(len(debs), 2)
            self.assertTrue(all("offline.deb" in path and " " in path for path in debs))
            self.assertFalse(any("base existing.deb" in path for path in debs))
            for option in ("--no-download", "--no-remove", "--no-install-recommends", "--no-upgrade"):
                self.assertIn(option, call["args"])
            self.assertEqual(call["locale"], "C")
        cache_verifications = self.read_state()["apt_cache_verifications"]
        self.assertEqual([entry["simulation"] for entry in cache_verifications], [True, False])
        self.assertEqual(cache_verifications[0]["archives"], cache_verifications[1]["archives"])
        self.assertEqual(cache_verifications[0]["sha256"], cache_verifications[1]["sha256"])
        self.assertEqual(cache_verifications[0]["sha256"], {
            "flatpak offline.deb": hashlib.sha256(b"flatpak").hexdigest(),
            "portal offline.deb": hashlib.sha256(b"xdg-desktop-portal").hexdigest(),
        })
        self.assertEqual(len(self.installs()), 2)

    def test_sudo_refusal_or_actual_apt_failure_prevent_flatpak_installation(self):
        (self.bin / "flatpak").unlink()
        for environment in ({"LANBRIDGE_TEST_SUDO_REFUSE": "1"}, {"LANBRIDGE_TEST_APT_FAIL": "1"}):
            with self.subTest(environment=environment):
                result = self.run_installer(**environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.installs(), [])
                self.assertFalse((self.bin / "flatpak").exists())

    def test_simulated_upgrade_removal_and_errors_stop_before_sudo(self):
        (self.bin / "flatpak").unlink()
        for environment in (
            {"LANBRIDGE_TEST_APT_PLAN": "Inst libc6 [2.39] (2.40 local [amd64])"},
            {"LANBRIDGE_TEST_APT_PLAN": "Remv libc6 [2.39]"},
            {"LANBRIDGE_TEST_SIMULATION_FAIL": "1"},
        ):
            with self.subTest(environment=environment):
                result = self.run_installer(**environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.commands("sudo"), [])
                self.assertTrue(all("--simulate" in call["args"] for call in self.commands("apt-get")))
                self.assertEqual(self.installs(), [])

    def test_failed_install_does_not_report_success_or_uninstall(self):
        result = self.run_installer(LANBRIDGE_TEST_INSTALL_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("离线安装完成", result.stdout)
        self.assertEqual(len(self.installs()), 1)

    def test_wrong_runtime_commit_stops_before_app_install(self):
        result = self.run_installer(LANBRIDGE_TEST_WRONG_RUNTIME="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("运行环境安装后的提交", result.stderr)
        self.assertEqual(len(self.installs()), 1)
        self.assertEqual(self.read_state()["app"], "")

    def test_wrong_app_commit_cannot_report_success(self):
        result = self.run_installer(LANBRIDGE_TEST_WRONG_APP="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("应用安装后的提交", result.stderr)
        self.assertNotIn("离线安装完成", result.stdout)

    def test_concurrent_installer_is_rejected_without_package_changes(self):
        import fcntl

        cache = self.home / "cache" / "LanBridge"
        cache.mkdir(parents=True)
        with (cache / "offline-install.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("另一个离线安装器", result.stderr)
        self.assert_no_package_operations()


if __name__ == "__main__":
    unittest.main()
