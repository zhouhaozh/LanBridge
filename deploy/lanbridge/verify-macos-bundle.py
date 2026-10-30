#!/usr/bin/python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Read-only portability, arm64, signing and loader checks for a local .app bundle."""

import argparse
import datetime
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile


MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}
LOAD_COMMANDS = {
    "LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB",
    "LC_LOAD_DYLINKER",
}


def inside(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def system_library(path):
    return inside(path, Path("/System/Library")) or inside(path, Path("/usr/lib"))


def run_tool(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, timeout=30, **kwargs)
    if result.returncode:
        raise RuntimeError("{}: {}".format(command[0], (result.stderr or result.stdout).strip()))
    return result.stdout


def load_commands(output):
    """Parse LC_LOAD_* and LC_RPATH without mistaking LC_ID_DYLIB for a dependency."""
    dependencies, rpaths = [], []
    command = None
    for line in output.splitlines():
        match = re.match(r"\s*cmd (LC_\w+)\s*$", line)
        if match:
            command = match.group(1)
            continue
        match = re.match(r"\s*(name|path) (.+) \(offset \d+\)\s*$", line)
        if not match:
            continue
        if command in LOAD_COMMANDS and match.group(1) == "name":
            dependencies.append({"command": command, "name": match.group(2)})
        elif command == "LC_RPATH" and match.group(1) == "path":
            rpaths.append(match.group(2))
    return dependencies, rpaths


def expand_path(name, loader, executable):
    if name == "@loader_path" or name.startswith("@loader_path/"):
        return (loader.parent / name[len("@loader_path"):].lstrip("/")).resolve()
    if name == "@executable_path" or name.startswith("@executable_path/"):
        return (executable.parent / name[len("@executable_path"):].lstrip("/")).resolve()
    if name.startswith("/"):
        return Path(name).resolve()
    return None


class BundleAudit:
    def __init__(self, bundle):
        self.bundle = bundle.resolve()
        self.images = {}
        self.errors = []
        self.edges = []
        self.bad_rpaths = []
        self._errors_seen = set()
        self._visited = set()

    def fail(self, message):
        if message not in self._errors_seen:
            self.errors.append(message)
            self._errors_seen.add(message)

    def short(self, path):
        return str(path.relative_to(self.bundle)) if inside(path, self.bundle) else str(path)

    def collect(self):
        for directory, subdirs, files in os.walk(self.bundle, followlinks=False):
            for name in subdirs + files:
                path = Path(directory) / name
                if path.is_symlink() and not inside(path.resolve(), self.bundle):
                    self.fail("Bundle symlink escapes: {} -> {}".format(self.short(path), path.resolve()))
            for name in files:
                path = (Path(directory) / name).resolve()
                if path in self.images or not inside(path, self.bundle) or not path.is_file():
                    continue
                with path.open("rb") as source:
                    if source.read(4) not in MACHO_MAGICS:
                        continue
                architectures = run_tool(["/usr/bin/lipo", "-archs", str(path)]).split()
                if "arm64" not in architectures:
                    self.fail("Missing arm64 slice: {} ({})".format(self.short(path), ", ".join(architectures)))
                    continue
                dependencies, rpaths = load_commands(run_tool(["/usr/bin/otool", "-arch", "arm64", "-l", str(path)]))
                header = run_tool(["/usr/bin/otool", "-arch", "arm64", "-hv", str(path)])
                self.images[path] = {
                    "path": self.short(path), "architectures": architectures,
                    "executable": bool(re.search(r"\bEXECUTE\b", header)),
                    "dependencies": dependencies, "rpaths": rpaths,
                }

    def visit(self, path, executable, inherited=()):
        # dyld runpaths are expanded in the image that declared them, then inherited by dependencies.
        image = self.images.get(path)
        if image is None:
            self.fail("Dependency is not an audited arm64 Mach-O: {}".format(self.short(path)))
            return
        rpaths = []
        for value in image["rpaths"]:
            resolved = expand_path(value, path, executable)
            portable = resolved is not None and (
                (value.startswith("/") and system_library(resolved)) or
                (not value.startswith("/") and inside(resolved, self.bundle))
            )
            if not portable:
                record = {"image": self.short(path), "rpath": value, "resolved": str(resolved)}
                if record not in self.bad_rpaths:
                    self.bad_rpaths.append(record)
                self.fail("Unsafe LC_RPATH: {}: {} -> {}".format(self.short(path), value, resolved))
                continue
            if resolved not in rpaths:
                rpaths.append(resolved)
        for value in inherited:
            if value not in rpaths:
                rpaths.append(value)
        key = (path, executable, tuple(rpaths))
        if key in self._visited:
            return
        self._visited.add(key)
        for dependency in image["dependencies"]:
            name = dependency["name"]
            if name.startswith("/") and not system_library(Path(name).resolve()):
                self.fail("Absolute non-system dependency is not relocatable: {}: {}".format(self.short(path), name))
            candidates = []
            if name.startswith("@rpath/"):
                candidates = [(base / name[len("@rpath/"):]).resolve() for base in rpaths]
            else:
                resolved = expand_path(name, path, executable)
                if resolved is not None and (name.startswith("/") or inside(resolved, self.bundle)):
                    candidates = [resolved]
            resolved = None
            for candidate in candidates:
                # Apple system libraries may exist only in the dyld shared cache, not as filesystem files.
                if system_library(candidate):
                    resolved = candidate
                    break
                if inside(candidate, self.bundle) and candidate.is_file():
                    resolved = candidate
                    break
            edge = {"image": self.short(path), "executable": self.short(executable),
                    "command": dependency["command"], "dependency": name,
                    "resolved": self.short(resolved) if resolved is not None else None}
            self.edges.append(edge)
            if resolved is None:
                self.fail("Unresolved or external dependency: {}: {}".format(self.short(path), name))
            elif not system_library(resolved):
                self.visit(resolved, executable, tuple(rpaths))

    def audit_dependencies(self, main):
        executables = [path for path, image in self.images.items() if image["executable"]]
        if main not in executables:
            self.fail("CFBundleExecutable is not an audited arm64 executable")
        for executable in executables:
            self.visit(executable, executable)
        # Qt plugins are loaded at runtime, so they are not necessarily in the main binary's LC_LOAD graph.
        main_rpaths = tuple(
            expanded for value in self.images.get(main, {}).get("rpaths", [])
            for expanded in [expand_path(value, main, main)]
            if expanded is not None and (inside(expanded, self.bundle) or system_library(expanded))
        )
        reached = {key[0] for key in self._visited}
        for path in self.images:
            if path not in reached:
                self.visit(path, main, main_rpaths)
        return executables


def smoke_check(executable):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("DYLD_") and key not in {
               "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH",
               "QT_QPA_PLATFORM", "QT_DEBUG_PLUGINS",
           }}
    try:
        with tempfile.TemporaryDirectory(prefix="lanbridge-loader-check-") as working:
            # --version initializes Qt settings before parsing in the upstream GUI; keep that state disposable.
            env["XDG_CONFIG_HOME"] = str(Path(working) / "config")
            env["XDG_STATE_HOME"] = str(Path(working) / "state")
            result = subprocess.run([str(executable), "--version"], cwd=working, env=env,
                                    text=True, capture_output=True, timeout=20)
        return {"passed": result.returncode == 0, "returncode": result.returncode,
                "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}
    except (subprocess.TimeoutExpired, OSError) as error:
        return {"passed": False, "error": str(error)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--report", type=Path, default=Path(__file__).resolve().parents[2] / "build/verification/bundle-audit.json")
    args = parser.parse_args()
    report = {"format_version": 1, "bundle": str(args.bundle.resolve()),
              "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "policy": "arm64; dependencies and rpaths stay within bundle except /System/Library and /usr/lib; signed; --version loads",
              "passed": False}
    audit = BundleAudit(args.bundle)
    try:
        if sys.platform != "darwin":
            raise RuntimeError("This validator must run on macOS with /usr/bin/otool, lipo and codesign")
        with (audit.bundle / "Contents/Info.plist").open("rb") as source:
            info = plistlib.load(source)
        executable = (audit.bundle / "Contents/MacOS" / info["CFBundleExecutable"]).resolve()
        if not inside(executable, audit.bundle):
            raise RuntimeError("CFBundleExecutable escapes the bundle")
        audit.collect()
        if not audit.images:
            audit.fail("No arm64 Mach-O images found")
        audit.audit_dependencies(executable)
        report["images"] = list(audit.images.values())
        report["dependency_checks"] = audit.edges
        report["unsafe_rpaths"] = audit.bad_rpaths
        try:
            run_tool(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(audit.bundle)])
            report["codesign"] = {"passed": True}
        except RuntimeError as error:
            report["codesign"] = {"passed": False, "error": str(error)}
            audit.fail(str(error))
        report["smoke_checks"] = {}
        # Only LanBridge's known --version entry points are executed; arbitrary nested executables are audited statically.
        for entry in (executable, audit.bundle / "Contents/MacOS/deskflow-core"):
            result = smoke_check(entry)
            report["smoke_checks"][audit.short(entry)] = result
            if not result["passed"]:
                audit.fail("--version failed for {}: {}".format(audit.short(entry), result.get("stderr", result.get("error", ""))))
    except (OSError, RuntimeError, KeyError, ValueError, subprocess.TimeoutExpired) as error:
        audit.fail(str(error))
    report["errors"] = audit.errors
    report["passed"] = not audit.errors
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("{}: {} Mach-O images; {} dependency checks; report {}".format(
        "PASS" if report["passed"] else "FAIL", len(audit.images), len(audit.edges), args.report))
    for error in audit.errors:
        print(error, file=sys.stderr)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
