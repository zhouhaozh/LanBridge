#!/usr/bin/python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Assemble a single offline installer from verified application/runtime bundles.

The product binaries are copied unchanged. Ubuntu host packages must already
have a checksum manifest derived from authenticated repository indexes.
"""

import argparse
import datetime
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import sys
import tarfile
import tempfile


APP_ID = "org.lanbridge.LanBridge"
RUNTIME_REF = "runtime/org.kde.Platform/x86_64/6.11"


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def checked_file(path, expected=None):
    metadata = path.lstat()
    if not stat.S_ISREG(metadata.st_mode):
        raise ValueError("Input must be a regular file, not a symlink: {}".format(path))
    actual = digest(path)
    if expected is not None and actual != expected:
        raise ValueError("SHA256 mismatch: {}".format(path))
    return {"sha256": actual, "bytes": metadata.st_size}


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def add_bytes(archive, name, data):
    member = tarfile.TarInfo(name)
    member.mode = 0o644
    member.size = len(data)
    archive.addfile(member, io.BytesIO(data))


class HashingReader:
    def __init__(self, source):
        self.source = source
        self.hash = hashlib.sha256()

    def read(self, size):
        data = self.source.read(size)
        self.hash.update(data)
        return data


def package(args):
    for value in (args.app_sha256, args.runtime_sha256, args.app_commit, args.runtime_commit):
        if not re.fullmatch(r"[0-9a-f]{64}", value):
            raise ValueError("Hashes and OSTree commits must be lowercase SHA256 values")
    for destination in (args.output, args.manifest):
        if os.path.lexists(destination):
            raise ValueError("Refusing to overwrite: {}".format(destination))
    if args.output.absolute() == args.manifest.absolute():
        raise ValueError("Installer and manifest must have different paths")

    inputs = {
        "app/lanbridge.flatpak": (args.app_bundle, checked_file(args.app_bundle, args.app_sha256)),
        "runtime/platform.flatpak": (args.runtime_bundle, checked_file(args.runtime_bundle, args.runtime_sha256)),
    }
    host_manifest_path = args.host_debs / "HOST-DEBS-MANIFEST.json"
    host_metadata = checked_file(host_manifest_path)
    host_manifest = json.loads(host_manifest_path.read_text(encoding="utf-8"))
    if not isinstance(host_manifest, dict):
        raise ValueError("Host manifest must be a JSON object")
    packages = host_manifest.get("packages")
    if not isinstance(packages, list) or not packages:
        raise ValueError("Host manifest must contain a nonempty packages array")
    seen = set()
    for item in packages:
        if not isinstance(item, dict):
            raise ValueError("Host package metadata must be a JSON object")
        name = item["filename"]
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.+%:=~-]+\.deb", name) or name in seen:
            raise ValueError("Unsafe or duplicate host package filename: {!r}".format(name))
        expected_hash = item["sha256"]
        if not isinstance(expected_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_hash):
            raise ValueError("Host package must have an authenticated SHA256: {}".format(name))
        if type(item["bytes"]) is not int or item["bytes"] < 0:
            raise ValueError("Host package size must be a nonnegative integer: {}".format(name))
        seen.add(name)
        metadata = checked_file(args.host_debs / name, expected_hash)
        if metadata["bytes"] != item["bytes"]:
            raise ValueError("Host package size mismatch: {}".format(name))
        inputs["host-debs/" + name] = (args.host_debs / name, metadata)
    if seen != {path.name for path in args.host_debs.glob("*.deb")}:
        raise ValueError("Host manifest and DEB directory must contain the same packages")
    inputs["host-debs/HOST-DEBS-MANIFEST.json"] = (host_manifest_path, host_metadata)
    payload_manifest = {
        "schema": 1,
        "version": "1.27.0",
        "target": {"distribution": "Ubuntu", "version": "24.04", "architecture": "x86_64"},
        "application": {"id": APP_ID, "commit": args.app_commit, **inputs["app/lanbridge.flatpak"][1]},
        "runtime": {"ref": RUNTIME_REF, "commit": args.runtime_commit, **inputs["runtime/platform.flatpak"][1]},
        "host_package_count": len(packages),
        "files": {name: metadata for name, (_, metadata) in sorted(inputs.items())},
    }
    manifest_data = json_bytes(payload_manifest)
    checksum_data = "".join(
        metadata["sha256"] + "  " + name + "\n" for name, (_, metadata) in sorted(inputs.items())
    )
    checksum_data += hashlib.sha256(manifest_data).hexdigest() + "  MANIFEST.json\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    published = []
    with tempfile.TemporaryDirectory(prefix=".offline-package-", dir=args.output.parent) as temporary:
        staging = Path(temporary)
        payload = staging / "payload.tar.gz"
        with payload.open("wb") as output:
            with gzip.GzipFile(filename="", mode="wb", compresslevel=1, fileobj=output, mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                    for name, (path, metadata) in sorted(inputs.items()):
                        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
                        with os.fdopen(descriptor, "rb") as source:
                            if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
                                raise ValueError("Input type changed: {}".format(path))
                            member = tarfile.TarInfo(name)
                            member.mode = 0o644
                            member.size = metadata["bytes"]
                            reader = HashingReader(source)
                            archive.addfile(member, reader)
                            if reader.hash.hexdigest() != metadata["sha256"]:
                                raise ValueError("Input changed while packaging: {}".format(path))
                    add_bytes(archive, "MANIFEST.json", manifest_data)
                    add_bytes(archive, "SHA256SUMS", checksum_data.encode("ascii"))
        payload_hash = digest(payload)
        template = args.template.read_text(encoding="utf-8")
        if not template.endswith("__LANBRIDGE_PAYLOAD__\n"):
            raise ValueError("Installer template must end with the payload marker")
        replacements = {
            "@PAYLOAD_SHA256@": payload_hash,
            "@PAYLOAD_BYTES@": str(payload.stat().st_size),
            "@PAYLOAD_LINE@": str(template.count("\n") + 1),
            "@APP_COMMIT@": args.app_commit,
            "@RUNTIME_COMMIT@": args.runtime_commit,
        }
        for token, value in replacements.items():
            if token not in template:
                raise ValueError("Template token missing: {}".format(token))
            template = template.replace(token, value)
        if re.search(r"@[A-Z_]+@", template):
            raise ValueError("Unresolved installer template token")
        installer = staging / "installer.run"
        with installer.open("wb") as output, payload.open("rb") as source:
            output.write(template.encode("utf-8"))
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                output.write(chunk)
        installer.chmod(0o755)
        result = {
            "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "installer": {"path": args.output.name, **checked_file(installer)},
            "payload": {"sha256": payload_hash, "bytes": payload.stat().st_size},
            **payload_manifest,
        }
        receipt = staging / "manifest.json"
        receipt.write_bytes(json_bytes(result))
        receipt.chmod(0o644)
        try:
            for pending, final in ((installer, args.output), (receipt, args.manifest)):
                # Atomic publication, with no overwrite even under a concurrent builder.
                os.link(pending, final)
                published.append((pending, final))
        except Exception:
            for pending, final in published:
                if final.exists() and os.path.samestat(pending.stat(), final.lstat()):
                    final.unlink()
            raise
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-bundle", type=Path, required=True)
    parser.add_argument("--app-sha256", required=True)
    parser.add_argument("--app-commit", required=True)
    parser.add_argument("--runtime-bundle", type=Path, required=True)
    parser.add_argument("--runtime-sha256", required=True)
    parser.add_argument("--runtime-commit", required=True)
    parser.add_argument("--host-debs", type=Path, required=True)
    parser.add_argument("--template", type=Path, default=Path(__file__).with_name("install-offline-linux.sh.in"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = package(args)
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError) as error:
        print("Offline installer packaging failed: {}".format(error), file=sys.stderr)
        return 1
    print(json.dumps(result["installer"], ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
