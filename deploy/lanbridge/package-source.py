#!/usr/bin/python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Package the current Git worktree, including non-ignored new source files.

Usage: package-source.py --archive dist/source.tar.gz --manifest dist/source.json
Regular-file hashes describe the archived bytes. Symlink hashes describe the
filesystem-encoded link target, not the contents of the target file. Archive
timestamps and ownership are normalized so unchanged inputs produce equal bytes.
"""

import argparse
import datetime
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import sys
import tarfile
import tempfile


UPSTREAM_COMMIT = "081f6478e654a112168879e67a41ce7d7e4ae3af"
ARCHIVE_ROOT = "lanbridge"
EXCLUDED_ROOTS = {
    ".git", ".cache", ".omx", ".venv", "build", "dist", "deps", "tmp",
    "vcpkg", "vcpkg_installed",
}


def git(root, *args):
    result = subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, check=False,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


def source_files(root):
    """Use NUL delimiters, preserving spaces, newlines and Unicode in paths."""
    paths = set(git(root, "ls-files", "--cached", "--others", "--exclude-standard", "-z").split(b"\0"))
    for raw in sorted(paths):
        if not raw:
            continue
        name = os.fsdecode(raw)
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or not relative.parts:
            raise RuntimeError("Unsafe source path: {!r}".format(name))
        if relative.parts[0] in EXCLUDED_ROOTS or "__pycache__" in relative.parts:
            continue
        path = root / name
        # Git can still list files beneath a tracked directory replaced by a
        # symlink. Never follow such a directory, even if its target is local.
        parent = root
        for part in relative.parts[:-1]:
            parent /= part
            if parent.is_symlink():
                raise RuntimeError("Source parent is a symlink: {}".format(parent))
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            continue  # A tracked deletion belongs to the current worktree.
        if stat.S_ISLNK(metadata.st_mode):
            target = os.readlink(path)
            try:
                (path.parent / target).resolve().relative_to(root)
            except (ValueError, RuntimeError):
                raise RuntimeError("Symlink escapes source root: {} -> {}".format(name, target))
            if os.path.isabs(target):
                raise RuntimeError("Absolute symlink is not portable: {} -> {}".format(name, target))
        elif not stat.S_ISREG(metadata.st_mode):
            raise RuntimeError("Unsupported source type (including Git submodules): {}".format(name))
        yield name, path, metadata


def identity(metadata):
    return (
        metadata.st_dev, metadata.st_ino, metadata.st_mode, metadata.st_size,
        metadata.st_mtime_ns, metadata.st_ctime_ns,
    )


class HashingReader:
    def __init__(self, source):
        self.source = source
        self.digest = hashlib.sha256()

    def read(self, size):
        data = self.source.read(size)
        self.digest.update(data)
        return data


def write_archive(snapshots, destination):
    records = []
    with destination.open("wb") as output:
        with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for name, path, metadata in snapshots:
                    if identity(path.lstat()) != identity(metadata):
                        raise RuntimeError("Source changed while packaging: {}".format(name))
                    member = tarfile.TarInfo(ARCHIVE_ROOT + "/" + name)
                    member.mode = stat.S_IMODE(metadata.st_mode)
                    record = {"path": name, "mode": format(member.mode, "04o")}
                    if stat.S_ISLNK(metadata.st_mode):
                        member.type = tarfile.SYMTYPE
                        member.linkname = os.readlink(path)
                        data = os.fsencode(member.linkname)
                        archive.addfile(member)
                        record.update(type="symlink", link_target=member.linkname,
                                      sha256=hashlib.sha256(data).hexdigest(), bytes=len(data))
                    else:
                        # O_NOFOLLOW also rejects replacement of the final file
                        # with a symlink between the metadata check and open.
                        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
                        with os.fdopen(descriptor, "rb") as source:
                            if identity(os.fstat(source.fileno())) != identity(metadata):
                                raise RuntimeError("Source changed while packaging: {}".format(name))
                            member.size = metadata.st_size
                            reader = HashingReader(source)
                            archive.addfile(member, reader)
                            if identity(os.fstat(source.fileno())) != identity(metadata):
                                raise RuntimeError("Source changed while packaging: {}".format(name))
                            record.update(type="file", sha256=reader.digest.hexdigest(), bytes=member.size)
                    records.append(record)
    # Check files already copied as well, rather than silently packaging a
    # mixture when an editor changes an earlier input during the archive write.
    for name, path, metadata in snapshots:
        if identity(path.lstat()) != identity(metadata):
            raise RuntimeError("Source changed while packaging: {}".format(name))
    return records


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for data in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def package(source, archive, manifest):
    root = source.resolve(strict=True)
    actual_root = Path(os.fsdecode(git(root, "rev-parse", "--show-toplevel")).strip()).resolve()
    if actual_root != root:
        raise RuntimeError("--source must be the Git worktree root: {}".format(actual_root))
    archive = archive.absolute()
    manifest = manifest.absolute()
    if archive.resolve() == manifest.resolve():
        raise RuntimeError("Archive and manifest must have different paths")
    for path in (archive, manifest):
        if os.path.lexists(path):
            raise RuntimeError("Refusing to overwrite: {}".format(path))
    head = git(root, "rev-parse", "HEAD").decode("ascii").strip()
    dirty = bool(git(root, "status", "--porcelain", "-z", "--untracked-files=normal"))
    # Inventory before creating temporary outputs, so an output directory
    # inside the worktree cannot accidentally add the archive to itself.
    snapshots = list(source_files(root))
    if not snapshots:
        raise RuntimeError("No source files found")
    temporary = []
    published = []
    try:
        for path in (archive, manifest):
            path.parent.mkdir(parents=True, exist_ok=True)
            descriptor, name = tempfile.mkstemp(prefix=".source-package-", dir=path.parent)
            os.close(descriptor)
            temporary.append(Path(name))
        records = write_archive(snapshots, temporary[0])
        result = {
            "upstream": "Deskflow v1.27.0",
            "upstream_commit": UPSTREAM_COMMIT,
            "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "git": {"head": head, "dirty": dirty},
            "archive": {"path": archive.name, "sha256": file_digest(temporary[0]),
                        "bytes": temporary[0].stat().st_size, "root": ARCHIVE_ROOT},
            "files": records,
        }
        temporary[1].write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        for pending, final in zip(temporary, (archive, manifest)):
            pending.chmod(0o644)
            # link publishes a completed file atomically and refuses even a
            # concurrent output creation; replace/rename could overwrite it.
            os.link(pending, final)
            published.append((pending, final))
        return result
    except Exception:
        for pending, final in published:
            if final.exists() and os.path.samestat(pending.stat(), final.lstat()):
                final.unlink()
        raise
    finally:
        for path in temporary:
            path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = package(args.source, args.archive, args.manifest)
    except (OSError, RuntimeError, ValueError, tarfile.TarError) as error:
        print("Source package failed: {}".format(error), file=sys.stderr)
        return 1
    print("Packaged {} source files: {}\nSHA256 {}\nManifest {}".format(
        len(result["files"]), args.archive, result["archive"]["sha256"], args.manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
